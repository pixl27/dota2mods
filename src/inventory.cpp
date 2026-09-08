// src/inventory.cpp — fake GC inventory records so Dota's own UI shows everything.
// MANUAL-MAP ONLY. Highest-risk module in the project. Smurf only, baby.
#include <Windows.h>
#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <map>
#include <fstream>
#include "db.h"
#include "json.hpp"

// Real LoadDB for the DLL: parses data/skins_full.json so the inject blob
// covers every skin. (The EXE has its own copy; the DLL needs one too.)
void LoadDB(const std::string& path) {
    const char* tries[] = { path.c_str(), "data/skins_full.json",
        "build/data/skins_full.json", "C:\\Temp\\opencode\\skins_full.json" };
    for (auto t : tries) {
        std::ifstream f(t);
        if (!f) continue;
        try {
            nlohmann::json j; f >> j;
            std::lock_guard<std::mutex> lk(g_DbMutex);
            g_DB.clear();
            if (j.contains("skins"))
                for (auto& e : j["skins"])
                    g_DB.push_back({ e.value("def", 0), e.value("name", ""),
                        e.value("hero", ""), e.value("slot", "misc"),
                        e.value("rarity", "common"), e.value("prefab", "") });
            if (!g_DB.empty()) return;
        } catch (...) {}
    }
}

// ---------- protobuf-lite writer (no lib needed for the 4 fields we fake) ----------
// CSOEconItem (dota_gcmessages_common.proto / gcsdk):
//   uint64 id = 1; uint32 account_id = 2; uint32 def_index = 4;
//   uint32 level = 5; uint32 quality = 6; uint32 inventory = 7;
//   We emit: id (fake 64-bit, high bit set so it never collides), account 0,
//   def_index = skin def, quality = rarity-mapped, inventory = 0xFFFFFFFF (backpack).

static void WriteVarint(std::vector<uint8_t>& o, uint64_t v) {
    while (v > 0x7F) { o.push_back((uint8_t)((v & 0x7F) | 0x80)); v >>= 7; }
    o.push_back((uint8_t)v);
}
static void WriteTag(std::vector<uint8_t>& o, int field, int wire) {
    WriteVarint(o, (uint64_t)(field << 3 | wire));
}

static int QualityFor(const std::string& rarity) {
    if (rarity.find("immortal") != std::string::npos) return 16; // gold display
    if (rarity.find("arcana") != std::string::npos)   return 9;  // green display
    if (rarity.find("legendary") != std::string::npos) return 7;
    if (rarity.find("mythical") != std::string::npos)  return 5;
    if (rarity.find("rare") != std::string::npos)      return 4;
    return 3;
}

static std::vector<uint8_t> FakeEconItem(uint64_t fakeId, int defIndex, const std::string& rarity) {
    std::vector<uint8_t> o;
    WriteTag(o, 1, 0); WriteVarint(o, fakeId);                    // id
    WriteTag(o, 2, 0); WriteVarint(o, 0);                         // account_id (0 = local)
    WriteTag(o, 4, 0); WriteVarint(o, (uint64_t)defIndex);        // def_index  <-- THE skin
    WriteTag(o, 5, 0); WriteVarint(o, 1);                         // level
    WriteTag(o, 6, 0); WriteVarint(o, (uint64_t)QualityFor(rarity)); // quality
    WriteTag(o, 7, 0); WriteVarint(o, 0xFFFFFFFFULL);              // inventory token
    return o;
}

// ---------- SO cache injection ----------
// CMsgSOCacheSubscribed layout (gcsdk_gcmessages.proto):
//   repeated CMsgSOSingleObject objects = 2, each:
//     { fixed64 owner = 1; int32 type_id = 2; bytes object_data = 3; }
// Econ items live at type_id 1. We append one SingleObject per skin,
// with object_data = serialized CSOEconItem above.
//
// Injection point: the GC dispatch in client.dll handling
// EGCBaseMsg k_EMsgGCClientWelcome / SOCacheSubscribed
// (CGCClient::OnMessage / CDOTAGCClient::HandleMessage). RVA comes from
// gc_hook.txt (see dump_offsets.py). Protobuf repeated fields concatenate
// cleanly, so we extend the buffer, then call the original.

#include <chrono>
#include <thread>
#include <atomic>

extern void DisableGCHook();

static std::atomic<bool> g_AutoUnhookEnabled{ true };
static std::atomic<bool> g_Injected{ false };

void SetAutoUnhook(bool enable) { g_AutoUnhookEnabled = enable; }
bool IsAutoUnhookEnabled() { return g_AutoUnhookEnabled; }
bool HasInjected() { return g_Injected; }

static uint64_t InitBaseId() {
    uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    // High range non-colliding dynamic ID without static signatures
    return (1ULL << 62) | (now << 24);
}
static uint64_t g_NextFakeId = 0;

// Build the full appended blob: serialized SingleObjects for every skin in g_DB.
// Called once at boot (DB is static), cached, appended to every Welcome/Cache msg.
static std::vector<uint8_t> g_InjectBlob;
static std::mutex g_InjectMutex;
static size_t g_InjectCount = 0;

void BuildInjectBlob() {
    std::lock_guard<std::mutex> lk(g_InjectMutex);
    g_InjectBlob.clear();
    g_InjectCount = 0;
    if (g_NextFakeId == 0) g_NextFakeId = InitBaseId();
    std::lock_guard<std::mutex> db(g_DbMutex);
    for (auto& s : g_DB) {
        if (s.defIndex <= 0) continue;
        auto item = FakeEconItem(g_NextFakeId++, s.defIndex, s.rarity);
        // wrap in CMsgSOSingleObject { owner=0, type_id=1, object_data=item }
        std::vector<uint8_t> single;
        WriteTag(single, 1, 1);  // fixed64 owner
        for (int i = 0; i < 8; i++) single.push_back(0);
        WriteTag(single, 2, 0); WriteVarint(single, 1);          // type_id = 1 (econ)
        WriteTag(single, 3, 2); WriteVarint(single, item.size()); // object_data
        single.insert(single.end(), item.begin(), item.end());
        // append as repeated field 2 (objects) of the outer Cache message:
        WriteTag(g_InjectBlob, 2, 2);
        WriteVarint(g_InjectBlob, single.size());
        g_InjectBlob.insert(g_InjectBlob.end(), single.begin(), single.end());
        g_InjectCount++;
    }
}

size_t InjectItemCount() { return g_InjectCount; }

// Hook target: the GC cache-dispatch fn in client.dll (RVA from gc_hook.txt).
// We don't parse — we extend the buffer, then call original.
typedef void(__fastcall* OnCacheFn)(void* self, void* msg, size_t len);
static OnCacheFn oOnCache = nullptr;
OnCacheFn* GetOnCacheOrigSlot() { return &oOnCache; }

void __fastcall hkOnCache(void* self, void* msg, size_t len) {
    std::lock_guard<std::mutex> lk(g_InjectMutex);
    if (!g_InjectBlob.empty() && msg && len > 0 && oOnCache) {
        size_t total = len + g_InjectBlob.size();
        uint8_t* grown = (uint8_t*)malloc(total);
        if (grown) {
            memcpy(grown, msg, len);
            memcpy(grown + len, g_InjectBlob.data(), g_InjectBlob.size());
            oOnCache(self, grown, total);
            free(grown);
            g_Injected = true;
            if (g_AutoUnhookEnabled) {
                // Post background thread to cleanly restore original client.dll .text bytes
                std::thread([]() {
                    Sleep(500);
                    DisableGCHook();
                }).detach();
            }
            return;
        }
    }
    if (oOnCache) oOnCache(self, msg, len);
}

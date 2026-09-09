#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <set>
#include "db.h"
#include "inventory.h"
#include "gc_loadout.h"
#include "gc_dispatch.h"
#include "../thirdparty/minhook/include/MinHook.h"
#include "diagnostics.h"

namespace {
std::mutex g_StateMutex, g_ReceiveMutex, g_LogMutex;
const char* g_LogPath = "C:\\Temp\\opencode\\wardrobe_gc.log";
InventoryStatus g_InventoryStatus;
std::atomic<int> g_Command{ 0 }; // Lifecycle changes run only on the inventory worker.
std::atomic<bool> g_Transform{ false }, g_ReceiveFailed{ false };
std::atomic<uint64_t> g_DeliveredAt{ 0 };
std::atomic<bool> g_ReserveInventory{ true };
gc::Bytes g_ItemFields;
gc::LocalLoadout g_LocalLoadout; // Guarded by g_ReceiveMutex, including sends.
struct LocalReply : gc::PendingPacket {
    uint64_t sequence = 0, queuedAt = 0;
    bool completesEquip = false;
    std::shared_ptr<const appearance::Snapshot> appearance;
};
std::deque<LocalReply> g_LocalReplies;
gc::PendingPacket g_Pending;
bool g_PendingInventory = false;
uint64_t g_QueueSequence = 0, g_PendingSequence = 0;
uint64_t g_AppearanceRevision = 0;
gc::CallbackWake g_CallbackWake;
int g_SteamPipe = 0, g_SteamUser = 0;
gc::RefreshWindow g_Refresh;
bool g_Setup = false, g_AvailableEnabled = false, g_RetrieveEnabled = false, g_SendEnabled = false;
bool g_CallbackEnabled = false, g_FreeCallbackEnabled = false;
bool g_Waiting = false, g_StopRequested = false;
uint64_t g_StopAt = 0, g_SteamId = 0;
void* g_Coordinator = nullptr;
void* g_AvailableTarget = nullptr;
void* g_RetrieveTarget = nullptr;
void* g_SendTarget = nullptr;
void* g_CallbackTarget = nullptr;
void* g_FreeCallbackTarget = nullptr;

// SteamGameCoordinator001 has three methods in this order, with no destructor
// slot. Use its public byte-buffer ABI, never a guessed client.dll callback.
// https://partner.steamgames.com/doc/api/ISteamGameCoordinator
using SendFn = gc::Result(__fastcall*)(void*, uint32_t, const void*, uint32_t);
using AvailableFn = bool(__fastcall*)(void*, uint32_t*);
using RetrieveFn = gc::Result(__fastcall*)(void*, uint32_t*, void*, uint32_t, uint32_t*);
SendFn g_Send = nullptr;
AvailableFn g_OriginalAvailable = nullptr;
RetrieveFn g_OriginalRetrieve = nullptr;
using CallbackFn = bool(__cdecl*)(int, gc::SteamCallback*, void*);
using FreeCallbackFn = void(__cdecl*)(int);
CallbackFn g_OriginalCallback = nullptr;
FreeCallbackFn g_OriginalFreeCallback = nullptr;
std::deque<std::string> g_LogLines;

void Log(const char* format, ...) {
    char line[1024];
    va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    SYSTEMTIME time{}; GetLocalTime(&time);
    char stamp[32]; snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u ",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    std::lock_guard<std::mutex> lock(g_LogMutex);
    if (g_LogLines.size() < 512) g_LogLines.emplace_back(std::string(stamp) + line);
}
void FlushLog() {
    std::deque<std::string> lines;
    { std::lock_guard<std::mutex> lock(g_LogMutex); lines.swap(g_LogLines); }
    if (lines.empty()) return;
    RotateLogIfLarge(g_LogPath);
    FILE* file = nullptr;
    fopen_s(&file, g_LogPath, "a");
    if (!file) return;
    for (const auto& line : lines) { fputs(line.c_str(), file); fputc('\n', file); }
    fclose(file);
}
void SetPhase(InventoryPhase phase, const std::string& detail) {
    { std::lock_guard<std::mutex> lock(g_StateMutex);
      g_InventoryStatus.phase = phase; g_InventoryStatus.detail = detail; }
    Log("%s", detail.c_str());
}
void HookError(const char* action, MH_STATUS result) {
    SetPhase(InventoryPhase::Failed, std::string(action) + ": " + MH_StatusToString(result));
}
void PublishHookState() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_InventoryStatus.hookActive = g_AvailableEnabled || g_RetrieveEnabled || g_SendEnabled || g_CallbackEnabled || g_FreeCallbackEnabled;
    g_InventoryStatus.equipActive = g_AvailableEnabled && g_RetrieveEnabled && g_SendEnabled &&
        g_CallbackEnabled && g_FreeCallbackEnabled && g_Transform;
}
void MarkDelivered() {
    g_DeliveredAt = GetTickCount64();
    g_ReserveInventory = false;
    if (!appearance::Read()) appearance::Publish(g_LocalLoadout.Appearance(++g_AppearanceRevision));
    { std::lock_guard<std::mutex> lock(g_StateMutex);
      g_InventoryStatus.recordsDelivered = true; ++g_InventoryStatus.cacheDeliveries; }
    if (g_Transform) SetPhase(InventoryPhase::Delivered, "Local inventory delivered; equip handler is active");
}

bool __cdecl OnCallback(int pipe, gc::SteamCallback* callback, void* call) {
    if (pipe != g_SteamPipe) return g_OriginalCallback(pipe, callback, call);
    const auto thread = GetCurrentThreadId();
    {
        std::unique_lock<std::mutex> receive(g_ReceiveMutex, std::try_to_lock);
        // A nested pump must not free a real event in place of our outstanding event.
        if (receive.owns_lock() && g_CallbackWake.outstanding && g_CallbackWake.thread == thread) return false;
    }
    // Steam events always run first, including GC connection/keepalive callbacks.
    if (g_OriginalCallback(pipe, callback, call)) return true;
    std::unique_lock<std::mutex> receive(g_ReceiveMutex, std::try_to_lock);
    if (!receive.owns_lock()) return false;
    { std::lock_guard<std::mutex> lock(g_StateMutex); ++g_InventoryStatus.callbackPolls; }
    const auto head = !g_Pending.Empty() ? g_PendingSequence :
        !g_LocalReplies.empty() ? g_LocalReplies.front().sequence : 0;
    const auto size = !g_Pending.Empty() ? g_Pending.bytes.size() :
        !g_LocalReplies.empty() ? g_LocalReplies.front().bytes.size() : 0;
    if (!g_CallbackWake.Issue(g_SteamUser, thread, GetTickCount64(), head, uint32_t(size), callback)) return false;
    { std::lock_guard<std::mutex> lock(g_StateMutex); ++g_InventoryStatus.wakeups; }
    return true;
}
void __cdecl OnFreeCallback(int pipe) {
    if (pipe == g_SteamPipe) {
        std::lock_guard<std::mutex> receive(g_ReceiveMutex);
        if (g_CallbackWake.Release(GetCurrentThreadId())) return;
    }
    g_OriginalFreeCallback(pipe);
}

gc::Result __fastcall OnSend(void* self, uint32_t type, const void* data, uint32_t size) {
    if (self != g_Coordinator || ((type & ~gc::ProtoFlag) != gc::EquipItems &&
        (type & ~gc::ProtoFlag) != gc::AdjustEquipped)) return g_Send(self, type, data, size);
    {
        std::lock_guard<std::mutex> receive(g_ReceiveMutex);
        if (g_Transform) {
            { std::lock_guard<std::mutex> lock(g_StateMutex);
              ++g_InventoryStatus.equipRequests; g_InventoryStatus.lastEquipType = type & ~gc::ProtoFlag; }
            try {
                if (g_LocalReplies.size() >= 128) {
                    std::lock_guard<std::mutex> lock(g_StateMutex);
                    g_InventoryStatus.equipDetail = "Equip queue is full; wait for Dota to read updates and retry";
                    return gc::InvalidMessage;
                }
                const auto started = std::chrono::steady_clock::now();
                auto result = g_LocalLoadout.Equip(type, data, size);
                const auto elapsed = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started).count());
                if (result.state != gc::EquipState::PassThrough) {
                    Log("Equip message %u: %s (%u entries, %llu us)", type & ~gc::ProtoFlag, result.detail.c_str(), result.count, elapsed);
                    { std::lock_guard<std::mutex> lock(g_StateMutex);
                      g_InventoryStatus.equipDetail = result.detail;
                      g_InventoryStatus.lastEquipUs = elapsed;
                      if (elapsed > g_InventoryStatus.maxEquipUs) g_InventoryStatus.maxEquipUs = elapsed;
                      if (result.state == gc::EquipState::Handled) {
                          g_InventoryStatus.localEquips += result.count; ++g_InventoryStatus.acceptedEquips;
                      } }
                    if (result.state == gc::EquipState::Invalid) return gc::InvalidMessage;
                    auto appearance = g_LocalLoadout.Appearance(++g_AppearanceRevision);
                    for (size_t i = 0; i < result.replies.size(); ++i) {
                        LocalReply reply;
                        static_cast<gc::PendingPacket&>(reply) = std::move(result.replies[i]);
                        reply.sequence = ++g_QueueSequence; reply.queuedAt = GetTickCount64();
                        reply.completesEquip = i + 1 == result.replies.size();
                        if (reply.completesEquip) reply.appearance = appearance;
                        g_LocalReplies.push_back(std::move(reply));
                    }
                    return gc::OK;
                }
                { std::lock_guard<std::mutex> lock(g_StateMutex);
                  g_InventoryStatus.equipDetail = "Owned-item equip sent to Steam"; }
            } catch (const std::exception& error) {
                Log("Local equip failed: %s", error.what());
                std::lock_guard<std::mutex> lock(g_StateMutex);
                g_InventoryStatus.equipDetail = std::string("Local equip failed: ") + error.what();
                return gc::InvalidMessage;
            }
        }
    }
    // Release our mutex before calling Steam; callbacks can re-enter the receiver.
    return g_Send(self, type, data, size);
}

bool __fastcall OnAvailable(void* self, uint32_t* size) {
    if (self != g_Coordinator) return g_OriginalAvailable(self, size);
    { std::lock_guard<std::mutex> lock(g_StateMutex); ++g_InventoryStatus.polls; }
    {
        std::lock_guard<std::mutex> receive(g_ReceiveMutex);
        if (!g_Pending.Empty()) {
            if (size) *size = uint32_t(g_Pending.bytes.size());
            return true;
        }
    }
    // Never hold our state mutex while Steam runs (it can re-enter OnSend).
    const bool available = g_OriginalAvailable(self, size);
    // Reserve room before Dota allocates its receive buffer. A caller that skips
    // this method is handled by g_Pending in OnRetrieve instead.
    if (available && size && g_Transform && g_ReserveInventory) {
        const uint64_t reserve = uint64_t(*size) + g_ItemFields.size() + 65536;
        if (reserve <= gc::MaxPacket) *size = uint32_t(reserve);
    }
    if (available) return true;
    std::lock_guard<std::mutex> receive(g_ReceiveMutex);
    if (g_LocalReplies.empty()) return false;
    if (size) *size = uint32_t(g_LocalReplies.front().bytes.size());
    return true;
}
gc::Result __fastcall OnRetrieve(void* self, uint32_t* type, void* dest, uint32_t capacity, uint32_t* size) {
    if (self != g_Coordinator) return g_OriginalRetrieve(self, type, dest, capacity, size);
    { std::lock_guard<std::mutex> lock(g_StateMutex); ++g_InventoryStatus.receives; }
    {
        std::lock_guard<std::mutex> receive(g_ReceiveMutex);
        if (!g_Pending.Empty()) {
            const auto result = g_Pending.Deliver(type, dest, capacity, size);
            if (result == gc::OK && g_PendingInventory) MarkDelivered();
            return result;
        }
    }
    // Drain genuine GC traffic before local previews. A preview must never
    // consume a real message's wakeup and strand the GC's connection response.
    const auto result = g_OriginalRetrieve(self, type, dest, capacity, size);
    std::lock_guard<std::mutex> receive(g_ReceiveMutex);
    if (result == gc::NoMessage && !g_LocalReplies.empty()) {
        auto& reply = g_LocalReplies.front();
        const auto kind = reply.type & ~gc::ProtoFlag;
        const auto delivered = reply.Deliver(type, dest, capacity, size);
        if (delivered == gc::OK) {
            if (reply.appearance) appearance::Publish(reply.appearance);
            const auto latency = GetTickCount64() - reply.queuedAt;
            {
                std::lock_guard<std::mutex> lock(g_StateMutex);
                if (kind == gc::UpdateMultiple) ++g_InventoryStatus.equipUpdates;
                if (kind == gc::EquipItemsResponse) ++g_InventoryStatus.equipAcks;
                if (reply.completesEquip) ++g_InventoryStatus.completedEquips;
                g_InventoryStatus.lastDeliveryMs = latency;
                g_InventoryStatus.equipDetail = reply.completesEquip ? "Local equip update and acknowledgement delivered" : "Equip update delivered; acknowledgement pending";
            }
            Log("Delivered local message %u, sequence %llu, after %llu ms", kind, reply.sequence, latency);
            g_LocalReplies.pop_front();
        }
        return delivered;
    }
    if (result != gc::OK || !type || !size || !dest) return result;
    { std::lock_guard<std::mutex> lock(g_StateMutex);
      ++g_InventoryStatus.packets; g_InventoryStatus.lastType = *type & ~gc::ProtoFlag; }
    if ((*type & ~gc::ProtoFlag) == 4004 || (*type & ~gc::ProtoFlag) == 4009) {
        gc::Bytes body, header;
        const bool parsed = gc::Unpack(*type, dest, *size, body, header);
        const auto status = parsed && (*type & ~gc::ProtoFlag) == 4009 ? uint32_t(gc::Value(body, 1)) : 0;
        { std::lock_guard<std::mutex> lock(g_StateMutex);
          ++g_InventoryStatus.connectionMessages; g_InventoryStatus.connectionStatus = status; }
        Log("GC connection message %u (status %u); %llu local replies waiting", *type & ~gc::ProtoFlag, status, g_LocalReplies.size());
    }
    if (!g_Transform || *size > capacity) return result;
    try {
        auto patch = g_LocalLoadout.RewritePacket(*type, dest, *size);
        if (patch.state == gc::PatchState::Invalid) {
            SetPhase(InventoryPhase::Failed, "Inventory packet format was not recognized; original packet preserved");
            g_ReceiveFailed = true;
        } else if (patch.state == gc::PatchState::NoLocalCache) {
            Log("Received GC message %u without a full local inventory cache", *type & ~gc::ProtoFlag);
        } else if (patch.state == gc::PatchState::Modified) {
            Log("Prepared GC message %u: %u -> %llu bytes", *type & ~gc::ProtoFlag,
                *size, static_cast<unsigned long long>(patch.bytes.size()));
            g_Pending.type = *type; g_Pending.bytes = std::move(patch.bytes);
            g_PendingSequence = ++g_QueueSequence;
            g_PendingInventory = (*type & ~gc::ProtoFlag) == gc::CacheSubscribed || (*type & ~gc::ProtoFlag) == gc::ClientWelcome;
            const auto delivered = g_Pending.Deliver(type, dest, capacity, size);
            if (delivered == gc::OK && g_PendingInventory) MarkDelivered();
            return delivered;
        }
    } catch (const std::exception& error) {
        SetPhase(InventoryPhase::Failed, std::string("Inventory processing failed: ") + error.what());
        g_ReceiveFailed = true;
    }
    return result;
}

template <typename T> T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
bool Setup() {
    HMODULE api = GetModuleHandleA("steam_api64.dll");
    if (!api) { SetPhase(InventoryPhase::Failed, "Steam API is not loaded; retry after Dota reaches its menu"); return false; }
    auto client = Export<void*(__cdecl*)()>(api, "SteamClient");
    auto user = Export<int(__cdecl*)()>(api, "SteamAPI_GetHSteamUser");
    auto pipe = Export<int(__cdecl*)()>(api, "SteamAPI_GetHSteamPipe");
    auto generic = Export<void*(__cdecl*)(void*, int, int, const char*)>(api, "SteamAPI_ISteamClient_GetISteamGenericInterface");
    auto steamUser = Export<void*(__cdecl*)()>(api, "SteamAPI_SteamUser_v023");
    auto steamId = Export<uint64_t(__cdecl*)(void*)>(api, "SteamAPI_ISteamUser_GetSteamID");
    if (!client || !user || !pipe || !generic || !steamUser || !steamId) {
        SetPhase(InventoryPhase::Failed, "Required Steam API exports are missing in this game build"); return false;
    }
    void* steamClient = client(); void* currentUser = steamUser();
    const int hUser = user(), hPipe = pipe();
    if (!steamClient || !currentUser || !hUser || !hPipe) {
        SetPhase(InventoryPhase::Failed, "Steam session is not ready; retry after connecting to Steam"); return false;
    }
    g_Coordinator = generic(steamClient, hUser, hPipe, "SteamGameCoordinator001");
    g_SteamId = steamId(currentUser);
    g_SteamUser = hUser; g_SteamPipe = hPipe;
    if (!g_Coordinator || !g_SteamId || !uint32_t(g_SteamId)) {
        SetPhase(InventoryPhase::Failed, "Steam Game Coordinator interface or local account is unavailable"); return false;
    }
    gc::Bytes cachedInventory;
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        const auto game = std::filesystem::path(exePath).parent_path().parent_path().parent_path() / "dota";
        const auto cachePath = game / ("cache_" + std::to_string(uint32_t(g_SteamId)) + "_1.soc");
        std::ifstream cacheFile(cachePath, std::ios::binary | std::ios::ate);
        if (cacheFile && cacheFile.tellg() > 0 && cacheFile.tellg() <= std::streamoff(gc::MaxPacket)) {
            gc::Bytes bytes(size_t(cacheFile.tellg())); cacheFile.seekg(0);
            if (cacheFile.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
                const auto cached = gc::InspectDiskInventory(bytes, g_SteamId);
                { std::lock_guard<std::mutex> lock(g_StateMutex); g_InventoryStatus.cachedLocalItems = cached.localItems; }
                if (cached.localItems) Log("Dota disk cache contains %llu saved local items from a previous run", cached.localItems);
                if (cached.valid) cachedInventory = std::move(bytes);
            }
        }
    }
    const auto callbackModule = GetModuleHandleA("steamclient64.dll");
    g_CallbackTarget = callbackModule ? reinterpret_cast<void*>(GetProcAddress(callbackModule, "Steam_BGetCallback")) : nullptr;
    g_FreeCallbackTarget = callbackModule ? reinterpret_cast<void*>(GetProcAddress(callbackModule, "Steam_FreeLastCallback")) : nullptr;
    if (!g_CallbackTarget || !g_FreeCallbackTarget) {
        SetPhase(InventoryPhase::Failed, "Steam callback exports unavailable; local equip cannot notify Dota"); return false;
    }

    LoadDB("data/skins_full.json");
    std::vector<uint32_t> definitions;
    { std::lock_guard<std::mutex> lock(g_DbMutex);
      std::set<uint32_t> seen;
      for (const auto& skin : g_DB) if (skin.defIndex > 0 && seen.insert(uint32_t(skin.defIndex)).second)
          definitions.push_back(uint32_t(skin.defIndex)); }
    if (definitions.empty()) {
        SetPhase(InventoryPhase::Failed, "Skin database is missing or empty; rebuild data/skins_full.json and retry"); return false;
    }
    const uint64_t seconds = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    g_LocalLoadout.Reset(definitions, g_SteamId, (1ULL << 62) | (seconds << 24));
    if (!cachedInventory.empty()) {
        g_LocalLoadout.RestoreDiskSelections(cachedInventory);
        Log("Restored %llu saved appearance slots", g_LocalLoadout.Appearance(0)->selections.size());
    }
    g_ItemFields = g_LocalLoadout.CatalogFields();
    { std::lock_guard<std::mutex> lock(g_StateMutex); g_InventoryStatus.items = definitions.size(); }
    if (g_ItemFields.size() > gc::MaxPacket / 2) {
        SetPhase(InventoryPhase::Failed, "Skin database is too large for the inventory receive buffer"); return false;
    }
    auto table = *static_cast<void***>(g_Coordinator);
    g_SendTarget = table[0];
    g_AvailableTarget = table[1]; g_RetrieveTarget = table[2];
    auto mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) { HookError("GC initialization failed", mh); return false; }
    struct Creation { void* target; void* detour; void** original; };
    const Creation hooks[] = {
        {g_AvailableTarget, reinterpret_cast<void*>(&OnAvailable), reinterpret_cast<void**>(&g_OriginalAvailable)},
        {g_RetrieveTarget, reinterpret_cast<void*>(&OnRetrieve), reinterpret_cast<void**>(&g_OriginalRetrieve)},
        {g_FreeCallbackTarget, reinterpret_cast<void*>(&OnFreeCallback), reinterpret_cast<void**>(&g_OriginalFreeCallback)},
        {g_CallbackTarget, reinterpret_cast<void*>(&OnCallback), reinterpret_cast<void**>(&g_OriginalCallback)},
        {g_SendTarget, reinterpret_cast<void*>(&OnSend), reinterpret_cast<void**>(&g_Send)}
    };
    for (size_t i = 0; i < std::size(hooks); ++i) {
        mh = MH_CreateHook(hooks[i].target, hooks[i].detour, hooks[i].original);
        if (mh != MH_OK) {
            for (size_t j = i; j > 0; --j) MH_RemoveHook(hooks[j - 1].target);
            HookError("GC receiver/notification hook creation failed", mh); return false;
        }
    }
    g_Setup = true;
    Log("SteamGameCoordinator001 receiver v6 ready; %llu catalog items; econ service 1 only",
        static_cast<unsigned long long>(definitions.size()));
    return true;
}
bool EnableHooks() {
    const std::pair<void*, bool*> hooks[] = {
        {g_AvailableTarget, &g_AvailableEnabled}, {g_RetrieveTarget, &g_RetrieveEnabled},
        {g_FreeCallbackTarget, &g_FreeCallbackEnabled}, {g_CallbackTarget, &g_CallbackEnabled}, {g_SendTarget, &g_SendEnabled}
    };
    for (size_t i = 0; i < std::size(hooks); ++i) {
        auto mh = MH_EnableHook(hooks[i].first);
        if (mh != MH_OK && mh != MH_ERROR_ENABLED) {
            for (size_t j = i; j > 0; --j) {
                const auto rollback = MH_DisableHook(hooks[j - 1].first);
                *hooks[j - 1].second = rollback != MH_OK && rollback != MH_ERROR_DISABLED;
            }
            PublishHookState(); HookError("Enabling GC receiver/notification failed", mh); return false;
        }
        *hooks[i].second = true;
    }
    PublishHookState(); return true;
}
void StopWhenDrained() {
    g_Transform = false;
    { std::lock_guard<std::mutex> receive(g_ReceiveMutex);
      if (!g_Pending.Empty() || !g_LocalReplies.empty() || g_CallbackWake.outstanding) return; }
    // Do not hold the receive mutex while MinHook suspends other threads.
    const std::pair<void*, bool*> hooks[] = {
        {g_SendTarget, &g_SendEnabled}, {g_CallbackTarget, &g_CallbackEnabled}, {g_FreeCallbackTarget, &g_FreeCallbackEnabled},
        {g_RetrieveTarget, &g_RetrieveEnabled}, {g_AvailableTarget, &g_AvailableEnabled}
    };
    for (const auto& hook : hooks) if (*hook.second) {
        auto mh = MH_DisableHook(hook.first);
        if (mh != MH_OK && mh != MH_ERROR_DISABLED) {
            HookError("Disabling GC receiver/notification failed", mh); g_StopRequested = false; PublishHookState(); return;
        }
        *hook.second = false;
    }
    g_StopRequested = false; PublishHookState(); Log("GC receiver paused; no pending packet");
}
}

void LoadDB(const std::string& path) {
    const char* paths[] = { path.c_str(), "data/skins_full.json", "build/data/skins_full.json",
        "skins_full.json", "C:\\Temp\\opencode\\skins_full.json" };
    for (const auto* candidate : paths) {
        std::ifstream file(candidate); if (!file) continue;
        try {
            json data; file >> data;
            if (!data.contains("skins") || !data["skins"].is_array()) continue;
            std::vector<SkinEntry> entries;
            for (const auto& e : data["skins"]) {
                if (!e.is_object() || e.value("def", 0) <= 0) continue;
                entries.push_back({ e.value("def", 0), e.value("name", ""), e.value("hero", ""),
                    e.value("slot", "misc"), e.value("rarity", "common"), e.value("prefab", "") });
            }
            if (entries.empty()) continue;
            { std::lock_guard<std::mutex> lock(g_DbMutex); g_DB = std::move(entries); }
            Log("Loaded skin database: %s", candidate); return;
        } catch (const std::exception& error) { Log("Cannot read %s: %s", candidate, error.what()); }
    }
}
void InitializeInventory() {
    CreateDirectoryA("C:\\Temp", nullptr); CreateDirectoryA("C:\\Temp\\opencode", nullptr);
    Log("Starting Wardrobe GC receiver v6, PID %lu, build %s %s", GetCurrentProcessId(), __DATE__, __TIME__);
    RequestInventoryRefresh();
}
void RequestInventoryRefresh() { g_Command = 1; }
void PauseInventory() { g_Command = 2; }
InventoryStatus GetInventoryStatus() {
    std::lock_guard<std::mutex> lock(g_StateMutex); return g_InventoryStatus;
}
static void TickInventoryImpl() {
    const uint64_t now = GetTickCount64();
    const int command = g_Command.exchange(0);
    if (command == 2) {
        g_Waiting = false; g_Transform = false; g_StopRequested = true; g_StopAt = now;
        SetPhase(InventoryPhase::Paused, "Inventory receiver paused");
    } else if (command == 1) {
        // Retrying an already delivered 13k-item catalog forces Dota to rebuild
        // its loadout UI. It is unnecessary for local equips and can stall it.
        if (g_Setup && GetInventoryStatus().recordsDelivered && !g_ReceiveFailed) {
            if (!EnableHooks()) return;
            g_Waiting = false; g_Transform = true; g_StopRequested = false;
            PublishHookState();
            SetPhase(InventoryPhase::Delivered, "Inventory already delivered; receiver resumed without a full reload");
            return;
        }
        g_Waiting = false; g_Transform = false;
        if ((!g_Setup && !Setup()) || !EnableHooks()) return;
        g_StopRequested = false; g_DeliveredAt = 0; g_ReceiveFailed = false;
        g_Refresh.Reset(now); g_Waiting = true; g_Transform = true;
        g_ReserveInventory = true;
        PublishHookState();
        { std::lock_guard<std::mutex> lock(g_StateMutex); g_InventoryStatus.refreshes = 0; }
        SetPhase(InventoryPhase::Waiting, "Requesting a fresh inventory cache from the GC");
    }
    const auto delivered = g_DeliveredAt.exchange(0);
    if (delivered) {
        g_Waiting = false; // Keep the receiver and notifications active for native loadout clicks.
    }
    if (g_ReceiveFailed.exchange(false)) {
        g_Waiting = false; g_Transform = false; g_StopRequested = true; g_StopAt = now;
    }
    if (g_StopRequested && now >= g_StopAt) StopWhenDrained();
    PublishHookState();
    if (!g_Waiting) return;
    if (g_Refresh.Expired(now)) {
        g_Waiting = false;
        auto state = GetInventoryStatus();
        SetPhase(InventoryPhase::TimedOut, state.packets
            ? "GC traffic arrived, but no full local inventory cache within 20 seconds; retry refresh"
            : "No inventory response within 20 seconds; check Dota's GC connection and retry refresh");
        return;
    }
    if (g_Refresh.Due(now)) {
        const auto packet = gc::RefreshPacket(g_SteamId);
        const auto result = g_Send(g_Coordinator, gc::CacheRefresh | gc::ProtoFlag, packet.data(), uint32_t(packet.size()));
        g_Refresh.Sent(now);
        { std::lock_guard<std::mutex> lock(g_StateMutex); g_InventoryStatus.refreshes = g_Refresh.attempts; }
        Log("Inventory refresh %u/%u: Steam result %d", g_Refresh.attempts, gc::RefreshWindow::MaxAttempts, int(result));
        if (result != gc::OK) {
            g_Waiting = false;
            SetPhase(InventoryPhase::Failed, result == gc::NotLoggedOn
                ? "Steam is not logged on; reconnect and retry refresh"
                : "Steam rejected the inventory refresh (result " + std::to_string(int(result)) + ")");
        }
    }
}

void TickInventory() {
    try { TickInventoryImpl(); }
    catch (const std::exception& error) {
        g_Waiting = false; g_Transform = false;
        SetPhase(InventoryPhase::Failed, std::string("Inventory initialization failed: ") + error.what());
    }
    {
        std::unique_lock<std::mutex> receive(g_ReceiveMutex, std::try_to_lock);
        if (receive.owns_lock()) {
            std::lock_guard<std::mutex> lock(g_StateMutex);
            g_InventoryStatus.pendingReplies = g_LocalReplies.size();
            g_InventoryStatus.oldestReplyMs = g_LocalReplies.empty() ? 0 : GetTickCount64() - g_LocalReplies.front().queuedAt;
            if (g_InventoryStatus.oldestReplyMs > 2000)
                g_InventoryStatus.equipDetail = g_InventoryStatus.callbackPolls == 0
                    ? "Steam notification dispatcher not observed; restart Dota with receiver v6"
                    : "Dota has not drained the equip queue; check notification/pending counters";
        }
    }
    FlushLog(); // Disk I/O stays off the game's send/receive/callback threads.
}

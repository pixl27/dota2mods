// src/browser.cpp — full hero/skin browser + one-click equip + live push link
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include "overlay.h"
#include "stealth.h"
#pragma comment(lib, "ws2_32.lib")

static char g_HeroSearch[64] = {};
static char g_SkinSearch[64] = {};
static std::string g_Page;
static std::string g_Flash;
static DWORD g_FlashUntil = 0;

static ImVec4 RarityColor(const std::string& r) {
    if (r.find("immortal") != std::string::npos) return ImVec4(1.0f, 0.78f, 0.2f, 1);
    if (r.find("arcana") != std::string::npos)   return ImVec4(0.45f, 1.0f, 0.45f, 1);
    if (r.find("mythical") != std::string::npos) return ImVec4(0.7f, 0.4f, 1.0f, 1);
    if (r.find("legendary") != std::string::npos) return ImVec4(1.0f, 0.4f, 0.7f, 1);
    if (r.find("rare") != std::string::npos)     return ImVec4(0.4f, 0.7f, 1.0f, 1);
    return ImVec4(0.75f, 0.75f, 0.75f, 1);
}

static std::string PrettyHero(const std::string& key) {
    const char* pre = "npc_dota_hero_";
    std::string h = key;
    if (!h.compare(0, strlen(pre), pre)) h = h.substr(strlen(pre));
    if (!h.empty()) h[0] = (char)toupper(h[0]);
    return h;
}

// push current loadout to the in-game DLL over localhost:3999. Silent if DLL not mapped.
static void PushLive(const std::string& hero) {
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(3999);
    if (connect(s, (sockaddr*)&a, sizeof(a)) != 0) { closesocket(s); return; }
    std::string msg = "HERO:" + hero + "\n";
    { std::lock_guard<std::mutex> lk(g_LoadoutMutex);
      auto it = g_Loadout.find(hero);
      if (it != g_Loadout.end())
          for (auto& [slot, def] : it->second)
              msg += slot + "=" + std::to_string(def) + "\n"; }
    send(s, msg.c_str(), (int)msg.size(), 0);
    closesocket(s);
}

static void Equip(const std::string& hero, const std::string& slot, int def) {
    { std::lock_guard<std::mutex> lk(g_LoadoutMutex); g_Loadout[hero][slot] = def; }
    SaveLoadout("loadout.json");
    PushLive(hero);
    if (g_Cfg.stealthWriter) stealth::PushLoadout(hero);
    g_Flash = "equipped def " + std::to_string(def) + " -> " + hero + " [" + slot + "]";
    g_FlashUntil = GetTickCount() + 2500;
}

void DrawWardrobe() {
    ImGui::SetNextWindowSize(ImVec2(760, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("wardrobe — every hero, every skin", &g_MenuOpen);

    if (!g_Live.hero.empty() && g_Page.empty()) g_Page = g_Live.hero;
    if (!g_Live.hero.empty()) ImGui::Text("playing now: %s", PrettyHero(g_Live.hero).c_str());
    if (!g_Flash.empty() && GetTickCount() < g_FlashUntil)
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "%s", g_Flash.c_str());

    ImGui::Columns(2, "browser", true);

    ImGui::InputText("hero?", g_HeroSearch, sizeof(g_HeroSearch));
    ImGui::BeginChild("heroes", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_DbMutex);
        std::string last;
        for (auto& s : g_DB) {
            if (s.hero == "_global" || s.hero == last) continue;
            last = s.hero;
            std::string pretty = PrettyHero(s.hero), pl = pretty, q = g_HeroSearch;
            std::transform(pl.begin(), pl.end(), pl.begin(), ::tolower);
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            if (q[0] && pl.find(q) == std::string::npos) continue;
            if (ImGui::Selectable(pretty.c_str(), g_Page == s.hero)) g_Page = s.hero;
        }
    }
    ImGui::EndChild();
    ImGui::NextColumn();

    ImGui::InputText("skin?", g_SkinSearch, sizeof(g_SkinSearch));
    ImGui::BeginChild("skins", ImVec2(0, 0), true);
    if (!g_Page.empty()) {
        ImGui::Text("%s", PrettyHero(g_Page).c_str());
        std::vector<std::string> slots;
        {
            std::lock_guard<std::mutex> lk(g_DbMutex);
            for (auto& s : g_DB)
                if (s.hero == g_Page && std::find(slots.begin(), slots.end(), s.slot) == slots.end())
                    slots.push_back(s.slot);
        }
        for (auto& slot : slots) {
            int curDef = -1;
            {
                std::lock_guard<std::mutex> lk(g_LoadoutMutex);
                auto hi = g_Loadout.find(g_Page);
                if (hi != g_Loadout.end()) {
                    auto si = hi->second.find(slot);
                    if (si != hi->second.end()) curDef = si->second;
                }
            }
            std::string curName = "default";
            {
                std::lock_guard<std::mutex> db(g_DbMutex);
                for (auto& e : g_DB) if (e.defIndex == curDef) { curName = e.name; break; }
            }
            if (ImGui::CollapsingHeader((slot + "  —  " + curName).c_str())) {
                if (ImGui::SmallButton((std::string("default##") + slot).c_str()))
                    Equip(g_Page, slot, -1);
                std::lock_guard<std::mutex> db(g_DbMutex);
                for (auto& e : g_DB) {
                    if (e.hero != g_Page || e.slot != slot) continue;
                    std::string q = g_SkinSearch, nl = e.name;
                    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                    std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                    if (q[0] && nl.find(q) == std::string::npos) continue;
                    bool isCur = (e.defIndex == curDef);
                    ImGui::PushStyleColor(ImGuiCol_Text, RarityColor(e.rarity));
                    std::string label = e.name + "  [" + e.rarity + "]" + (isCur ? "  <equipped>" : "");
                    if (ImGui::Selectable(label.c_str(), isCur)) Equip(g_Page, slot, e.defIndex);
                    ImGui::PopStyleColor();
                }
            }
        }
        ImGui::Separator();
        ImGui::Text("full sets:");
        {
            std::lock_guard<std::mutex> db(g_DbMutex);
            std::map<std::string, std::vector<const SkinEntry*>> sets;
            for (auto& e : g_DB) {
                if (e.hero != g_Page) continue;
                std::string set = e.name;
                auto p = set.find(" of ");
                if (p == std::string::npos) p = set.find(" - ");
                if (p == std::string::npos) continue;
                sets[set.substr(0, p)].push_back(&e);
            }
            for (auto& [sn, items] : sets) {
                if (items.size() < 2) continue;
                if (ImGui::SmallButton(("wear: " + sn + " (" + std::to_string(items.size()) + " pcs)").c_str())) {
                    for (auto* it : items) Equip(g_Page, it->slot, it->defIndex);
                    g_Flash = "full set equipped: " + sn;
                    g_FlashUntil = GetTickCount() + 2500;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("wear everything rare+ (this hero)")) {
            std::lock_guard<std::mutex> db(g_DbMutex);
            auto rank = [](const std::string& r) {
                if (r.find("immortal") != std::string::npos) return 5;
                if (r.find("arcana") != std::string::npos) return 4;
                if (r.find("legendary") != std::string::npos) return 3;
                if (r.find("mythical") != std::string::npos) return 2;
                if (r.find("rare") != std::string::npos) return 1;
                return 0;
            };
            std::map<std::string, const SkinEntry*> best;
            for (auto& e : g_DB) {
                if (e.hero != g_Page) continue;
                auto it = best.find(e.slot);
                if (it == best.end() || rank(e.rarity) > rank(it->second->rarity))
                    best[e.slot] = &e;
            }
            for (auto& [slot, ent] : best)
                Equip(g_Page, slot, ent->defIndex);
        }
    } else ImGui::TextDisabled("pick a hero on the left, baby.");
    ImGui::EndChild();
    ImGui::Columns(1);

    ImGui::Separator();
    ImGui::Checkbox("stealth writer live-push", &g_Cfg.stealthWriter);
    ImGui::SameLine();
    if (ImGui::Button("save all")) SaveLoadout("loadout.json");
    ImGui::TextDisabled("INSERT toggles • gold=immortal green=arcana");
    ImGui::End();
}

// src/browser.cpp — full hero/skin browser + one-click equip + live push link
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include "overlay.h"
#include "gamestate.h"
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
    static const std::map<std::string, std::string> kAliases = {
        {"npc_dota_hero_nevermore", "Shadow Fiend"},
        {"npc_dota_hero_zuus", "Zeus"},
        {"npc_dota_hero_windrunner", "Windranger"},
        {"npc_dota_hero_shredder", "Timbersaw"},
        {"npc_dota_hero_rattletrap", "Clockwerk"},
        {"npc_dota_hero_obsidian_destroyer", "Outworld Destroyer"},
        {"npc_dota_hero_furion", "Nature's Prophet"},
        {"npc_dota_hero_life_stealer", "Lifestealer"},
        {"npc_dota_hero_necrolyte", "Necrophos"},
        {"npc_dota_hero_doom_bringer", "Doom"},
        {"npc_dota_hero_treant", "Treant Protector"},
        {"npc_dota_hero_queenofpain", "Queen of Pain"},
        {"npc_dota_hero_skeleton_king", "Wraith King"},
        {"npc_dota_hero_wisp", "Io"},
        {"npc_dota_hero_centaur", "Centaur Warrunner"},
        {"npc_dota_hero_magnataur", "Magnus"},
        {"npc_dota_hero_abyssal_underlord", "Underlord"},
        {"npc_dota_hero_sand_king", "Sand King"},
        {"npc_dota_hero_shadow_shaman", "Shadow Shaman"},
        {"npc_dota_hero_storm_spirit", "Storm Spirit"},
        {"npc_dota_hero_witch_doctor", "Witch Doctor"},
        {"npc_dota_hero_vengefulspirit", "Vengeful Spirit"},
        {"npc_dota_hero_antimage", "Anti-Mage"},
    };
    auto it = kAliases.find(key);
    if (it != kAliases.end()) return it->second;

    const char* pre = "npc_dota_hero_";
    std::string h = key;
    if (!h.compare(0, strlen(pre), pre)) h = h.substr(strlen(pre));

    std::string res;
    bool capNext = true;
    for (char c : h) {
        if (c == '_') {
            res += ' ';
            capNext = true;
        } else {
            res += capNext ? (char)toupper(c) : c;
            capNext = false;
        }
    }
    return res.empty() ? key : res;
}

static void Equip(const std::string& hero, const std::string& slot, int def) {
    {
        std::lock_guard<std::mutex> lk(g_LoadoutMutex);
        g_Loadout[hero][slot] = def;
    }
    SaveLoadout("loadout.json");
    g_Flash = "equipped def " + std::to_string(def) + " -> " + hero + " [" + slot + "]";
    g_FlashUntil = GetTickCount() + 2500;
}

// Cached active hero data for performance
static std::string s_CachedHero;
static std::vector<std::string> s_CachedSlots;
static std::map<std::string, std::vector<const SkinEntry*>> s_CachedSlotItems;
static std::map<std::string, std::vector<const SkinEntry*>> s_CachedSets;

static void RefreshHeroCache(const std::string& hero) {
    s_CachedHero = hero;
    s_CachedSlots.clear();
    s_CachedSlotItems.clear();
    s_CachedSets.clear();
    if (hero.empty()) return;

    std::lock_guard<std::mutex> lk(g_DbMutex);
    for (const auto& s : g_DB) {
        if (s.hero != hero) continue;
        if (std::find(s_CachedSlots.begin(), s_CachedSlots.end(), s.slot) == s_CachedSlots.end()) {
            s_CachedSlots.push_back(s.slot);
        }
        s_CachedSlotItems[s.slot].push_back(&s);

        if (!s.bundle.empty()) {
            s_CachedSets[s.bundle].push_back(&s);
        } else {
            auto p = s.name.find(" of ");
            if (p != std::string::npos) {
                s_CachedSets[s.name.substr(p + 4)].push_back(&s);
            } else {
                p = s.name.find(" - ");
                if (p != std::string::npos) {
                    s_CachedSets[s.name.substr(p + 3)].push_back(&s);
                }
            }
        }
    }
}

void DrawWardrobe() {
    ImGui::SetNextWindowSize(ImVec2(760, 700), ImGuiCond_FirstUseEver);
    ImGui::Begin("wardrobe — every hero, every skin", &g_MenuOpen);

    std::string liveHero = g_Live.getHero();
    if (!liveHero.empty() && g_Page.empty()) {
        g_Page = liveHero;
    }
    if (!liveHero.empty()) {
        ImGui::Text("playing now: %s", PrettyHero(liveHero).c_str());
    }
    if (!g_Flash.empty() && GetTickCount() < g_FlashUntil) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", g_Flash.c_str());
    }

    ImGui::Columns(2, "browser", true);

    ImGui::InputText("hero?", g_HeroSearch, sizeof(g_HeroSearch));
    ImGui::BeginChild("heroes", ImVec2(0, 0), true);
    {
        std::lock_guard<std::mutex> lk(g_DbMutex);
        for (const auto& h : g_UniqueHeroes) {
            std::string pretty = PrettyHero(h);
            std::string pl = pretty, q = g_HeroSearch;
            std::transform(pl.begin(), pl.end(), pl.begin(), ::tolower);
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            if (q[0] && pl.find(q) == std::string::npos) continue;
            if (ImGui::Selectable(pretty.c_str(), g_Page == h)) {
                g_Page = h;
            }
        }
    }
    ImGui::EndChild();
    ImGui::NextColumn();

    if (g_Page != s_CachedHero) {
        RefreshHeroCache(g_Page);
    }

    ImGui::InputText("skin?", g_SkinSearch, sizeof(g_SkinSearch));
    ImGui::BeginChild("skins", ImVec2(0, 0), true);
    if (!g_Page.empty()) {
        ImGui::Text("%s", PrettyHero(g_Page).c_str());

        for (const auto& slot : s_CachedSlots) {
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
            auto itItems = s_CachedSlotItems.find(slot);
            if (itItems != s_CachedSlotItems.end()) {
                for (const auto* e : itItems->second) {
                    if (e->defIndex == curDef) {
                        curName = e->name;
                        break;
                    }
                }
            }

            if (ImGui::CollapsingHeader((slot + "  —  " + curName).c_str())) {
                if (ImGui::SmallButton((std::string("default##") + slot).c_str())) {
                    Equip(g_Page, slot, -1);
                }
                if (itItems != s_CachedSlotItems.end()) {
                    for (const auto* e : itItems->second) {
                        std::string q = g_SkinSearch, nl = e->name;
                        std::transform(q.begin(), q.end(), q.begin(), ::tolower);
                        std::transform(nl.begin(), nl.end(), nl.begin(), ::tolower);
                        if (q[0] && nl.find(q) == std::string::npos) continue;

                        bool isCur = (e->defIndex == curDef);
                        ImGui::PushStyleColor(ImGuiCol_Text, RarityColor(e->rarity));
                        std::string label = e->name + "  [" + e->rarity + "]" + (isCur ? "  <equipped>" : "");
                        if (ImGui::Selectable(label.c_str(), isCur)) {
                            Equip(g_Page, slot, e->defIndex);
                        }
                        ImGui::PopStyleColor();
                    }
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("full sets:");
        for (const auto& [sn, items] : s_CachedSets) {
            if (items.size() < 2) continue;
            std::string btn = "wear: " + sn + " (" + std::to_string(items.size()) + " pcs)";
            if (ImGui::SmallButton(btn.c_str())) {
                for (const auto* it : items) {
                    Equip(g_Page, it->slot, it->defIndex);
                }
                g_Flash = "full set equipped: " + sn;
                g_FlashUntil = GetTickCount() + 2500;
            }
        }

        ImGui::Separator();
        if (ImGui::Button("wear everything rare+ (this hero)")) {
            auto rank = [](const std::string& r) {
                if (r.find("immortal") != std::string::npos) return 5;
                if (r.find("arcana") != std::string::npos) return 4;
                if (r.find("legendary") != std::string::npos) return 3;
                if (r.find("mythical") != std::string::npos) return 2;
                if (r.find("rare") != std::string::npos) return 1;
                return 0;
            };
            std::map<std::string, const SkinEntry*> best;
            for (const auto& [slot, items] : s_CachedSlotItems) {
                for (const auto* e : items) {
                    auto it = best.find(e->slot);
                    if (it == best.end() || rank(e->rarity) > rank(it->second->rarity)) {
                        best[e->slot] = e;
                    }
                }
            }
            for (const auto& [slot, ent] : best) {
                Equip(g_Page, slot, ent->defIndex);
            }
        }
    } else {
        ImGui::TextDisabled("pick a hero on the left.");
    }
    ImGui::EndChild();
    ImGui::Columns(1);

    ImGui::Separator();
    if (ImGui::Button("save all")) SaveLoadout("loadout.json");
    ImGui::SameLine();
    if (ImGui::Button("close overlay")) g_MenuOpen = false;
    ImGui::SameLine();
    if (ImGui::Button("exit app")) PostQuitMessage(0);

    ImGui::TextDisabled("END toggles • gold=immortal green=arcana");
    ImGui::End();
}

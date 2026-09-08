// src/db.h
#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include "json.hpp"
using json = nlohmann::json;

struct SkinEntry {
    int defIndex = 0;
    std::string name, hero, slot, rarity, prefab;
};

struct Config {
    bool stealthWriter = false;
    bool streamproof = true;
    int  gsiPort = 3000;
    std::string gsiToken = "wardrobe-local-token";
};

extern Config g_Cfg;
extern std::vector<SkinEntry> g_DB;
extern std::mutex g_DbMutex;
extern std::map<std::string, std::map<std::string, int>> g_Loadout;
extern std::mutex g_LoadoutMutex;

void LoadDB(const std::string& path);
void SaveLoadout(const std::string& path);
void LoadLoadout(const std::string& path);

// wardrobe_tools.exe — the maintenance jobs that must work on a machine without
// Python, which is most machines Wardrobe is handed to.
//
//   wardrobe_tools compat [client.dll]
//       Runs the DLL's own resolver (same header, same profile, built together)
//       over the installed client.dll. Exit 0: this Wardrobe works with this
//       Dota. Exit 1: it does not, and the line says what is missing. Exit 2:
//       nothing to check (Dota not found, file unreadable).
//
//   wardrobe_tools catalog [game/dota] [skins_full.json]
//       Counts the cosmetics of the installed Dota (read from pak01, same rules
//       as gen_full_db.py) missing from the catalog. Exit 0: up to date; 1: some
//       are missing (a content update came out); 2: nothing to compare.
//
//   wardrobe_tools repair <game/dota> [--catalog FILE] [--backup-directory DIR]
//                         [--repair | --list-backups | --restore latest|STAMP]
//       Port of repair_inventory_cache.py, with the same rules and the same
//       backup layout, so either can restore what the other saved. Read-only
//       unless --repair or --restore; both refuse while Dota runs.
//
// The first output line of compat is machine-readable ("compat resolved=..."),
// the rest is for the person reading the journal.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>
#include "catalog_builder.h"
#include "native_appearance_resolver.h"
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

namespace {

// ------------------------------------------------------------------ shared
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
std::string Utf8(const fs::path& path) { return Utf8(path.wstring()); }

bool ReadAll(const fs::path& path, std::vector<uint8_t>& out, uint64_t limit) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > limit) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    out.resize(size_t(size));
    return size == 0 || bool(stream.read(reinterpret_cast<char*>(out.data()), std::streamsize(size)));
}

std::string Sha256(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32]{};
    bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
    if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    if (ok && !data.empty()) ok = BCryptHashData(hash, const_cast<PUCHAR>(data.data()), ULONG(data.size()), 0) >= 0;
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) throw std::runtime_error("SHA-256 indisponible");
    char hex[65]{};
    for (int i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}
std::string FileSha256(const fs::path& path) {
    std::vector<uint8_t> data;
    if (!ReadAll(path, data, 1ull << 32)) throw std::runtime_error("lecture impossible : " + Utf8(path.filename()));
    return Sha256(data);
}

fs::path ExecutableDirectory() {
    wchar_t module[MAX_PATH]{};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    return fs::path(module).parent_path();
}

// Steam records its library roots; Dota can live in any of them.
fs::path DotaGame() {
    std::vector<fs::path> roots;
    const struct { HKEY hive; const wchar_t* key; const wchar_t* value; } places[] = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"},
    };
    for (const auto& place : places) {
        HKEY key{};
        if (RegOpenKeyExW(place.hive, place.key, 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) continue;
        wchar_t value[MAX_PATH]{};
        DWORD size = sizeof(value);
        if (RegQueryValueExW(key, place.value, nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS)
            roots.emplace_back(value);
        RegCloseKey(key);
    }
    for (size_t i = 0, count = roots.size(); i < count; ++i) {
        std::ifstream file(roots[i] / "steamapps" / "libraryfolders.vdf");
        std::string line;
        while (std::getline(file, line)) {
            const auto key = line.find("\"path\"");
            if (key == std::string::npos) continue;
            const auto open = line.find('"', key + 6), close = line.rfind('"');
            if (open == std::string::npos || close <= open) continue;
            std::string found = line.substr(open + 1, close - open - 1);
            for (size_t at = 0; (at = found.find("\\\\", at)) != std::string::npos; ++at) found.erase(at, 1);
            const int size = MultiByteToWideChar(CP_UTF8, 0, found.c_str(), -1, nullptr, 0);
            std::wstring wide(size_t(size > 0 ? size - 1 : 0), L'\0');
            if (size > 1) MultiByteToWideChar(CP_UTF8, 0, found.c_str(), -1, wide.data(), size);
            roots.emplace_back(wide);
        }
    }
    std::error_code ignored;
    for (const auto& root : roots) {
        const auto game = root / "steamapps" / "common" / "dota 2 beta" / "game" / "dota";
        if (fs::exists(game / "bin" / "win64" / "client.dll", ignored)) return game;
    }
    return {};
}

// ------------------------------------------------------------------ compat
// Maps the file the way the loader would place it, so the resolver reads
// sections at their virtual addresses exactly as it does inside the game.
uint8_t* MapImage(const fs::path& path, size_t& mapped) {
    std::vector<uint8_t> raw;
    if (!ReadAll(path, raw, 1ull << 30) || raw.size() < sizeof(IMAGE_DOS_HEADER)) return nullptr;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > raw.size()) return nullptr;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
    const size_t image = nt->OptionalHeader.SizeOfImage;
    auto base = static_cast<uint8_t*>(VirtualAlloc(nullptr, image, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base) return nullptr;
    memcpy(base, raw.data(), std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, std::min(image, raw.size())));
    const auto section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const size_t at = section[i].VirtualAddress, from = section[i].PointerToRawData;
        size_t size = section[i].SizeOfRawData;
        if (from >= raw.size() || at >= image) continue;
        size = std::min({size, raw.size() - from, image - at});
        memcpy(base + at, raw.data() + from, size);
    }
    mapped = image;
    return base;
}

appearance::profile::Resolution Resolve(uintptr_t base) {
    __try { return appearance::profile::Resolver().Run(base); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        appearance::profile::Resolution failed;
        strcpy_s(failed.failure, "image illisible pendant la résolution");
        return failed;
    }
}

int Compat(int argc, wchar_t** argv) {
    fs::path client = argc > 2 ? fs::path(argv[2]) : DotaGame() / "bin" / "win64" / "client.dll";
    std::error_code ignored;
    if (client.empty() || !fs::exists(client, ignored)) {
        printf("compat resolved=? exact=? moved=? failure=client.dll introuvable\n");
        printf("Installation de Dota 2 introuvable : rien à vérifier.\n");
        return 2;
    }
    size_t size = 0;
    uint8_t* image = MapImage(client, size);
    if (!image) {
        printf("compat resolved=? exact=? moved=? failure=client.dll illisible\n");
        printf("Impossible de lire %s.\n", Utf8(client).c_str());
        return 2;
    }
    const auto begin = GetTickCount64();
    const auto result = Resolve(uintptr_t(image));
    VirtualFree(image, 0, MEM_RELEASE);
    printf("compat resolved=%d exact=%d moved=%u failure=%s\n", result.resolved, result.exact, result.moved, result.failure);
    printf("[*] %s\n", Utf8(client).c_str());
    if (result.resolved) {
        printf("[OK] Cette version de Wardrobe fonctionne avec le Dota 2 installé (%s, %llu ms).\n",
               result.exact ? "version vérifiée telle quelle"
                            : "le code a bougé, Wardrobe le retrouve seul au chargement",
               static_cast<unsigned long long>(GetTickCount64() - begin));
        return 0;
    }
    printf("[!] Cette version de Wardrobe ne fonctionne pas avec le Dota 2 installé : %s.\n", result.failure);
    printf("    Dota 2 a changé plus qu'une simple mise à jour habituelle. Il faut une version de Wardrobe à jour.\n");
    return 1;
}

// ------------------------------------------------------------------ catalog
// Reads one file out of pak01 (VPK v2: a directory tree, data in numbered
// archives or after the tree). Same format dota_vpk.py reads.
bool ReadFromVpk(const fs::path& game, const std::string& wanted, std::vector<uint8_t>& out) {
    std::vector<uint8_t> dir;
    if (!ReadAll(game / "pak01_dir.vpk", dir, 1ull << 30) || dir.size() < 28) return false;
    uint32_t signature = 0, version = 0, treeSize = 0;
    memcpy(&signature, dir.data(), 4);
    memcpy(&version, dir.data() + 4, 4);
    memcpy(&treeSize, dir.data() + 8, 4);
    if (signature != 0x55AA1234) return false;
    const size_t header = version == 2 ? 28 : 12;
    size_t at = header;
    auto text = [&]() -> std::string {
        const size_t start = at;
        while (at < dir.size() && dir[at]) ++at;
        std::string value(reinterpret_cast<const char*>(dir.data() + start), at - start);
        ++at;
        return value;
    };
    const auto dot = wanted.rfind('.'), slash = wanted.rfind('/');
    const std::string wantedExt = wanted.substr(dot + 1), wantedPath = wanted.substr(0, slash), wantedName = wanted.substr(slash + 1, dot - slash - 1);
    while (at < dir.size()) {
        const std::string extension = text();
        if (extension.empty()) break;
        while (at < dir.size()) {
            const std::string path = text();
            if (path.empty()) break;
            while (at < dir.size()) {
                const std::string name = text();
                if (name.empty()) break;
                if (at + 18 > dir.size()) return false;
                uint16_t preload = 0, archive = 0;
                uint32_t offset = 0, length = 0;
                memcpy(&preload, dir.data() + at + 4, 2);
                memcpy(&archive, dir.data() + at + 6, 2);
                memcpy(&offset, dir.data() + at + 8, 4);
                memcpy(&length, dir.data() + at + 12, 4);
                at += 18;
                const size_t preloadAt = at;
                at += preload;
                if (extension != wantedExt || path != wantedPath || name != wantedName) continue;
                out.assign(dir.begin() + preloadAt, dir.begin() + preloadAt + preload);
                if (archive == 0x7FFF) {
                    const size_t base = header + treeSize + offset;
                    if (base + length > dir.size()) return false;
                    out.insert(out.end(), dir.begin() + base, dir.begin() + base + length);
                    return true;
                }
                char numbered[32];
                sprintf_s(numbered, "pak01_%03u.vpk", archive);
                std::ifstream file(game / numbered, std::ios::binary);
                if (!file) return false;
                file.seekg(offset);
                const size_t before = out.size();
                out.resize(before + length);
                return bool(file.read(reinterpret_cast<char*>(out.data() + before), length));
            }
        }
    }
    return false;
}

std::set<uint64_t> Definitions(const fs::path& catalog);

// The rules of gen_full_db.py, so "missing" means exactly what a regeneration
// would add: a hero cosmetic or a client-side global one, not a bundle,
// courier, recipe or anything else the catalog leaves out on purpose.
int CatalogCheck(int argc, wchar_t** argv) {
    const fs::path game = argc > 2 && argv[2][0] != L'-' ? fs::path(argv[2]) : DotaGame();
    fs::path catalog = argc > 3 ? fs::path(argv[3]) : fs::path{};
    std::error_code ignored;
    if (catalog.empty())
        for (const auto& candidate : {ExecutableDirectory() / "data" / "skins_full.json", ExecutableDirectory().parent_path() / "data" / "skins_full.json"})
            if (fs::exists(candidate, ignored)) { catalog = candidate; break; }
    std::vector<uint8_t> raw;
    if (game.empty() || !ReadFromVpk(game, "scripts/items/items_game.txt", raw)) {
        printf("catalog missing=? total=?\nImpossible de lire les objets du jeu installé.\n");
        return 2;
    }
    std::set<uint64_t> known;
    try { known = Definitions(catalog); } catch (const std::exception& error) {
        printf("catalog missing=? total=?\n%s\n", error.what());
        return 2;
    }
    std::string text;
    text.reserve(raw.size());
    for (uint8_t c : raw) if (c != '\r') text.push_back(char(c));
    static const std::set<std::string> excluded = {"bundle", "treasure_chest", "retired_treasure_chest", "key", "socket_gem", "league", "tool",
                                                    "sticker", "sticker_capsule", "dynamic_recipe", "player_card", "emoticon_tool", "showcase_decoration"};
    static const std::set<std::string> globals = {"loading_screen", "hud_skin", "music", "terrain", "emblem", "cursor_pack", "announcer", "versus_screen"};
    const size_t items = text.find("\n\t\"items\"\n\t{");
    if (items == std::string::npos) { printf("catalog missing=? total=?\nFormat des objets du jeu inconnu.\n"); return 2; }
    const size_t itemsEnd = text.find("\n\t}", items + 10);
    size_t at = items, expected = 0;
    std::vector<std::string> missing, defaults;
    auto field =[&](const std::string& block, const char* key) -> std::string {
        const std::string needle = std::string("\n\t\t\t\"") + key + "\"";
        const size_t p = block.find(needle);
        if (p == std::string::npos) return {};
        const size_t open = block.find('"', p + needle.size());
        const size_t close = block.find('"', open + 1);
        return open == std::string::npos || close == std::string::npos ? std::string() : block.substr(open + 1, close - open - 1);
    };
    while ((at = text.find("\n\t\t\"", at)) != std::string::npos && at < itemsEnd) {
        const size_t close = text.find('"', at + 4);
        const std::string key = text.substr(at + 4, close - at - 4);
        const size_t open = text.find("\n\t\t{", close);
        const size_t end = text.find("\n\t\t}", open);
        if (close == std::string::npos || open != close + 1 || end == std::string::npos) { at += 4; continue; }
        const std::string block = text.substr(open, end - open);
        at = end + 4;
        if (key.empty() || key.find_first_not_of("0123456789") != std::string::npos) continue;
        std::string prefab = field(block, "prefab");
        prefab = prefab.substr(0, prefab.find(' '));
        if (excluded.count(prefab)) continue;
        bool hero = false;
        if (const size_t used = block.find("\n\t\t\t\"used_by_heroes\""); used != std::string::npos) {
            const size_t stop = block.find("\n\t\t\t}", used);
            const std::string list = block.substr(used, stop == std::string::npos ? std::string::npos : stop - used);
            for (size_t h = 0; (h = list.find("\"npc_dota_hero_", h)) != std::string::npos; ++h) {
                const size_t valueAt = list.find('"', list.find('"', h + 1) + 1);
                if (valueAt != std::string::npos && list.compare(valueAt, 3, "\"1\"") == 0) { hero = true; break; }
            }
        }
        if (!hero && !globals.count(prefab)) continue;
        ++expected;
        // Real cosmetics first in the examples; a hero's default pieces are the least telling.
        if (!known.count(std::stoull(key))) (prefab == "default_item" ? defaults : missing).push_back(field(block, "name"));
    }
    missing.insert(missing.end(), defaults.begin(), defaults.end());
    printf("catalog missing=%zu total=%zu\n", missing.size(), expected);
    if (missing.empty()) {
        printf("[OK] Le catalogue contient tous les cosmétiques du Dota 2 installé (%zu).\n", expected);
        return 0;
    }
    printf("[!] %zu cosmétique(s) du jeu installé manquent au catalogue, par exemple :\n", missing.size());
    for (size_t i = 0; i < missing.size() && i < 8; ++i) printf("    %s\n", missing[i].c_str());
    return 1;
}

// Regenerates data/skins_full.json from the installed game, as update_db.py
// does with Python. Every output is written beside itself and swapped in; the
// previous catalog stays as skins_full.json.previous.
int CatalogBuild(int argc, wchar_t** argv) {
    namespace cb = wardrobe::catalog_builder;
    fs::path game = DotaGame();
    std::vector<fs::path> outputs;
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--game" && i + 1 < argc) game = argv[++i];
        else outputs.emplace_back(arg);
    }
    if (outputs.empty()) outputs.push_back(ExecutableDirectory() / "data" / "skins_full.json");
    if (game.empty()) { printf("[!] Installation de Dota 2 introuvable.\n"); return 2; }
    printf("[*] Lecture des objets dans %s\n", Utf8(game).c_str());
    std::vector<uint8_t> items, english, dota;
    if (!ReadFromVpk(game, "scripts/items/items_game.txt", items)) { printf("[!] scripts/items/items_game.txt illisible.\n"); return 2; }
    ReadFromVpk(game, "resource/localization/items_english.txt", english);
    ReadFromVpk(game, "resource/localization/dota_english.txt", dota);
    const std::string text(items.begin(), items.end());
    const auto root = cb::Parser(text).Parse();
    auto catalog = cb::Build(*root);
    std::unordered_map<std::string, std::string> tokens;
    cb::LoadTokens(std::string(english.begin(), english.end()), tokens);
    cb::LoadTokens(std::string(dota.begin(), dota.end()), tokens);
    const auto names = cb::ApplyNames(catalog, tokens);
    // A parse that went wrong must never replace a catalog that works.
    if (catalog.skins.size() < 1000 || catalog.heroes.size() < 100) {
        printf("[!] Résultat incohérent (%zu objets, %zu héros) : catalogue actuel conservé.\n", catalog.skins.size(), catalog.heroes.size());
        return 1;
    }
    const std::string json = cb::ToJson(catalog);
    for (const auto& output : outputs) {
        std::error_code error;
        fs::create_directories(output.parent_path(), error);
        fs::path fresh = output;
        fresh += L".new";
        {
            std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
            out.write(json.data(), std::streamsize(json.size()));
            if (!out) { printf("[!] Écriture impossible : %s\n", Utf8(fresh).c_str()); return 1; }
        }
        if (fs::exists(output, error)) {
            fs::path previous = output;
            previous += L".previous";
            fs::copy_file(output, previous, fs::copy_options::overwrite_existing, error);
        }
        if (!MoveFileExW(fresh.c_str(), output.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            printf("[!] Remplacement impossible : %s\n", Utf8(output).c_str());
            return 1;
        }
        printf("[OK] %s\n", Utf8(output).c_str());
    }
    printf("[OK] Catalogue : %zu objets pour %zu héros ; noms : %zu/%zu traduits.\n", catalog.skins.size(), catalog.heroes.size(),
           names.resolved, names.total);
    printf("     Redémarre Dota 2 et réactive Wardrobe pour voir les nouveaux objets.\n");
    return 0;
}

// ------------------------------------------------------------------ repair
// CMsgSerializedSOCache, read just far enough to count Wardrobe's items.
struct Field { uint32_t number, wire; uint64_t value; const uint8_t* data; size_t size; };

std::vector<Field> Fields(const uint8_t* data, size_t size) {
    size_t at = 0;
    auto varint = [&]() -> uint64_t {
        uint64_t value = 0;
        for (int shift = 0; shift < 70; shift += 7) {
            if (at >= size) throw std::invalid_argument("varint tronqué");
            const uint8_t byte = data[at++];
            if (shift == 63 && byte > 1) throw std::invalid_argument("varint trop long");
            value |= uint64_t(byte & 127) << shift;
            if (!(byte & 128)) return value;
        }
        throw std::invalid_argument("varint invalide");
    };
    std::vector<Field> result;
    while (at < size) {
        const uint64_t tag = varint();
        Field field{uint32_t(tag >> 3), uint32_t(tag & 7), 0, nullptr, 0};
        if (!field.number) throw std::invalid_argument("champ invalide");
        if (field.wire == 0) {
            field.value = varint();
        } else if (field.wire == 1 || field.wire == 2 || field.wire == 5) {
            const uint64_t length = field.wire == 2 ? varint() : field.wire == 1 ? 8 : 4;
            if (length > size - at) throw std::invalid_argument("champ tronqué");
            field.data = data + at;
            field.size = size_t(length);
            at += size_t(length);
            if (field.wire != 2)
                for (size_t i = 0; i < field.size; ++i) field.value |= uint64_t(field.data[i]) << (8 * i);
        } else {
            throw std::invalid_argument("type de champ non pris en charge");
        }
        result.push_back(field);
    }
    return result;
}
// The last scalar occurrence wins, as in protobuf.
uint64_t Value(const std::vector<Field>& fields, uint32_t number) {
    for (auto it = fields.rbegin(); it != fields.rend(); ++it) if (it->number == number && it->wire != 2) return it->value;
    return 0;
}

struct Report { std::string file, sha256, error; size_t bytes = 0, items = 0, wardrobeItems = 0; };

Report Inspect(const fs::path& path, const std::set<uint64_t>& definitions) {
    Report report;
    report.file = Utf8(path.filename());
    std::vector<uint8_t> data;
    if (!ReadAll(path, data, 64ull * 1024 * 1024)) throw std::invalid_argument("cache trop gros ou illisible");
    report.bytes = data.size();
    report.sha256 = Sha256(data);
    const auto top = Fields(data.data(), data.size());
    if (Value(top, 1) != 4) throw std::invalid_argument("version de cache non prise en charge");
    for (const auto& outer : top) {
        if (outer.number != 2 || outer.wire != 2) continue;
        const auto cache = Fields(outer.data, outer.size);
        const uint64_t account = Value(cache, 2) & 0xffffffffull;
        for (const auto& groupField : cache) {
            if (groupField.number != 4 || groupField.wire != 2) continue;
            const auto group = Fields(groupField.data, groupField.size);
            if (Value(group, 1) != 1) continue;
            for (const auto& itemField : group) {
                if (itemField.number != 2 || itemField.wire != 2) continue;
                const auto item = Fields(itemField.data, itemField.size);
                ++report.items;
                // Wardrobe's ID namespace, this account, and a definition it ships.
                const uint64_t id = Value(item, 1);
                if (id >> 62 == 1 && Value(item, 2) == account && definitions.count(Value(item, 4))) ++report.wardrobeItems;
            }
        }
    }
    return report;
}

bool DotaRunning() {
    char forced[4]{};   // tests only: stands in for a running or closed game
    if (GetEnvironmentVariableA("WARDROBE_TEST_DOTA_RUNNING", forced, sizeof(forced))) return forced[0] == '1';
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;   // unsure: refuse rather than guess
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry))
        found = !_wcsicmp(entry.szExeFile, L"dota2.exe");
    CloseHandle(snapshot);
    return found;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    char text[32]{};
    strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

fs::path DefaultBackups() {
    wchar_t root[MAX_PATH]{};
    const fs::path base = GetEnvironmentVariableW(L"LOCALAPPDATA", root, MAX_PATH) ? fs::path(root) : ExecutableDirectory();
    return base / "Wardrobe" / "recovery";
}

std::set<uint64_t> Definitions(const fs::path& catalog) {
    std::vector<uint8_t> data;
    if (!ReadAll(catalog, data, 1ull << 30)) throw std::runtime_error("catalogue introuvable : " + Utf8(catalog));
    std::set<uint64_t> result;
    const std::string text(data.begin(), data.end());
    for (size_t at = 0; (at = text.find("\"def\":", at)) != std::string::npos;) {
        at += 6;
        while (at < text.size() && text[at] == ' ') ++at;
        uint64_t value = 0;
        bool digits = false;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9') { value = value * 10 + uint64_t(text[at++] - '0'); digits = true; }
        if (digits) result.insert(value);
    }
    if (result.empty()) throw std::runtime_error("le catalogue ne contient aucun objet : " + Utf8(catalog));
    return result;
}

std::vector<fs::path> Backups(const fs::path& directory) {
    std::vector<fs::path> found;
    std::error_code ignored;
    if (!fs::is_directory(directory, ignored)) return found;
    for (const auto& entry : fs::directory_iterator(directory, ignored))
        if (entry.is_directory() && fs::exists(entry.path() / "manifest.json", ignored)) found.push_back(entry.path());
    std::sort(found.begin(), found.end());
    return found;
}

// The manifest is a JSON list of reports; only "file" and "sha256" matter here,
// and Python's json.dumps writes each report's "file" before its "sha256".
std::vector<std::pair<std::string, std::string>> ReadManifest(const fs::path& path) {
    std::vector<uint8_t> data;
    if (!ReadAll(path, data, 16ull << 20)) throw std::runtime_error("manifeste illisible");
    const std::string text(data.begin(), data.end());
    auto string = [&](const char* key, size_t from, size_t& end) -> std::string {
        const std::string needle = std::string("\"") + key + "\"";
        const size_t at = text.find(needle, from);
        if (at == std::string::npos) { end = std::string::npos; return {}; }
        const size_t open = text.find('"', text.find(':', at + needle.size()) + 1);
        const size_t close = text.find('"', open + 1);
        end = close == std::string::npos ? std::string::npos : close + 1;
        return open == std::string::npos || close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
    };
    std::vector<std::pair<std::string, std::string>> entries;
    for (size_t at = 0;;) {
        size_t end = 0;
        const auto file = string("file", at, end);
        if (end == std::string::npos) break;
        const auto sha = string("sha256", end, end);
        if (end == std::string::npos || file.empty() || sha.size() != 64 || file.find_first_of("/\\") != std::string::npos)
            throw std::runtime_error("manifeste invalide");
        entries.emplace_back(file, sha);
        at = end;
    }
    if (entries.empty()) throw std::runtime_error("manifeste vide");
    return entries;
}

void WriteManifest(const fs::path& path, const std::vector<Report>& reports) {
    std::ofstream out(path, std::ios::binary);
    out << "[\n";
    for (size_t i = 0; i < reports.size(); ++i) {
        const auto& r = reports[i];
        out << "  {\n    \"file\": \"" << r.file << "\",\n    \"bytes\": " << r.bytes << ",\n    \"sha256\": \"" << r.sha256
            << "\",\n    \"items\": " << r.items << ",\n    \"wardrobe_items\": " << r.wardrobeItems << "\n  }"
            << (i + 1 < reports.size() ? ",\n" : "\n");
    }
    out << "]\n";
    if (!out) throw std::runtime_error("écriture du manifeste impossible");
}

int Restore(const fs::path& game, const fs::path& directory, const std::wstring& which) {
    const auto backups = Backups(directory);
    if (backups.empty()) { printf("[!] Aucune sauvegarde dans %s.\n", Utf8(directory).c_str()); return 2; }
    const fs::path chosen = which == L"latest" ? backups.back() : directory / which;
    if (std::find(backups.begin(), backups.end(), chosen) == backups.end()) {
        printf("[!] Sauvegarde inconnue : %s (voir --list-backups).\n", Utf8(which).c_str());
        return 2;
    }
    if (DotaRunning()) { printf("[!] Ferme complètement Dota 2 avant de restaurer son cache d'inventaire.\n"); return 2; }
    const auto manifest = ReadManifest(chosen / "manifest.json");
    for (const auto& [file, sha] : manifest) {
        std::error_code ignored;
        if (!fs::exists(chosen / file, ignored) || FileSha256(chosen / file) != sha)
            throw std::runtime_error("sauvegarde " + file + " absente ou abîmée ; rien n'a été restauré");
    }
    const std::string stamp = Stamp();
    for (const auto& [file, sha] : manifest) {
        const fs::path source = chosen / file, target = game / file;
        std::error_code error;
        if (fs::exists(target, error)) {
            const fs::path aside = chosen / (file + ".replaced-" + stamp);
            fs::rename(target, aside);
            printf("Cache recréé par Steam mis de côté : %s -> %s\n", file.c_str(), Utf8(aside).c_str());
        }
        fs::copy_file(source, target, fs::copy_options::overwrite_existing);
        if (FileSha256(target) != sha) throw std::runtime_error("vérification échouée après la copie de " + file);
        printf("[OK] %s restauré depuis %s\n", file.c_str(), Utf8(chosen.filename()).c_str());
    }
    return 0;
}

int Repair(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: wardrobe_tools repair <game/dota> [--catalog FILE] [--backup-directory DIR] "
               "[--repair | --list-backups | --restore latest|STAMP]\n");
        return 2;
    }
    std::error_code error;
    const fs::path game = fs::canonical(argv[2], error);
    if (error || game.filename() != "dota" || game.parent_path().filename() != "game") {
        printf("[!] Il faut le dossier game\\dota de Dota 2 : %s\n", Utf8(std::wstring(argv[2])).c_str());
        return 2;
    }
    fs::path catalog, backups = DefaultBackups();
    std::wstring restore;
    bool repair = false, list = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--catalog" && i + 1 < argc) catalog = argv[++i];
        else if (option == L"--backup-directory" && i + 1 < argc) backups = argv[++i];
        else if (option == L"--restore" && i + 1 < argc) restore = argv[++i];
        else if (option == L"--repair") repair = true;
        else if (option == L"--list-backups") list = true;
        else { printf("[!] Option inconnue : %s\n", Utf8(option).c_str()); return 2; }
    }
    backups = fs::absolute(backups);
    if (list) {
        const auto found = Backups(backups);
        if (found.empty()) printf("Aucune sauvegarde dans %s.\n", Utf8(backups).c_str());
        for (const auto& backup : found) {
            std::string files;
            for (const auto& entry : fs::directory_iterator(backup, error))
                if (entry.path().extension() == ".soc") files += (files.empty() ? "" : ", ") + Utf8(entry.path().filename());
            printf("%s : %s\n", Utf8(backup.filename()).c_str(), files.empty() ? "(vide)" : files.c_str());
        }
        return 0;
    }
    if (!restore.empty()) return Restore(game, backups, restore);

    if (catalog.empty()) {
        for (const auto& candidate : {ExecutableDirectory() / "data" / "skins_full.json",
                                      ExecutableDirectory().parent_path() / "data" / "skins_full.json"})
            if (fs::exists(candidate, error)) { catalog = candidate; break; }
        if (catalog.empty()) { printf("[!] Catalogue data\\skins_full.json introuvable.\n"); return 2; }
    }
    const auto definitions = Definitions(catalog);
    std::vector<Report> affected;
    size_t files = 0;
    std::vector<fs::path> caches;
    for (const auto& entry : fs::directory_iterator(game, error)) {
        const auto name = entry.path().filename().wstring();
        if (name.rfind(L"cache_", 0) == 0 && name.size() > 10 && name.compare(name.size() - 6, 6, L"_1.soc") == 0)
            caches.push_back(entry.path());
    }
    std::sort(caches.begin(), caches.end());
    for (const auto& path : caches) {
        ++files;
        try {
            const auto report = Inspect(path, definitions);
            printf("%s : %zu objet(s) de Wardrobe sur %zu\n", report.file.c_str(), report.wardrobeItems, report.items);
            if (report.wardrobeItems) affected.push_back(report);
        } catch (const std::invalid_argument& why) {
            printf("%s : illisible (%s), laissé tel quel\n", Utf8(path.filename()).c_str(), why.what());
        }
    }
    if (!files) printf("Aucun cache d'inventaire dans %s.\n", Utf8(game).c_str());
    if (affected.empty()) { printf("[OK] Aucun objet d'aperçu de Wardrobe dans le cache de Steam : rien à faire.\n"); return 0; }
    if (!repair) { printf("[*] %zu cache(s) concerné(s). Relance avec --repair pour les mettre de côté.\n", affected.size()); return 0; }
    if (DotaRunning()) { printf("[!] Ferme complètement Dota 2 avant de réparer son cache d'inventaire.\n"); return 2; }

    const fs::path backup = backups / Stamp();
    fs::create_directories(backups);
    if (!CreateDirectoryW(backup.c_str(), nullptr)) throw std::runtime_error("dossier de sauvegarde déjà présent ou impossible à créer");
    WriteManifest(backup / "manifest.json", affected);
    for (const auto& report : affected) {
        const fs::path source = fs::canonical(game / report.file);
        if (source.parent_path() != game || DotaRunning()) throw std::runtime_error("le cache a bougé ou Dota a démarré ; réparation arrêtée");
        if (FileSha256(source) != report.sha256) throw std::runtime_error("le cache a changé depuis l'inspection ; réparation arrêtée");
        const fs::path destination = backup / report.file;
        fs::copy_file(source, destination);
        if (FileSha256(destination) != report.sha256) throw std::runtime_error("la copie de sauvegarde ne correspond pas ; original conservé");
        fs::remove(source);
        printf("[OK] Cache concerné mis de côté : %s -> %s\n", report.file.c_str(), Utf8(destination).c_str());
    }
    printf("[OK] Steam reconstruira ces caches à la prochaine connexion de Dota 2.\n");
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const std::wstring command = argc > 1 ? argv[1] : L"";
    try {
        if (command == L"compat") return Compat(argc, argv);
        if (command == L"repair") return Repair(argc, argv);
        if (command == L"catalog") return CatalogCheck(argc, argv);
        if (command == L"catalog-build") return CatalogBuild(argc, argv);
    } catch (const std::exception& error) {
        printf("[!] %s\n", error.what());
        return 1;
    }
    printf("usage: wardrobe_tools compat [client.dll]\n"
           "       wardrobe_tools catalog [game/dota] [skins_full.json]\n"
           "       wardrobe_tools repair <game/dota> [--catalog FILE] [--backup-directory DIR] [--repair | --list-backups | --restore latest|STAMP]\n");
    return 2;
}

// Maps the shipping client.dll the way Windows would and runs the profile
// resolver over it, so the DLL's own relocation logic is checked against the
// real binary rather than a synthetic one. Pass any argument to blank every
// recorded hint first, which forces each entry through its search path.
#include <cstdio>
#include <string>
#include <vector>
#include "../src/native_appearance_resolver.h"
using namespace appearance::profile;

static std::wstring ClientPath(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        std::wstring wide;
        for (const char* at = argv[1]; *at; ++at) wide.push_back(wchar_t(uint8_t(*at)));
        return wide;
    }
    // Steam records its library roots; the caller can always pass a path instead.
    static const wchar_t* keys[][2] = {{L"Software\\Valve\\Steam", L"SteamPath"},
                                       {L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"}};
    for (int i = 0; i < 2; ++i) {
        HKEY key{};
        const HKEY root = i == 0 ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        if (RegOpenKeyExW(root, keys[i][0], 0, KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) continue;
        wchar_t value[MAX_PATH]{};
        DWORD size = sizeof(value);
        const bool ok = RegQueryValueExW(key, keys[i][1], nullptr, nullptr, reinterpret_cast<BYTE*>(value), &size) == ERROR_SUCCESS;
        RegCloseKey(key);
        if (!ok) continue;
        std::wstring path = std::wstring(value) + L"/steamapps/common/dota 2 beta/game/dota/bin/win64/client.dll";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    return L"";
}

static uintptr_t MapImage(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<uint8_t> raw(size_t(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, raw.data(), DWORD(raw.size()), &read, nullptr);
    CloseHandle(file);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(raw.data());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
    auto base = static_cast<uint8_t*>(VirtualAlloc(nullptr, nt->OptionalHeader.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    memcpy(base, raw.data(), nt->OptionalHeader.SizeOfHeaders);
    auto section = IMAGE_FIRST_SECTION(reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew));
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(base + section[i].VirtualAddress, raw.data() + section[i].PointerToRawData, section[i].SizeOfRawData);
    return uintptr_t(base);
}

int main(int argc, char** argv) {
    std::vector<uintptr_t> functions, values;
    for (const auto& signature : Functions) functions.push_back(*signature.target);
    for (const auto& site : Sites) values.push_back(*site.target);
    const auto path = ClientPath(argc, argv);
    if (path.empty()) {
        printf("SKIP: Dota 2 is not installed here; pass the path to client.dll to check a specific build\n");
        return 0;
    }
    const auto base = MapImage(path.c_str());
    if (!base) { printf("SKIP: could not read %ls\n", path.c_str()); return 0; }
    const bool blankHints = argc > 1 && argv[argc - 1][0] == '-';
    if (blankHints) {
        for (const auto& signature : Functions) *signature.target = 0;
        for (const auto& site : Sites) *site.target = 0;
    }
    const auto begin = GetTickCount64();
    const auto result = Resolver().Run(base);
    printf("resolved=%d exact=%d moved=%u searched=%u ms=%llu failure=%s\n", result.resolved, result.exact,
           result.moved, result.searched, static_cast<unsigned long long>(GetTickCount64() - begin), result.failure);
    unsigned wrong = 0;
    for (size_t i = 0; i < std::size(Functions); ++i)
        if (*Functions[i].target != functions[i]) {
            ++wrong;
            printf("  FN  %-24s recorded %#llx got %#llx\n", Functions[i].name,
                   static_cast<unsigned long long>(functions[i]), static_cast<unsigned long long>(*Functions[i].target));
        }
    for (size_t i = 0; i < std::size(Sites); ++i)
        if (*Sites[i].target != values[i]) {
            ++wrong;
            printf("  VAL %-24s recorded %#llx got %#llx\n", Sites[i].name,
                   static_cast<unsigned long long>(values[i]), static_cast<unsigned long long>(*Sites[i].target));
        }
    if (!result.resolved || wrong) {
        printf("FAIL: the profile no longer resolves against this client.dll; run python refresh_profile.py\n");
        return 1;
    }
    printf("PASS: %zu functions and %zu values resolved from the installed client.dll%s\n",
           std::size(Functions), std::size(Sites), blankHints ? " with every recorded address blanked" : "");
    return 0;
}

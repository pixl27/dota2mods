#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>
#include "../wardrobe_session.h"

// Everything the launcher knows about the machine it is running on. Each check
// answers one question the user would otherwise have to answer by hand, and
// answers "no" rather than guessing when it cannot tell.
namespace wardrobe::env {

namespace fs = std::filesystem;

inline std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring wide(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), size);
    return wide;
}

inline std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

// ------------------------------------------------------------------ processes
struct Process { DWORD pid = 0; fs::path image; };

inline Process FindProcess(const wchar_t* name) {
    Process found;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (!_wcsicmp(entry.szExeFile, name)) { found.pid = entry.th32ProcessID; break; }
    CloseHandle(snapshot);
    if (!found.pid) return found;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, found.pid)) {
        wchar_t path[MAX_PATH]{};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(process, 0, path, &size)) found.image = path;
        CloseHandle(process);
    }
    return found;
}

// ------------------------------------------------------------------ Dota 2
inline std::vector<fs::path> SteamLibraries() {
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
    // Steam records every extra library folder in this file, so a Dota installed
    // on another drive is found without asking the user where it is.
    for (size_t i = 0, count = roots.size(); i < count; ++i) {
        std::ifstream file(roots[i] / "steamapps" / "libraryfolders.vdf");
        if (!file) continue;
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::regex path(R"vdf("path"\s+"([^"]+)")vdf");
        for (std::sregex_iterator at(text.begin(), text.end(), path), end; at != end; ++at) {
            std::string found = (*at)[1].str();
            size_t escape = 0;
            while ((escape = found.find("\\\\", escape)) != std::string::npos) found.replace(escape++, 2, "\\");
            roots.emplace_back(Widen(found));
        }
    }
    return roots;
}

// <library>/steamapps/common/dota 2 beta/game/dota
inline fs::path DotaGameDirectory(const fs::path& runningImage = {}) {
    std::error_code ignored;
    if (!runningImage.empty()) {
        // <root>/game/bin/win64/dota2.exe
        const auto candidate = runningImage.parent_path().parent_path().parent_path() / "dota";
        if (fs::exists(candidate / "pak01_dir.vpk", ignored)) return candidate;
    }
    for (const auto& root : SteamLibraries()) {
        const auto candidate = root / "steamapps" / "common" / "dota 2 beta" / "game" / "dota";
        if (fs::exists(candidate / "pak01_dir.vpk", ignored)) return candidate;
    }
    return {};
}

// ------------------------------------------------------------------ tools
// Windows ships "app execution aliases": tiny reparse points on PATH that open
// the Microsoft Store instead of running anything. One of them is python.exe, and
// it is present even when Python is not installed, so finding a file named
// python.exe on PATH says nothing on its own.
inline bool Runnable(const fs::path& candidate) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) return false;
    return info.nFileSizeHigh != 0 || info.nFileSizeLow > 16 * 1024;
}

// The first entry on PATH that is a real executable wins, so a real interpreter
// further along the list is still found when an alias shadows it.
inline fs::path Which(const wchar_t* executable) {
    std::wstring path(1 << 15, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"PATH", path.data(), DWORD(path.size()));
    if (!length || length > path.size()) return {};
    path.resize(length);
    for (size_t start = 0; start <= path.size();) {
        const size_t end = path.find(L';', start);
        const auto directory = path.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!directory.empty()) {
            const fs::path candidate = fs::path(directory) / executable;
            if (Runnable(candidate)) return candidate;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return {};
}

inline fs::path VisualStudioBuildTools() {
    std::error_code ignored;
    wchar_t programFiles[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", programFiles, MAX_PATH)) return {};
    const fs::path vswhere = fs::path(programFiles) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    return fs::exists(vswhere, ignored) ? vswhere : fs::path{};
}

// ------------------------------------------------------------------ layout
// The launcher runs either from build\ next to the binaries or from the project
// root during development; both layouts resolve to the same set of files.
struct Layout {
    fs::path exeDirectory, project, build, data;

    static Layout Discover() {
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        Layout layout;
        layout.exeDirectory = fs::path(module).parent_path();
        std::error_code ignored;
        const auto parent = layout.exeDirectory.parent_path();
        const bool insideBuild = fs::exists(parent / "build.bat", ignored);
        layout.project = insideBuild ? parent : layout.exeDirectory;
        layout.build = insideBuild ? layout.exeDirectory : layout.project / "build";
        layout.data = fs::exists(layout.exeDirectory / "data" / "skins_full.json", ignored)
                          ? layout.exeDirectory / "data" : layout.project / "data";
        return layout;
    }

    // A script may sit beside the executable or in the project it was built from.
    fs::path Script(const std::wstring& name) const {
        std::error_code ignored;
        for (const auto& directory : {exeDirectory, project})
            if (fs::exists(directory / name, ignored)) return directory / name;
        return {};
    }
    fs::path Binary(const std::wstring& name) const {
        std::error_code ignored;
        for (const auto& directory : {build, exeDirectory, project})
            if (fs::exists(directory / name, ignored)) return directory / name;
        return {};
    }
};

// ------------------------------------------------------------------ catalog
struct Catalog {
    bool present = false;
    size_t entries = 0;
    std::string updated;   // local date of the file, or empty
};

inline Catalog ReadCatalog(const fs::path& file) {
    Catalog catalog;
    std::error_code ignored;
    if (file.empty() || !fs::exists(file, ignored)) return catalog;
    catalog.present = true;
    std::ifstream stream(file, std::ios::binary);
    // Counting the records is enough to show the catalog is real; parsing three
    // megabytes of JSON to display one number would not be.
    std::string chunk(1 << 20, '\0');
    std::string carry;
    while (stream.read(chunk.data(), std::streamsize(chunk.size())) || stream.gcount()) {
        const std::string text = carry + chunk.substr(0, size_t(stream.gcount()));
        size_t at = 0;
        while ((at = text.find("\"def\":", at)) != std::string::npos) { ++catalog.entries; at += 6; }
        carry = text.size() > 8 ? text.substr(text.size() - 8) : text;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attributes)) {
        FILETIME local{};
        SYSTEMTIME when{};
        if (FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &local) && FileTimeToSystemTime(&local, &when)) {
            char text[32]{};
            sprintf_s(text, "%02u/%02u/%04u", when.wDay, when.wMonth, when.wYear);
            catalog.updated = text;
        }
    }
    return catalog;
}

// ------------------------------------------------------------------ the DLL's own report
// The injected DLL appends one line per state change; its last line is the only
// first-hand account of what happened inside the game.
inline std::string LastLogLine(const fs::path& file) {
    std::error_code ignored;
    if (!fs::exists(file, ignored)) return {};
    std::ifstream stream(file, std::ios::binary);
    if (!stream) return {};
    stream.seekg(0, std::ios::end);
    const auto size = stream.tellg();
    const std::streamoff window = 8192;
    stream.seekg(size > window ? size - window : std::streampos(0));
    std::string line, last;
    while (std::getline(stream, line))
        if (!line.empty() && line.back() != '\r') last = line;
        else if (line.size() > 1) last = line.substr(0, line.size() - 1);
    return last;
}

// ------------------------------------------------------------------ the whole picture
struct Snapshot {
    Layout layout;
    DWORD dotaPid = 0;
    fs::path dotaGame;
    bool wardrobeActive = false;
    bool loaderPresent = false, payloadPresent = false;
    Catalog catalog;
    bool pythonPresent = false, compilerPresent = false;
    // A shared copy carries the binaries but no source: the actions that end in
    // a rebuild cannot work there, and must not be offered as if they could.
    bool projectSources = false;
    fs::path python;
    std::string appearanceLine, receiverLine;

    bool DotaRunning() const { return dotaPid != 0; }
    bool DotaInstalled() const { return !dotaGame.empty(); }
    bool BinariesPresent() const { return loaderPresent && payloadPresent; }
    bool ReadyToInject() const { return DotaRunning() && BinariesPresent() && catalog.present && !wardrobeActive; }
};

inline Snapshot Look() {
    Snapshot snapshot;
    snapshot.layout = Layout::Discover();
    const auto dota = FindProcess(L"dota2.exe");
    snapshot.dotaPid = dota.pid;
    snapshot.dotaGame = DotaGameDirectory(dota.image);
    snapshot.wardrobeActive = dota.pid && HasWardrobeSession(dota.pid);
    snapshot.loaderPresent = !snapshot.layout.Binary(L"map.exe").empty();
    snapshot.payloadPresent = !snapshot.layout.Binary(L"wardrobe_dll.dll").empty();
    snapshot.catalog = ReadCatalog(snapshot.layout.data / "skins_full.json");
    snapshot.python = Which(L"python.exe");
    snapshot.pythonPresent = !snapshot.python.empty();
    snapshot.compilerPresent = !Which(L"cl.exe").empty() || !VisualStudioBuildTools().empty();
    std::error_code ignored;
    snapshot.projectSources = fs::exists(snapshot.layout.project / "build.bat", ignored) &&
                              fs::exists(snapshot.layout.project / "src" / "app" / "app.cpp", ignored);
    snapshot.appearanceLine = LastLogLine("C:\\Temp\\opencode\\wardrobe_appearance.log");
    snapshot.receiverLine = LastLogLine("C:\\Temp\\opencode\\wardrobe_gc.log");
    return snapshot;
}

}  // namespace wardrobe::env

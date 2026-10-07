#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
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

// ------------------------------------------------------------------ compatibility
// Whether this Wardrobe works with the installed Dota is decided by
// wardrobe_tools.exe, built from the same profile as the DLL in the same build:
// its verdict is the DLL's own. Asking the app's compiled-in copy instead would
// keep a stale answer after "Mettre à jour" rebuilds the DLL behind this window.
enum class CompatState { Unknown, Checking, Compatible, Incompatible, NoTool, NoDota };
struct Compatibility {
    CompatState state = CompatState::Unknown;
    bool exact = false;       // the very build the profile was recorded from
    std::string failure;      // what no longer resolves, as the resolver names it
};

// Whether the catalog still holds every cosmetic of the installed Dota. A
// content update adds items the catalog cannot offer until it is regenerated;
// only real missing cosmetics count, not every edit Valve makes to the file.
enum class FreshState { Unknown, Checking, UpToDate, Outdated, NoData };
struct Freshness {
    FreshState state = FreshState::Unknown;
    size_t missing = 0;
    std::vector<std::string> examples;   // a few names, for the sentence on the home screen
};

struct Captured { int exitCode = -1; std::string output; };

// A short helper run to completion, hidden, with its output kept.
inline Captured RunCaptured(std::wstring commandLine, DWORD timeoutMs) {
    Captured result;
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 1 << 16)) return result;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = startup.hStdError = writeEnd;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    CloseHandle(writeEnd);
    if (!started) { CloseHandle(readEnd); return result; }
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read) result.output.append(buffer, read);
    CloseHandle(readEnd);
    if (WaitForSingleObject(process.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        result.exitCode = int(code);
    } else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return result;
}

inline Compatibility ParseCompatibility(const Captured& run) {
    Compatibility result;
    const auto line = run.output.substr(0, run.output.find('\n'));
    if (run.exitCode == 2 || line.rfind("compat resolved=", 0) != 0) {
        result.state = CompatState::NoDota;
        return result;
    }
    result.state = line.find("resolved=1") != std::string::npos ? CompatState::Compatible : CompatState::Incompatible;
    result.exact = line.find("exact=1") != std::string::npos;
    if (const auto at = line.find("failure="); at != std::string::npos) {
        result.failure = line.substr(at + 8);
        while (!result.failure.empty() && (result.failure.back() == '\r' || result.failure.back() == ' ')) result.failure.pop_back();
    }
    return result;
}

inline Freshness ParseFreshness(const Captured& run) {
    Freshness result;
    std::vector<std::string> lines;
    for (size_t start = 0; start < run.output.size();) {
        size_t end = run.output.find('\n', start);
        if (end == std::string::npos) end = run.output.size();
        std::string line = run.output.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }
    if (run.exitCode == 2 || lines.empty() || lines[0].rfind("catalog missing=", 0) != 0) {
        result.state = FreshState::NoData;
        return result;
    }
    result.missing = strtoull(lines[0].c_str() + 16, nullptr, 10);
    result.state = result.missing ? FreshState::Outdated : FreshState::UpToDate;
    for (const auto& line : lines)
        if (line.rfind("    ", 0) == 0 && result.examples.size() < 3) result.examples.push_back(line.substr(4));
    return result;
}

// Both checks run wardrobe_tools.exe, built with the DLL, and each re-runs only
// when one of its inputs changed (a Dota patch, a rebuild, a new catalog):
// mapping a hundred megabytes every few seconds would be pure waste.
class CompatWatch {
public:
    CompatWatch() : worker_([this] { Loop(); }) {}
    ~CompatWatch() { stop_ = true; worker_.join(); }
    Compatibility Current() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current_;
    }
    Freshness Catalog() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return catalog_;
    }
    void Nudge() { forced_ = true; }

private:
    static std::wstring Stamp(const fs::path& file) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (file.empty() || !GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return L"-";
        return std::to_wstring(data.nFileSizeLow) + L":" + std::to_wstring(data.ftLastWriteTime.dwLowDateTime) + L":" +
               std::to_wstring(data.ftLastWriteTime.dwHighDateTime);
    }
    void Set(const Compatibility& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = value;
    }
    void SetCatalog(const Freshness& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        catalog_ = value;
    }
    void Loop() {
        std::wstring checked, checkedCatalog;
        while (!stop_) {
            const auto layout = Layout::Discover();
            const auto tool = layout.Binary(L"wardrobe_tools.exe");
            const auto game = DotaGameDirectory(FindProcess(L"dota2.exe").image);
            auto client = game.empty() ? fs::path{} : game / "bin" / "win64" / "client.dll";
            // For reviewing the screens only: check another file instead of the installed client.dll.
            wchar_t forced[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"WARDROBE_TEST_CLIENT", forced, MAX_PATH)) client = forced;
            const bool force = forced_.exchange(false);
            const auto key = tool.wstring() + L"|" + Stamp(tool) + L"|" + client.wstring() + L"|" + Stamp(client);
            if (tool.empty()) {
                Set({CompatState::NoTool, false, {}});
            } else if (client.empty()) {
                Set({CompatState::NoDota, false, {}});
            } else if (key != checked || force) {
                if (key != checked) Set({CompatState::Checking, false, {}});
                Set(ParseCompatibility(RunCaptured(L"\"" + tool.wstring() + L"\" compat \"" + client.wstring() + L"\"", 60000)));
                checked = key;
            }
            // The catalog against the cosmetics in the game's own archive.
            const auto catalog = layout.data / "skins_full.json";
            const auto archive = game.empty() ? fs::path{} : game / "pak01_dir.vpk";
            const auto catalogKey = tool.wstring() + L"|" + Stamp(tool) + L"|" + Stamp(archive) + L"|" + Stamp(catalog);
            if (tool.empty() || game.empty()) {
                SetCatalog({FreshState::NoData, 0, {}});
            } else if (catalogKey != checkedCatalog || force) {
                if (catalogKey != checkedCatalog) SetCatalog({FreshState::Checking, 0, {}});
                SetCatalog(ParseFreshness(RunCaptured(L"\"" + tool.wstring() + L"\" catalog \"" + game.wstring() + L"\" \"" +
                                                      catalog.wstring() + L"\"", 60000)));
                checkedCatalog = catalogKey;
            }
            for (int i = 0; i < 40 && !stop_ && !forced_; ++i) Sleep(50);
        }
    }
    mutable std::mutex mutex_;
    Compatibility current_;
    Freshness catalog_;
    std::atomic<bool> stop_{false}, forced_{false};
    std::thread worker_;
};

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

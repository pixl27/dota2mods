// Wardrobe-Setup.exe — one file, one button.
//
// The application, its loader, library, tools and catalog travel appended to
// this executable (an LZMS-compressed archive, decoded by Windows' own
// Compression API, so nothing else is needed). Installing is per-user: no
// administrator rights, %LOCALAPPDATA%\Programs\Wardrobe, a Desktop and Start
// menu shortcut, an entry in Windows' installed apps, then Wardrobe opens.
// Running it again updates in place, even while Wardrobe is open.
//
//   Wardrobe-Setup.exe                    the window
//   Uninstall.exe --uninstall             (copied into the install) removes it
//   --target DIR --no-shell --no-launch   tests: install elsewhere, touch nothing global
//   --silent                              no window; exit code 0 on success
//   --update --target DIR                 started by Wardrobe itself: installs at once, then reopens it
//   --capture PNG --state S               draws one state for a review
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <compressapi.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "app/brand_mark.h"
#include "app/capture.h"
#include "app/theme.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "cabinet.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace wardrobe;
namespace fs = std::filesystem;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace {

// Footer: magic, payload offset, compressed size, archive size, SHA-256 of the
// archive. LZMS has no checksum of its own: a damaged download would otherwise
// decompress into garbage of the right length and install it.
constexpr char FooterMagic[8] = {'W', 'R', 'D', 'S', 'T', 'P', '0', '2'};
constexpr size_t FooterSize = 64;
constexpr const wchar_t* UninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Wardrobe";

// ------------------------------------------------------------------ text
std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), size);
    return wide;
}
std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(size_t(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), narrow.data(), size, nullptr, nullptr);
    return narrow;
}

fs::path SelfPath() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) result = raw;
    CoTaskMemFree(raw);
    return result;
}

// ------------------------------------------------------------------ payload
struct Entry { std::string path; size_t offset = 0, size = 0; uint64_t modified = 0; };
struct Payload {
    std::vector<uint8_t> data;   // the decompressed archive
    std::vector<Entry> entries;
    std::string version;
    uint64_t stubSize = 0;       // this executable without the payload: the uninstaller
    uint64_t totalBytes = 0;
    std::string error;
};

Payload ReadPayload() {
    Payload payload;
    std::ifstream self(SelfPath(), std::ios::binary);
    if (!self) { payload.error = "Impossible de relire le programme d'installation."; return payload; }
    self.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(self.tellg());
    char footer[FooterSize]{};
    if (size < sizeof(footer)) { payload.error = "Programme d'installation incomplet."; return payload; }
    self.seekg(std::streamoff(size - sizeof(footer)));
    self.read(footer, sizeof(footer));
    if (memcmp(footer, FooterMagic, 8) != 0) {
        payload.error = "Ce programme ne contient pas Wardrobe : il sert seulement à le désinstaller.";
        return payload;
    }
    uint64_t offset = 0, packed = 0, raw = 0;
    memcpy(&offset, footer + 8, 8);
    memcpy(&packed, footer + 16, 8);
    memcpy(&raw, footer + 24, 8);
    if (offset + packed + sizeof(footer) != size || raw > (1ull << 31)) { payload.error = "Programme d'installation abîmé."; return payload; }
    std::vector<uint8_t> compressed(static_cast<size_t>(packed));
    self.seekg(std::streamoff(offset));
    self.read(reinterpret_cast<char*>(compressed.data()), std::streamsize(packed));
    DECOMPRESSOR_HANDLE decompressor = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decompressor)) { payload.error = "Décompression indisponible."; return payload; }
    payload.data.resize(size_t(raw));
    SIZE_T produced = 0;
    const BOOL ok = Decompress(decompressor, compressed.data(), compressed.size(), payload.data.data(), payload.data.size(), &produced);
    CloseDecompressor(decompressor);
    if (!ok || produced != raw) { payload.error = "Programme d'installation abîmé (décompression)."; return payload; }
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        UCHAR digest[32]{};
        bool hashed = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (hashed) hashed = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        if (hashed) hashed = BCryptHashData(hash, payload.data.data(), ULONG(payload.data.size()), 0) >= 0;
        if (hashed) hashed = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!hashed || memcmp(digest, footer + 32, 32) != 0) {
            payload.data.clear();
            payload.error = "Programme d'installation abîmé : le fichier a été modifié ou mal téléchargé. Redemande-le.";
            return payload;
        }
    }

    // "WRDA", u32 format, u16 + version, u32 count, then { u16 + path, u64 size, u64 unix time, bytes }
    size_t at = 0;
    auto need = [&](size_t bytes) { return at + bytes <= payload.data.size(); };
    auto u16 = [&]() { uint16_t v = 0; memcpy(&v, &payload.data[at], 2); at += 2; return v; };
    auto u32 = [&]() { uint32_t v = 0; memcpy(&v, &payload.data[at], 4); at += 4; return v; };
    auto u64 = [&]() { uint64_t v = 0; memcpy(&v, &payload.data[at], 8); at += 8; return v; };
    if (!need(10) || memcmp(payload.data.data(), "WRDA", 4) != 0) { payload.error = "Archive inconnue."; return payload; }
    at = 4;
    if (u32() != 1) { payload.error = "Archive d'une version plus récente."; return payload; }
    const uint16_t versionLength = u16();
    if (!need(versionLength + 4)) { payload.error = "Archive tronquée."; return payload; }
    payload.version.assign(reinterpret_cast<const char*>(&payload.data[at]), versionLength);
    at += versionLength;
    const uint32_t count = u32();
    for (uint32_t i = 0; i < count; ++i) {
        if (!need(2)) { payload.error = "Archive tronquée."; return payload; }
        const uint16_t length = u16();
        if (!need(length + 16)) { payload.error = "Archive tronquée."; return payload; }
        Entry entry;
        entry.path.assign(reinterpret_cast<const char*>(&payload.data[at]), length);
        at += length;
        entry.size = size_t(u64());
        entry.modified = u64();
        entry.offset = at;
        if (!need(entry.size) || entry.path.find("..") != std::string::npos || entry.path.empty() || entry.path[0] == '/' ||
            entry.path.find(':') != std::string::npos) { payload.error = "Archive invalide."; return payload; }
        at += entry.size;
        payload.totalBytes += entry.size;
        payload.entries.push_back(entry);
    }
    payload.stubSize = offset;
    return payload;
}

// ------------------------------------------------------------------ install
struct Options {
    fs::path target;
    bool shell = true, launch = true, silent = false;
};

struct Progress {
    std::atomic<float> fraction{0};
    std::atomic<bool> finished{false}, failed{false};
    std::mutex mutex;
    std::string current, error;
    void Set(const std::string& now) { std::lock_guard<std::mutex> lock(mutex); current = now; }
    void Fail(const std::string& why) { { std::lock_guard<std::mutex> lock(mutex); error = why; } failed = true; finished = true; }
};

fs::path DefaultTarget() { return KnownFolder(FOLDERID_UserProgramFiles) / "Wardrobe"; }

bool WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (size) out.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    return bool(out);
}

// Writes beside the file, then swaps it in. A running executable cannot be
// overwritten but can be renamed, so Wardrobe may stay open during an update.
bool Place(const fs::path& target, const uint8_t* data, size_t size, uint64_t modified = 0) {
    std::error_code ignored;
    fs::create_directories(target.parent_path(), ignored);
    fs::path fresh = target;
    fresh += L".new";
    if (!WriteFile(fresh, data, size)) return false;
    if (modified) {
        // Unix seconds to FILETIME: the date the file had when it was packaged.
        const uint64_t ticks = (modified + 11644473600ull) * 10000000ull;
        FILETIME when{DWORD(ticks & 0xffffffff), DWORD(ticks >> 32)};
        HANDLE file = CreateFileW(fresh.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) { SetFileTime(file, nullptr, nullptr, &when); CloseHandle(file); }
    }
    if (MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    fs::path aside = target;
    aside += L".old";
    DeleteFileW(aside.c_str());
    if (MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING) &&
        MoveFileExW(fresh.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    DeleteFileW(fresh.c_str());
    return false;
}

bool Shortcut(const fs::path& link, const fs::path& target, const wchar_t* description) {
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
    shell->SetPath(target.c_str());
    shell->SetWorkingDirectory(target.parent_path().c_str());
    shell->SetIconLocation(target.c_str(), 0);
    shell->SetDescription(description);
    IPersistFile* file = nullptr;
    bool ok = SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)));
    if (ok) { ok = SUCCEEDED(file->Save(link.c_str(), TRUE)); file->Release(); }
    shell->Release();
    return ok;
}

void RegisterUninstall(const fs::path& dir, const std::string& version, uint64_t bytes) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, UninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto text = [&](const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    };
    auto number = [&](const wchar_t* name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)); };
    text(L"DisplayName", L"Wardrobe");
    text(L"DisplayVersion", Widen(version));
    text(L"Publisher", L"Wardrobe");
    text(L"InstallLocation", dir.wstring());
    text(L"DisplayIcon", (dir / L"Wardrobe.exe").wstring());
    text(L"UninstallString", L"\"" + (dir / L"Uninstall.exe").wstring() + L"\" --uninstall");
    number(L"NoModify", 1);
    number(L"NoRepair", 1);
    number(L"EstimatedSize", DWORD(bytes / 1024));
    RegCloseKey(key);
}

bool InstalledAt(const fs::path& dir) {
    std::error_code ignored;
    return fs::exists(dir / L"Wardrobe.exe", ignored);
}

void Install(const Payload& payload, const Options& options, Progress& progress) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path dir = options.target;
    uint64_t written = 0;
    for (const auto& entry : payload.entries) {
        progress.Set(entry.path);
        const fs::path target = dir / Widen(entry.path);
        if (!Place(target, payload.data.data() + entry.offset, entry.size, entry.modified)) {
            progress.Fail("Impossible d'écrire " + entry.path + ". Ferme Wardrobe s'il est ouvert, puis réessaie.");
            CoUninitialize();
            return;
        }
        written += entry.size;
        progress.fraction = 0.9f * float(double(written) / double(std::max<uint64_t>(payload.totalBytes, 1)));
    }
    // The uninstaller is this program without its payload.
    progress.Set("Uninstall.exe");
    {
        std::ifstream self(SelfPath(), std::ios::binary);
        std::vector<uint8_t> stub(size_t(payload.stubSize));
        self.read(reinterpret_cast<char*>(stub.data()), std::streamsize(stub.size()));
        if (!self || !Place(dir / L"Uninstall.exe", stub.data(), stub.size())) {
            progress.Fail("Impossible d'installer le désinstalleur.");
            CoUninitialize();
            return;
        }
    }
    progress.fraction = 0.95f;
    if (options.shell) {
        progress.Set("raccourcis");
        const fs::path app = dir / L"Wardrobe.exe";
        Shortcut(KnownFolder(FOLDERID_Desktop) / L"Wardrobe.lnk", app, L"Cosmétiques Dota 2");
        Shortcut(KnownFolder(FOLDERID_Programs) / L"Wardrobe.lnk", app, L"Cosmétiques Dota 2");
        RegisterUninstall(dir, payload.version, payload.totalBytes + payload.stubSize);
    }
    // Leftovers of an update made while Wardrobe was open go once it is closed.
    std::error_code ignored;
    for (const auto& item : fs::recursive_directory_iterator(dir, ignored))
        if (item.path().extension() == L".old") DeleteFileW(item.path().c_str());
    progress.fraction = 1.0f;
    progress.finished = true;
    CoUninitialize();
}

bool ProcessRunningFrom(const fs::path& dir, const wchar_t* image) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok && !found; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, image)) continue;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)) {
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                std::error_code ignored;
                found = fs::equivalent(fs::path(path).parent_path(), dir, ignored);
            }
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
    return found;
}

// Removes what the installer created: the shortcuts, the entry in Windows'
// list, and the installation folder. Inventory backups (%LOCALAPPDATA%\
// Wardrobe\recovery) belong to the user and stay.
void Uninstall(const fs::path& dir, bool shell, Progress& progress) {
    if (ProcessRunningFrom(dir, L"Wardrobe.exe")) { progress.Fail("Wardrobe est ouvert. Ferme-le, puis réessaie."); return; }
    progress.Set("raccourcis");
    if (shell) {
        std::error_code ignored;
        for (const auto& link : {KnownFolder(FOLDERID_Desktop) / L"Wardrobe.lnk", KnownFolder(FOLDERID_Programs) / L"Wardrobe.lnk"})
            fs::remove(link, ignored);
        RegDeleteKeyW(HKEY_CURRENT_USER, UninstallKey);
    }
    progress.fraction = 0.3f;
    progress.Set("fichiers");
    std::error_code error;
    // Only a folder that is recognisably Wardrobe's is ever emptied.
    if (!fs::exists(dir / L"Wardrobe.exe", error) && !fs::exists(dir / L"Uninstall.exe", error)) {
        progress.Fail("Ce dossier ne ressemble pas à une installation de Wardrobe ; rien n'a été supprimé.");
        return;
    }
    fs::remove_all(dir, error);
    if (error && fs::exists(dir)) {
        progress.Fail("Certains fichiers n'ont pas pu être supprimés : " + Narrow(dir.wstring()));
        return;
    }
    progress.fraction = 1.0f;
    progress.finished = true;
}

// The uninstaller lives in the folder it deletes: it copies itself to the
// temporary folder, runs from there, and that copy removes itself afterwards.
bool RelaunchFromTemp(const fs::path& dir) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const fs::path copy = fs::path(temp) / L"Wardrobe-Uninstall.exe";
    std::error_code error;
    fs::copy_file(SelfPath(), copy, fs::copy_options::overwrite_existing, error);
    if (error) return false;
    std::wstring arguments = L"--uninstall-run \"" + dir.wstring() + L"\"";
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", copy.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL)) > 32;
}

void DeleteSelfLater() {
    const std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"" + SelfPath().wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring mutable_ = command;
    if (CreateProcessW(nullptr, mutable_.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
}

// ------------------------------------------------------------------ window
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
ID3D11ShaderResourceView* g_brand = nullptr;

bool CreateDevice(HWND window) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = window;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, wanted, 2, D3D11_SDK_VERSION,
                                             &description, &g_swap, &g_device, &level, &g_context)))
        return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = texture.Height = brand::MarkSize;
    texture.MipLevels = texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_IMMUTABLE;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{brand::Mark, UINT(brand::MarkSize * 4), 0};
    ID3D11Texture2D* mark = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&texture, &data, &mark)) && mark) {
        g_device->CreateShaderResourceView(mark, nullptr, &g_brand);
        mark->Release();
    }
    return g_target != nullptr;
}

void ReleaseDevice() {
    if (g_brand) g_brand->Release();
    if (g_target) g_target->Release();
    if (g_swap) g_swap->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
}

LRESULT WINAPI Proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l)) return true;
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, w, l);
}

bool SaveFrame(const char* path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    D3D11_TEXTURE2D_DESC description{};
    back->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device->CreateTexture2D(&description, nullptr, &staging))) { back->Release(); return false; }
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    bool ok = false;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        std::vector<uint8_t> rows(size_t(description.Width) * description.Height * 3);
        for (uint32_t y = 0; y < description.Height; ++y) {
            const auto* source = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch;
            for (uint32_t x = 0; x < description.Width; ++x)
                for (int c = 0; c < 3; ++c) rows[(size_t(y) * description.Width + x) * 3 + c] = source[x * 4 + c];
        }
        g_context->Unmap(staging, 0);
        ok = capture::WritePng(path, rows.data(), description.Width, description.Height);
    }
    staging->Release();
    back->Release();
    return ok;
}

enum class Screen { Ready, Working, Done, Failed, ConfirmRemove, Removed, Broken };

struct Setup {
    Payload payload;
    Options options;
    Progress progress;
    std::thread worker;
    Screen screen = Screen::Ready;
    bool removing = false, existing = false;
    double doneAt = 0;
    std::string failure;
};

void Centered(ImFont* font, float size, ImU32 tint, const char* text) {
    ImGui::PushFont(font, size);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::PopFont();
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    ui::Label(font, size, tint, text);
}
void CenteredWrapped(ImFont* font, float size, ImU32 tint, const char* text, float width) {
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    ImGui::PushFont(font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, tint);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

void OrbCentered(ui::Mark mark, float radius) {
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - radius * 2 - 12) * 0.5f);
    ui::StatusOrb(mark, radius);
}

void Feature(const char* glyph, const char* text, float width) {
    using namespace ui;
    const float left = (ImGui::GetWindowWidth() - width) * 0.5f;
    ImGui::SetCursorPosX(left);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(ImVec2(at.x + 14, at.y + 11), 14, color::Fade(color::Violet, 0.14f), 32);
    IconAt(draw, ImVec2(at.x + 14, at.y + 11), glyph, 13, color::AccentText);
    ImGui::SetCursorPosX(left + 38);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1);
    Wrapped(fonts().text, metric::TextBody, color::Muted, text, width - 38);
    Spacer(6);
}

void StartWork(Setup& setup, bool remove) {
    setup.removing = remove;
    setup.progress.fraction = 0;
    setup.progress.finished = false;
    setup.progress.failed = false;
    if (setup.worker.joinable()) setup.worker.join();
    setup.screen = Screen::Working;
    setup.worker = std::thread([&setup, remove] {
        if (remove) Uninstall(setup.options.target, setup.options.shell, setup.progress);
        else Install(setup.payload, setup.options, setup.progress);
    });
}

void DrawSetup(Setup& setup) {
    using namespace ui;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    Backdrop(ImGui::GetBackgroundDrawList(), viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y));
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(44, 40));
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    if (setup.screen == Screen::Working && setup.progress.finished) {
        if (setup.progress.failed) {
            std::lock_guard<std::mutex> lock(setup.progress.mutex);
            setup.failure = setup.progress.error;
            setup.screen = Screen::Failed;
        } else {
            setup.screen = setup.removing ? Screen::Removed : Screen::Done;
            setup.doneAt = ImGui::GetTime();
        }
    }

    const float width = ImGui::GetWindowWidth();
    const float column = width - 88;
    const float t = PageEnter(int(setup.screen));
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // brand
    {
        constexpr float mark = 72;
        const ImVec2 at(ImGui::GetWindowPos().x + (width - mark) * 0.5f, ImGui::GetCursorScreenPos().y);
        Glow(draw, ImVec2(at.x + mark * 0.5f, at.y + mark * 0.5f), 120, color::Fade(color::Violet, 0.28f));
        if (g_brand)
            draw->AddImageRounded(ImTextureRef(ImTextureID(g_brand)), at, ImVec2(at.x + mark, at.y + mark), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, 18);
        ImGui::Dummy(ImVec2(mark, mark));
        Spacer(14);
        Centered(fonts().display, 36, color::Text, "Wardrobe");
        Spacer(2);
        Centered(fonts().text, metric::TextBody, color::Muted, u8"Cosmétiques Dota 2, visibles sur ton écran");
        Spacer(30);
    }

    const bool busy = setup.screen == Screen::Working;
    switch (setup.screen) {
    case Screen::Ready: {
        static char where[512];
        // Shown from the user's folder: the whole path is long and says nothing more.
        std::wstring shown = setup.options.target.wstring();
        const std::wstring home = KnownFolder(FOLDERID_Profile).wstring();
        if (!home.empty() && shown.rfind(home, 0) == 0) shown = L"…" + shown.substr(home.size());
        sprintf_s(where, u8"Installé dans %s", Narrow(shown).c_str());
        if (setup.existing)
            Feature(icon::Sync, u8"Une version est déjà installée : elle sera mise à jour.", column);
        Feature(icon::Desktop, u8"Un raccourci sur le Bureau et dans le menu Démarrer.", column);
        Feature(icon::Shield, u8"Rien d'autre à installer : ni Python, ni droits d'administrateur.", column);
        Feature(icon::Folder, where, column);
        Spacer(18);
        ImGui::SetCursorPosX((width - column) * 0.5f);
        if (PrimaryButton(setup.existing ? u8"Mettre à jour Wardrobe" : "Installer Wardrobe", icon::Download, nullptr, true, column, 56))
            StartWork(setup, false);
        Spacer(18);
        ImGui::SetCursorPosX((width - column) * 0.5f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        IconAt(draw, ImVec2(at.x + 8, at.y + 9), icon::Warning, 13, color::Warn);
        ImGui::SetCursorPosX((width - column) * 0.5f + 24);
        Wrapped(fonts().text, metric::TextSmall, color::Faint,
                u8"Dota 2 est protégé par VAC. Charger Wardrobe dans le jeu peut entraîner une sanction du compte : "
                u8"utilise-le en connaissance de cause.", column - 24);
        break;
    }
    case Screen::Working: {
        OrbCentered(Mark::Busy, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, setup.removing ? u8"Désinstallation…" : u8"Installation…");
        Spacer(18);
        ImGui::SetCursorPosX((width - column) * 0.5f);
        ProgressBar("progress", setup.progress.fraction, column, 8);
        Spacer(10);
        std::string now;
        { std::lock_guard<std::mutex> lock(setup.progress.mutex); now = setup.progress.current; }
        Centered(fonts().text, metric::TextSmall, color::Faint, now.c_str());
        break;
    }
    case Screen::Done: {
        OrbCentered(Mark::Ok, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, u8"C'est prêt");
        Spacer(6);
        CenteredWrapped(fonts().text, metric::TextBody, color::Muted,
                        setup.options.launch ? u8"Wardrobe s'ouvre. Tu le retrouveras ensuite sur le Bureau et dans le menu Démarrer."
                                             : u8"Wardrobe est installé. Tu le trouveras sur le Bureau et dans le menu Démarrer.",
                        column - 60);
        break;
    }
    case Screen::Failed: {
        OrbCentered(Mark::Bad, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, setup.removing ? u8"La désinstallation a échoué" : u8"L'installation a échoué");
        Spacer(6);
        CenteredWrapped(fonts().text, metric::TextBody, color::Muted, setup.failure.c_str(), column - 40);
        Spacer(22);
        const float half = (column - metric::Gap) * 0.5f;
        ImGui::SetCursorPosX((width - column) * 0.5f);
        if (SecondaryButton("Fermer", nullptr, true, half, 52)) PostQuitMessage(1);
        ImGui::SameLine(0, metric::Gap);
        if (PrimaryButton(u8"Réessayer", icon::Refresh, nullptr, true, half, 52)) StartWork(setup, setup.removing);
        break;
    }
    case Screen::ConfirmRemove: {
        OrbCentered(Mark::Warn, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, u8"Désinstaller Wardrobe ?");
        Spacer(6);
        CenteredWrapped(fonts().text, metric::TextBody, color::Muted,
                        u8"L'application, ses raccourcis et son entrée dans Windows seront retirés. Les sauvegardes "
                        u8"d'inventaire restent dans %LOCALAPPDATA%\\Wardrobe.", column - 40);
        Spacer(22);
        const float half = (column - metric::Gap) * 0.5f;
        ImGui::SetCursorPosX((width - column) * 0.5f);
        if (SecondaryButton("Annuler", nullptr, true, half, 52)) PostQuitMessage(1);
        ImGui::SameLine(0, metric::Gap);
        if (PrimaryButton(u8"Désinstaller", icon::Delete, nullptr, true, half, 52)) StartWork(setup, true);
        break;
    }
    case Screen::Removed: {
        OrbCentered(Mark::Ok, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, u8"Wardrobe a été désinstallé");
        Spacer(6);
        CenteredWrapped(fonts().text, metric::TextBody, color::Muted, u8"Merci de l'avoir essayé.", column - 40);
        Spacer(22);
        ImGui::SetCursorPosX((width - column) * 0.5f);
        if (PrimaryButton("Fermer", nullptr, nullptr, true, column, 52)) PostQuitMessage(0);
        break;
    }
    case Screen::Broken: {
        OrbCentered(Mark::Bad, 30);
        Spacer(12);
        Centered(fonts().strong, metric::TextTitle, color::Text, u8"Programme d'installation incomplet");
        Spacer(6);
        CenteredWrapped(fonts().text, metric::TextBody, color::Muted, setup.payload.error.c_str(), column - 40);
        Spacer(22);
        ImGui::SetCursorPosX((width - column) * 0.5f);
        if (SecondaryButton("Fermer", nullptr, true, column, 52)) PostQuitMessage(1);
        break;
    }
    }
    (void)busy;
    PageExit(t);
    ImGui::End();
}

void StyleWindowFrame(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x0D, 0x0F, 0x15), text = RGB(0xC9, 0xCF, 0xDC), border = RGB(0x1E, 0x22, 0x2C);
    DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, 36, &text, sizeof(text));
    DwmSetWindowAttribute(window, 34, &border, sizeof(border));
    const int round = 2;
    DwmSetWindowAttribute(window, 33, &round, sizeof(round));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Setup setup;
    std::wstring capture, captureState;
    bool uninstall = false, uninstallRun = false, autoStart = false;
    if (int count = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count)) {
        for (int i = 1; i < count; ++i) {
            const std::wstring arg = argv[i];
            if (arg == L"--target" && i + 1 < count) setup.options.target = argv[++i];
            else if (arg == L"--no-shell") setup.options.shell = false;
            else if (arg == L"--no-launch") setup.options.launch = false;
            else if (arg == L"--silent") setup.options.silent = true;
            else if (arg == L"--uninstall") uninstall = true;
            else if (arg == L"--update") autoStart = true;
            else if (arg == L"--uninstall-run" && i + 1 < count) { uninstallRun = true; setup.options.target = argv[++i]; }
            else if (arg == L"--capture" && i + 1 < count) capture = argv[++i];
            else if (arg == L"--state" && i + 1 < count) captureState = argv[++i];
        }
        LocalFree(argv);
    }
    if (setup.options.target.empty()) setup.options.target = uninstall ? SelfPath().parent_path() : DefaultTarget();
    // Tests drive the real update path through Wardrobe, whose environment the
    // installer inherits: this keeps such runs off the Desktop and the registry.
    if (char forced[4]{}; GetEnvironmentVariableA("WARDROBE_TEST_NO_SHELL", forced, sizeof(forced)) && forced[0] == '1')
        setup.options.shell = false;

    // Uninstall.exe --uninstall: hand over to a copy outside the folder it removes.
    if (uninstall && !uninstallRun && capture.empty()) {
        if (!RelaunchFromTemp(setup.options.target))
            MessageBoxW(nullptr, L"Impossible de lancer la désinstallation.", L"Wardrobe", MB_ICONERROR);
        return 0;
    }

    if (!uninstallRun) {
        setup.payload = ReadPayload();
        setup.existing = InstalledAt(setup.options.target);
    }
    if (uninstallRun || uninstall) setup.screen = Screen::ConfirmRemove;
    else if (!setup.payload.error.empty()) setup.screen = Screen::Broken;

    if (setup.options.silent) {
        if (setup.screen == Screen::Broken) return 2;
        if (uninstallRun) Uninstall(setup.options.target, setup.options.shell, setup.progress);
        else Install(setup.payload, setup.options, setup.progress);
        return setup.progress.failed ? 1 : 0;
    }

    // Review captures: draw one state without doing anything.
    if (!captureState.empty()) {
        if (captureState == L"working") { setup.screen = Screen::Working; setup.progress.fraction = 0.62f; setup.progress.Set("data/skins_full.json"); }
        else if (captureState == L"done") setup.screen = Screen::Done;
        else if (captureState == L"failed") { setup.screen = Screen::Failed; setup.failure = u8"Impossible d'écrire Wardrobe.exe. Ferme Wardrobe s'il est ouvert, puis réessaie."; }
        else if (captureState == L"remove") setup.screen = Screen::ConfirmRemove;
        else if (captureState == L"removed") setup.screen = Screen::Removed;
        else if (captureState == L"update") setup.existing = true;
    }

    const int width = 620, height = 640;
    WNDCLASSEXW window{sizeof(window), CS_CLASSDC, Proc, 0, 0, instance, LoadIconW(instance, MAKEINTRESOURCEW(1)), nullptr, nullptr,
                       nullptr, L"WardrobeSetup", LoadIconW(instance, MAKEINTRESOURCEW(1))};
    RegisterClassExW(&window);
    RECT area{0, 0, width, height};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&area, style, FALSE, 0, 96);
    const int w = area.right - area.left, h = area.bottom - area.top;
    HWND handle = CreateWindowW(window.lpszClassName, uninstallRun || uninstall ? L"Désinstaller Wardrobe" : L"Installer Wardrobe", style,
                                (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                                nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(handle)) {
        ReleaseDevice();
        MessageBoxW(nullptr, L"Impossible d'initialiser l'affichage (DirectX 11).", L"Wardrobe", MB_ICONERROR);
        return 1;
    }
    StyleWindowFrame(handle);
    ShowWindow(handle, capture.empty() ? SW_SHOWNORMAL : SW_SHOWNA);
    UpdateWindow(handle);

    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ui::LoadFonts();
    ui::ApplyStyle();
    ImGui_ImplWin32_Init(handle);
    ImGui_ImplDX11_Init(g_device, g_context);

    // Wardrobe downloaded this installer and closed itself: no question to ask.
    if (autoStart && setup.screen == Screen::Ready) {
        // Give the closing Wardrobe a moment; its executable is moved aside anyway.
        Sleep(600);
        StartWork(setup, false);
    }
    int frames = 0;
    bool running = true, launched = false;
    int exitCode = 0;
    while (running) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
            if (message.message == WM_QUIT) { running = false; exitCode = int(message.wParam); }
        }
        if (!running) break;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawSetup(setup);
        ImGui::Render();
        const float clear[4] = {0.035f, 0.043f, 0.063f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);

        if (!capture.empty() && ++frames > 40) {
            SaveFrame(Narrow(capture).c_str());
            running = false;
        }
        // Installed: open Wardrobe, let "C'est prêt" be read, then step aside.
        if (capture.empty() && setup.screen == Screen::Done) {
            if (!launched && setup.options.launch) {
                const fs::path app = setup.options.target / L"Wardrobe.exe";
                ShellExecuteW(nullptr, L"open", app.c_str(), nullptr, setup.options.target.c_str(), SW_SHOWNORMAL);
                launched = true;
            }
            if (ImGui::GetTime() - setup.doneAt > 2.2) running = false;
        }
    }
    if (setup.worker.joinable()) setup.worker.join();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseDevice();
    DestroyWindow(handle);
    if (uninstallRun && setup.screen == Screen::Removed) DeleteSelfLater();
    CoUninitialize();
    return exitCode;
}

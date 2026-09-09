// src/dllmain.cpp - native inventory and game-thread appearance integration.
// Build: build.bat already does it -> build\wardrobe_dll.dll

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstdio>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "db.h"
#include "inventory.h"
#include "native_appearance.h"
#include "wardrobe_session.h"
#include "../thirdparty/minhook/include/MinHook.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "ws2_32.lib")

// ---------- globals ----------
Config g_Cfg;
std::vector<SkinEntry> g_DB;
std::mutex g_DbMutex;
std::map<std::string, std::map<std::string, int>> g_Loadout;
std::mutex g_LoadoutMutex;
void SaveLoadout(const std::string&) {}
void LoadLoadout(const std::string&) {}
void LoadDB(const std::string&);   // implemented in inventory.cpp (real JSON parse)

static uintptr_t g_ClientBase = 0;
static std::atomic<bool> g_Running{ true };

// ---------- Present hook: menu inside the game + status ----------

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn oPresent = nullptr;
static HWND g_hWnd = nullptr;
static WNDPROC oWndProc = nullptr;
static ID3D11Device* g_Dev = nullptr;
static ID3D11DeviceContext* g_Ctx = nullptr;
static bool g_Init = false, g_Show = false;

static LRESULT CALLBACK DllWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    __try {
        extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
        if (m == WM_KEYDOWN && w == VK_INSERT) { g_Show = !g_Show; return 0; }
        if (g_Show && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (oWndProc) return CallWindowProc(oWndProc, h, m, w, l);
    return DefWindowProc(h, m, w, l);
}

static void RenderInGame() {
    ImGui::Begin("wardrobe — live", &g_Show, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Wardrobe v6 - Équipement natif");
    // Both status structs carry strings; refresh a few times per second, not per frame.
    static appearance::Status cachedNative; static InventoryStatus cachedInventory; static uint64_t cachedAt = 0;
    if (const auto now = GetTickCount64(); now - cachedAt >= 250) { cachedAt = now; cachedNative = appearance::NativeStatus(); cachedInventory = GetInventoryStatus(); }
    const auto& native = cachedNative; const auto& inventory = cachedInventory;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 65);
    ImGui::TextWrapped("%s", appearance::PhaseText(native.phase));
    if (native.hero) {
        ImGui::TextDisabled("Héros: %u | objets résolus: %u/%u | mises à jour: %llu",
            native.hero, native.matched, native.expected, (unsigned long long)native.rebuilds);
        ImGui::TextDisabled("Modèles: %u enregistrés | %u connus | %u indisponibles | resync: %u",
            native.registered, native.known, native.unavailable, native.resyncs);
        if (native.renderModel[0]) {
            const bool broken = strstr(native.renderModel, "error") != nullptr;
            ImGui::TextColored(broken ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.6f, 0.9f, 0.6f, 1), "Modèle rendu: %s", native.renderModel);
        }
        if (GetTickCount64() > native.lastSeenMs + 3000)
            ImGui::TextDisabled("En attente du héros dans la partie");
    }
    ImGui::TextDisabled("Équipe directement dans Dota. La tenue se met à jour automatiquement en partie.");
    ImGui::PopTextWrapPos();
    ImGui::Separator();
    const bool delivered = inventory.phase == InventoryPhase::Delivered;
    const bool failed = inventory.phase == InventoryPhase::Failed || inventory.phase == InventoryPhase::TimedOut;
    const bool waiting = inventory.phase == InventoryPhase::Starting || inventory.phase == InventoryPhase::Waiting;
    ImGui::TextDisabled("Dota PID: %lu | build: %s %s", GetCurrentProcessId(), __DATE__, __TIME__);
    ImGui::TextColored(delivered ? ImVec4(0.4f, 1, 0.4f, 1) : failed ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.8f, 0.2f, 1),
        "Inventory: %s", delivered ? "records delivered" : failed ? "needs attention" : waiting ? "refreshing" : "paused");
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 65);
    ImGui::TextWrapped("%s", inventory.detail.c_str());
    ImGui::PopTextWrapPos();
    if (inventory.cachedLocalItems) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 65);
        ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Dota disk cache: %llu saved preview items", (unsigned long long)inventory.cachedLocalItems);
        ImGui::TextWrapped("These can appear without Wardrobe running. Close Dota and use repair_inventory_cache.py to restore Steam's inventory cache.");
        ImGui::PopTextWrapPos();
    }
    ImGui::Text("Catalog: %llu items | GC receiver: %s", (unsigned long long)inventory.items,
        inventory.hookActive ? "active" : "inactive");
    ImGui::TextDisabled("Refreshes: %u/3 | polls: %llu | reads: %llu | packets: %llu | last: %u",
        inventory.refreshes, (unsigned long long)inventory.polls, (unsigned long long)inventory.receives,
        (unsigned long long)inventory.packets, inventory.lastType);
    ImGui::Text("Local equip: %s | accepted: %llu | completed: %llu | items: %llu",
        inventory.equipActive ? "active" : "inactive", (unsigned long long)inventory.acceptedEquips,
        (unsigned long long)inventory.completedEquips, (unsigned long long)inventory.localEquips);
    ImGui::TextColored(inventory.oldestReplyMs > 2000 ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1),
        "Pending replies: %llu | oldest: %llu ms | last delivery: %llu ms",
        (unsigned long long)inventory.pendingReplies, (unsigned long long)inventory.oldestReplyMs,
        (unsigned long long)inventory.lastDeliveryMs);
    ImGui::TextDisabled("Updates: %llu | acknowledgements: %llu | notifications: %llu | callback polls: %llu",
        (unsigned long long)inventory.equipUpdates, (unsigned long long)inventory.equipAcks,
        (unsigned long long)inventory.wakeups, (unsigned long long)inventory.callbackPolls);
    ImGui::TextDisabled("Equip processing: %.2f ms (max %.2f ms) | GC state: %u",
        inventory.lastEquipUs / 1000.0, inventory.maxEquipUs / 1000.0, inventory.connectionStatus);
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 65);
    ImGui::TextWrapped("%s", inventory.equipDetail.c_str());
    ImGui::TextDisabled("Completed = update and acknowledgement delivered. One outfit can contain several items.");
    ImGui::TextDisabled("Equip in Dota's native loadout screen. Dota can save preview records to its disk cache.");
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(waiting);
    if (ImGui::Button(inventory.recordsDelivered ? "Resume inventory receiver" : "Retry inventory refresh")) RequestInventoryRefresh();
    ImGui::EndDisabled();
    if (inventory.hookActive) {
        ImGui::SameLine();
        if (ImGui::Button("Pause GC receiver")) PauseInventory();
    }
    ImGui::TextDisabled("Log: C:\\Temp\\opencode\\wardrobe_gc.log");

    ImGui::TextDisabled("INSERT : afficher / masquer Wardrobe");
    ImGui::End();
}

static bool g_HudDisabled = false;
static void InitializeHud(IDXGISwapChain* ch) {
    if (FAILED(ch->GetDevice(__uuidof(ID3D11Device), (void**)&g_Dev)) || !g_Dev) return;
    g_Dev->GetImmediateContext(&g_Ctx);
    DXGI_SWAP_CHAIN_DESC d{}; ch->GetDesc(&d);
    g_hWnd = d.OutputWindow;
    ImGui::CreateContext();
    // The built-in ImGui font has no accented glyphs and the HUD is French.
    ImGuiIO& io = ImGui::GetIO();
    char font[MAX_PATH]{};
    if (GetWindowsDirectoryA(font, MAX_PATH)) strcat_s(font, "\\Fonts\\segoeui.ttf");
    if (!font[0] || !io.Fonts->AddFontFromFileTTF(font, 17.0f, nullptr, io.Fonts->GetGlyphRangesDefault())) io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_Dev, g_Ctx);
    ImGui::StyleColorsDark();
    oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)DllWndProc);
    g_Init = true;
}
static bool SameDevice(IDXGISwapChain* ch) {
    // Dota can present through a recreated swap chain (display mode changes);
    // never create views on a device that did not own this back buffer.
    ID3D11Device* device = nullptr;
    if (FAILED(ch->GetDevice(__uuidof(ID3D11Device), (void**)&device)) || !device) return false;
    const bool same = device == g_Dev; device->Release(); return same;
}
static void DrawHud(IDXGISwapChain* ch) {
    volatile bool begun = false;
    __try {
        ID3D11Texture2D* b = nullptr;
        if (SUCCEEDED(ch->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b)) && b) {
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(g_Dev->CreateRenderTargetView(b, nullptr, &rtv)) && rtv) {
                ImGui::GetIO().MouseDrawCursor = true;
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame(); begun = true;
                RenderInGame();
                ImGui::Render(); begun = false;

                ID3D11RenderTargetView* prevRTV = nullptr;
                ID3D11DepthStencilView* prevDSV = nullptr;
                g_Ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);
                g_Ctx->OMSetRenderTargets(1, &rtv, nullptr);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                g_Ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
                if (prevRTV) prevRTV->Release();
                if (prevDSV) prevDSV->Release();
                rtv->Release();
            }
            b->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Never leave ImGui inside a frame (the next NewFrame would assert),
        // and stop drawing instead of hiding the same fault every frame.
        if (begun) ImGui::EndFrame();
        g_HudDisabled = true;
    }
}
static HRESULT __stdcall hkPresent(IDXGISwapChain* ch, UINT s, UINT f) {
    if (!g_Init) InitializeHud(ch);
    if (g_Show && g_Init && !g_HudDisabled && SameDevice(ch)) DrawHud(ch);
    else if (g_Init) ImGui::GetIO().MouseDrawCursor = false;
    return oPresent(ch, s, f);
}

// ---------- boot ----------

static HANDLE g_WardrobeSession = nullptr;
DWORD WINAPI MainThread(LPVOID mod) {
    HANDLE session = ClaimWardrobeSession(GetCurrentProcessId());
    if (!session) return 0;
    g_WardrobeSession = session;
    g_ClientBase = (uintptr_t)GetModuleHandleA("client.dll");
    int waits = 0;
    while (!g_ClientBase && waits++ < 120) { Sleep(500); g_ClientBase = (uintptr_t)GetModuleHandleA("client.dll"); }

    // Initialize independently of rendering and request the cache even when
    // the initial GC welcome arrived before this DLL was loaded.
    MH_Initialize();
    appearance::InitializeNative();
    InitializeInventory();
    std::thread([]() { while (g_Running) { TickInventory(); appearance::TickNativeDiagnostics(); Sleep(250); } }).detach();

    while (!GetModuleHandleA("d3d11.dll")) Sleep(500);

    // Create a temporary dummy window to safely retrieve the DX11 Present vtable pointer
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "WardrobeDummy";
    RegisterClassA(&wc);
    HWND dummyHwnd = CreateWindowA("WardrobeDummy", "dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    D3D_FEATURE_LEVEL lv;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummyHwnd ? dummyHwnd : GetDesktopWindow();
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    ID3D11Device* d = nullptr; IDXGISwapChain* s = nullptr; ID3D11DeviceContext* c = nullptr;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &sd, &s, &d, &lv, &c))) {
        void* present = (*(void***)s)[8];
        s->Release(); d->Release(); c->Release();
        MH_CreateHook(present, &hkPresent, (void**)&oPresent);
        MH_EnableHook(present);
    }
    if (dummyHwnd) DestroyWindow(dummyHwnd);
    UnregisterClassA("WardrobeDummy", wc.hInstance);

    // erase PE headers if mod pointer is valid
    if (mod) {
        DWORD old;
        VirtualProtect(mod, 0x1000, PAGE_READWRITE, &old);
        memset(mod, 0, 0x1000);
        VirtualProtect(mod, 0x1000, old, &old);
    }

    return 0;
}

// map.exe copies the sections but nothing registers .pdata for them: without
// unwind data the first C++ throw (JSON parsing) would fail-fast dota2.exe.
static void RegisterUnwindInfo(HMODULE module) {
    HMODULE loaded = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(module), &loaded) && loaded) return; // loaded by the OS: already registered
    const auto base = reinterpret_cast<uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto& exceptions = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!exceptions.VirtualAddress || !exceptions.Size) return;
    RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(base + exceptions.VirtualAddress),
        exceptions.Size / sizeof(RUNTIME_FUNCTION), reinterpret_cast<DWORD64>(base));
}
BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        RegisterUnwindInfo(h);
        CreateThread(nullptr, 0, MainThread, h, 0, nullptr);
    }
    return TRUE;
}

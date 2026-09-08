// src/main.cpp
#include <thread>
#include "overlay.h"
#include "gamestate.h"
#include "stealth.h"

Config g_Cfg;
std::vector<SkinEntry> g_DB;
std::mutex g_DbMutex;
std::map<std::string, std::map<std::string, int>> g_Loadout;
std::mutex g_LoadoutMutex;
LiveState g_Live;

ID3D11Device* g_Dev = nullptr;
ID3D11DeviceContext* g_Ctx = nullptr;
IDXGISwapChain* g_Swap = nullptr;
ID3D11RenderTargetView* g_RTV = nullptr;
HWND g_Overlay = nullptr;
bool g_MenuOpen = true;

void LoadDB(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    json j; f >> j;
    std::lock_guard<std::mutex> lk(g_DbMutex);
    g_DB.clear();
    if (j.contains("skins"))
        for (auto& e : j["skins"])
            g_DB.push_back({ e.value("def", 0), e.value("name", ""),
                e.value("hero", ""), e.value("slot", "misc"),
                e.value("rarity", "common"), e.value("prefab", "") });
}

void SaveLoadout(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_LoadoutMutex);
    json j = json::object();
    for (auto& [h, slots] : g_Loadout)
        for (auto& [s, d] : slots) j[h][s] = d;
    std::ofstream f(path); f << j.dump(2);
}

void LoadLoadout(const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    try {
        json j; f >> j;
        std::lock_guard<std::mutex> lk(g_LoadoutMutex);
        for (auto it = j.begin(); it != j.end(); ++it)
            for (auto s = it->begin(); s != it->end(); ++s)
                g_Loadout[it.key()][s.key()] = s.value().get<int>();
    } catch (...) {}
}

namespace stealth {
    bool PushLoadout(const std::string& hero) {
        if (!g_Off.valid) return false;
        DWORD pid = FindDotaPid();
        if (!pid) return false;
        HANDLE h = SpoofedHandle(pid);
        if (!h || h == INVALID_HANDLE_VALUE) return false;
        // --- wire to dumper offsets here ---
        // read client.dll base, walk localHero -> wearables,
        // write g_Loadout[hero][slot] defs, set reapply flag.
        CloseHandle(h);
        return true;
    }
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_KEYDOWN && w == VK_INSERT) { g_MenuOpen = !g_MenuOpen; return 0; }
    return DefWindowProc(h, m, w, l);
}

void MakeOverlay(HINSTANCE inst) {
    WNDCLASSA wc{ CS_HREDRAW | CS_VREDRAW, OverlayProc, 0, 0, inst,
        nullptr, nullptr, nullptr, nullptr, "WardrobeOverlay" };
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_Overlay = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        "WardrobeOverlay", "Wardrobe", WS_POPUP,
        0, 0, sw, sh, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_Overlay, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS mg{ -1,-1,-1,-1 };
    DwmExtendFrameIntoClientArea(g_Overlay, &mg);
    if (g_Cfg.streamproof)
        SetWindowDisplayAffinity(g_Overlay, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_Overlay, SW_SHOW);
}

void InitDX() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_Overlay;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_Swap, &g_Dev, nullptr, &g_Ctx);
    ID3D11Texture2D* back = nullptr;
    g_Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_Dev->CreateRenderTargetView(back, nullptr, &g_RTV);
    back->Release();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_Overlay);
    ImGui_ImplDX11_Init(g_Dev, g_Ctx);
    ImGui::StyleColorsDark();
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    LoadDB("data/skins_full.json");
    if (g_DB.empty()) LoadDB("build/data/skins_full.json");
    if (g_DB.empty()) LoadDB("skins_full.json");
    LoadLoadout("loadout.json");

    std::thread(GSIThread).detach();
    MakeOverlay(inst);
    InitDX();

    MSG msg{};
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) { SaveLoadout("loadout.json"); return 0; }
        }
        LONG ex = GetWindowLong(g_Overlay, GWL_EXSTYLE);
        if (g_MenuOpen) SetWindowLong(g_Overlay, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        else SetWindowLong(g_Overlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (g_MenuOpen) DrawWardrobe();
        ImGui::Render();
        float clear[4] = { 0,0,0,0 };
        g_Ctx->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Ctx->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_Swap->Present(1, 0);
    }
}

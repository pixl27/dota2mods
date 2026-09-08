// src/main.cpp
#include <thread>
#include <set>
#include "overlay.h"
#include "gamestate.h"
#include "stealth.h"

Config g_Cfg;
std::vector<SkinEntry> g_DB;
std::vector<std::string> g_UniqueHeroes;
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
    try {
        json j; f >> j;
        std::lock_guard<std::mutex> lk(g_DbMutex);
        g_DB.clear();
        g_UniqueHeroes.clear();
        std::set<std::string> heroSet;
        if (j.contains("skins") && j["skins"].is_array()) {
            for (auto& e : j["skins"]) {
                std::string h = e.value("hero", "");
                std::string bname = "";
                if (e.contains("bundles") && e["bundles"].is_array() && !e["bundles"].empty()) {
                    bname = e["bundles"][0].value("name", "");
                }
                g_DB.push_back({
                    e.value("def", 0),
                    e.value("name", ""),
                    h,
                    e.value("slot", "misc"),
                    e.value("rarity", "common"),
                    e.value("prefab", ""),
                    bname
                });
                if (!h.empty() && h != "_global") {
                    heroSet.insert(h);
                }
            }
        }
        for (const auto& h : heroSet) {
            g_UniqueHeroes.push_back(h);
        }
    } catch (...) {}
}

void SaveLoadout(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_LoadoutMutex);
    json j = json::object();
    for (auto& [h, slots] : g_Loadout)
        for (auto& [s, d] : slots) j[h][s] = d;
    std::ofstream f(path);
    if (f) f << j.dump(2);
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

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

void MakeOverlay(HINSTANCE inst) {
    WNDCLASSA wc{ CS_HREDRAW | CS_VREDRAW, OverlayProc, 0, 0, inst,
        nullptr, nullptr, nullptr, nullptr, "WardrobeOverlay" };
    RegisterClassA(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    g_Overlay = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        "WardrobeOverlay", "Wardrobe", WS_POPUP,
        0, 0, sw, sh, nullptr, nullptr, inst, nullptr);
    SetLayeredWindowAttributes(g_Overlay, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS mg{ -1,-1,-1,-1 };
    DwmExtendFrameIntoClientArea(g_Overlay, &mg);
    if (g_Cfg.streamproof)
        SetWindowDisplayAffinity(g_Overlay, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_Overlay, SW_SHOW);
}

bool InitDX() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_Overlay;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_Swap, &g_Dev, nullptr, &g_Ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_Swap, &g_Dev, nullptr, &g_Ctx);
        if (FAILED(hr)) return false;
    }

    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_Swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
        return false;
    }
    g_Dev->CreateRenderTargetView(back, nullptr, &g_RTV);
    back->Release();

    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_Overlay);
    ImGui_ImplDX11_Init(g_Dev, g_Ctx);
    ImGui::StyleColorsDark();
    return true;
}

void CleanupDX() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_RTV) { g_RTV->Release(); g_RTV = nullptr; }
    if (g_Swap) { g_Swap->Release(); g_Swap = nullptr; }
    if (g_Ctx) { g_Ctx->Release(); g_Ctx = nullptr; }
    if (g_Dev) { g_Dev->Release(); g_Dev = nullptr; }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    LoadDB("data/skins_full.json");
    if (g_DB.empty()) LoadDB("build/data/skins_full.json");
    if (g_DB.empty()) LoadDB("skins_full.json");
    LoadLoadout("loadout.json");
    stealth::LoadOffsets();

    std::thread(GSIThread).detach();
    MakeOverlay(inst);
    if (!InitDX()) return 1;

    bool prevInsert = false;
    bool lastMenuState = g_MenuOpen;

    MSG msg{};
    while (true) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                SaveLoadout("loadout.json");
                CleanupDX();
                return 0;
            }
        }

        bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertDown && !prevInsert) {
            g_MenuOpen = !g_MenuOpen;
        }
        prevInsert = insertDown;

        if (g_MenuOpen != lastMenuState) {
            LONG_PTR ex = GetWindowLongPtr(g_Overlay, GWL_EXSTYLE);
            if (g_MenuOpen) {
                SetWindowLongPtr(g_Overlay, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
                SetForegroundWindow(g_Overlay);
            } else {
                SetWindowLongPtr(g_Overlay, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
            }
            lastMenuState = g_MenuOpen;
        }

        if (!g_MenuOpen) {
            Sleep(16);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawWardrobe();

        ImGui::Render();
        float clear[4] = { 0, 0, 0, 0 };
        g_Ctx->OMSetRenderTargets(1, &g_RTV, nullptr);
        g_Ctx->ClearRenderTargetView(g_RTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_Swap->Present(1, 0);
    }
}

// src/dllmain.cpp — live model swap payload. MANUAL-MAP ONLY. Never LoadLibrary.
// Build: build.bat already does it -> build\wardrobe_dll.dll

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <cstdio>
#include "db.h"
#include "../thirdparty/minhook/include/MinHook.h"

// inventory.cpp — GC inventory unlock (Dota's own loadout UI shows everything)
void BuildInjectBlob();
size_t InjectItemCount();
typedef void(__fastcall* OnCacheFn)(void* self, void* msg, size_t len);
void __fastcall hkOnCache(void* self, void* msg, size_t len);
OnCacheFn* GetOnCacheOrigSlot();

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

// ---------- offsets: filled by dump_offsets.py -> offsets.bin, read at boot ----------
// NEVER hardcode these. They shift every major patch. Stale offsets = crash or worse.
struct GameOffsets {
    uint32_t magic = 0x57415244; // "WARD"
    uint32_t version = 1;
    uintptr_t dwLocalPlayerHero = 0;   // client.dll + X -> C_DOTA_BaseNPC_Hero*
    uintptr_t m_hWearables = 0;        // hero + X -> EHANDLE[8] wearable list
    uintptr_t m_ItemView = 0;          // C_EconWearable + X -> CEconItemView
    uintptr_t m_iItemDefIndex = 0;     // itemView + X -> int32 def index  <-- THE write
    uintptr_t m_nFallbackPaint = 0;    // itemView + X -> paint kit (styles)
    uintptr_t m_bNeedReapply = 0;      // hero + X -> bool force re-apply wearables
    uintptr_t dwEntityList = 0;        // client.dll + X -> entity list base
    uintptr_t fnFullUpdate = 0;        // client.dll + X -> force full update fn
};
static GameOffsets g_Off;
static uintptr_t g_ClientBase = 0;
static std::atomic<bool> g_Running{ true };
static std::string g_ForcedHero;       // hero key, set from menu or EXE link

// ---------- tiny read/write wrappers (we're in-process: direct pointers) ----------

template<typename T>
static T Read(uintptr_t a) { return *(T*)a; }
template<typename T>
static void Write(uintptr_t a, const T& v) { *(T*)a = v; }

// resolve EHANDLE -> entity via entity list: handle & 0xFFF -> index, standard source pattern
static uintptr_t HandleToEnt(uintptr_t entList, uint32_t handle) {
    uint32_t idx = handle & 0xFFF;
    // chunked list: (idx >> 9) chunk, (idx & 0x1FF) slot, 0x10 stride — source engine standard
    uintptr_t chunk = Read<uintptr_t>(entList + 0x8 * (idx >> 9) + 0x10);
    if (!chunk) return 0;
    uintptr_t ent = Read<uintptr_t>(chunk + 0x70 * (idx & 0x1FF));
    return ent;
}

// ---------- THE swap: push one hero's loadout into live entities ----------

static bool PushHero(const std::string& heroKey) {
    if (!g_Off.dwLocalPlayerHero || !g_Off.m_hWearables || !g_Off.m_iItemDefIndex || !g_ClientBase)
        return false;

    __try {
        uintptr_t hero = Read<uintptr_t>(g_ClientBase + g_Off.dwLocalPlayerHero);
        if (!hero) return false;

        std::map<std::string, int> wants;
        {
            std::lock_guard<std::mutex> lk(g_LoadoutMutex);
            auto it = g_Loadout.find(heroKey);
            if (it == g_Loadout.end()) return false;
            wants = it->second;
        }

        uintptr_t entList = Read<uintptr_t>(g_ClientBase + g_Off.dwEntityList);
        if (!entList) return false;

        // slot order must match m_hWearables layout: head, shoulder, arms, belt, weapon, ...
        const char* order[] = { "head","shoulder","arms","belt","weapon","mount","ambient","ward" };
        for (int i = 0; i < 8; i++) {
            auto w = wants.find(order[i]);
            if (w == wants.end() || w->second < 0) continue;   // -1 = leave default

            uint32_t handle = Read<uint32_t>(hero + g_Off.m_hWearables + i * 4);
            if (handle == 0xFFFFFFFF || handle == 0) continue;
            uintptr_t wearable = HandleToEnt(entList, handle);
            if (!wearable) continue;

            uintptr_t itemView = wearable + g_Off.m_ItemView;
            Write<int32_t>(itemView + g_Off.m_iItemDefIndex, (int32_t)w->second);
            // reset paint to stock so styles don't corrupt the new item
            if (g_Off.m_nFallbackPaint)
                Write<int32_t>(itemView + g_Off.m_nFallbackPaint, 0);
        }

        // force the hero to re-wear everything
        if (g_Off.m_bNeedReapply)
            Write<bool>(hero + g_Off.m_bNeedReapply, true);
        if (g_Off.fnFullUpdate) {
            using Fn_t = void(__fastcall*)(uintptr_t);
            ((Fn_t)(g_ClientBase + g_Off.fnFullUpdate))(hero);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---------- keeper thread: re-apply on spawn (death/respawn/pick wipe wearables) ----------

static void KeeperThread() {
    uintptr_t lastHero = 0;
    while (g_Running) {
        Sleep(1000);
        if (!g_ForcedHero.empty() && g_Off.dwLocalPlayerHero) {
            uintptr_t hero = Read<uintptr_t>(g_ClientBase + g_Off.dwLocalPlayerHero);
            if (hero && hero != lastHero) {   // new spawn or new pick
                Sleep(1500);                  // let entities settle
                PushHero(g_ForcedHero);
                lastHero = hero;
            }
        }
    }
}

// ---------- TCP link: wardrobe.exe sends loadout -> DLL applies live ----------
// localhost only. No named pipes, no window messages, nothing enumerable.

static void LinkThread() {
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(3999);
    int o = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&o, sizeof(o));
    bind(srv, (sockaddr*)&a, sizeof(a));
    listen(srv, 2);
    char buf[65536];
    while (g_Running) {
        SOCKET c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::string data;
        int n;
        while ((n = recv(c, buf, sizeof(buf), 0)) > 0) data.append(buf, n);
        closesocket(c);
        if (data.empty()) continue;
        // format: HERO:npc_dota_hero_x\nslot=def\nslot=def\n...
        auto nl = data.find('\n');
        std::string hero = (data.compare(0, 5, "HERO:") == 0) ? data.substr(5, nl - 5) : "";
        if (hero.empty()) continue;
        if (hero.back() == '\r') hero.pop_back();
        { std::lock_guard<std::mutex> lk(g_LoadoutMutex);
          std::istringstream ss(data.substr(nl + 1));
          std::string line;
          while (std::getline(ss, line)) {
              if (!line.empty() && line.back() == '\r') line.pop_back();
              auto eq = line.find('=');
              if (eq == std::string::npos) continue;
              try {
                  g_Loadout[hero][line.substr(0, eq)] = std::stoi(line.substr(eq + 1));
              } catch (...) {}
          } }
        g_ForcedHero = hero;
        PushHero(hero);   // instant — no waiting for the keeper tick
    }
}

// ---------- Present hook: menu inside the game + status ----------

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn oPresent = nullptr;
static HWND g_hWnd = nullptr;
static WNDPROC oWndProc = nullptr;
static ID3D11Device* g_Dev = nullptr;
static ID3D11DeviceContext* g_Ctx = nullptr;
static ID3D11RenderTargetView* g_RTV = nullptr;
static bool g_Init = false, g_Show = false;
static std::string g_Status = "idle";

static LRESULT CALLBACK DllWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (m == WM_KEYDOWN && w == VK_INSERT) { g_Show = !g_Show; return 0; }
    if (g_Show && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    return CallWindowProc(oWndProc, h, m, w, l);
}

static void RenderInGame() {
    ImGui::Begin("wardrobe — live", &g_Show, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("status: %s", g_Status.c_str());
    ImGui::Text("hero: %s", g_ForcedHero.empty() ? "(none pushed yet)" : g_ForcedHero.c_str());
    if (!g_Off.dwLocalPlayerHero)
        ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "offsets missing — run dump_offsets.py first");
    else
        ImGui::TextColored(ImVec4(0.4f, 1, 0.4f, 1), "offsets loaded — swap is live");
    if (ImGui::Button("re-push current hero") && !g_ForcedHero.empty()) {
        g_Status = PushHero(g_ForcedHero) ? "pushed ok" : "push failed";
    }
    ImGui::TextDisabled("INSERT toggles • loadout comes from wardrobe.exe");
    ImGui::End();
}

static HRESULT __stdcall hkPresent(IDXGISwapChain* ch, UINT s, UINT f) {
    if (!g_Init) {
        if (SUCCEEDED(ch->GetDevice(__uuidof(ID3D11Device), (void**)&g_Dev)) && g_Dev) {
            g_Dev->GetImmediateContext(&g_Ctx);
            DXGI_SWAP_CHAIN_DESC d{}; ch->GetDesc(&d);
            g_hWnd = d.OutputWindow;
            ID3D11Texture2D* b = nullptr;
            if (SUCCEEDED(ch->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&b)) && b) {
                g_Dev->CreateRenderTargetView(b, nullptr, &g_RTV);
                b->Release();
            }
            ImGui::CreateContext();
            ImGui_ImplWin32_Init(g_hWnd);
            ImGui_ImplDX11_Init(g_Dev, g_Ctx);
            ImGui::StyleColorsDark();
            oWndProc = (WNDPROC)SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, (LONG_PTR)DllWndProc);
            g_Init = true;
        }
    }
    if (g_Show && g_RTV && g_Ctx) {
        ImGui::GetIO().MouseDrawCursor = true;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderInGame();
        ImGui::Render();

        ID3D11RenderTargetView* prevRTV = nullptr;
        ID3D11DepthStencilView* prevDSV = nullptr;
        g_Ctx->OMGetRenderTargets(1, &prevRTV, &prevDSV);

        g_Ctx->OMSetRenderTargets(1, &g_RTV, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_Ctx->OMSetRenderTargets(1, &prevRTV, prevDSV);
        if (prevRTV) prevRTV->Release();
        if (prevDSV) prevDSV->Release();
    } else if (g_Init) {
        ImGui::GetIO().MouseDrawCursor = false;
    }
    return oPresent(ch, s, f);
}

// ---------- boot ----------

static bool LoadOffsets() {
    // loader drops offsets.bin next to the game or passes path via registry-free env
    const char* paths[] = { "offsets.bin", "build/offsets.bin", "C:\\Temp\\opencode\\offsets.bin" };
    for (auto p : paths) {
        FILE* f = nullptr;
        fopen_s(&f, p, "rb");
        if (!f) continue;
        GameOffsets o{};
        bool ok = fread(&o, 1, sizeof(o), f) == sizeof(o) && o.magic == 0x57415244;
        fclose(f);
        if (ok) { g_Off = o; return true; }
    }
    return false;
}

// Automatic in-memory discovery of the GC Cache dispatch function in client.dll.
// Searches for string "SOCacheSubscribed", resolves RIP-relative LEA references,
// and locates the handler callback or function entry point.
static uintptr_t FindGCHookAuto(uintptr_t base) {
    if (!base) return 0;
    __try {
        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

        PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t textStart = 0; size_t textSize = 0;
        uintptr_t rdataStart = 0; size_t rdataSize = 0;

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
            char sname[9] = {};
            memcpy(sname, sec->Name, 8);
            if (!strcmp(sname, ".text")) {
                textStart = base + sec->VirtualAddress;
                textSize = sec->Misc.VirtualSize;
            } else if (!strcmp(sname, ".rdata")) {
                rdataStart = base + sec->VirtualAddress;
                rdataSize = sec->Misc.VirtualSize;
            }
        }

        if (!rdataStart) { rdataStart = base; rdataSize = nt->OptionalHeader.SizeOfImage; }
        if (!textStart) { textStart = base; textSize = nt->OptionalHeader.SizeOfImage; }

        // 1. Locate string "SOCacheSubscribed" in .rdata
        const char target[] = "SOCacheSubscribed";
        size_t tlen = sizeof(target) - 1;
        uintptr_t strAddr = 0;
        for (uintptr_t p = rdataStart; p <= rdataStart + rdataSize - tlen; p++) {
            if (memcmp((const void*)p, target, tlen) == 0) {
                strAddr = p;
                break;
            }
        }
        if (!strAddr) return 0;

        // 2. Scan .text for RIP-relative LEA instructions referencing strAddr
        uintptr_t xrefAddr = 0;
        for (uintptr_t p = textStart; p <= textStart + textSize - 7; p++) {
            uint8_t b0 = *(uint8_t*)p;
            uint8_t b1 = *(uint8_t*)(p + 1);
            if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x8D) {
                uint8_t modrm = *(uint8_t*)(p + 2);
                if ((modrm & 0xC7) == 0x05) {
                    int32_t disp = *(int32_t*)(p + 3);
                    if (p + 7 + disp == strAddr) {
                        xrefAddr = p;
                        break;
                    }
                }
            }
        }
        if (!xrefAddr) return 0;

        // 2b. Check if nearby instructions (within 40 bytes) have another LEA pointing to .text
        uintptr_t scanStart = (xrefAddr >= 40) ? (xrefAddr - 40) : textStart;
        for (uintptr_t p = scanStart; p <= xrefAddr + 40 && p <= textStart + textSize - 7; p++) {
            if (p == xrefAddr) continue;
            uint8_t b0 = *(uint8_t*)p;
            uint8_t b1 = *(uint8_t*)(p + 1);
            if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x8D) {
                uint8_t modrm = *(uint8_t*)(p + 2);
                if ((modrm & 0xC7) == 0x05) {
                    int32_t disp = *(int32_t*)(p + 3);
                    uintptr_t tgt = p + 7 + disp;
                    if (tgt >= textStart && tgt < textStart + textSize) {
                        return tgt - base;
                    }
                }
            }
        }

        // 3. Fallback: walk backward from xrefAddr to find function prologue
        for (uintptr_t p = xrefAddr; p > xrefAddr - 0x1000 && p > textStart; p--) {
            uint8_t prev = *(uint8_t*)(p - 1);
            if (prev == 0xCC || prev == 0xC3 || prev == 0x90) {
                uint8_t c0 = *(uint8_t*)p;
                uint8_t c1 = *(uint8_t*)(p + 1);
                uint8_t c2 = *(uint8_t*)(p + 2);
                if ((c0 == 0x48 && c1 == 0x89 && c2 == 0x5C) ||
                    (c0 == 0x48 && c1 == 0x83 && c2 == 0xEC) ||
                    (c0 == 0x48 && c1 == 0x81 && c2 == 0xEC) ||
                    (c0 == 0x40 && (c1 == 0x53 || c1 == 0x55 || c1 == 0x57)) ||
                    (c0 == 0x55 && c1 == 0x48 && (c2 == 0x89 || c2 == 0x8B))) {
                    return p - base;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

DWORD WINAPI MainThread(LPVOID mod) {
    g_ClientBase = (uintptr_t)GetModuleHandleA("client.dll");
    int waits = 0;
    while (!g_ClientBase && waits++ < 120) { Sleep(500); g_ClientBase = (uintptr_t)GetModuleHandleA("client.dll"); }

    LoadOffsets();   // missing = menu shows red warning, PushHero refuses. Safe by default.

    // inventory unlock: fake GC records from data/skins_full.json so Dota's OWN
    // loadout UI lists every skin. Needs data/skins_full.json next to the DLL
    // (loader copies it) — empty DB = blob skipped, writer still works.
    LoadDB("data/skins_full.json");
    BuildInjectBlob();

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
        MH_Initialize();
        MH_CreateHook(present, &hkPresent, (void**)&oPresent);
        MH_EnableHook(present);
    }
    if (dummyHwnd) DestroyWindow(dummyHwnd);
    UnregisterClassA("WardrobeDummy", wc.hInstance);

    // GC inventory hook: append fake SO records to every Welcome/Cache message.
    // gc_hook.txt holds the RVA (from dump_offsets.py). If missing, FindGCHookAuto scans client.dll.
    {
        uintptr_t rva = 0;
        FILE* gf = nullptr;
        const char* gc_paths[] = { "gc_hook.txt", "build/gc_hook.txt", "C:\\Temp\\opencode\\gc_hook.txt" };
        for (auto gp : gc_paths) {
            fopen_s(&gf, gp, "rb");
            if (gf) break;
        }
        if (gf) {
            char line[64] = {};
            fread(line, 1, sizeof(line) - 1, gf);
            fclose(gf);
            rva = (uintptr_t)strtoull(line, nullptr, 16);
        }
        if (!rva && g_ClientBase) {
            rva = FindGCHookAuto(g_ClientBase);
        }
        if (rva && g_ClientBase) {
            void* target = (void*)(g_ClientBase + rva);
            MH_CreateHook(target, &hkOnCache, (void**)GetOnCacheOrigSlot());
            MH_EnableHook(target);
        }
    }

    // erase PE headers if mod pointer is valid
    if (mod) {
        DWORD old;
        VirtualProtect(mod, 0x1000, PAGE_READWRITE, &old);
        memset(mod, 0, 0x1000);
        VirtualProtect(mod, 0x1000, old, &old);
    }

    std::thread(KeeperThread).detach();
    std::thread(LinkThread).detach();
    {
        char st[128];
        sprintf_s(st, "live | inv unlock: %llu items",
            (unsigned long long)InjectItemCount());
        g_Status = g_Off.dwLocalPlayerHero ? st : "no offsets (inv only)";
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, MainThread, h, 0, nullptr);
    }
    return TRUE;
}

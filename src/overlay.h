// src/overlay.h
#pragma once
#include <Windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "db.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

extern ID3D11Device* g_Dev;
extern ID3D11DeviceContext* g_Ctx;
extern IDXGISwapChain* g_Swap;
extern ID3D11RenderTargetView* g_RTV;
extern HWND g_Overlay;
extern bool g_MenuOpen;

void MakeOverlay(HINSTANCE inst);
bool InitDX();
void CleanupDX();
void DrawWardrobe();   // defined in browser.cpp

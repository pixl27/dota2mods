@echo off
REM wardrobe/build.bat — double-click this. DLL + EXE + loader, one shot.
setlocal enabledelayedexpansion

where cl >nul 2>nul
if errorlevel 1 (
    echo [*] cl.exe not in PATH. Searching for Visual Studio vcvars64.bat...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
            if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
                echo [*] Found Visual Studio: %%i
                call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul
            )
        )
    )
)

where cl >nul 2>nul
if errorlevel 1 (
    for %%p in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) do (
        if exist %%p (
            echo [*] Found Visual Studio at %%p
            call %%p >nul
            goto :found_vs
        )
    )
)
:found_vs

where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo [!] MSVC compiler (cl.exe) not found.
    echo     Please install Visual Studio with "Desktop development with C++", or
    echo     run this script from "x64 Native Tools Command Prompt for VS".
    echo.
    pause
    exit /b 1
)

if not exist build mkdir build
if not exist data mkdir data

set IMGUI=thirdparty\imgui\imgui.cpp thirdparty\imgui\imgui_draw.cpp thirdparty\imgui\imgui_tables.cpp thirdparty\imgui\imgui_widgets.cpp thirdparty\imgui\backends\imgui_impl_win32.cpp thirdparty\imgui\backends\imgui_impl_dx11.cpp
set INCLUDES=/Ithirdparty /Ithirdparty\imgui /Ithirdparty\imgui\backends /Ithirdparty\minhook\include /Isrc
set LIBS=d3d11.lib dxgi.lib dwmapi.lib ws2_32.lib psapi.lib

echo [*] Building wardrobe.exe (overlay, undetectable layer)...
cl /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE %INCLUDES% src\main.cpp src\browser.cpp %IMGUI% /link %LIBS% /OUT:build\wardrobe.exe user32.lib gdi32.lib
if errorlevel 1 ( echo [!] EXE build failed & pause & exit /b 1 )

echo [*] Building wardrobe_dll.dll (live-swap payload, manual-map only)...
cl /nologo /O2 /EHsc /std:c++17 /LD /DUNICODE /D_UNICODE %INCLUDES% src\dllmain.cpp src\inventory.cpp %IMGUI% thirdparty\minhook\src\buffer.c thirdparty\minhook\src\hook.c thirdparty\minhook\src\trampoline.c thirdparty\minhook\src\hde\hde64.c /link %LIBS% /OUT:build\wardrobe_dll.dll user32.lib gdi32.lib
if errorlevel 1 ( echo [!] DLL build failed ^(EXE is still fine — DLL is optional^) & pause & exit /b 1 )

echo [*] Building map.exe (manual-map loader)...
cl /nologo /O2 /EHsc /std:c++17 src\loader.cpp /link /OUT:build\map.exe
if errorlevel 1 ( echo [!] Loader build failed & pause & exit /b 1 )

echo.
echo [*] Copying cfg + data helpers...
copy /Y cfg\gamestate_integration_wardrobe.cfg build\ >nul
copy /Y gen_full_db.py build\ >nul
copy /Y gen_names.py build\ >nul
copy /Y dump_offsets.py build\ >nul

echo.
echo [OK] Done. Folder build\ now holds:
echo      wardrobe.exe      - the overlay. Run this. No admin, no injection.
echo      wardrobe_dll.dll  - live-swap payload. map.exe ONLY, never double-click.
echo      map.exe           - manual-map loader. Usage: map.exe wardrobe_dll.dll
echo      gamestate_integration_wardrobe.cfg - copy into Dota cfg folder (see README).
echo.
pause

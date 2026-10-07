@echo off
setlocal
cd /d "%~dp0"
set "WARDROBE_NO_PAUSE="
if /I "%~1"=="--no-pause" set "WARDROBE_NO_PAUSE=1"
REM wardrobe/build.bat — double-click this. DLL + EXE + loader, one shot.
set "VCVARS="
where cl >nul 2>nul
if not errorlevel 1 goto :have_cl

echo [*] cl.exe not in PATH. Searching for Visual Studio vcvars64.bat...
set "PF86=%ProgramFiles(x86)%"
set "PF=%ProgramFiles%"

if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" call "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath > "%TEMP%\vs_install.txt"
if exist "%TEMP%\vs_install.txt" set /p VS_PATH=<"%TEMP%\vs_install.txt"
if exist "%TEMP%\vs_install.txt" del "%TEMP%\vs_install.txt" >nul 2>nul
if defined VS_PATH if exist "%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS if exist "%PF%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%PF86%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%PF86%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if defined VCVARS goto :run_vcvars
goto :have_cl

:run_vcvars
call "%VCVARS%" >nul

:have_cl
where cl >nul 2>nul
if errorlevel 1 goto :no_cl
goto :has_cl

:no_cl
echo.
echo [!] MSVC compiler cl.exe not found.
echo     Please install Visual Studio with "Desktop development with C++", or
echo     run this script from "x64 Native Tools Command Prompt for VS".
echo.
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:has_cl
if not exist build mkdir build
if not exist data mkdir data
REM Object files go under build\obj; the exe and dll compile ImGui with different flags.
for %%D in (app dll map tools setup) do if not exist build\obj\%%D mkdir build\obj\%%D
REM asInvoker: never ask for administrator rights. Without it Windows elevates
REM any executable whose name contains "Setup" on its own.
set MANIFEST=/MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'"

set IMGUI=thirdparty\imgui\imgui.cpp thirdparty\imgui\imgui_draw.cpp thirdparty\imgui\imgui_tables.cpp thirdparty\imgui\imgui_widgets.cpp thirdparty\imgui\backends\imgui_impl_win32.cpp thirdparty\imgui\backends\imgui_impl_dx11.cpp
set INCLUDES=/Ithirdparty /Ithirdparty\imgui /Ithirdparty\imgui\backends /Ithirdparty\minhook\include /Isrc
set LIBS=d3d11.lib dxgi.lib dwmapi.lib ws2_32.lib psapi.lib

echo [*] Building Wardrobe.exe (the application)...
REM A running exe cannot be overwritten but can be renamed: this lets the window
REM rebuild itself ("Mettre a jour Wardrobe"). The old copy goes once it is closed.
if exist build\Wardrobe.old.exe del /q build\Wardrobe.old.exe >nul 2>nul
if exist build\Wardrobe.exe move /y build\Wardrobe.exe build\Wardrobe.old.exe >nul 2>nul
rc /nologo /fo build\obj\app\wardrobe.res src\app\wardrobe.rc
if errorlevel 1 goto :exe_fail
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /Fobuild\obj\app\ /DUNICODE /D_UNICODE /Ithirdparty /Ithirdparty\imgui /Ithirdparty\imgui\backends /Isrc src\app\app.cpp %IMGUI% build\obj\app\wardrobe.res /link /SUBSYSTEM:WINDOWS /OUT:build\Wardrobe.exe %MANIFEST% d3d11.lib dxgi.lib dwmapi.lib shell32.lib advapi32.lib user32.lib gdi32.lib
if errorlevel 1 goto :exe_fail

echo [*] Building wardrobe_dll.dll (live-swap payload, manual-map only)...
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /LD /Fobuild\obj\dll\ /DUNICODE /D_UNICODE %INCLUDES% src\dllmain.cpp src\inventory.cpp src\native_appearance.cpp %IMGUI% thirdparty\minhook\src\buffer.c thirdparty\minhook\src\hook.c thirdparty\minhook\src\trampoline.c thirdparty\minhook\src\hde\hde64.c /link %LIBS% /OUT:build\wardrobe_dll.dll user32.lib gdi32.lib
if errorlevel 1 goto :dll_fail

echo [*] Building map.exe (manual-map loader)...
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /Fobuild\obj\map\ src\loader.cpp /link /OUT:build\map.exe
if errorlevel 1 goto :map_fail

echo [*] Building wardrobe_tools.exe (compatibility check and inventory restore, no Python needed)...
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Fobuild\obj\tools\ /Isrc src\tools.cpp /link /OUT:build\wardrobe_tools.exe
if errorlevel 1 goto :tools_fail

echo [*] Building setup_stub.exe (the installer, before package.py appends the files to it)...
rc /nologo /fo build\obj\setup\setup.res src\setup\setup.rc
if errorlevel 1 goto :setup_fail
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /Fobuild\obj\setup\ /DUNICODE /D_UNICODE /Ithirdparty /Ithirdparty\imgui /Ithirdparty\imgui\backends /Isrc src\setup\setup.cpp %IMGUI% build\obj\setup\setup.res /link /SUBSYSTEM:WINDOWS /OUT:build\setup_stub.exe %MANIFEST% d3d11.lib dxgi.lib dwmapi.lib shell32.lib ole32.lib advapi32.lib user32.lib gdi32.lib cabinet.lib
if errorlevel 1 goto :setup_fail
goto :build_ok

:exe_fail
echo [!] Application build failed
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:dll_fail
echo [!] DLL build failed
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:map_fail
echo [!] Loader build failed
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:tools_fail
echo [!] Tools build failed
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:setup_fail
echo [!] Installer build failed
if not defined WARDROBE_NO_PAUSE pause
exit /b 1

:build_ok

echo.
echo [*] Copying cfg + data helpers...
if not exist build\data mkdir build\data
if exist data\skins_full.json copy /Y data\skins_full.json build\data\ >nul
if exist data\native_profile.json copy /Y data\native_profile.json build\data\ >nul
for %%F in (cfg\gamestate_integration_wardrobe.cfg gen_full_db.py gen_names.py update_db.py dota_vpk.py repair_inventory_cache.py verify_profile.py refresh_profile.py launch_level3.bat) do (
    if exist "%%F" (copy /Y "%%F" build\ >nul) else echo [!] missing %%F
)

echo.
echo [OK] Done. Folder build\ now holds:
echo      Wardrobe.exe      - the application. Run this; everything is in it.
echo      wardrobe_dll.dll  - in-game payload, loaded by the application.
echo      map.exe           - the loader the application calls.
echo      wardrobe_tools.exe - compatibility check and inventory restore, used by the application.
echo      setup_stub.exe    - the installer; python package.py turns it into Wardrobe-Setup.exe.
echo      launch_level3.bat - GC receiver with automatic inventory refresh.
echo      refresh_profile.py - re-locates the appearance profile in a new client.dll after a Dota update.
echo      update_db.py      - rebuilds data\skins_full.json straight from Dota's VPK after an update.
echo      repair_inventory_cache.py - inspect/restore Dota's saved inventory cache.
echo      gamestate_integration_wardrobe.cfg - copy into Dota cfg folder (see README).
echo.
if not defined WARDROBE_NO_PAUSE pause

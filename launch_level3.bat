@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"
title Wardrobe - Level 3 1-Click Launcher
cls

echo ===================================================================
echo             Wardrobe Level 3 (GC Inventory Unlock)
echo ===================================================================
echo.

REM 1. Verify build artifacts exist
if not exist "build\map.exe" if not exist "map.exe" (
    echo [!] map.exe not found!
    echo [*] Running build.bat to compile binaries...
    call build.bat --no-pause
    if errorlevel 1 (
        echo [!] Compilation failed. Please check build errors above.
        pause
        exit /b 1
    )
)

set "MAP_EXE=build\map.exe"
if not exist "!MAP_EXE!" set "MAP_EXE=map.exe"

set "DLL_PATH=build\wardrobe_dll.dll"
if not exist "!DLL_PATH!" set "DLL_PATH=wardrobe_dll.dll"
if not exist "!DLL_PATH!" (
    echo [!] wardrobe_dll.dll is missing. Run build.bat from the project folder.
    pause
    exit /b 1
)
if not exist "data\skins_full.json" if not exist "build\data\skins_full.json" if not exist "skins_full.json" (
    echo [!] Skin database is missing. Run gen_full_db.py, then build.bat.
    pause
    exit /b 1
)

REM 2. Check if Dota 2 is running
echo [*] Checking if Dota 2 is running...
tasklist /FI "IMAGENAME eq dota2.exe" 2>NUL | find /I /N "dota2.exe">NUL
if errorlevel 1 (
    echo.
    echo [!] Dota 2 is NOT currently running.
    echo [*] Please launch Dota 2 and wait until you reach the main menu.
    echo.
    pause
    cls
    echo [*] Re-checking for dota2.exe...
    tasklist /FI "IMAGENAME eq dota2.exe" 2>NUL | find /I /N "dota2.exe">NUL
    if errorlevel 1 (
        echo [!] Dota 2 still not detected. Aborting.
        pause
        exit /b 1
    )
)

REM 3. The Steam GC receiver uses the public interface; no client.dll RVA is needed.
echo [*] Using Steam GC receiver v6 with econ-cache isolation and equip notifications.
REM 3b. Warn early when Dota's client.dll no longer matches the verified appearance profile.
set "VERIFY=verify_profile.py"
if not exist "!VERIFY!" set "VERIFY=build\verify_profile.py"
where python >nul 2>nul
if not errorlevel 1 if exist "!VERIFY!" python "!VERIFY!"

REM 4. Map the DLL into dota2.exe
echo [*] Injecting wardrobe_dll.dll into dota2.exe...
"!MAP_EXE!" "!DLL_PATH!"
if !errorlevel! equ 2 (
    echo [!] Wardrobe is already loaded. Use INSERT or restart Dota to change builds.
    pause
    exit /b 2
)
if errorlevel 1 (
    echo.
    echo [!] Injection failed. If prompted, please run this script as Administrator.
    pause
    exit /b 1
)

echo.
echo ===================================================================
echo [OK] DLL loaded. Check the in-game receiver status for the inventory result.
echo.
echo  1. Switch to Dota 2.
echo  2. Press INSERT to open the status overlay.
echo  3. Inventory refresh runs automatically, with a 20-second timeout.
echo  4. If it fails, use Retry inventory refresh and read the displayed reason.
echo  5. Equip from Dota's native loadout screen. Keep the receiver active.
echo     INSERT shows accepted/completed equips, pending replies, and latency.
echo     Resume inventory receiver does not reload an already delivered catalog.
echo  Log: C:\Temp\opencode\wardrobe_gc.log
echo ===================================================================
echo.
pause

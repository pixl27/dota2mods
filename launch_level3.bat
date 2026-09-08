@echo off
setlocal enabledelayedexpansion
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
    call build.bat
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

REM 2. Check if Dota 2 is running
echo [*] Checking if Dota 2 is running...
tasklist /FI "IMAGENAME eq dota2.exe" 2>NUL | find /I /N "dota2.exe">NUL
if "%ERRORLEVEL%"=="1" (
    echo.
    echo [!] Dota 2 is NOT currently running.
    echo [*] Please launch Dota 2 and wait until you reach the main menu.
    echo.
    pause
    cls
    echo [*] Re-checking for dota2.exe...
    tasklist /FI "IMAGENAME eq dota2.exe" 2>NUL | find /I /N "dota2.exe">NUL
    if "%ERRORLEVEL%"=="1" (
        echo [!] Dota 2 still not detected. Aborting.
        pause
        exit /b 1
    )
)

REM 3. Auto-find GC hook RVA if gc_hook.txt is missing
if not exist "build\gc_hook.txt" if not exist "gc_hook.txt" if not exist "C:\Temp\opencode\gc_hook.txt" (
    echo.
    echo [*] gc_hook.txt not found. Running automated RVA detector...
    python auto_find_gc.py
    echo.
)

REM 4. Map the DLL into dota2.exe
echo [*] Injecting wardrobe_dll.dll into dota2.exe...
"!MAP_EXE!" "!DLL_PATH!"
if errorlevel 1 (
    echo.
    echo [!] Injection failed. If prompted, please run this script as Administrator.
    pause
    exit /b 1
)

echo.
echo ===================================================================
echo [SUCCESS] Wardrobe Level 3 is active!
echo.
echo  1. Switch to Dota 2.
echo  2. Press INSERT to open the status overlay.
echo  3. Navigate to Heroes - Armory to equip any cosmetic item!
echo ===================================================================
echo.
pause

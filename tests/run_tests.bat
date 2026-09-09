@echo off
setlocal
cd /d "%~dp0.."
where cl >nul 2>nul
if not errorlevel 1 goto :compile
set "GC_VS_PATH="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "GC_VS_PATH=%%I"
if not defined GC_VS_PATH exit /b 1
call "%GC_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
:compile
if not exist build mkdir build
cl /nologo /EHsc /W4 /std:c++17 /utf-8 /Isrc tests\gc_protocol_test.cpp /Fo:build\gc_protocol_test.obj /Fe:build\gc_protocol_test.exe
if errorlevel 1 exit /b 1
build\gc_protocol_test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W4 /std:c++17 /utf-8 /Isrc tests\gc_loadout_test.cpp /Fo:build\gc_loadout_test.obj /Fe:build\gc_loadout_test.exe
if errorlevel 1 exit /b 1
build\gc_loadout_test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W3 /std:c++17 /utf-8 /Isrc /Ithirdparty tests\gc_receiver_test.cpp /Fo:build\gc_receiver_test.obj /Fe:build\gc_receiver_test.exe
if errorlevel 1 exit /b 1
build\gc_receiver_test.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W4 /std:c++17 /utf-8 /Isrc tests\native_appearance_test.cpp /Fo:build\native_appearance_test.obj /Fe:build\native_appearance_test.exe
if errorlevel 1 exit /b 1
build\native_appearance_test.exe
if errorlevel 1 exit /b 1
python tests\test_inventory_cache_repair.py
exit /b %ERRORLEVEL%

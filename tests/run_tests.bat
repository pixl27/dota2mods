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
cl /nologo /EHsc /W4 /std:c++17 /utf-8 /Isrc tests\resolver_against_client.cpp /Fo:build\resolver_against_client.obj /Fe:build\resolver_against_client.exe advapi32.lib
if errorlevel 1 exit /b 1
REM Checks the recorded profile against the installed Dota: first as recorded, then
REM with every recorded address blanked so each entry goes through its search path.
build\resolver_against_client.exe
if errorlevel 1 exit /b 1
build\resolver_against_client.exe -blank-hints
if errorlevel 1 exit /b 1
python tests\test_inventory_cache_repair.py
if errorlevel 1 exit /b 1
if not exist build\obj\tools mkdir build\obj\tools
cl /nologo /O2 /EHsc /W4 /std:c++17 /utf-8 /DUNICODE /D_UNICODE /Fobuild\obj\tools\ /Isrc src\tools.cpp /link /OUT:build\wardrobe_tools.exe
if errorlevel 1 exit /b 1
REM The verdict the application shows on its home screen, for the installed Dota.
build\wardrobe_tools.exe compat
if errorlevel 1 exit /b 1
python tests\test_wardrobe_tools.py
if errorlevel 1 exit /b 1
REM The catalog built without Python must stay identical to update_db.py's.
python tests\test_catalog_builder.py
if errorlevel 1 exit /b 1
REM The one-click installer, in scratch folders (skipped until package.py has run).
python tests\test_installer.py
exit /b %ERRORLEVEL%

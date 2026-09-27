@echo off
setlocal
rem Builds build\WallpaperPlus.exe (the always-running player) and build\WallpaperPlusSettings.exe
rem (the settings window) with the MSVC command-line tools, and copies the settings UI files.

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VSCMD_VER call "%VCVARS%" >nul || exit /b 1

cd /d "%~dp0"
if not exist build\obj\player mkdir build\obj\player
if not exist build\obj\settings mkdir build\obj\settings

set CFLAGS=/nologo /std:c++20 /O2 /GL /MT /EHsc /W4 /permissive- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Isrc\common
set LFLAGS=/LTCG /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF

rc /nologo /fo build\obj\app.res res\app.rc || exit /b 1

echo === player ===
cl %CFLAGS% src\player\*.cpp src\common\*.cpp /Fobuild\obj\player\ /Fe:build\WallpaperPlus.exe build\obj\app.res ^
   /link %LFLAGS% ^
   d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib propsys.lib dwmapi.lib wtsapi32.lib ^
   user32.lib shell32.lib gdi32.lib ole32.lib advapi32.lib || exit /b 1

if not exist src\settings\main.cpp goto :done
echo === settings ===
cl %CFLAGS% /Ithird_party /Ithird_party\webview2\build\native\include /bigobj ^
   src\settings\*.cpp src\common\*.cpp /Fobuild\obj\settings\ /Fe:build\WallpaperPlusSettings.exe build\obj\app.res ^
   /link %LFLAGS% third_party\webview2\build\native\x64\WebView2LoaderStatic.lib ^
   user32.lib shell32.lib gdi32.lib ole32.lib oleaut32.lib advapi32.lib dwmapi.lib shlwapi.lib crypt32.lib version.lib ^
   || exit /b 1
if exist build\ui rmdir /s /q build\ui
xcopy /e /i /q ui build\ui >nul || exit /b 1

:done
echo Build OK

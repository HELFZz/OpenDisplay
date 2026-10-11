@echo off
rem Builds OpenDisplay Control Panel (x64 Release) with MSVC.
setlocal
set MSVC=D:\VS2022\VC\Auxiliary\Build\vcvars64.bat
if not exist "%MSVC%" (
  echo vcvars64.bat not found at %MSVC%
  exit /b 1
)

call "%MSVC%" >nul
if errorlevel 1 exit /b 1

cd /d "%~dp0"
if not exist bin mkdir bin

rc /nologo /fo bin\control.res control.rc
if errorlevel 1 (
  echo RC FAILED
  exit /b 1
)

cl /nologo /std:c++17 /O2 /W3 /EHsc ^
  /I..\host ^
  /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
  main.cpp ..\host\virtual_display.cpp bin\control.res ^
  /Fe:bin\opendisplay-control.exe /Fo:bin\ ^
  /link d3d11.lib dxgi.lib setupapi.lib advapi32.lib shell32.lib user32.lib ^
  /SUBSYSTEM:WINDOWS

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo OK: bin\opendisplay-control.exe
exit /b 0

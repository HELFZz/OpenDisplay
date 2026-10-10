@echo off
rem Builds opendisplay-host.exe (x64 Release) with MSVC.
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

cl /nologo /std:c++17 /O2 /W3 /EHsc ^
  /I..\proto /I..\third_party\nv-codec-headers\include ^
  /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
  main.cpp server.cpp dda_capture.cpp nvenc_encoder.cpp input.cpp virtual_display.cpp ^
  /Fe:bin\opendisplay-host.exe /Fo:bin\ ^
  /link d3d11.lib dxgi.lib ws2_32.lib user32.lib ole32.lib setupapi.lib advapi32.lib gdi32.lib

if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo OK: bin\opendisplay-host.exe
exit /b 0

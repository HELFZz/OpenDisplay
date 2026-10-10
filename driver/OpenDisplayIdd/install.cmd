@echo off
rem Installs the OpenDisplayIdd test-signed driver package.
rem MUST be run from an elevated (Administrator) prompt.
rem Requires test signing: bcdedit /set testsigning on  + reboot.
setlocal
set PKG=%~dp0x64\Release\OpenDisplayIdd
set DEVGEN=D:\dev\OpenDisplay\driver\packages\Microsoft.Windows.WDK.x64.10.0.28000.2526\c\tools\10.0.28000.0\x64\devgen.exe

if not exist "%PKG%\opendisplayidd.cat" (
  echo Build the driver first: driver\OpenDisplayIdd\build.cmd
  exit /b 1
)

echo Importing driver package into the driver store...
pnputil /add-driver "%PKG%\OpenDisplayIdd.inf" /install
if errorlevel 1 exit /b %ERRORLEVEL%

if exist "%DEVGEN%" (
  echo Creating root-enumerated device...
  "%DEVGEN%" /add /bus Root /hardwareid Root\OpenDisplayIdd
) else (
  echo devgen.exe not found, creating device via devcon fallback...
  devcon.exe install "%PKG%\OpenDisplayIdd.inf" Root\OpenDisplayIdd
)

echo Done. Check Device Manager -^> Display adapters for "OpenDisplay Virtual Display".
exit /b 0

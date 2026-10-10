@echo off
rem Removes the OpenDisplayIdd device and driver package.
rem Run from an elevated (Administrator) prompt.
setlocal
set DEVCON=D:\dev\OpenDisplay\driver\packages\Microsoft.Windows.WDK.x64.10.0.28000.2526\c\tools\10.0.28000.0\x64\devcon.exe

"%DEVCON%" remove Root\OpenDisplayIdd
if errorlevel 1 devcon.exe remove Root\OpenDisplayIdd

echo.
echo Published driver packages with "opendisplay" in the name:
pnputil /enum-drivers | findstr /i /c:"opendisplay"
echo.
echo To fully remove the package, run as admin:
echo   pnputil /delete-driver oemXX.inf /uninstall /force
exit /b 0

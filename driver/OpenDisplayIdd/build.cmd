@echo off
rem Builds OpenDisplayIdd (UMDF/IddCx) x64 Release.
rem Uses the custom WindowsUserModeDriver10.0 toolset (WDK VSIX was not installed).
setlocal
set MSBUILD=D:\VS2022\MSBuild\Current\Bin\amd64\MSBuild.exe
set VCT=D:\dev\OpenDisplay\tools\msbuild-vc\v170\

"%MSBUILD%" "%~dp0OpenDisplayIdd.vcxproj" -nologo -m -nodeReuse:false -v:minimal ^
  -p:Configuration=Release -p:Platform=x64 ^
  -p:VCTargetsPath=%VCT% ^
  -p:Driver_SpectreMitigation=false -p:SpectreMitigation=false -p:RunCodeAnalysis=false
exit /b %ERRORLEVEL%

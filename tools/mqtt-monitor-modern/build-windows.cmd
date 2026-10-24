@echo off
setlocal
cd /d "%~dp0"
dotnet publish CakeAdaptMonitor.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=None -o dist\win-x64
if errorlevel 1 (
  echo Build failed. Install the .NET 10 SDK and check the error above.
  pause
  exit /b 1
)
echo Built dist\win-x64\CakeAdaptMonitor.exe
pause

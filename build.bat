@echo off
setlocal
cd /d "%~dp0"
gcc -Os -s -mwindows "-Wl,--stack=65536" src\crosshair.c -o crosshair.exe -lgdi32 -luser32 -lole32 -lwindowscodecs -luuid -lshell32
if errorlevel 1 (
  echo build failed
  exit /b 1
)
for %%A in (crosshair.exe) do echo built crosshair.exe %%~zA bytes

@echo off
rem Update Grabbed by the Ghoulies: double-click. Gets the latest code from
rem GitHub, rebuilds what changed, and starts the game.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-ghoulies.ps1" -Pull -Run
if errorlevel 1 (
    echo.
    echo The update stopped. The message above says where; copy it for Claude.
    pause
)

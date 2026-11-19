@echo off
rem Play Grabbed by the Ghoulies: double-click. Starts the game; the first
rem time (or if it isn't built yet) it builds it first, which takes a while.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-ghoulies.ps1" -Play %*
if errorlevel 1 (
    echo.
    echo The build stopped. The message above says where; copy it for Claude.
    pause
)

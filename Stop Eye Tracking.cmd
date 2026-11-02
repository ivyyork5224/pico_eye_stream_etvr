@echo off
rem Double-click to stop the Pico Neo 3 eye tracking sender.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_eyestream.ps1" -Stop
echo.
pause

@echo off
rem Double-click to install / update the "Pico Neo 3 Eye + ETVR lids" module for VRCFaceTracking (close VRCFT first).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_vrcft_module.ps1"
echo.
pause

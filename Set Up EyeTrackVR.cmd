@echo off
rem Double-click to point EyeTrackVR at the headset's eye cameras and send its eyelids to the VRCFT module (port 9010).
rem Run Start Eye Tracking + Cameras first so the headset's address is known.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_etvr.ps1"
echo.
pause

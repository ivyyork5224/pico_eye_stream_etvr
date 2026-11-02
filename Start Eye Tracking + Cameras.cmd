@echo off
rem Double-click to start the eye tracking sender AND the infrared eye camera streams
rem (for Baballonia / EyeTrackVR).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0start_eyestream.ps1" -Cameras
echo.
pause

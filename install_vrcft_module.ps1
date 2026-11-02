# Installs (or removes) the "Pico Neo 3 Eye + ETVR lids" module for VRCFaceTracking 6 (Steam):
# gaze from the headset's eye tracker, eyelids from the EyeTrackVR app.
# It replaces the plain "Pico Neo 3 Pro Eye" module (both listen on UDP 7777), which is
# moved to CustomLibs_disabled and put back by -Remove.
# Close VRCFaceTracking first.
#   powershell -ExecutionPolicy Bypass -File install_vrcft_module.ps1            install / update
#   powershell -ExecutionPolicy Bypass -File install_vrcft_module.ps1 -Remove    remove

param([switch]$Remove)

$guid = 'e4d1a9c7-2b6f-4f38-8a15-6c0b3e9f7d21'
$plainGuid = 'b7c3e1a2-5f4d-4e8a-9c61-0e3f7a9d2b54'
$libs = Join-Path $env:APPDATA 'VRCFaceTracking\CustomLibs'
$parked = Join-Path $env:APPDATA 'VRCFaceTracking\CustomLibs_disabled'
$dst = Join-Path $libs $guid
$dll = Join-Path $PSScriptRoot 'vrcft\dist\PicoNeo3EyeEtvrModule.dll'

if (Get-Process VRCFaceTracking -ErrorAction SilentlyContinue) {
    Write-Host "Close VRCFaceTracking first, then run this again." -ForegroundColor Yellow
    exit 1
}

if ($Remove) {
    if (Test-Path $dst) { Remove-Item $dst -Recurse -Force; Write-Host "Removed the Pico Neo 3 Eye + ETVR lids module." }
    else { Write-Host "The module isn't installed." }
    if ((Test-Path (Join-Path $parked $plainGuid)) -and -not (Test-Path (Join-Path $libs $plainGuid))) {
        Move-Item (Join-Path $parked $plainGuid) $libs
        Write-Host "Put the plain Pico Neo 3 Pro Eye module back."
    }
    exit 0
}

if (-not (Test-Path $dll)) { Write-Host "Missing $dll" -ForegroundColor Red; exit 1 }

if (Test-Path (Join-Path $libs $plainGuid)) {
    New-Item -ItemType Directory -Force $parked | Out-Null
    Remove-Item (Join-Path $parked $plainGuid) -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item (Join-Path $libs $plainGuid) $parked
    Write-Host "Set the plain Pico Neo 3 Pro Eye module aside ($parked)."
}

New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item $dll $dst -Force
$json = @"
{
  "ModuleId": "$guid",
  "LastUpdated": "$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))",
  "Version": "0.3.0",
  "IsLocal": true,
  "Downloads": 0,
  "Ratings": 0,
  "Rating": 0.0,
  "AuthorName": "local",
  "ModuleName": "Pico Neo 3 Eye + ETVR lids",
  "ModuleDescription": "Gaze from a Pico Neo 3 Pro Eye (eyestream, UDP 7777) with eyelids from the EyeTrackVR app (OSC, UDP 9010).",
  "UsageInstructions": "Start Eye Tracking + Cameras, set ETVR to OSC port 9010 with VRCFT v2 output on and pyVRCFT off, then start VRCFT.",
  "DownloadUrl": "file:///$($PSScriptRoot -replace '\\','/')/INSTALL.md",
  "ModulePageUrl": "file:///$($PSScriptRoot -replace '\\','/')/INSTALL.md",
  "DllFileName": "PicoNeo3EyeEtvrModule.dll",
  "FileHash": null
}
"@
[IO.File]::WriteAllText((Join-Path $dst 'module.json'), $json, (New-Object Text.UTF8Encoding $false))
Write-Host "Installed the Pico Neo 3 Eye + ETVR lids module into $dst" -ForegroundColor Green

# Other eye modules grab VRCFT's eye slot first; warn about the ones we know of
$others = Get-ChildItem $libs -Recurse -Filter *.dll -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'ETVR|Baballonia|Pico4|PicoStreaming' -and $_.Name -ne 'PicoNeo3EyeEtvrModule.dll' }
if ($others) {
    Write-Host "`nOther eye-capable modules are installed; VRCFT gives the eyes to whichever loads first:" -ForegroundColor Yellow
    $others | ForEach-Object { Write-Host "  $($_.Name)" }
    Write-Host "Remove or disable them in VRCFT's Module Registry if the eyes don't move."
}

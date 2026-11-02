# Sets up the EyeTrackVR app for the "Pico Neo 3 Eye + ETVR lids" VRCFT module:
#   camera sources  -> the headset's eye camera streams (http://<headset>:8080/ and :8081/)
#   OSC port 9010   -> where the module listens (instead of VRChat's 9000)
#   VRCFT v2 output on, pyVRCFT off (pyVRCFT only sends one eyelid value for both eyes)
# Closes EyeTrackVR if it's running and keeps a backup of its settings next to them.
#   powershell -ExecutionPolicy Bypass -File setup_etvr.ps1 [-HeadsetIp 192.168.1.50] [-EtvrDir "C:\Program Files (x86)\EyeTrackVR"]
# Changing settings under Program Files may need an Administrator PowerShell.

param([string]$HeadsetIp = "", [string]$EtvrDir = "C:\Program Files (x86)\EyeTrackVR")

$f = Join-Path $EtvrDir 'eyetrack_settings.json'
if (-not (Test-Path $f)) {
    Write-Host "EyeTrackVR settings not found at $f. Start EyeTrackVR once, or pass -EtvrDir." -ForegroundColor Red
    exit 1
}
if (-not $HeadsetIp) {
    $ipFile = Join-Path $PSScriptRoot '.headset_ip'
    if (Test-Path $ipFile) { $HeadsetIp = (Get-Content $ipFile).Trim() }
}
if (-not $HeadsetIp) {
    Write-Host "Headset IP unknown. Run Start Eye Tracking + Cameras first, or pass -HeadsetIp." -ForegroundColor Red
    exit 1
}

if (Get-Process eyetrackapp -ErrorAction SilentlyContinue) {
    Write-Host "Closing EyeTrackVR (it would overwrite the settings when it exits)."
    Get-Process eyetrackapp | Stop-Process -Force
    Start-Sleep 2
}

$backup = "$f.before_pico_$(Get-Date -Format yyyyMMdd_HHmmss)"
Copy-Item $f $backup

# edit as text so everything else in the file stays exactly as ETVR wrote it
$s = [IO.File]::ReadAllText($f)
function Set-Value([string]$key, [string]$json) {
    $pattern = '("' + [regex]::Escape($key) + '"\s*:\s*)("[^"]*"|true|false|-?[\d.]+)'
    if ($script:s -notmatch $pattern) { Write-Host "  setting $key not found, skipped" -ForegroundColor Yellow; return }
    $script:s = [regex]::Replace($script:s, $pattern, { param($m) $m.Groups[1].Value + $json })
}
# capture_source appears once in each eye's section; set it by section
$left = "http://${HeadsetIp}:8080/"
$right = "http://${HeadsetIp}:8081/"
$s = [regex]::Replace($s, '("left_eye"\s*:\s*\{[^{}]*?"capture_source"\s*:\s*)"[^"]*"', { param($m) $m.Groups[1].Value + "`"$left`"" })
$s = [regex]::Replace($s, '("right_eye"\s*:\s*\{[^{}]*?"capture_source"\s*:\s*)"[^"]*"', { param($m) $m.Groups[1].Value + "`"$right`"" })
Set-Value 'gui_osc_port' '9010'
Set-Value 'gui_osc_vrcft_v2' 'true'
Set-Value 'gui_pyvrcft' 'false'
[IO.File]::WriteAllText($f, $s, (New-Object Text.UTF8Encoding $false))

$check = ([IO.File]::ReadAllText($f) | ConvertFrom-Json)
Write-Host "EyeTrackVR set up (backup: $backup):" -ForegroundColor Green
Write-Host "  left camera  $($check.left_eye.capture_source)"
Write-Host "  right camera $($check.right_eye.capture_source)"
Write-Host "  OSC port $($check.settings.gui_osc_port), VRCFT v2 $($check.settings.gui_osc_vrcft_v2), pyVRCFT $($check.settings.gui_pyvrcft)"
Write-Host "Start EyeTrackVR again."

# Records the headset's camera / tracking errors and its load while we experiment, so a head
# tracking loss can be matched to what eyecam / ETVR were doing at that moment.
#   powershell -ExecutionPolicy Bypass -File watch_headset.ps1
# Writes logs\headset_watch_<time>\ in this project (logcat.txt, load.txt). Ctrl+C to stop.

param([string]$Serial = "")

. (Join-Path $PSScriptRoot '..\find_adb.ps1')
$adb = Find-Adb
if (-not $Serial) {
    $Serial = ((& $adb devices) -match '\sdevice$' | Select-Object -First 1) -split '\s+' | Select-Object -First 1
}
if (-not $Serial) { Write-Host "No headset connected."; exit 1 }

$dir = Join-Path $PSScriptRoot "..\logs\headset_watch_$(Get-Date -Format yyyy-MM-dd_HHmm)"
New-Item -ItemType Directory -Force $dir | Out-Null
Write-Host "Recording to $dir"

# camera / tracking / eye service messages, warnings and up
$tags = 'PvrTrackingService:W PvrTrackingCamera:W CamQuick:W CamComm1.0-QuickClient:W EyeTrackingService:V Stationservice:W *:S'
$logcat = Start-Process $adb -ArgumentList "-s $Serial logcat -v time $tags" -NoNewWindow -PassThru `
    -RedirectStandardOutput "$dir\logcat.txt" -RedirectStandardError "$dir\logcat_err.txt"

try {
    while ($true) {
        $load = (& $adb -s $Serial shell "cat /proc/loadavg; pidof eyestream eyecam; grep -c . /data/local/tmp/eyecam.log") -join ' | '
        "$(Get-Date -Format HH:mm:ss) $load" | Add-Content "$dir\load.txt"
        Start-Sleep 15
    }
} finally {
    Stop-Process -Id $logcat.Id -ErrorAction SilentlyContinue
}

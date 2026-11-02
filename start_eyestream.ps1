# Starts the eye tracking sender on the Pico Neo 3 Pro Eye.
#   powershell -ExecutionPolicy Bypass -File start_eyestream.ps1
# After a headset reboot, plug in USB first: this turns Wi-Fi adb back on,
# then the cable can be unplugged.
# -Stop stops the sender (and the eye cameras) instead.
# -Cameras also starts eyecam: the infrared eye camera streams for Baballonia / EyeTrackVR
#   (left http://<headset>:8080/  right http://<headset>:8081/).
# Spoken messages go to the default audio output, or to the device named in
# voice_device.txt (part of its name, e.g. "Stealth 600") if that file exists.

param([switch]$Stop, [switch]$Cameras, [string]$PcIp = "", [int]$Port = 7777)

. (Join-Path $PSScriptRoot 'find_adb.ps1')
$adb = Find-Adb

$voiceFile = Join-Path $PSScriptRoot 'voice_device.txt'
$voiceDevice = if (Test-Path $voiceFile) { (Get-Content $voiceFile | Select-Object -First 1).Trim() } else { '' }

function Say($text) {
    Write-Host $text
    try {
        $v = New-Object -ComObject SAPI.SpVoice
        if ($voiceDevice) {
            $o = $v.GetAudioOutputs()
            for ($i = 0; $i -lt $o.Count; $i++) {
                if ($o.Item($i).GetDescription() -like "*$voiceDevice*") { $v.AudioOutput = $o.Item($i) }
            }
        }
        [void]$v.Speak($text)
    } catch {
        # chosen device busy (e.g. held by Voicemeeter): use the default output
        try { [void](New-Object -ComObject SAPI.SpVoice).Speak($text) } catch {}
    }
}

# Only one start at a time (two senders starting together can hang Pico's eye service)
$mutex = New-Object System.Threading.Mutex($false, 'Local\PicoEyeStreamStart')
if (-not $mutex.WaitOne(0)) {
    Say "Eye tracking is already being started in another window."
    exit 1
}

if (-not $adb) {
    Say "A D B was not found. Install Android platform tools, see INSTALL dot M D."
    exit 1
}
& $adb start-server | Out-Null

# Headset Wi-Fi IP: ask over USB if plugged in, else use the last known one
$ipFile = Join-Path $PSScriptRoot '.headset_ip'
$headsetIp = if (Test-Path $ipFile) { (Get-Content $ipFile).Trim() } else { '' }

# adb may already be connected over Wi-Fi (e.g. from an earlier run)
$wifi = (& $adb devices) -match '^\d+\.\d+\.\d+\.\d+:5555\s+device$' | Select-Object -First 1
if (-not $headsetIp -and $wifi) { $headsetIp = ($wifi -split ':')[0] }

$usb = (& $adb devices) -match '^\w+\s+device$' | Where-Object { $_ -notmatch ':' } | Select-Object -First 1
if ($usb) {
    $serial = ($usb -split '\s+')[0]
    $ipLine = & $adb -s $serial shell "ip -4 addr show wlan0" | Select-String 'inet (\d+\.\d+\.\d+\.\d+)'
    if ($ipLine) { $headsetIp = $ipLine.Matches[0].Groups[1].Value; $headsetIp | Out-File -Encoding ascii $ipFile }
    if (-not ((& $adb devices) -match "^$([regex]::Escape($headsetIp)):5555\s+device")) {
        & $adb -s $serial tcpip 5555 | Out-Null
        Start-Sleep 4
    }
}
if (-not $headsetIp) {
    if ((& $adb devices) -match '\sunauthorized$') {
        Say "The headset is asking for permission. Put it on, accept Allow U S B debugging, and run this again."
    } else {
        Say "Headset not found. Plug in the U S B cable and try again."
    }
    exit 1
}

$target = "${headsetIp}:5555"
& $adb connect $target | Out-Null
if (-not ((& $adb devices) -match "^$([regex]::Escape($target))\s+device")) {
    Say "Can't reach the headset over Wi-Fi. Plug in the U S B cable and run this again."
    exit 1
}
$env:ANDROID_SERIAL = $target

# Stop any running copies (eyecam turns the image dump off when it exits)
& $adb shell "pkill -TERM eyestream; pkill -TERM eyecam" | Out-Null
# wait for the old copies to really exit; starting a new sender while the old one is still
# stopping leaves the headset's eye tracking engine stuck (started, but no new frames)
for ($i = 0; $i -lt 20; $i++) {
    if (-not (& $adb shell "pidof eyestream eyecam")) { break }
    Start-Sleep -Milliseconds 250
}
& $adb shell "pkill -KILL eyestream; pkill -KILL eyecam" | Out-Null
Start-Sleep -Milliseconds 800
if ($Stop) {
    & $adb shell "setprop debug.pxr.et.tobii.dumpimg 0; rm -rf /sdcard/pvreyetracking" | Out-Null
    Say "Eye tracking stopped."
    exit 0
}

# Make sure the headset has the same programs as this folder (copies them if missing or different)
foreach ($name in 'eyestream', 'eyecam') {
    $local = Join-Path $PSScriptRoot "headset\$name"
    if (-not (Test-Path $local)) { continue }
    $want = (Get-FileHash $local -Algorithm MD5).Hash.ToLower()
    $have = ((& $adb shell "md5sum /data/local/tmp/$name 2>/dev/null") -split '\s+')[0]
    if ($have -ne $want) {
        & $adb push $local /data/local/tmp/$name | Out-Null
        & $adb shell "chmod 755 /data/local/tmp/$name"
    }
}

# PC address on the headset's network
if (-not $PcIp) {
    $prefix = ($headsetIp -split '\.')[0..2] -join '.'
    $PcIp = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -like "$prefix.*" } | Select-Object -First 1).IPAddress
}
if (-not $PcIp) { Say "Couldn't find this PC on the headset's network."; exit 1 }

& $adb shell "setsid nohup /data/local/tmp/eyestream $PcIp $Port > /data/local/tmp/eyestream.log 2>&1 < /dev/null &"
Start-Sleep 2
if ($Cameras) {
    & $adb shell "setsid nohup /data/local/tmp/eyecam 75 90 > /data/local/tmp/eyecam.log 2>&1 < /dev/null &"
    Start-Sleep 1
    Write-Host "Eye cameras: left http://${headsetIp}:8080/  right http://${headsetIp}:8081/"
    # address window with Copy buttons; also refreshes Baballonia's saved addresses
    Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$(Join-Path $PSScriptRoot 'show_eye_addresses.ps1')`""
}

# Pico's eye service sometimes hangs (after hours of running, or two clients at once).
# Then it reports "started" but delivers no frames, and only a headset reboot fixes it.
Start-Sleep 6
$log = (& $adb shell "cat /data/local/tmp/eyestream.log") -join "`n"
$awake = (& $adb shell "dumpsys power | grep mWakefulness=") -match 'Awake'
if ($awake -and ($log -notmatch 'StartEyeTracking' -or $log -match 'no new frames')) {
    Say "Pico's eye tracking service on the headset is stuck. Please reboot the headset, plug in U S B, and run this again."
    Write-Host $log
    exit 1
}

$procId = & $adb shell "pidof eyestream"
if ($procId -and -not $awake) {
    Say "Eye tracking is ready and starts when you put the headset on. You can unplug the U S B cable."
} elseif ($procId) {
    if ($Cameras) { Say "Eye tracking and eye cameras are running. You can unplug the U S B cable." }
    else { Say "Eye tracking sender is running. You can unplug the U S B cable." }
    & $adb shell "cat /data/local/tmp/eyestream.log"
} else {
    Say "The eye tracking sender did not start."
    & $adb shell "cat /data/local/tmp/eyestream.log"
    exit 1
}

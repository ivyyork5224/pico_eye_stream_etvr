# Shows the eye camera addresses for Baballonia / EyeTrackVR in a small window,
# with Copy buttons and a live check that the headset answers.
# If Baballonia is closed and its saved camera addresses don't match the headset's
# current IP, they are updated.
#   powershell -ExecutionPolicy Bypass -File show_eye_addresses.ps1

param([switch]$NoWindow)

. (Join-Path $PSScriptRoot 'find_adb.ps1')
$adb = Find-Adb
$ipFile = Join-Path $PSScriptRoot '.headset_ip'

# Current headset IP: ask the headset if adb can reach it, else the last known one
$headsetIp = if (Test-Path $ipFile) { (Get-Content $ipFile).Trim() } else { '' }
try {
    $dev = (& $adb devices) -match '\sdevice$' | Select-Object -First 1
    if ($dev) {
        $serial = ($dev -split '\s+')[0]
        $line = & $adb -s $serial shell "ip -4 addr show wlan0" | Select-String 'inet (\d+\.\d+\.\d+\.\d+)'
        if ($line) {
            $headsetIp = $line.Matches[0].Groups[1].Value
            $headsetIp | Out-File -Encoding ascii $ipFile
        }
    }
} catch {}

$left = "http://${headsetIp}:8080/"
$right = "http://${headsetIp}:8081/"

# Keep Baballonia's saved addresses in step with the headset IP (only while it's closed,
# since it rewrites the file on exit)
$babSettings = Join-Path $env:APPDATA 'ProjectBabble\ApplicationData\LocalSettings.json'
$babNote = ''
if ($headsetIp -and (Test-Path $babSettings)) {
    $json = [IO.File]::ReadAllText($babSettings)
    $new = $json -replace '("LastOpenedLeftCamera":\s*")[^"]*(")', "`${1}$left`${2}" `
                 -replace '("LastOpenedRightCamera":\s*")[^"]*(")', "`${1}$right`${2}"
    if ($new -ne $json) {
        if (Get-Process Baballonia* -ErrorAction SilentlyContinue) {
            $babNote = 'Baballonia has old addresses saved. Close it and run this again to update them, or paste the new ones.'
        } else {
            Copy-Item $babSettings "$babSettings.bak" -Force
            [IO.File]::WriteAllText($babSettings, $new, (New-Object Text.UTF8Encoding $false))
            $babNote = 'Updated the addresses saved in Baballonia.'
        }
    } else {
        $babNote = 'Baballonia already has these addresses.'
    }
}

function Test-Cam($url) {
    try {
        $r = Invoke-WebRequest "${url}snap.jpg" -TimeoutSec 3 -UseBasicParsing
        return $r.StatusCode -eq 200
    } catch { return $false }
}

if ($NoWindow) {
    "Left eye:  $left"
    "Right eye: $right"
    if ($babNote) { $babNote }
    exit 0
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Pico eye camera addresses'
$form.Size = New-Object System.Drawing.Size(560, 250)
$form.StartPosition = 'CenterScreen'
$form.TopMost = $true
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$font = New-Object System.Drawing.Font('Segoe UI', 10)
$form.Font = $font

function Add-Row($y, $label, $url) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $label; $l.Location = New-Object System.Drawing.Point(15, ($y + 4)); $l.AutoSize = $true
    $form.Controls.Add($l)

    $t = New-Object System.Windows.Forms.TextBox
    $t.Text = $url; $t.ReadOnly = $true
    $t.Location = New-Object System.Drawing.Point(100, $y); $t.Width = 250
    $form.Controls.Add($t)

    $c = New-Object System.Windows.Forms.Button
    $c.Text = 'Copy'; $c.Location = New-Object System.Drawing.Point(360, ($y - 1)); $c.Width = 70
    $c.Add_Click({ [System.Windows.Forms.Clipboard]::SetText($url); $status.Text = "Copied $url" }.GetNewClosure())
    $form.Controls.Add($c)

    $s = New-Object System.Windows.Forms.Label
    $s.Text = 'checking...'; $s.Location = New-Object System.Drawing.Point(440, ($y + 4)); $s.AutoSize = $true
    $form.Controls.Add($s)
    return $s
}

# created before the rows, because their Copy buttons write to it
$status = New-Object System.Windows.Forms.Label
$status.Text = "Headset: $headsetIp"; $status.Location = New-Object System.Drawing.Point(15, 150); $status.AutoSize = $true
$status.ForeColor = [System.Drawing.Color]::DimGray
$form.Controls.Add($status)

$leftState = Add-Row 20 'Left eye' $left
$rightState = Add-Row 60 'Right eye' $right

$note = New-Object System.Windows.Forms.Label
$note.Text = $babNote; $note.Location = New-Object System.Drawing.Point(15, 105)
$note.Size = New-Object System.Drawing.Size(520, 40)
$form.Controls.Add($note)

$recheck = New-Object System.Windows.Forms.Button
$recheck.Text = 'Check again'; $recheck.Location = New-Object System.Drawing.Point(330, 165); $recheck.Width = 100
$form.Controls.Add($recheck)

$close = New-Object System.Windows.Forms.Button
$close.Text = 'Close'; $close.Location = New-Object System.Drawing.Point(440, 165); $close.Width = 90
$close.Add_Click({ $form.Close() })
$form.Controls.Add($close)

$check = {
    foreach ($pair in @(@($leftState, $left), @($rightState, $right))) {
        $ok = Test-Cam $pair[1]
        $pair[0].Text = if ($ok) { 'online' } else { 'no picture yet' }
        $pair[0].ForeColor = if ($ok) { [System.Drawing.Color]::Green } else { [System.Drawing.Color]::Firebrick }
    }
}
$recheck.Add_Click({ $leftState.Text = 'checking...'; $rightState.Text = 'checking...'; $form.Refresh(); & $check })
$form.Add_Shown({ $form.Refresh(); & $check })

[void]$form.ShowDialog()

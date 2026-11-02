# Returns the path to adb.exe: on PATH, in a platform-tools folder next to this project,
# in C:\platform-tools, or in the Android SDK. Dot-source it:  . (Join-Path $PSScriptRoot 'find_adb.ps1')

function Find-Adb {
    $cmd = Get-Command adb -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $PSScriptRoot 'platform-tools\adb.exe'),
        'C:\platform-tools\adb.exe',
        (Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe')
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

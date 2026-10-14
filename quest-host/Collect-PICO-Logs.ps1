param([string]$Adb = 'adb', [string]$Serial)
$ErrorActionPreference = 'Stop'
$adbArgs = @()
if ($Serial) { $adbArgs = @('-s', $Serial) }
& $Adb @adbArgs get-state
if ($LASTEXITCODE -ne 0) { throw 'Connect and authorize one PICO over USB, or specify -Serial.' }
$output = Join-Path $PSScriptRoot ('pico-report-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $output | Out-Null
$remote = '/sdcard/Android/data/com.astrobotquest.vrhost.evertec82.pico/files'
foreach ($name in @('host.log','core.log','host.prev.log','core.prev.log','vrhost.txt')) {
    & $Adb @adbArgs pull "$remote/$name" (Join-Path $output $name)
    if ($LASTEXITCODE -ne 0) { Write-Warning "$name was not available on the headset." }
}
& $Adb @adbArgs logcat -d -t 4000 | Set-Content (Join-Path $output 'logcat.txt') -Encoding UTF8
& $Adb @adbArgs shell dumpsys package com.astrobotquest.vrhost.evertec82.pico |
    Set-Content (Join-Path $output 'package.txt') -Encoding UTF8
Compress-Archive -Path "$output/*" -DestinationPath "$output.zip"
Write-Host "Report: $output.zip"
Write-Host 'Review before sharing: Android logs may contain personal information from other apps.'

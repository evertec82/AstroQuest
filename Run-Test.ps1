param([ValidateSet('Play', 'Recovery', 'Original', 'FullRefresh')][string]$Mode = 'Play')
$ErrorActionPreference = 'Stop'
$pcRoot = Join-Path $PSScriptRoot 'pc-vr'
$archive = Join-Path $PSScriptRoot ('test-logs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + $Mode)
New-Item -ItemType Directory -Force $archive | Out-Null
$env:SHADPS4_XR_SHARED_QUEUE = '0'
$env:SHADPS4_VR_RETRY_SECONDS = if ($Mode -eq 'Original') { '600' } else { '15' }
$env:SHADPS4_VR_PACE = if ($Mode -eq 'FullRefresh') { '1' } elseif ($Mode -eq 'Play') { '' } else { '0' }
$env:SHADPS4_FRAME_STATS = '1'
$env:SHADPS4_FRAME_STATS_EVERY = '5'
Copy-Item -LiteralPath (Join-Path $pcRoot 'settings.txt') -Destination $archive
@("Mode=$Mode", "Started=$(Get-Date -Format o)", "RetrySeconds=$env:SHADPS4_VR_RETRY_SECONDS",
  "FixedPace=$env:SHADPS4_VR_PACE", "ExecutableSHA256=$((Get-FileHash (Join-Path $pcRoot 'shadps4.exe')).Hash)") |
    Set-Content -LiteralPath (Join-Path $archive 'run-info.txt') -Encoding UTF8
Write-Host "Choose resolution and Full framerate or an FPS cap in the settings window. Set headset refresh in SteamVR or Virtual Desktop. Disable Virtual Desktop SSW or SteamVR motion smoothing for native-FPS testing."
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $pcRoot 'launch.ps1') -ForceMenu
} finally {
    # The menu saves the selected settings before launching; archive those choices.
    Copy-Item -LiteralPath (Join-Path $pcRoot 'settings.txt') -Destination $archive -Force
    $logFolder = Join-Path $pcRoot 'user/log'
    if (Test-Path -LiteralPath $logFolder) {
        Get-ChildItem -LiteralPath $logFolder -File |
            ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $archive }
    }
    $logPath = Join-Path $archive 'shad_log.txt'
    if (Test-Path $logPath) {
        Select-String -LiteralPath $logPath -Pattern 'Full-refresh recovery|Headset uses Vulkan queue|Headset queue|Report: Headset:|SetPace:|Change: The scene|first picture handed|title.s clock:|frame stats at|frame path:|frame shortcuts:' |
            ForEach-Object { $_.Line } | Set-Content -LiteralPath (Join-Path $archive 'performance.txt') -Encoding UTF8
    }
    Write-Host "Archived this run in $archive"
}

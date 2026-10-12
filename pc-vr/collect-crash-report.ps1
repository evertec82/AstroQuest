param([string]$Root = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$report = Join-Path $Root ('reports\crash-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $report -Force | Out-Null
# Deliberately exclude games, saves, shader caches and memory dumps.
foreach ($name in @('BUILD.txt', 'settings.txt', 'user/config.json', 'user/log')) {
    $source = Join-Path $Root $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination $report -Recurse
    }
}
$exe = Join-Path $Root 'shadps4.exe'
if (Test-Path -LiteralPath $exe) {
    Get-FileHash -LiteralPath $exe -Algorithm SHA256 | Format-List |
        Out-File (Join-Path $report 'executable-hash.txt')
}
@("Collected=$(Get-Date -Format o)", "OS=$([Environment]::OSVersion.VersionString)",
  "PowerShell=$($PSVersionTable.PSVersion)") |
    Set-Content (Join-Path $report 'system.txt') -Encoding UTF8
Compress-Archive -LiteralPath $report -DestinationPath "$report.zip"
Write-Host "Crash report saved: $report.zip"
Write-Host 'Review before sharing: logs and settings can contain local paths. No game files or saves are included.'

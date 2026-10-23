# Registers (or with -Uninstall removes) the xrperf layer as an implicit OpenXR layer
# for the current user. No admin rights needed. The layer only hooks processes
# listed in xrperf.ini, every other OpenXR app passes straight through.
#
# The registry write goes through reg.exe started by explorer.exe on purpose: when this
# script runs from inside an MSIX-packaged app (e.g. the Claude desktop app), its HKCU
# writes land in the package's private registry and the game, started normally, never
# sees them. A process started by Explorer runs outside the package.
param([switch]$Uninstall)
$ErrorActionPreference = "Stop"

$key = "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit"
$manifest = Join-Path $PSScriptRoot "build\out\xrperf_layer.json"
$work = Join-Path $PSScriptRoot "build"

if (-not $Uninstall -and -not (Test-Path $manifest)) { throw "$manifest not found, run build.ps1 first" }

$cmd = Join-Path $work "xrperf_register.cmd"
$out = Join-Path $work "xrperf_register.txt"
Remove-Item $out -ErrorAction SilentlyContinue
$action = if ($Uninstall) { "reg delete `"$key`" /v `"$manifest`" /f" }
          else { "reg add `"$key`" /v `"$manifest`" /t REG_DWORD /d 0 /f" }
Set-Content -Path $cmd -Encoding ASCII -Value @(
    "@$action > `"$out`" 2>&1"
    "@reg query `"$key`" >> `"$out`" 2>&1"
)
Start-Process explorer.exe -ArgumentList "`"$cmd`""

$deadline = (Get-Date).AddSeconds(15)
while (-not (Test-Path $out) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 300 }
Start-Sleep -Milliseconds 500
Get-Content $out -ErrorAction SilentlyContinue
Remove-Item $cmd, $out -ErrorAction SilentlyContinue
Write-Host ($(if ($Uninstall) { "xrperf layer unregistered" } else { "xrperf layer registered: $manifest" }))

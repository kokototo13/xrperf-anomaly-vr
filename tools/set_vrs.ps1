# Switches foveated rendering (NVIDIA VRS) on or off. Used by VRS_ON.bat / VRS_OFF.bat.
# Takes effect in a running game within a second.
param([Parameter(Mandatory)][ValidateSet("on", "off")][string]$State)

# Release package: ini next to the dll one level up; development tree: build\out
$ini = Join-Path $PSScriptRoot "..\xrperf.ini"
if (-not (Test-Path $ini)) { $ini = Join-Path $PSScriptRoot "..\build\out\xrperf.ini" }
$lines = Get-Content $ini
$inSection = $false
$lines = foreach ($line in $lines) {
    if ($line -match '^\[(.+)\]') { $inSection = $Matches[1] -eq 'foveated' }
    if ($inSection -and $line -match '^enabled=') { "enabled=$(if ($State -eq 'on') { 1 } else { 0 })" } else { $line }
}
Set-Content $ini $lines -Encoding ASCII

Write-Host ""
Write-Host "  Foveated rendering (VRS) $($State.ToUpper()). Applied to the running game within a second."
Write-Host ""

# Waits for AnomalyDX11*.exe, lets it get through OpenXR init, then diffs its loaded
# modules against a baseline list (one full path per line) taken from a direct launch.
param([string]$Baseline = "$env:TEMP\xrperf_mods_direct.txt", [int]$TimeoutSec = 900, [int]$SettleSec = 45)

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$game = $null
while (-not $game -and (Get-Date) -lt $deadline) {
    $game = Get-Process | Where-Object { $_.Name -like "AnomalyDX11*" } | Select-Object -First 1
    if (-not $game) { Start-Sleep -Milliseconds 500 }
}
if (-not $game) { "timeout: game did not start"; return }

"game pid $($game.Id) started $($game.StartTime), waiting $SettleSec s for OpenXR init"
Start-Sleep $SettleSec
$game.Refresh()
$mods = $game.Modules | Select-Object -ExpandProperty FileName | Sort-Object
$base = Get-Content $Baseline
"modules now: $($mods.Count), baseline: $($base.Count)"
"--- only in this launch (=>) / only in direct launch (<=)"
Compare-Object $base $mods | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }
"--- xrperf_loads.log tail"
Get-Content "$env:LOCALAPPDATA\xrperf\xrperf_loads.log" | Select-Object -Last 2

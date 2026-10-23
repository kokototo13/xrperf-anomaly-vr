# A/B test for foveated rendering in one game session: starts the game, waits for the
# player to load a save, then switches VRS off/on every few seconds (the layer picks up
# the ini change live) and compares the game's GPU time per phase. Closes the game after.
param([int]$PhaseSec = 10, [int]$Rounds = 2, [string]$GamePath = "C:\S.T.A.L.K.E.R. Anomaly")
$ErrorActionPreference = "Stop"
$game = $GamePath
$root = Join-Path $PSScriptRoot ".."
$ini = Join-Path $root "build\out\xrperf.ini"
$layerLog = Join-Path $root "build\out\logs\xrperf.log"
$runs = Join-Path $root "runs"

foreach ($p in @(Get-Process AnomalyDX11 -ErrorAction SilentlyContinue)) { $p.CloseMainWindow() | Out-Null; if (-not $p.WaitForExit(20000)) { $p.Kill() } }
(Get-Content $ini) -replace '^stats_interval=.*', 'stats_interval=2' | Set-Content $ini -Encoding ASCII
& (Join-Path $PSScriptRoot "set_vrs.ps1") -State off | Out-Null

$cmd = Join-Path $runs "bench_start.cmd"
Set-Content $cmd -Encoding ASCII -Value @("@cd /d `"$game\bin`"", "@start `"`" `"$game\bin\AnomalyDX11.exe`"")
$started = Get-Date
Start-Process explorer.exe -ArgumentList "`"$cmd`""
$deadline = (Get-Date).AddSeconds(60)
while (-not ($proc = Get-Process AnomalyDX11 -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
if (-not $proc) { throw "game did not start" }

function GpuLines {
    Get-Content $layerLog -ErrorAction SilentlyContinue | Select-String '^\[(\d\d:\d\d:\d\d\.\d+)\]   game GPU: avg ([\d.]+) ms' |
        ForEach-Object { [pscustomobject]@{ Time = $started.Date + [TimeSpan]::Parse($_.Matches[0].Groups[1].Value); Gpu = [double]$_.Matches[0].Groups[2].Value } } |
        Where-Object Time -ge $started
}

"load the save now..."
$deadline = (Get-Date).AddSeconds(600)
do {
    Start-Sleep 1
    $last = @(GpuLines | Select-Object -Last 2)
    $loaded = $last.Count -eq 2 -and @($last | Where-Object Gpu -gt 3).Count -eq 2
} until ($loaded -or $proc.HasExited -or (Get-Date) -gt $deadline)
if (-not $loaded) { throw "level did not load" }
"in level, settling 6 s"
Start-Sleep 6

$phases = @()
for ($i = 0; $i -lt $Rounds; $i++) {
    foreach ($state in "off", "on") {
        & (Join-Path $PSScriptRoot "set_vrs.ps1") -State $state | Out-Null
        $t0 = Get-Date
        "VRS $state for $PhaseSec s"
        Start-Sleep $PhaseSec
        $phases += [pscustomobject]@{ State = $state; Start = $t0; End = Get-Date }
    }
}
& (Join-Path $PSScriptRoot "set_vrs.ps1") -State off | Out-Null

if (-not $proc.HasExited) { $proc.CloseMainWindow() | Out-Null; if (-not $proc.WaitForExit(30000)) { $proc.Kill() } }
(Get-Content $ini) -replace '^stats_interval=.*', 'stats_interval=5' | Set-Content $ini -Encoding ASCII
Copy-Item $layerLog (Join-Path $runs "vrs_ab.log") -Force

# A stats window covers the 2 s before its timestamp: skip the first window of each phase
# (it straddles the switch) and use the rest
$all = @(GpuLines)
$result = foreach ($p in $phases) {
    $w = @($all | Where-Object { $_.Time -gt $p.Start.AddSeconds(2.5) -and $_.Time -le $p.End })
    [pscustomobject]@{
        VRS = $p.State
        From = $p.Start.ToString("HH:mm:ss")
        Windows = $w.Count
        GpuMs = if ($w.Count) { [math]::Round(($w | Measure-Object Gpu -Average).Average, 2) } else { $null }
    }
}
$result | Format-Table -AutoSize | Out-String
foreach ($s in "off", "on") {
    $v = @($result | Where-Object { $_.VRS -eq $s -and $_.GpuMs })
    "VRS {0,-3}: avg game GPU {1:N2} ms" -f $s, ($v | Measure-Object GpuMs -Average).Average
}

# Automated benchmark: starts the game, loads a save through the console once the VR
# session is focused (headset worn), records the xrperf layer's stats for a fixed time
# while standing still, then closes the game and prints averages for that window.
#
# usage: bench.ps1 -Scale 1.0 -Label nofsr [-Save save_2] [-Seconds 30] [-ManualLoad]
#   -ManualLoad: the player loads the save from the menu; the script only waits for it
param(
    [Parameter(Mandatory)][double]$Scale,
    [Parameter(Mandatory)][string]$Label,
    [string]$Save = "save_2",
    [int]$Seconds = 30,
    [int]$SettleSec = 20,
    [switch]$ManualLoad,
    [string]$Upscaler = "fsr",
    [string]$GamePath = "C:\S.T.A.L.K.E.R. Anomaly"
)
$ErrorActionPreference = "Stop"
$game = $GamePath
$layerOut = Join-Path $PSScriptRoot "..\build\out"
$runs = Join-Path $PSScriptRoot "..\runs"
New-Item -ItemType Directory -Force $runs | Out-Null

foreach ($n in "AnomalyDX11", "AoeVrLauncher") {
    foreach ($p in @(Get-Process $n -ErrorAction SilentlyContinue)) {
        $p.CloseMainWindow() | Out-Null
        if (-not $p.WaitForExit(20000)) { $p.Kill() }
    }
}

$ini = Join-Path $layerOut "xrperf.ini"
(Get-Content $ini) -replace '^render_scale=.*', "render_scale=$Scale" -replace '^upscaler=.*', "upscaler=$Upscaler" |
    Set-Content $ini -Encoding ASCII

# Started through Explorer so the game runs outside this tool's MSIX container
# (otherwise it would not see the layer's registry entry)
$cmd = Join-Path $runs "bench_start.cmd"
Set-Content $cmd -Encoding ASCII -Value @(
    "@cd /d `"$game\bin`""
    "@start `"`" `"$game\bin\AnomalyDX11.exe`""
)
$started = Get-Date
Start-Process explorer.exe -ArgumentList "`"$cmd`""

$deadline = (Get-Date).AddSeconds(60)
while (-not ($proc = Get-Process AnomalyDX11 -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
if (-not $proc) { throw "game did not start" }

function EngineLog {
    Get-ChildItem "$game\appdata\logs" -Filter "xray_*.log" | Where-Object LastWriteTime -gt $started |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

if ($ManualLoad) {
    "load $Save from the menu now..."
} else {
    # The game only renders (and reads input) while the OpenXR session is visible or focused,
    # which SteamVR allows only while the headset is in use (not in standby)
    "waiting for the VR session to become visible (put the headset on)..."
    $deadline = (Get-Date).AddSeconds(300)
    do {
        Start-Sleep 2
        $log = EngineLog
        $state = if ($log) { Select-String -Path $log.FullName -SimpleMatch "Session state ->" | Select-Object -Last 1 }
        $focused = $state -and $state.Line -match 'VISIBLE|FOCUSED'
    } until ($focused -or $proc.HasExited -or (Get-Date) -gt $deadline)
    if (-not $focused) { throw "VR session never became visible" }
    # The main menu keeps loading for a while after the session starts; keys sent too early
    # land in the menu instead of the console (Enter can hit "Quit")
    Start-Sleep 25

    "loading $Save"
    & (Join-Path $PSScriptRoot "game_console.ps1") -Command "load $Save"
}

# Wait for the level to be on screen, then let streaming settle. The engine flushes its
# log late (15-20 s), so this watches the layer's log instead: in a level the game's GPU
# time per frame goes from well under a millisecond (menu) to several milliseconds.
$layerLog = Join-Path $layerOut "logs\xrperf.log"
$deadline = (Get-Date).AddSeconds($(if ($ManualLoad) { 600 } else { 240 }))
do {
    Start-Sleep 1
    $gpuLines = Get-Content $layerLog -ErrorAction SilentlyContinue | Select-String '^\[(\d\d:\d\d:\d\d)\.\d+\]   game GPU: avg ([\d.]+) ms' |
        Where-Object { $started.Date + [TimeSpan]::Parse($_.Matches[0].Groups[1].Value) -ge $started } |
        Select-Object -Last 2
    $loaded = @($gpuLines).Count -eq 2 -and @($gpuLines | Where-Object { [double]$_.Matches[0].Groups[2].Value -gt 3 }).Count -eq 2
} until ($loaded -or $proc.HasExited -or (Get-Date) -gt $deadline)
if (-not $loaded) { throw "save did not load (game exited: $($proc.HasExited))" }
"loaded, settling for $SettleSec s"
Start-Sleep $SettleSec

"measuring for $Seconds s"
$t0 = Get-Date
Start-Sleep $Seconds
$t1 = Get-Date

$proc.CloseMainWindow() | Out-Null
if (-not $proc.WaitForExit(30000)) { $proc.Kill() }
Start-Sleep 1

$layerLog = Join-Path $layerOut "logs\xrperf.log"
Copy-Item $layerLog (Join-Path $runs "bench_$Label.log") -Force

# Average the 5-second windows that fall inside [t0, t1]
$fps = @(); $frameMs = @(); $missed = 0; $frames = 0; $gpu = @(); $gpuWorst = @(); $fsr = @(); $badWindows = 0
$hzSeen = @()
foreach ($line in Get-Content $layerLog) {
    if ($line -notmatch '^\[(\d\d:\d\d:\d\d\.\d+)\]') { continue }
    $ts = [datetime]::ParseExact($Matches[1], "HH:mm:ss.fff", $null)
    $ts = $t0.Date + $ts.TimeOfDay
    if ($ts -lt $t0.AddSeconds(2) -or $ts -gt $t1) { continue }
    if ($line -match 'frames: ([\d.]+) fps, avg ([\d.]+) ms.*missed vsync (\d+)/(\d+) \(headset ([\d.]+) Hz\)') {
        $fps += [double]$Matches[1]; $frameMs += [double]$Matches[2]
        $missed += [int]$Matches[3]; $frames += [int]$Matches[4]
        if ([int]$Matches[3] -gt 0) { $badWindows++ }
        $hzSeen += $Matches[5]
    } elseif ($line -match 'game GPU: avg ([\d.]+) ms, worst ([\d.]+) ms') {
        $gpu += [double]$Matches[1]; $gpuWorst += [double]$Matches[2]
    } elseif ($line -match '(?:FSR|NIS) GPU cost: avg ([\d.]+) ms') {
        $fsr += [double]$Matches[1]
    }
}
function Avg($a) { if ($a.Count) { "{0:N2}" -f (($a | Measure-Object -Average).Average) } else { "-" } }

$summary = @(
    "=== $Label (render_scale $Scale, upscaler $Upscaler, $Save, $Seconds s) ==="
    "windows: $($fps.Count)"
    "fps:            $(Avg $fps)"
    "frame time ms:  $(Avg $frameMs)"
    "missed vsync:   $missed / $frames (windows with misses: $badWindows / $($fps.Count))"
    "SteamVR rate:   $(($hzSeen | Group-Object | ForEach-Object { "$($_.Name) Hz x$($_.Count)" }) -join ', ')"
    "game GPU ms:    avg $(Avg $gpu), heaviest window $(if ($gpu.Count) { '{0:N2}' -f ($gpu | Measure-Object -Maximum).Maximum } else { '-' }), worst frame $(if ($gpuWorst.Count) { '{0:N2}' -f ($gpuWorst | Measure-Object -Maximum).Maximum } else { '-' })"
    "upscaler GPU ms: $(Avg $fsr)"
)
$summary | Set-Content (Join-Path $runs "bench_$Label.txt") -Encoding UTF8
$summary

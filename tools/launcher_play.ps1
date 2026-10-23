# Clicks the Play button in the running AOE VR Launcher (via UI Automation), waits for the
# game to reach OpenXR session init and reports whether the xrperf layer got loaded.
param([int]$TimeoutSec = 120, [string]$GamePath = "C:\S.T.A.L.K.E.R. Anomaly")

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
# "ИГРАТЬ В VR" spelled with char codes: Windows PowerShell 5.1 reads BOM-less scripts as ANSI
$playText = (-join [char[]](0x0418, 0x0413, 0x0420, 0x0410, 0x0422, 0x042C, 0x20, 0x0412)) + ' VR'
$launcher = Get-Process AoeVrLauncher -ErrorAction Stop | Select-Object -First 1
$root = [Windows.Automation.AutomationElement]::RootElement
$deadline = (Get-Date).AddSeconds(30)
do {
    $win = $root.FindFirst([Windows.Automation.TreeScope]::Children,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::ProcessIdProperty, $launcher.Id)))
    $text = if ($win) { $win.FindFirst([Windows.Automation.TreeScope]::Descendants,
        (New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $playText))) }
    if (-not $text) { Start-Sleep 1 }
} until ($text -or (Get-Date) -gt $deadline)
if (-not $text) { throw "Play button not found" }

$walker = [Windows.Automation.TreeWalker]::ControlViewWalker
$btn = $walker.GetParent($text)
while ($btn -and $btn.Current.ControlType.ProgrammaticName -ne 'ControlType.Button') { $btn = $walker.GetParent($btn) }
$btn.GetCurrentPattern([Windows.Automation.InvokePattern]::Pattern).Invoke()

$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not ($game = Get-Process AnomalyDX11 -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep 1 }
if (-not $game) { "game did not start"; return }

$logs = Join-Path $GamePath "appdata\logs"
$loadsLog = Join-Path $PSScriptRoot "..\build\out\logs\xrperf_loads.log"
do {
    Start-Sleep 2
    $loaded = Get-Content $loadsLog -ErrorAction SilentlyContinue | Select-String "pid $($game.Id):" -Quiet
    $log = Get-ChildItem $logs -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $session = Select-String -Path $log.FullName -SimpleMatch "OpenXR session initialized" -Quiet
} until ($loaded -or (Get-Date) -gt $deadline)
"launcher pid $($launcher.Id) -> game pid $($game.Id): layer loaded = $([bool]$loaded), session = $([bool]$session)"

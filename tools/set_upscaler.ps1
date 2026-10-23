# Switches the xrperf upscaler. Used by FSR.bat / NIS.bat / OFF.bat.
# FSR and NIS take effect immediately in a running game (the layer re-reads the ini
# every second). The render resolution is only applied when the game starts.
param([Parameter(Mandatory)][ValidateSet("fsr", "nis", "off")][string]$Mode, [double]$Scale = 0.85)

# Release package: ini next to the dll one level up; development tree: build\out
$ini = Join-Path $PSScriptRoot "..\xrperf.ini"
if (-not (Test-Path $ini)) { $ini = Join-Path $PSScriptRoot "..\build\out\xrperf.ini" }
$lines = Get-Content $ini
$current = [double](($lines | Where-Object { $_ -match '^render_scale=' }) -replace '^render_scale=', '')

if ($Mode -eq "off") {
    $upscaler = "none"
    $newScale = 1.0
} else {
    $upscaler = $Mode
    $newScale = if ($current -lt 1.0) { $current } else { $Scale }
}

$lines = $lines -replace '^upscaler=.*', "upscaler=$upscaler" -replace '^render_scale=.*', "render_scale=$newScale"
Set-Content $ini $lines -Encoding ASCII

Write-Host ""
switch ($Mode) {
    "off" {
        Write-Host "  Upscaling OFF."
        Write-Host "  In a running game: SteamVR now stretches the smaller image (instant)."
        Write-Host "  Native resolution (render_scale 1.0) applies the next time the game starts."
    }
    default {
        Write-Host "  $($Mode.ToUpper()) ON, render_scale $newScale."
        if ($current -ge 1.0) {
            Write-Host "  The game was set to native resolution: restart the game for $($Mode.ToUpper()) to kick in."
        } else {
            Write-Host "  Applied to the running game within a second."
        }
    }
}
Write-Host ""

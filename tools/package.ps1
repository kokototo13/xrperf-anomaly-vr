# Builds the release folder and zip for sharing: dist\xrperf-anomaly-vr(.zip)
# Uses the template xrperf.ini (manifest\), never the local live config.
$ErrorActionPreference = "Stop"
$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$out = Join-Path $root "build\out"
$dist = Join-Path $root "dist\xrperf-anomaly-vr"
$zip = Join-Path $root "dist\xrperf-anomaly-vr.zip"

if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Force "$dist\tools", "$dist\licenses" | Out-Null

Copy-Item "$out\xrperf_layer.dll", "$out\xrperf_layer.json" $dist
Copy-Item "$root\manifest\xrperf.ini" $dist
Copy-Item "$root\FSR.bat", "$root\NIS.bat", "$root\OFF.bat" $dist
Copy-Item "$root\package\install.bat", "$root\package\uninstall.bat", "$root\package\README.txt", "$root\package\README.ru.txt" $dist
Copy-Item "$root\LICENSE" "$dist\licenses\xrperf.txt"
Copy-Item "$root\tools\set_upscaler.ps1" "$dist\tools"

Copy-Item "$root\external\FidelityFX-FSR\license.txt" "$dist\licenses\AMD_FidelityFX_FSR.txt"
Copy-Item "$root\external\NVIDIAImageScaling\licence.txt" "$dist\licenses\NVIDIA_Image_Scaling.txt"
Copy-Item "$root\external\OpenXR-SDK\LICENSE" "$dist\licenses\OpenXR_SDK.txt"

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $dist -DestinationPath $zip
Get-ChildItem $dist -Recurse -File | Select-Object @{ n = 'File'; e = { $_.FullName.Substring($dist.Length + 1) } }, Length
"zip: $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"

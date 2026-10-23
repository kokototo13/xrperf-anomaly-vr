# Downloads the few OpenVR SDK files the optional vr_timing_logger tool needs
# into external/openvr. The layer itself does not use OpenVR.
$ErrorActionPreference = "Stop"
$dir = Join-Path $PSScriptRoot "..\external\openvr"
New-Item -ItemType Directory -Force "$dir\headers", "$dir\lib" | Out-Null
$base = "https://raw.githubusercontent.com/ValveSoftware/openvr/v2.5.1"
Invoke-WebRequest "$base/headers/openvr.h" -OutFile "$dir\headers\openvr.h" -UseBasicParsing
Invoke-WebRequest "$base/lib/win64/openvr_api.lib" -OutFile "$dir\lib\openvr_api.lib" -UseBasicParsing
Invoke-WebRequest "$base/bin/win64/openvr_api.dll" -OutFile "$dir\lib\openvr_api.dll" -UseBasicParsing
Invoke-WebRequest "$base/LICENSE" -OutFile "$dir\LICENSE" -UseBasicParsing
"OpenVR SDK files in $dir"

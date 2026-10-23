@echo off
rem Registers xrperf as an OpenXR layer for the current user (no admin needed).
rem Run again if you move this folder.
reg add "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%~dp0xrperf_layer.json" /t REG_DWORD /d 0 /f >nul
if errorlevel 1 (
    echo   Install FAILED.
) else (
    echo.
    echo   xrperf installed. Start Anomaly VR as usual.
    echo   Log: %~dp0logs\xrperf.log
    echo.
)
pause

@echo off
rem Removes the xrperf OpenXR layer registration. The folder can be deleted afterwards.
reg delete "HKCU\SOFTWARE\Khronos\OpenXR\1\ApiLayers\Implicit" /v "%~dp0xrperf_layer.json" /f >nul 2>&1
echo.
echo   xrperf removed. The game now runs exactly as without it.
echo.
pause

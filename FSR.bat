@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\set_upscaler.ps1" -Mode fsr
timeout /t 3 >nul

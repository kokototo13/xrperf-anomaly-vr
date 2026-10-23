@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\set_upscaler.ps1" -Mode off
timeout /t 5 >nul

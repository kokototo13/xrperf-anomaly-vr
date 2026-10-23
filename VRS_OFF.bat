@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\set_vrs.ps1" -State off
timeout /t 3 >nul

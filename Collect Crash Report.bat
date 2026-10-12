@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0pc-vr\collect-crash-report.ps1"
pause

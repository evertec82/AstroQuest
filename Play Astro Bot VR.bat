@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-Test.ps1" -Mode FullRefresh
pause

@echo off
setlocal
cd /d "%~dp0"

where powershell.exe >nul 2>nul
if errorlevel 1 (
  echo Revia setup requires Windows PowerShell 5.1 or newer.
  exit /b 1
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Tools\Setup-Revia.ps1" %*
exit /b %errorlevel%

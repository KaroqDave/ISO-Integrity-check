@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0iso-integrity-check.ps1" %*
exit /b %ERRORLEVEL%

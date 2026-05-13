@echo off
REM Run from CMD: build.cmd -ShowSize  (forwards all args to project\build.ps1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*

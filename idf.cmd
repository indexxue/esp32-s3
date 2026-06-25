@echo off
REM Generic idf.py wrapper (for cmd.exe — .ps1 opens in editor if run directly from cmd).
REM Usage: idf -Project project release 1.2.3
REM        idf build
setlocal
set "SCRIPT_DIR=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%idf.ps1" %*
exit /b %ERRORLEVEL%

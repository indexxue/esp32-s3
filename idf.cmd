@echo off
REM Generic idf.py wrapper (for cmd.exe — .ps1 opens in editor if run directly from cmd).
REM Usage: idf -Project project release 1.2.3
REM        idf build
REM        idf -p COM13 flash monitor
setlocal
set "SCRIPT_DIR=%~dp0"
REM Use -Command (not -File) so idf.py flags like -p / -D reach the script on Windows PS 5.1.
powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%SCRIPT_DIR%idf.ps1' -- %*"
exit /b %ERRORLEVEL%

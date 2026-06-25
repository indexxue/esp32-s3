@echo off
REM Remove project build output (use from cmd.exe).
REM Example: clean_build.cmd
setlocal
set "ROOT=%~dp0"
if exist "%ROOT%project\build" (
    rd /s /q "%ROOT%project\build"
    echo Cleaned project\build
) else (
    echo project\build does not exist
)
exit /b 0

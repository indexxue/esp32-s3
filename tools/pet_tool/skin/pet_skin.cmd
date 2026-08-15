@echo off
cd /d "%~dp0"
py -3 -m pip show PySide6 >nul 2>&1
if errorlevel 1 (
  echo Installing GUI deps...
  py -3 -m pip install -r requirements.txt
)
py -3 run_gui.py
if errorlevel 1 pause

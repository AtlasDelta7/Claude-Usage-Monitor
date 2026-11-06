@echo off
rem Starts the bridge in the background (no window). Logs go to bridge.log.
cd /d "%~dp0"
start "" ".venv\Scripts\pythonw.exe" bridge.py

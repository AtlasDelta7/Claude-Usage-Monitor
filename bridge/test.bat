@echo off
cd /d "%~dp0"
.venv\Scripts\python bridge.py --test
pause

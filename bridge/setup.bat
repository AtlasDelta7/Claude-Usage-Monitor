@echo off
rem One-time setup: creates a private Python environment and installs the one dependency.
cd /d "%~dp0"
python -m venv .venv || (echo Python not found - install it from python.org first & pause & exit /b 1)
.venv\Scripts\python -m pip install --upgrade pip >nul
.venv\Scripts\pip install -r requirements.txt
if not exist config.json copy config.example.json config.json >nul
echo.
echo Done. Now edit config.json, then run test.bat
pause

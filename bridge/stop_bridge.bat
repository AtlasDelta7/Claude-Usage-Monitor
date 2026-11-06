@echo off
rem Stops the background bridge (any pythonw.exe running bridge.py).
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='pythonw.exe'\" | Where-Object { $_.CommandLine -like '*bridge.py*' } | ForEach-Object { Stop-Process -Id $_.ProcessId }"
echo Bridge stopped.

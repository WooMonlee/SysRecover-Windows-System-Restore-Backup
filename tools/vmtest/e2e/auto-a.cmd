@echo off
REM auto-a.cmd -- RunOnce wrapper: start the unattended GUI automation detached.
REM No /min: the console stays visible so a screendump shows live progress.
start "auto-a" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\zjgui\auto-a.ps1"

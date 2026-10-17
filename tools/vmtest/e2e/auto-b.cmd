@echo off
rem zj-e2e S5 leg: system B first boot - answer the "kept old system" startup
rem dialog with "delete old system" and log the outcome. See auto-b.ps1.
start "auto-b" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "C:\zjgui\auto-b.ps1"

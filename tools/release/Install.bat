@echo off
rem Installs TLMP (Torchlight co-op) into your Torchlight folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
pause

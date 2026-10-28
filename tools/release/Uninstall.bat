@echo off
rem Removes TLMP from your Torchlight folder and puts the game's own files back.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" -Uninstall %*
pause

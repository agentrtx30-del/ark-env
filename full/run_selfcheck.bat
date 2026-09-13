@echo off
cd /d "%~dp0.."
full\build\ark-full.exe --selfcheck
pause
@echo off
cd /d "%~dp0"
python viewer_pc.py
if errorlevel 1 pause

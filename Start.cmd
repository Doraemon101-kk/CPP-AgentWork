@echo off
setlocal
chcp 65001 >nul
title CppPilot
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher\Start.ps1"
if errorlevel 1 pause

@echo off
rem Orange Pi 4 Pro (A733): prepare an SD card that installs Windows 11 ARM64.
rem Opens the installer window (asks for administrator rights).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\Install-A733Windows.ps1" %*

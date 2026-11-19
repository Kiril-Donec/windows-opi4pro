@echo off
rem ==========================================================================
rem  Orange Pi 4 Pro (A733) Windows installer - stage 2, runs on the board.
rem  Started by winpeshl.ini from the WinPE that the PC installer put on the
rem  card. Loads the SD driver, applies Windows to the card's Windows
rem  partition, adds the A733 drivers, writes the boot files and reboots.
rem ==========================================================================
setlocal EnableExtensions EnableDelayedExpansion
title Orange Pi 4 Pro - Windows installer (stage 2)
color 1F
set "A=X:\a733"
set "LOG=X:\a733\install.log"
set "WIN="
set "ESP="

call :say "Orange Pi 4 Pro Windows installer, stage 2"
call :say "Do not power off the board until it reboots by itself."
wpeinit

call :say "[1/7] Loading the SD card driver"
drvload "%A%\drivers\sd\A733SdMmc.inf" >>"%LOG%" 2>&1

call :say "[2/7] Looking for the card partitions"
for /l %%i in (1,1,40) do (
    call :find
    if defined WIN if defined ESP goto :found
    if %%i==5 call :mountall
    if %%i==20 call :mountall
    ping -n 3 127.0.0.1 >nul
)
call :fail "card partitions not found (SD driver not loaded?)"
goto :end

:found
call :say "      Windows partition %WIN%:  boot partition %ESP%:"
if not exist "%WIN%:\a733install\config.cmd" (call :fail "%WIN%:\a733install\config.cmd missing" & goto :end)
call "%WIN%:\a733install\config.cmd"
call :say "      image %IMG%, index %IDX%"

if exist "%WIN%:\Windows\System32\config\SYSTEM" (
    call :say "      Windows files already present: skipping apply (earlier attempt)"
    goto :drivers
)
call :say "[3/7] Applying Windows - this takes 20-60 minutes on an SD card"
dism /English /Apply-Image /ImageFile:"%WIN%:\a733install\%IMG%" /Index:%IDX% /ApplyDir:%WIN%:\ /Compact
if errorlevel 1 (call :fail "DISM apply failed" & goto :end)

:drivers
call :say "[4/7] Adding the A733 drivers (SD card, Ethernet)"
dism /English /Image:%WIN%:\ /Add-Driver /Driver:"%A%\drivers" /Recurse /ForceUnsigned
if errorlevel 1 (call :fail "DISM add-driver failed" & goto :end)

call :say "[5/7] Settings for an SD card system"
rem (the Setup WinPE has no where.exe / findstr.exe / timeout.exe)
if not exist "%SystemRoot%\System32\reg.exe" (
    call :say "      reg.exe not in this WinPE: skipped (not critical)"
) else (
    reg load HKLM\A733SYS "%WIN%:\Windows\System32\config\SYSTEM" >>"%LOG%" 2>&1
    if not errorlevel 1 (
        rem no hibernation / fast startup: clean power-off, no hiberfil.sys
        reg add HKLM\A733SYS\ControlSet001\Control\Power /v HibernateEnabled /t REG_DWORD /d 0 /f >>"%LOG%" 2>&1
        reg add HKLM\A733SYS\ControlSet001\Control\Power /v HibernateEnabledDefault /t REG_DWORD /d 0 /f >>"%LOG%" 2>&1
        rem fewer small background writes to the card
        for %%S in (SysMain WSearch DiagTrack) do reg add HKLM\A733SYS\ControlSet001\Services\%%S /v Start /t REG_DWORD /d 4 /f >>"%LOG%" 2>&1
        reg unload HKLM\A733SYS >>"%LOG%" 2>&1
    )
    reg load HKLM\A733SW "%WIN%:\Windows\System32\config\SOFTWARE" >>"%LOG%" 2>&1
    if not errorlevel 1 (
        rem allow "I don't have internet" during setup
        reg add "HKLM\A733SW\Microsoft\Windows\CurrentVersion\OOBE" /v BypassNRO /t REG_DWORD /d 1 /f >>"%LOG%" 2>&1
        reg unload HKLM\A733SW >>"%LOG%" 2>&1
    )
)

call :say "[6/7] Writing the boot files"
rem Keep the installer boot files until bcdboot has succeeded.
if exist "%ESP%:\EFI.a733pe" rd /s /q "%ESP%:\EFI.a733pe"
ren "%ESP%:\EFI" EFI.a733pe
if errorlevel 1 (call :fail "cannot rename %ESP%:\EFI" & goto :end)
bcdboot %WIN%:\Windows /s %ESP%: /f UEFI
if errorlevel 1 (
    rd /s /q "%ESP%:\EFI" 2>nul
    ren "%ESP%:\EFI.a733pe" EFI
    call :fail "bcdboot failed - installer boot files restored, you can retry"
    goto :end
)
if not exist "%ESP%:\EFI\Boot\bootaa64.efi" (
    md "%ESP%:\EFI\Boot" 2>nul
    copy /y "%ESP%:\EFI\Microsoft\Boot\bootmgfw.efi" "%ESP%:\EFI\Boot\bootaa64.efi" >>"%LOG%" 2>&1
)
rem The A733 drivers are test-signed.
bcdedit /store "%ESP%:\EFI\Microsoft\Boot\BCD" /set {default} testsigning on >>"%LOG%" 2>&1
bcdedit /store "%ESP%:\EFI\Microsoft\Boot\BCD" /set {default} nointegritychecks on >>"%LOG%" 2>&1
bcdedit /store "%ESP%:\EFI\Microsoft\Boot\BCD" /set {bootmgr} timeout 2 >>"%LOG%" 2>&1

call :say "[7/7] Cleaning up the installer files"
rd /s /q "%ESP%:\EFI.a733pe"
rd /s /q "%ESP%:\sources" 2>nul
rd /s /q "%ESP%:\boot" 2>nul
del /f /q "%ESP%:\a733-esp.tag" 2>nul
copy /y "%LOG%" "%WIN%:\a733-install.log" >nul 2>&1
rd /s /q "%WIN%:\a733install"

call :say "Done. Rebooting into Windows setup in 15 seconds."
ping -n 16 127.0.0.1 >nul
wpeutil reboot
goto :end

rem --------------------------------------------------------------------------
:find
for %%L in (C D E F G H I J K L M N O P Q R S T U V W Y Z) do (
    if exist "%%L:\a733install\a733.tag" set "WIN=%%L"
    if exist "%%L:\a733-esp.tag" set "ESP=%%L"
)
exit /b 0

:mountall
rem Volumes on a removable card may have no letter in WinPE: give them one.
set "FREE=M N O P Q R T U V Y"
for /f "tokens=1" %%V in ('mountvol') do (
    set "T=%%V"
    if "!T:~0,10!"=="\\?\Volume" (
        for /f "tokens=1*" %%a in ("!FREE!") do (
            mountvol %%a: %%V >nul 2>&1
            if not errorlevel 1 set "FREE=%%b"
        )
    )
)
exit /b 0

:say
echo %~1
>>"%LOG%" echo %~1
exit /b 0

:fail
color 4F
call :say "ERROR: %~1"
call :say "Log: %LOG%. Take a photo of this screen for a bug report."
if defined WIN copy /y "%LOG%" "%WIN%:\a733-install.log" >nul 2>&1
exit /b 1

:end
echo.
echo Type "wpeutil reboot" to restart or "wpeutil shutdown" to power off.
endlocal

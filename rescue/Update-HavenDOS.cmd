@echo off
rem ====================================================================
rem  Update-HavenDOS.cmd -- put the HavenDOS update on the EFI System
rem  Partition. Double-click it and approve the prompt. That is all.
rem
rem  It needs havendos.elf and update.inf beside it, and it hands both
rem  to Stage-HavenDOS-Update.ps1, which writes exactly two files:
rem
rem      \EFI\HAVENDOS\update\havendos.elf
rem      \EFI\HAVENDOS\update\update.inf
rem
rem  Nothing else on the ESP is touched. \EFI\Microsoft, \EFI\BOOT and
rem  the BCD are not gone near.
rem
rem  This wrapper exists because the manual route was five steps: unzip,
rem  open an Administrator PowerShell in the right folder, set the
rem  execution policy, type the script name with two path arguments,
rem  then boot. It does the first four.
rem ====================================================================
setlocal EnableExtensions
cd /d "%~dp0"

echo HavenDOS update stager
echo.

rem --- the payload must be beside this file -------------------------------
if not exist "%~dp0havendos.elf" goto :nokernel
if not exist "%~dp0update.inf" goto :nomanifest
if not exist "%~dp0Stage-HavenDOS-Update.ps1" goto :noscript

rem --- elevate if we are not already admin -------------------------------
rem  fltmc is the cheap reliable admin test: it fails for a standard user.
fltmc >nul 2>&1
if errorlevel 1 (
    echo Asking for Administrator rights -- mountvol needs them.
    rem  The path goes through the environment rather than being pasted
    rem  into the PowerShell command line: a single-quoted literal would
    rem  break on a folder whose name contains an apostrophe, and a
    rem  PowerShell variable is passed as one argument whatever spaces
    rem  it holds.
    set "HD_SELF=%~f0"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath $env:HD_SELF -Verb RunAs"
    exit /b 0
)

rem --- pick a drive letter the ESP can be mounted on ---------------------
rem  Stage-HavenDOS-Update.ps1 refuses if its letter is already in use, so
rem  the letter is chosen here rather than left to fail on a machine that
rem  happens to have an S: mapped.
set "ESPLETTER="
for %%L in (S T U V W Y Z Q R N M) do (
    if not defined ESPLETTER if not exist %%L:\ set "ESPLETTER=%%L"
)
if not defined ESPLETTER goto :noletter
echo Mounting the EFI System Partition on %ESPLETTER%: while working.
echo.

rem --- stage it ----------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Stage-HavenDOS-Update.ps1" -Kernel "%~dp0havendos.elf" -Manifest "%~dp0update.inf" -MountLetter %ESPLETTER%
if errorlevel 1 goto :failed

echo.
echo ====================================================================
echo  Staged. Now boot HavenDOS and type:   update
echo.
echo  Use a boot entry WITHOUT 'live' -- on the ISO that is entry 4,
echo  "Update installed system from the ESP". A live shell cannot see
echo  the HavenDOS partition and update will refuse with
echo  "No FAT16 partition found".
echo ====================================================================
echo.
pause
exit /b 0

:nokernel
echo ERROR: havendos.elf is not in this folder.
echo        Put it beside this file and run it again.
goto :bail

:nomanifest
echo ERROR: update.inf is not in this folder.
echo        It must be the one built with this kernel -- the staging
echo        script checks its size and crc32 against havendos.elf and
echo        refuses if they disagree.
goto :bail

:noscript
echo ERROR: Stage-HavenDOS-Update.ps1 is not in this folder.
echo        All four files belong in the same folder.
goto :bail

:noletter
echo ERROR: no free drive letter to mount the ESP on.
echo        Disconnect a mapped drive and try again.
goto :bail

:failed
echo.
echo The update was NOT staged. The error above says why; the most
echo common ones are a kernel and manifest that do not match, and
echo Windows not having an EFI System Partition because the machine
echo booted in legacy BIOS mode.
goto :bail

:bail
echo.
pause
exit /b 1

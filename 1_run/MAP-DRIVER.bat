@echo off
setlocal
cd /d "%~dp0"

rem kdmapper calls AdjustTokenPrivileges for SeDebugPrivilege. That privilege is
rem absent from a UAC-filtered token, so the mapper must start elevated. Same
rem relaunch pattern as ..\APPLY_AND_RUN.bat.
net session >nul 2>&1
if errorlevel 1 (
    powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    if not errorlevel 1 exit /b 0
    echo Could not request administrator rights. Run this file as administrator.
    pause
    exit /b 1
)

rem EasyAntiCheat strips token privileges from live processes and rejects manual
rem mapping. Map before the game starts, with EAC and the overlay closed.
powershell.exe -NoProfile -Command "$p = Get-Process -Name 'EasyAntiCheat*','RobRun*' -ErrorAction SilentlyContinue; if ($p) { exit 1 }"
if errorlevel 1 (
    echo Close Rob Run, its overlay and EasyAntiCheat, then run this file again.
    pause
    exit /b 1
)

if not exist "%~dp0wm_shared.sys" (
    echo wm_shared.sys is missing next to this script.
    pause
    exit /b 1
)
if not exist "%~dp0kdmapper.exe" (
    echo kdmapper.exe is missing next to this script.
    pause
    exit /b 1
)

echo Elevated token confirmed. Mapping wm_shared.sys.
"%~dp0kdmapper.exe" "%~dp0wm_shared.sys"
if errorlevel 1 (
    echo.
    echo MAP_FAIL
    pause
    exit /b 1
)

echo.
echo MAP_OK - wm_shared.sys is resident until reboot.
pause
exit /b 0

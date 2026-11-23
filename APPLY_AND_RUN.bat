@echo off
setlocal
cd /d "%~dp0"

rem The update replaces files next to this script. Ask for administrator rights
rem once instead of failing halfway through a file copy.
net session >nul 2>&1
if errorlevel 1 (
    powershell.exe -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
    if not errorlevel 1 exit /b 0
    echo Could not request administrator rights. Run this file as administrator.
    pause
    exit /b 1
)

if defined RRToolTarget (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ApplyUpdates.ps1" -Target "%RRToolTarget%"
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0ApplyUpdates.ps1"
)
if errorlevel 1 (
    echo Update failed. Read the error above.
    pause
    exit /b 1
)
powershell.exe -NoProfile -Command "if (Get-Process -Name EasyAntiCheat_EOS_Setup -ErrorAction SilentlyContinue) { exit 1 }"
if errorlevel 1 (
    echo An EasyAntiCheat installer is already open. Complete or close it, then run this file again.
    pause
    exit /b 1
)

rem Fix EAC from the Steam library that actually holds the game. RRToolSteam
rem overrides the search for libraries outside the usual drives. Plain
rem sequential checks keep the batch free of labels, which cmd resolves per
rem parsed line and which made the earlier subroutine unreliable.
set "FIXEAC="
if defined RRToolSteam if exist "%RRToolSteam%\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=%RRToolSteam%\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "%ProgramFiles(x86)%\Steam\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=%ProgramFiles(x86)%\Steam\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "%ProgramFiles%\Steam\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=%ProgramFiles%\Steam\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "C:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=C:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "D:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=D:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "E:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=E:\SteamLibrary\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "C:\Steam\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=C:\Steam\steamapps\common\Rob Run\fix_eac.bat"
if not defined FIXEAC if exist "D:\Steam\steamapps\common\Rob Run\fix_eac.bat" set "FIXEAC=D:\Steam\steamapps\common\Rob Run\fix_eac.bat"
if defined FIXEAC (
    call "%FIXEAC%"
) else (
    echo Rob Run fix_eac.bat was not found. Set RRToolSteam to your Steam folder to run it.
)

start "" /D "%~dp01_run" "%~dp01_run\RobRunAIO.exe"
exit /b 0

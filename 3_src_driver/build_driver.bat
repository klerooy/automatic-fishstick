@echo off
setlocal EnableDelayedExpansion
rem Build WmShared driver.
rem Searches: traditional WDK install, NuGet packages, manual WM_WDK override.
rem Usage: build_driver.bat
rem    or: set WM_WDK=C:\path\to\wdk\root && build_driver.bat
cd /d "%~dp0"

rem --- Find MSVC ---
set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Insiders"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found. Install VS 2022 C++ workload.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo [ERROR] vcvars64 failed & exit /b 1 )

rem --- Find WDK headers (km\ntddk.h) ---
set "KM_INC="
set "SH_INC="
set "KM_LIB="

rem 0) Manual override: WM_WDK points to the kit root containing Include\ and Lib\
if defined WM_WDK (
    for /d %%D in ("%WM_WDK%\Include\*") do (
        if exist "%%D\km\ntddk.h" (
            set "KM_INC=%%D\km"
            set "SH_INC=%%D\shared"
        )
    )
    for /d %%D in ("%WM_WDK%\Lib\*") do (
        if exist "%%D\km\x64\ntoskrnl.lib" set "KM_LIB=%%D\km\x64"
    )
    if defined KM_INC if defined KM_LIB goto found_wdk
)

rem 1) Traditional WDK: C:\Program Files (x86)\Windows Kits\10
set "KIT10=C:\Program Files (x86)\Windows Kits\10"
if exist "%KIT10%\Include" (
    for /f "delims=" %%V in ('dir /b /ad /o-n "%KIT10%\Include" 2^>nul') do (
        if not defined KM_INC if exist "%KIT10%\Include\%%V\km\ntddk.h" (
            set "KM_INC=%KIT10%\Include\%%V\km"
            set "SH_INC=%KIT10%\Include\%%V\shared"
        )
    )
    for /f "delims=" %%V in ('dir /b /ad /o-n "%KIT10%\Lib" 2^>nul') do (
        if not defined KM_LIB if exist "%KIT10%\Lib\%%V\km\x64\ntoskrnl.lib" (
            set "KM_LIB=%KIT10%\Lib\%%V\km\x64"
        )
    )
)
if defined KM_INC if defined KM_LIB goto found_wdk

rem 2) NuGet packages in .\packages (relative to this script)
set "NUGET_DIR=%~dp0packages"
if exist "%NUGET_DIR%" (
    for /d %%P in ("%NUGET_DIR%\Microsoft.Windows.WDK.x64.*") do (
        for /d %%V in ("%%P\c\Include\*") do (
            if not defined KM_INC if exist "%%V\km\ntddk.h" (
                set "KM_INC=%%V\km"
                set "SH_INC=%%V\shared"
            )
        )
        for /d %%V in ("%%P\c\Lib\*") do (
            if not defined KM_LIB if exist "%%V\km\x64\ntoskrnl.lib" (
                set "KM_LIB=%%V\km\x64"
            )
        )
    )
)
if defined KM_INC if defined KM_LIB goto found_wdk

rem 3) NuGet packages in parent directory (solution-level restore)
set "NUGET_DIR=%~dp0..\packages"
if exist "%NUGET_DIR%" (
    for /d %%P in ("%NUGET_DIR%\Microsoft.Windows.WDK.x64.*") do (
        for /d %%V in ("%%P\c\Include\*") do (
            if not defined KM_INC if exist "%%V\km\ntddk.h" (
                set "KM_INC=%%V\km"
                set "SH_INC=%%V\shared"
            )
        )
        for /d %%V in ("%%P\c\Lib\*") do (
            if not defined KM_LIB if exist "%%V\km\x64\ntoskrnl.lib" (
                set "KM_LIB=%%V\km\x64"
            )
        )
    )
)
if defined KM_INC if defined KM_LIB goto found_wdk

rem 4) NuGet global cache: %USERPROFILE%\.nuget\packages
set "NUGET_DIR=%USERPROFILE%\.nuget\packages\microsoft.windows.wdk.x64"
if exist "%NUGET_DIR%" (
    for /d %%P in ("%NUGET_DIR%\*") do (
        for /d %%V in ("%%P\c\Include\*") do (
            if not defined KM_INC if exist "%%V\km\ntddk.h" (
                set "KM_INC=%%V\km"
                set "SH_INC=%%V\shared"
            )
        )
        for /d %%V in ("%%P\c\Lib\*") do (
            if not defined KM_LIB if exist "%%V\km\x64\ntoskrnl.lib" (
                set "KM_LIB=%%V\km\x64"
            )
        )
    )
)
if defined KM_INC if defined KM_LIB goto found_wdk

echo [ERROR] WDK headers not found.
echo.
echo Fix option A - NuGet (no MSI install needed):
echo   nuget install Microsoft.Windows.WDK.x64 -Version 10.0.26100.6584 -OutputDirectory packages
echo   nuget install Microsoft.Windows.SDK.CPP.x64 -Version 10.0.26100.6584 -OutputDirectory packages
echo   nuget install Microsoft.Windows.SDK.CPP -Version 10.0.26100.6584 -OutputDirectory packages
echo.
echo Fix option B - WDK installer:
echo   1. VS Installer - Individual Components - Windows Driver Kit
echo   2. Run wdksetup.exe from https://go.microsoft.com/fwlink/?linkid=2335869
echo.
echo Fix option C - Manual path:
echo   set WM_WDK=C:\Program Files (x86)\Windows Kits\10
echo   build_driver.bat
exit /b 1

:found_wdk
echo WDK include: %KM_INC%
echo WDK libs:    %KM_LIB%

rem --- Verify CRT include for kernel (ucrt) ---
set "UCRT_INC="
if exist "%KIT10%\Include" (
    for /f "delims=" %%V in ('dir /b /ad /o-n "%KIT10%\Include" 2^>nul') do (
        if not defined UCRT_INC if exist "%KIT10%\Include\%%V\ucrt\stdio.h" (
            set "UCRT_INC=%KIT10%\Include\%%V\ucrt"
        )
    )
)

rem --- Compile ---
if not exist x64\Release mkdir x64\Release

cl /nologo /O2 /kernel /GS- /W3 /EHsc- /D_AMD64_ /D_WIN64 ^
/I "%KM_INC%" /I "%SH_INC%" /I "." /c wm_driver.cpp /Fo:x64\Release\wm_driver.obj || goto fail

cl /nologo /O2 /kernel /GS- /W3 /EHsc- /D_AMD64_ /D_WIN64 ^
/I "%KM_INC%" /I "%SH_INC%" /I "." /c wm_resolver.cpp /Fo:x64\Release\wm_resolver.obj || goto fail

rem --- Link ---
link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /MACHINE:X64 ^
     /NODEFAULTLIB /MERGE:.rdata=.text ^
     /OUT:x64\Release\wm_shared.sys ^
     x64\Release\wm_driver.obj x64\Release\wm_resolver.obj ^
     "%KM_LIB%\ntoskrnl.lib" "%KM_LIB%\hal.lib" "%KM_LIB%\BufferOverflowK.lib"
if errorlevel 1 goto fail

copy /Y x64\Release\wm_shared.sys wm_shared.sys >nul
echo.
echo BUILD_OK  --^>  wm_shared.sys
echo Map: kd2.exe --auto wm_shared.sys
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1
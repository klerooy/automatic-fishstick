@echo off
setlocal
rem Build rr_exec.sys - the APC channel driver for the managed-call transport.
rem
rem No WDK on this machine, so there are no kernel headers and no ntoskrnl.lib:
rem
rem   1. dumpbin lists the running kernel's export table,
rem   2. make_importlib.py turns it into ntoskrnl.def,
rem   3. lib.exe makes the import library the linker needs,
rem   4. cl.exe compiles the headerless driver with no CRT,
rem   5. link.exe emits a native image with DriverEntry as the entry point.
rem
rem The result is a normal PE32+ image with an ntoskrnl import table and
rem relocations, which is what kd2.exe maps. Nothing is loaded here.
cd /d "%~dp0"

set "VS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community"
    "C:\Program Files\Microsoft Visual Studio\18\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Community"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
) do if not defined VS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set "VS=%%~P"
if not defined VS (
    echo [ERROR] vcvars64.bat not found - edit the VS detection above.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 ( echo VCVARS_FAIL & exit /b 1 )

echo [1/4] kernel exports
dumpbin /nologo /exports "%SystemRoot%\System32\ntoskrnl.exe" > nt_exports.txt || goto fail
echo [2/4] import library
python make_importlib.py nt_exports.txt ntoskrnl.def || goto fail
lib /nologo /def:ntoskrnl.def /machine:x64 /out:ntoskrnl.lib || goto fail
if not exist ntoskrnl.lib ( echo IMPORTLIB_FAIL & goto fail )

echo [3/4] compile
rem /Zl keeps default-library directives out of the object; /GS- and the
rem exception flags remove every CRT dependency. The driver has its own zero
rem and its own exit paths.
cl /nologo /c /O2 /GS- /Gm- /Gy /Zl /W3 /D_AMD64_ /DAMD64 ^
   /Fo:rr_exec.obj rr_exec.c || goto fail

echo [4/4] link
rem No CRT, no default libs, native subsystem, DriverEntry as the entry point.
link /nologo /DRIVER /SUBSYSTEM:NATIVE /MACHINE:X64 /ENTRY:DriverEntry ^
     /NODEFAULTLIB /ALIGN:0x1000 /FIXED:NO /OPT:REF /OPT:ICF ^
     /OUT:rr_exec.sys rr_exec.obj ntoskrnl.lib || goto fail

echo.
echo BUILD_OK  -^>  rr_exec.sys
echo Map a copy with kd2.exe (see BUILD.md, "Map the APC driver").
exit /b 0

:fail
echo BUILD_FAIL
exit /b 1

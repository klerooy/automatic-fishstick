@echo off
setlocal
:: Loads rr_exec.sys through the normal service path and pings the channel.
:: Needs one of: a mapped image (kd2), driver signature enforcement off for this
:: boot, or testsigning. Otherwise StartService answers 577 (0x241).
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" >nul 2>&1
  exit /b
)
set HERE=%~dp0
set PROBE=%HERE%..\2_src_port\rr_probe.exe
echo [1/3] load %HERE%rr_exec.sys
"%PROBE%" load "%HERE%rr_exec.sys"
echo [2/3] service state
sc query rr_exec | findstr /i "STATE"
echo [3/3] channel ping
"%PROBE%" chan
endlocal

@echo off
setlocal
cd /d "%~dp0"
set "PS1=%~dp0switch_renderer.ps1"
if not exist "%PS1%" set "PS1=%~dp0EFMI-Vulkan\switch_renderer.ps1"
if not exist "%PS1%" (
    echo switch_renderer.ps1 not found next to this file or in EFMI-Vulkan\ - see README.txt
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Mode toggle %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%

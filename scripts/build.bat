@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1"
if errorlevel 1 (
    echo.
    echo VRFusion build failed. See the error above.
    pause
    exit /b 1
)
echo.
echo VRFusion build succeeded.
pause

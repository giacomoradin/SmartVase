@echo off
set PROJECT_DIR=C:\SmartVase_prject\smartvase\firmware\2_platform-controller_mega\Radin_Giacomo_SmartVase_PlatformController_ArduinoMega
echo Building Arduino-Mega project in %PROJECT_DIR%...
"C:\Users\Giacomo Radin\.platformio\penv\Scripts\pio.exe" run -d %PROJECT_DIR%
if %errorlevel% neq 0 (
    echo Arduino-Mega build FAILED!
) else (
    echo Arduino-Mega build SUCCEEDED!
)
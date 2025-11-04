@echo off
set PROJECT_DIR=C:\SmartVase_prject\smartvase\firmware\1_esp32-hub\Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard
echo Building ESP32-Hub project in %PROJECT_DIR%...
pio run -d %PROJECT_DIR%
if %errorlevel% neq 0 (
    echo ESP32-Hub build FAILED!
) else (
    echo ESP32-Hub build SUCCEEDED!
)
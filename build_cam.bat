@echo off
set PROJECT_DIR=C:\SmartVase_prject\smartvase\firmware\3_esp32-cam\Radin_Giacomo_SmartVase_VisionCoProcessor_ESP32CAM
echo Building ESP32-CAM project in %PROJECT_DIR%...
pio run -d %PROJECT_DIR%
if %errorlevel% neq 0 (
    echo ESP32-CAM build FAILED!
) else (
    echo ESP32-CAM build SUCCEEDED!
)
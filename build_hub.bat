@echo off
set PROJECT_DIR=%~dp0firmware\1_esp32-hub\Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard
echo Building ESP32-Hub project in %PROJECT_DIR%...
"%USERPROFILE%\.platformio\penv\Scripts\pio.exe" run -d "%PROJECT_DIR%" %*
if %errorlevel% neq 0 (
    echo ESP32-Hub build FAILED!
) else (
    echo ESP32-Hub build SUCCEEDED!
)

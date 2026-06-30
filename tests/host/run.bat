@echo off
REM Compila ed esegue gli unit test host (g++). Offline-friendly.
REM Richiede g++ nel PATH (es. Strawberry Perl: C:\Strawberry\c\bin).
setlocal enabledelayedexpansion
set DIR=%~dp0
if "%CXX%"=="" set CXX=g++

set FAIL=0
call :run_one test_crc16
call :run_one test_crc_utils
call :run_one test_command_policy
call :run_one test_sensor_policy
call :run_one test_persistence
if !FAIL! neq 0 ( echo SUITE: FAILED & exit /b 1 )
echo SUITE: ALL PASSED
exit /b 0

:run_one
echo === %1 ===
%CXX% -std=c++17 -Wall -Wextra -I "%DIR%stubs" -I "%DIR%..\..\firmware\1_esp32-hub\Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard\include" "%DIR%%1.cpp" -o "%DIR%build_%1.exe"
if errorlevel 1 ( echo [BUILD FAILED] %1 & set FAIL=1 & goto :eof )
"%DIR%build_%1.exe"
if errorlevel 1 set FAIL=1
echo.
goto :eof

@echo off
setlocal
rem =================================================================
rem Script to regenerate the C/H files from smartvase.proto
rem Version: 2.3 (English output; added path for nanopb.proto)
rem
rem INSTRUCTIONS: the script is self-contained. Just run it
rem from this folder (infra/smartvase-proto).
rem =================================================================

echo.
echo   =======================================================
echo     SmartVase - Protocol Buffers Code Generator
echo   =======================================================
echo.

rem --- STEP 1: Prerequisite checks ---
if not exist ".\venv\Scripts\python.exe" (
    echo [ERROR] Virtual environment 'venv' not found or incomplete!
    goto :end
)
if not exist ".\smartvase.proto" (
    echo [ERROR] File 'smartvase.proto' not found!
    goto :end
)

rem --- Key paths ---
set PYTHON_EXE=.\venv\Scripts\python.exe
set PLUGIN_EXE=.\venv\Scripts\protoc-gen-nanopb.exe
set NANOPB_PROTO_PATH=.\venv\Lib\site-packages\nanopb\generator\proto

if not exist "%PLUGIN_EXE%" (
    echo [ERROR] 'nanopb' plugin not found. Run the installation:
    echo   .\venv\Scripts\pip.exe install nanopb protobuf grpcio-tools
    goto :end
)
if not exist "%NANOPB_PROTO_PATH%\nanopb.proto" (
    echo [ERROR] File 'nanopb.proto' not found in the expected path.
    echo   Check the 'nanopb' package installation.
    goto :end
)

echo [INFO] Prerequisites verified.

rem --- STEP 2: Code generation ---
echo [INFO] Compiling 'smartvase.proto'...
echo.

rem Add the path for nanopb.proto with the -I"%NANOPB_PROTO_PATH%" flag
%PYTHON_EXE% -m grpc_tools.protoc -I. -I"%NANOPB_PROTO_PATH%" --plugin=protoc-gen-nanopb=%PLUGIN_EXE% --nanopb_out=. smartvase.proto

rem --- STEP 3: Result check ---
if %errorlevel% neq 0 (
    echo.
    echo [ERROR] Code generation failed.
    echo   Check the error messages above.
    echo.
) else (
    echo.
    echo [SUCCESS] Files 'smartvase.pb.c' and 'smartvase.pb.h'
    echo           were generated/updated successfully!
    echo.
    echo [REMINDER] Patch the generated smartvase.pb.h:
    echo            #include ^<pb.h^>  --^>  #include "pb.h"
    echo            The pre-build hook syncs the copies into the firmwares.
    echo.
)

:end
echo [INFO] Done.
pause

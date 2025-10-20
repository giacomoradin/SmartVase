@echo off
setlocal
rem =================================================================
rem Script per la rigenerazione dei file C/H da smartvase.proto
rem Autore: Gemini
rem Versione: 2.2 (Aggiunto path per nanopb.proto)
rem
rem ISTRUZIONI: Lo script e' ora autonomo. Basta eseguirlo
rem dalla cartella principale del progetto.
rem =================================================================

echo.
echo   =======================================================
echo     SmartVase - Generatore di Codice Protocol Buffers
echo   =======================================================
echo.

rem --- PASSO 1: Controllo dei prerequisiti ---
if not exist ".\venv\Scripts\python.exe" (
    echo [ERRORE] Ambiente virtuale 'venv' non trovato o incompleto!
    goto :end
)
if not exist ".\smartvase.proto" (
    echo [ERRORE] File 'smartvase.proto' non trovato!
    goto :end
)

rem --- Definiamo i percorsi chiave ---
set PYTHON_EXE=.\venv\Scripts\python.exe
set PLUGIN_EXE=.\venv\Scripts\protoc-gen-nanopb.exe
set NANOPB_PROTO_PATH=.\venv\Lib\site-packages\nanopb\generator\proto

if not exist "%PLUGIN_EXE%" (
    echo [ERRORE] Plugin 'nanopb' non trovato. Esegui l'installazione:
    echo   .\venv\Scripts\pip.exe install nanopb protobuf grpcio-tools
    goto :end
)
if not exist "%NANOPB_PROTO_PATH%\nanopb.proto" (
    echo [ERRORE] File 'nanopb.proto' non trovato nel percorso atteso.
    echo   Verifica l'installazione del pacchetto 'nanopb'.
    goto :end
)

echo [INFO] Prerequisiti verificati.

rem --- PASSO 2: Generazione del codice ---
echo [INFO] Avvio della compilazione di 'smartvase.proto'...
echo.

rem Aggiungiamo il path per nanopb.proto con il flag -I"%NANOPB_PROTO_PATH%"
%PYTHON_EXE% -m grpc_tools.protoc -I. -I"%NANOPB_PROTO_PATH%" --plugin=protoc-gen-nanopb=%PLUGIN_EXE% --nanopb_out=. smartvase.proto

rem --- PASSO 3: Verifica del risultato ---
if %errorlevel% neq 0 (
    echo.
    echo [ERRORE] La generazione del codice e' fallita.
    echo   Controlla i messaggi di errore qui sopra.
    echo.
) else (
    echo.
    echo [SUCCESSO!] I file 'smartvase.pb.c' e 'smartvase.pb.h'
    echo             sono stati generati/aggiornati con successo!
    echo.
)

:end
echo [INFO] Processo completato.
pause
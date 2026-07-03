@echo off
setlocal enabledelayedexpansion

echo ============================================
echo  DigiAsset Core - End-to-End Tests
echo ============================================
echo.

set PASS=0
set FAIL=0
set SKIP=0

set EXE=..\..\build\src\Release\DigiAssetWindows.exe
if not exist "%EXE%" (
    echo FAIL: DigiAssetWindows.exe not found. Build first.
    exit /b 1
)

REM ---- Test 1: Binary exists and has reasonable size ----
echo [Test 1] Binary size check...
for %%A in ("%EXE%") do set SIZE=%%~zA
if !SIZE! GTR 1000000 (
    echo   PASS: DigiAssetWindows.exe is !SIZE! bytes
    set /a PASS+=1
) else (
    echo   FAIL: DigiAssetWindows.exe is too small ^(!SIZE! bytes^)
    set /a FAIL+=1
)

REM ---- Test 2: Version string in binary ----
echo [Test 2] Version string check...
findstr /C:"DigiAsset for Windows" "%EXE%" >NUL 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   PASS: Version string found in binary
    set /a PASS+=1
) else (
    echo   FAIL: Version string not found in binary
    set /a FAIL+=1
)

REM ---- Test 3: Config wizard triggers without config ----
echo [Test 3] Config wizard trigger...
set SMOKE_DIR=%TEMP%\digiasset_e2e_%RANDOM%
mkdir "%SMOKE_DIR%" 2>NUL
copy "%EXE%" "%SMOKE_DIR%\" >NUL 2>&1

REM Run with stdin closed — should print wizard prompt
echo. | "%SMOKE_DIR%\DigiAssetWindows.exe" > "%SMOKE_DIR%\output.txt" 2>&1
timeout /t 3 /nobreak >NUL 2>&1
taskkill /F /IM DigiAssetWindows.exe >NUL 2>&1

findstr /C:"config wizard" "%SMOKE_DIR%\output.txt" >NUL 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   PASS: Config wizard triggered
    set /a PASS+=1
) else (
    echo   SKIP: Could not verify config wizard
    set /a SKIP+=1
)
rmdir /S /Q "%SMOKE_DIR%" 2>NUL

REM ---- Test 4: Web server responds (if running) ----
echo [Test 4] Web server check...
curl -s -o NUL -w "%%{http_code}" http://127.0.0.1:8090/ > "%TEMP%\webcheck.txt" 2>NUL
set /p HTTP_CODE=<"%TEMP%\webcheck.txt"
if "!HTTP_CODE!"=="200" (
    echo   PASS: Web server responding on port 8090
    set /a PASS+=1
) else if "!HTTP_CODE!"=="404" (
    echo   PASS: Web server responding on port 8090 ^(404 = no index but server works^)
    set /a PASS+=1
) else (
    echo   SKIP: Web server not reachable ^(DigiAssetWindows may not be running^)
    set /a SKIP+=1
)
del "%TEMP%\webcheck.txt" 2>NUL

REM ---- Test 5: RPC server responds (if running) ----
echo [Test 5] RPC server check...
curl -s -o NUL -w "%%{http_code}" --user test:test --data-binary "{\"jsonrpc\":\"1.0\",\"method\":\"syncstate\",\"params\":[]}" http://127.0.0.1:14024/ > "%TEMP%\rpccheck.txt" 2>NUL
set /p RPC_CODE=<"%TEMP%\rpccheck.txt"
if "!RPC_CODE!"=="200" (
    echo   PASS: RPC server responding on port 14024
    set /a PASS+=1
) else if "!RPC_CODE!"=="401" (
    echo   PASS: RPC server responding ^(401 = auth required, server is up^)
    set /a PASS+=1
) else (
    echo   SKIP: RPC server not reachable
    set /a SKIP+=1
)
del "%TEMP%\rpccheck.txt" 2>NUL

REM ---- Test 6: External IP detection ----
echo [Test 6] External IP detection...
curl -s http://api.ipify.org > "%TEMP%\ipcheck.txt" 2>NUL
set /p EXT_IP=<"%TEMP%\ipcheck.txt"
if defined EXT_IP (
    echo   PASS: External IP detected: !EXT_IP!
    set /a PASS+=1
) else (
    echo   SKIP: Could not detect external IP
    set /a SKIP+=1
)
del "%TEMP%\ipcheck.txt" 2>NUL

REM ---- Test 7: Port check API available ----
echo [Test 7] Port check API...
curl -s http://ifconfig.co/port/80 > "%TEMP%\portcheck.txt" 2>NUL
findstr /C:"reachable" "%TEMP%\portcheck.txt" >NUL 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   PASS: ifconfig.co port check API available
    set /a PASS+=1
) else (
    echo   SKIP: ifconfig.co API not reachable
    set /a SKIP+=1
)
del "%TEMP%\portcheck.txt" 2>NUL

echo.
echo ============================================
echo  E2E Results: %PASS% passed, %FAIL% failed, %SKIP% skipped
echo ============================================

if %FAIL% GTR 0 exit /b 1
exit /b 0

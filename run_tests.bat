@echo off
setlocal enabledelayedexpansion

set PASS=0
set FAIL=0
set SKIP=0
set TOTAL_TESTS=0

echo ============================================
echo  DigiAsset for Windows - Test Suite
echo ============================================
echo.

REM --- TIER 1: Unit Tests ---
echo [TIER 1] Unit Tests (no external services)
echo -------------------------------------------

set UNIT_EXE=build\tests\Release\Unit_Tests_run.exe
if not exist "%UNIT_EXE%" (
    set UNIT_EXE=build\tests\Debug\Unit_Tests_run.exe
)

if not exist "%UNIT_EXE%" (
    echo   SKIP: Unit test binary not found. Build with -DBUILD_TEST=ON first.
    set /a SKIP+=1
    goto :tier2
)

"%UNIT_EXE%" --gtest_output=xml:test_results_unit.xml
if !ERRORLEVEL! EQU 0 (
    echo   PASS: All unit tests passed
    set /a PASS+=1
) else (
    echo   FAIL: Unit tests failed ^(exit code !ERRORLEVEL!^)
    set /a FAIL+=1
)
echo.

:tier2
REM --- TIER 2: Binary Checks ---
echo [TIER 2] Binary Checks
echo -------------------------------------------

set EXE=build\src\Release\DigiAssetWindows.exe
set CLI=build\cli\Release\DigiAssetWindows-cli.exe

if exist "%EXE%" (
    for %%A in ("%EXE%") do set SIZE=%%~zA
    if !SIZE! GTR 1000000 (
        echo   PASS: DigiAssetWindows.exe exists ^(!SIZE! bytes^)
        set /a PASS+=1
    ) else (
        echo   FAIL: DigiAssetWindows.exe too small
        set /a FAIL+=1
    )
) else (
    echo   FAIL: DigiAssetWindows.exe not found
    set /a FAIL+=1
)

if exist "%CLI%" (
    echo   PASS: DigiAssetWindows-cli.exe exists
    set /a PASS+=1
) else (
    echo   SKIP: DigiAssetWindows-cli.exe not found
    set /a SKIP+=1
)
echo.

:tier3
REM --- TIER 3: Fuzz Self-Tests ---
echo [TIER 3] Fuzz Self-Tests
echo -------------------------------------------

set FUZZ_BLOB=tests\fuzz\fuzz_blob.exe
set FUZZ_CONFIG=tests\fuzz\fuzz_config.exe
set FUZZ_DB=tests\fuzz\fuzz_db.exe

REM Try to build fuzz tests if not already built
if not exist "%FUZZ_BLOB%" (
    echo   Building fuzz tests...
    pushd tests\fuzz
    cl /EHsc /I..\..\src /nologo FuzzBlob.cpp ..\..\src\Blob.cpp /Fe:fuzz_blob.exe >NUL 2>&1
    popd
)

if exist "%FUZZ_BLOB%" (
    echo. | "%FUZZ_BLOB%"
    if !ERRORLEVEL! EQU 0 (
        set /a PASS+=1
    ) else (
        echo   FAIL: Blob fuzz self-test
        set /a FAIL+=1
    )
) else (
    echo   SKIP: Blob fuzz binary not available
    set /a SKIP+=1
)

if not exist "%FUZZ_CONFIG%" (
    pushd tests\fuzz
    cl /EHsc /I..\..\src /nologo FuzzConfig.cpp ..\..\src\Config.cpp /Fe:fuzz_config.exe >NUL 2>&1
    popd
)

if exist "%FUZZ_CONFIG%" (
    echo. | "%FUZZ_CONFIG%"
    if !ERRORLEVEL! EQU 0 (
        set /a PASS+=1
    ) else (
        echo   FAIL: Config fuzz self-test
        set /a FAIL+=1
    )
) else (
    echo   SKIP: Config fuzz binary not available
    set /a SKIP+=1
)
echo.

:tier4
REM --- TIER 4: E2E Tests (if app is running) ---
echo [TIER 4] End-to-End Tests
echo -------------------------------------------

curl -s -o NUL -w "%%{http_code}" http://127.0.0.1:8090/ > "%TEMP%\e2e_web.txt" 2>NUL
set /p WEB_CODE=<"%TEMP%\e2e_web.txt"
del "%TEMP%\e2e_web.txt" 2>NUL

if "!WEB_CODE!"=="200" (
    echo   PASS: Web server responding
    set /a PASS+=1
) else if "!WEB_CODE!"=="404" (
    echo   PASS: Web server responding ^(404 = server up, no index^)
    set /a PASS+=1
) else (
    echo   SKIP: Web server not reachable ^(start DigiAssetWindows.exe first^)
    set /a SKIP+=1
)

curl -s -o NUL -w "%%{http_code}" http://127.0.0.1:14024/ > "%TEMP%\e2e_rpc.txt" 2>NUL
set /p RPC_CODE=<"%TEMP%\e2e_rpc.txt"
del "%TEMP%\e2e_rpc.txt" 2>NUL

if "!RPC_CODE!"=="200" (
    echo   PASS: RPC server responding
    set /a PASS+=1
) else if "!RPC_CODE!"=="401" (
    echo   PASS: RPC server responding ^(auth required^)
    set /a PASS+=1
) else (
    echo   SKIP: RPC server not reachable
    set /a SKIP+=1
)

curl -s http://ifconfig.co/port/80 2>NUL | findstr /C:"reachable" >NUL 2>&1
if !ERRORLEVEL! EQU 0 (
    echo   PASS: External port check API available
    set /a PASS+=1
) else (
    echo   SKIP: ifconfig.co not reachable
    set /a SKIP+=1
)

echo.
echo ============================================
echo  Results: %PASS% passed, %FAIL% failed, %SKIP% skipped
echo ============================================
echo.
echo  Tier 1: Unit tests (gtest)
echo  Tier 2: Binary validation
echo  Tier 3: Fuzz self-tests
echo  Tier 4: E2E (requires running app)
echo ============================================

if %FAIL% GTR 0 exit /b 1
exit /b 0

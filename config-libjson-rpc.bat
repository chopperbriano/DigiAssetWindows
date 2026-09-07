@echo off
REM Configure the vendored libjson-rpc-cpp subproject.
REM
REM The pinned upstream commit calls cmake_policy(SET CMP0042 OLD), which CMake 4.x
REM rejects outright ("Policy CMP0042 may not be set to OLD behavior") - so a fresh
REM --recursive clone cannot configure this dependency at all. Upstream
REM (l0stman/libjson-rpc-cpp) has no fix and we are already at its tip, so the fix
REM lives here as a patch that this script applies.
REM
REM git apply --check succeeds only when the patch is NOT yet applied, which makes
REM this idempotent: re-running the script is a no-op rather than an error.
set "ORIG=%CD%"
set "SCRIPT_DIR=%~dp0"
cd "%SCRIPT_DIR%libjson-rpc-cpp"

git apply --check "%SCRIPT_DIR%patches\libjson-rpc-cpp-cmp0042.patch" >nul 2>&1
if not errorlevel 1 (
    git apply "%SCRIPT_DIR%patches\libjson-rpc-cpp-cmp0042.patch"
    if not errorlevel 1 (
        echo Applied patches\libjson-rpc-cpp-cmp0042.patch ^(CMake 4.x compatibility^).
    ) else (
        echo WARNING: could not apply libjson-rpc-cpp-cmp0042.patch - configure may fail.
    )
) else (
    echo CMP0042 patch already applied - skipping.
)

REM HTTP_CLIENT/HTTP_SERVER=NO: those are the only parts of libjson-rpc-cpp that need
REM libcurl, and this fork does not use them - it compiles its own copy of the connector
REM (src/jsonrpccpp/client/connectors/httpclient.cpp) backed by WinHTTP instead. Without
REM this the configure fails with "Could NOT find CURL" on any machine that does not
REM happen to have a discoverable libcurl, which is every clean checkout.
cmake -B build -DCMAKE_INSTALL_PREFIX=install -DCMAKE_PREFIX_PATH=..\jsoncpp\install ^
      -DHTTP_CLIENT=NO -DHTTP_SERVER=NO -DCOMPILE_TESTS=NO -DCOMPILE_EXAMPLES=NO
cd "%ORIG%"

#!/bin/bash
set -e

# DigiAsset Core - Linux Build & Test Script
# Prerequisites: DigiByte Core must be running and synced on this machine

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo " DigiAsset Core - Linux Build & Test"
echo "=========================================="

# ---- Install dependencies ----
echo ""
echo "[1/5] Installing build dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    libcurl4-openssl-dev \
    libsqlite3-dev \
    libssl-dev \
    libjsoncpp-dev \
    libjsonrpccpp-dev \
    libjsonrpccpp-tools \
    pkg-config

# ---- Initialize googletest submodule ----
echo ""
echo "[2/5] Setting up Google Test..."
if [ ! -d "tests/lib/googletest/CMakeLists.txt" ]; then
    git submodule update --init --recursive
fi

# ---- Configure with CMake ----
echo ""
echo "[3/5] Configuring with CMake..."
rm -rf build
mkdir -p build
cd build
cmake .. -DBUILD_TEST=ON -DBUILD_CLI=ON -DBUILD_WEB=ON

# ---- Build ----
echo ""
echo "[4/5] Building (this may take a few minutes)..."
NPROC=$(nproc 2>/dev/null || echo 4)
make -j"$NPROC"

echo ""
echo "Build complete!"
echo "  Binaries: $SCRIPT_DIR/bin/"
echo "  Tests:    $SCRIPT_DIR/bin/Google_Tests_run"

# ---- Run tests ----
echo ""
echo "[5/5] Running tests..."
echo ""

# Tests that need a live DigiByte Core node, and the rpcTest.db fixture that
# DigiAssetTransaction.existingAssetTransactions builds from it(which also needs IPFS).
# Without those, these fail rather than skip - deliberately, see tests/RPCMethods.h.
NEEDS_NODE='RPCMethodsTest.*:DigiByteCore.*:DigiAsset.getStrCount:PermanentStoragePool.mctrivia_allAddressesRecognized'

# must run from bin/ - the tests resolve ../tests/testFiles/ and config.cfg relative to cwd
cd "$SCRIPT_DIR/bin"

if digibyte-cli getblockchaininfo >/dev/null 2>&1; then
    echo "DigiByte Core is running - running the full suite."
    echo ""
    # build the fixture database first; everything in RPCMethodsTest depends on it
    ./Google_Tests_run --gtest_filter='DigiAssetTransaction.existingAssetTransactions' || true
    ./Google_Tests_run
else
    echo "DigiByte Core does not appear to be running."
    echo "Running the offline suite only - the node dependent tests are excluded"
    echo "rather than left to fail, so a green run here still means something."
    echo ""
    echo "For the full suite: start digibyted and IPFS, wait for sync, then re-run"
    echo "this script(or ./bin/Google_Tests_run from the bin directory)."
    echo ""
    ./Google_Tests_run --gtest_filter="-$NEEDS_NODE"
fi
echo ""
echo "=========================================="
echo " Done!"
echo "=========================================="

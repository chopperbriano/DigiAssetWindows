# SIGILL Crash Investigation - DigiAssetTransaction Test

## Problem
`DigiAssetTransaction.existingAssetTransactions` crashes with **exit code 132 (SIGILL)** consistently at **99.7% of the first IPFS file download**.

## Environment
- macOS, running from `/Users/mc/Desktop/DigiAsset_Core/bin/`
- IPFS daemon running (installed via `brew install ipfs`, initialized, daemon started)
- All other 56 tests pass

## What Happens
1. Test calls `ipfs.downloadFile("QmNPyr5tkm48cUu5iMbReiM8GN8AW6PRpzUztPFadaxC8j", "../tests/testFiles/assetTest.csv", true)`
2. Progress bar shows 0% → 99.7%, then process dies (no output after that)
3. Exit code 132 = signal 4 (SIGILL — illegal instruction)

## Key Suspect: 27GB WAL File
From a previous test run that ran to near-completion:
```
tests/testFiles/assetTest.db-wal   27,877,390,872 bytes  (27 GB!)
tests/testFiles/assetTest.db         243,351,552 bytes  (243 MB)
tests/testFiles/assetTest.csv         60,400,486 bytes  (60 MB — downloaded OK)
```
The WAL file grew to 27GB for a 243MB database. This suggests either:
- Unbounded writes without checkpointing during the test
- The test is replaying the entire blockchain into a local DB
- Could be causing OOM → SIGILL via abort

## Test Code Location
`tests/DigiAssetTransactionTest.cpp` lines 44-45:
```cpp
ipfs.downloadFile("QmNPyr5tkm48cUu5iMbReiM8GN8AW6PRpzUztPFadaxC8j", "../tests/testFiles/assetTest.csv", true);
ipfs.downloadFile("QmVoawgnYej8TNwpBB7DtJ75KbrAB99k7f9VAWzqSLJBeX", "../tests/testFiles/assetTest.db", true);
```
- Third arg `true` = `pinAlso` — pins to local IPFS node AND downloads
- `pin/add/` is called BEFORE the download
- `_command("cat?arg=" + cid, {}, 0, filePath)` does the actual download

## Download Code
`src/IPFS.cpp:474`:
```cpp
void IPFS::downloadFile(const string& cid, const string& filePath, bool pinAlso) {
    if (!isValidCID(cid)) throw exceptionInvalidCID(cid);
    if (isLostCID(cid)) throw exceptionTimeout();
    if (pinAlso) _command("pin/add/" + cid);   // <-- pins first
    _command("cat?arg=" + cid, {}, 0, filePath); // <-- downloads to file
}
```
`_command` is at `src/IPFS.cpp:211` — uses CurlHandler internally.

## Investigation Steps

### Step 1: Check disk space
```bash
df -h /Users/mc
```
If disk is nearly full, that's likely the cause.

### Step 2: Clean up the giant WAL file
```bash
# These files are safe to delete — they're test artifacts
rm -f /Users/mc/Desktop/DigiAsset_Core/tests/testFiles/assetTest.db-wal
rm -f /Users/mc/Desktop/DigiAsset_Core/tests/testFiles/assetTest.db-shm
rm -f /Users/mc/Desktop/DigiAsset_Core/tests/testFiles/rpcTest.db-wal
rm -f /Users/mc/Desktop/DigiAsset_Core/tests/testFiles/rpcTest.db-shm
```

### Step 3: Run with address sanitizer to get better crash info
```bash
cd /Users/mc/Desktop/DigiAsset_Core
cmake -B cmake-build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DBUILD_TEST=ON -DBUILD_CLI=OFF -DBUILD_WEB=OFF -DBUILD_QT=OFF
cmake --build cmake-build-asan --target Google_Tests_run -j4
```

### Step 4: Run just the failing test with lldb for stack trace
```bash
cd /Users/mc/Desktop/DigiAsset_Core/bin
lldb ./Google_Tests_run -- --gtest_filter=DigiAssetTransaction.existingAssetTransactions
# In lldb: run
# When it crashes: bt (backtrace)
```

### Step 5: Check what IPFS pin/add actually does
The `pin/add/` call before download could be the slow/crashing step if the file is being fetched from the network. The progress bar is for the `cat` download, so if the crash is at 99.7% it's during `cat`, not `pin`.

### Step 6: Check CurlHandler for the write callback
Look at `src/CurlHandler.cpp` — specifically the write callback when downloading to a file. The crash at 99.7% could be:
- Buffer overflow in write callback
- File handle being closed prematurely
- curl write to a full disk

```bash
grep -n "progress\|write\|callback\|CURLOPT" /Users/mc/Desktop/DigiAsset_Core/src/CurlHandler.cpp | head -40
```

### Step 7: Check if it's actually an OOM kill
```bash
# Check system logs for OOM events around the time of crash
log show --last 2h --predicate 'eventMessage contains "out of memory"' 2>/dev/null | tail -20
```

## Most Likely Root Cause
The 27GB WAL file strongly suggests the test wrote massive amounts of data to SQLite without checkpointing. This could have filled the disk, causing curl's file write to fail, which then causes an assertion/abort (SIGILL via `__builtin_trap`).

## Quick Fix to Try First
Delete the WAL files (Step 2), check disk space (Step 1), then rerun. If disk space was the issue it should work.

## How to Run Tests
```bash
# Make sure IPFS daemon is running first:
ipfs daemon &
sleep 3

# Run just the transaction test:
cd /Users/mc/Desktop/DigiAsset_Core/bin
./Google_Tests_run --gtest_filter=DigiAssetTransaction.existingAssetTransactions

# Or run all tests:
./Google_Tests_run
```

## Branch Info
- Working branch: `fast` (also merged to `development`)
- All non-IPFS tests pass (56/56)
- IPFS daemon: installed via brew, peer ID: 12D3KooWMac7ZPBLPWBYcjcjwCvE7GG6LbaKPB8WH2yAXkPxsJpt

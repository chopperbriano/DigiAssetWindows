# Qt GUI + Web Docs Cheat Sheet

Covers ground for **Task 1/2/3 GUI work** (Create Asset / Send Asset / Balances tabs) and
**Task 5 docs work** (remove outdated wallet note, fix readme, verify RPC doc coverage).

---

## 1. `qt/` directory — full file map

```
qt/CMakeLists.txt      # manually-maintained source list (NOT auto-registered like RPC methods!)
qt/main.cpp            # app entrypoint: launches daemon, splash screen, sync polling, tab host
qt/PlotManager.h/.cpp   # (not investigated in depth — unrelated to sync/tabs)
qt/RPCLoader.h/.cpp     # thin wrapper that owns one DigiByteCore connection to the daemon
qt/tabs/SyncTab.h/.cpp  # the ONLY existing tab — use as the template for new tabs
```

### `qt/main.cpp` — startup flow

1. `startCoreProcess()` (line 33) spawns `digiasset_core` as a `QProcess` from
   `QCoreApplication::applicationDirPath() + "/digiasset_core"` — i.e. it expects the daemon
   binary sitting next to the Qt binary in the same `bin/` dir.
2. `showLoadingScreen()` (line 164) builds a splash widget with a progress bar / spinner and
   starts a `QTimer` on a **15-second interval** calling `updateLoadingProgress`.
3. `updateLoadingProgress()` (line 96) calls `loader.getCore().sendcommand("syncstate", args)`
   against the daemon RPC, reads `result["count"]` / `result["sync"]`, and switches on `sync`:
   `1`=stopped/error, `2`=initializing, `3`=rewinding, `4`=optimizing, default=actively syncing
   (shows progress bar + ETA via `estimateTimeRemaining`).
4. **Tab creation happens here** — confirmed, this is exactly where prior notes said:

```cpp
// qt/main.cpp lines 148-157
if (sync == 0) {
    timer->stop();
    splash.close();

    tabWidget = new QTabWidget();
    SyncTab *syncTab = new SyncTab();
    tabWidget->addTab(syncTab, "Sync Status");
    tabWidget->resize(800, 600);
    tabWidget->show();
}
```

To add "Create Asset" / "Send Asset" / "Balances" tabs, add them here as more
`tabWidget->addTab(new MyTab(), "My Tab Name");` calls, right alongside the `SyncTab` line.

### `qt/RPCLoader` — daemon connection wrapper

```cpp
// RPCLoader.h
class RPCLoader {
public:
    RPCLoader();
    DigiByteCore& getCore();
private:
    DigiByteCore dgb;
    bool isConnected;
};

// RPCLoader.cpp
RPCLoader::RPCLoader() : isConnected(false) {
    dgb.setFileName("config.cfg", true);   // true = useAssetPort
    dgb.makeConnection();
    isConnected = true;
}
```

**Important architectural point:** `setFileName(..., true)` sets `_useAssetPort = true` inside
`DigiByteCore` (see `src/DigiByteCore.cpp:58-119`), which makes the connection target
`config.getInteger("rpcassetport", 14024)` — i.e. **the digiasset_core daemon's own RPC port**,
NOT DigiByte Core's wallet port (14022) directly. Since the daemon's RPC server forwards
unrecognized methods straight to the DigiByte wallet (see `02_rpc_cli_and_build.md`), this
means **a single `DigiByteCore` connection from the Qt app can call both custom asset RPCs
and standard wallet RPCs interchangeably** via `sendcommand(name, args)`. New tabs don't need
two separate connections.

There's a global singleton `RPCLoader loader;` in `main.cpp` (file-scope, line 27) — but
`SyncTab` does NOT use it; it makes its own separate `DigiByteCore _dgbCore` member and
connects independently (see below). Follow SyncTab's pattern (own connection per tab) rather
than trying to share `main.cpp`'s `loader` unless you intend to change that.

### `qt/tabs/SyncTab` — the template to copy

**Header** (`SyncTab.h`):
```cpp
class SyncTab : public QWidget {
    Q_OBJECT
public:
    explicit SyncTab(QWidget *parent = nullptr);
private slots:
    void updateSyncStatus();
private:
    QLabel * _syncLabel;
    QTimer * _timer;
    DigiByteCore _dgbCore;
    // ...layout widgets...
};
```

**Constructor pattern** (`SyncTab.cpp:8-32`):
```cpp
SyncTab::SyncTab(QWidget *parent) : QWidget(parent), _dgbCore() {
    _dgbCore.setFileName("config.cfg", true);
    _dgbCore.makeConnection();

    QVBoxLayout *layout = new QVBoxLayout(this);
    // ...build widgets, add to layout...

    _timer = new QTimer(this);
    connect(_timer, &QTimer::timeout, this, &SyncTab::updateSyncStatus);
    _timer->start(15000);  // poll every 15s

    updateSyncStatus();  // populate immediately, don't wait for first timer tick
}
```

**RPC call pattern** (`SyncTab.cpp:34-97`, `updateSyncStatus`):
```cpp
try {
    Json::Value args = Json::arrayValue;
    Json::Value result = _dgbCore.sendcommand("syncstate", args);
    int syncHeight = result["count"].asInt();
    // ...
    Json::Value exchangeRates = _dgbCore.sendcommand("getexchangerates", args);
    for (const auto &rate : exchangeRates) { /* build widgets dynamically */ }
} catch (const DigiByteException &e) {
    _syncLabel->setText("Error fetching sync state.");
}
```

This is the exact shape a "Balances" tab needs: call an RPC (e.g. `getaddressholdings`, or
whatever new wallet-wide balance RPC gets built per Task 3), parse the `Json::Value` result,
and rebuild a dynamic list of widgets. A "Send Asset" / "Create Asset" tab would instead use
form widgets (`QLineEdit`, `QComboBox`, a submit `QPushButton`) that call a `sendcommand(...)`
on click rather than on a timer.

### Registering a new tab — MANUAL step required

Unlike RPC methods (auto-registered by CMake glob), **Qt tab files must be added by hand** to
`qt/CMakeLists.txt`:

```cmake
set(HEADER_FILES
        PlotManager.h
        RPCLoader.h
        tabs/SyncTab.h        # <-- add tabs/CreateAssetTab.h etc. here
        ...
        )
set(SOURCE_FILES
        PlotManager.cpp
        RPCLoader.cpp
        tabs/SyncTab.cpp      # <-- add tabs/CreateAssetTab.cpp etc. here
        ...
        )
```

Forgetting this means the new tab file compiles nowhere and silently isn't part of the build.

---

## 2. `web/` directory — documentation server

`web/main.cpp` runs a minimal Boost.Beast HTTP server (`digiasset_core-web`, port from
`config.getInteger("webport", 8090)`, default 8090). Full file list:

```
web/CMakeLists.txt
web/main.cpp            # the HTTP server itself
web/index.html           # main doc page — the RPC nav / TOC (1121 lines)
web/css/style.css
web/js/main.js
web/rpc/*.html           # ~140 files — standard DigiByte/Bitcoin-Core-style wallet RPC docs
web/rpc/index.html
```

### Path resolution — KEY MECHANISM (`web/main.cpp:80-99`)

```cpp
if (req.target().substr(0,5)=="/src/") {
    path = ".." + string(req.target());               // serve raw from src/
} else if (req.target().substr(0,5)=="/rpc/") {
    // rpc method check if it has been overwritten
    path = "../src/RPC/Methods/" + string(req.target()).substr(5);
    if (!fileExists(path)) {
        path = "../web" + string(req.target());        // fall back to web/rpc/*.html
    }
} else {
    path = "../web" + string(req.target());
}
```

So **every URL under `/rpc/<name>.html` is checked against `src/RPC/Methods/<name>.html`
FIRST**, and only falls back to `web/rpc/<name>.html` if no override exists. This is why
`index.html` can link `href="/rpc/getrawtransaction.html"` even though that specific doc file
physically lives in `src/RPC/Methods/getrawtransaction.html`, not `web/rpc/`. Custom
DigiAsset-aware overrides of standard method names (`getrawtransaction`, `gettxout`,
`listunspent`, `send`, `sendmany`, `sendtoaddress`) work this way — they silently replace the
stock wallet doc page with the DigiAsset-aware one at the same URL.

### RPC doc coverage audit (as of 2026-07-13, `last_tasks` branch)

All 35 `.cpp` files in `src/RPC/Methods/` **already have a matching `.html`** file in the
same directory — no missing doc files. Cross-checked every method name against
`web/index.html`'s "DigiAssets RPCs" section (`web/index.html:907-1121`) and the rest of the
page (since 6 method names collide with standard wallet-RPC names and are linked from other
sections instead):

- **34 of 35 are linked and reachable.**
- **`getrandom` (`src/RPC/Methods/getrandom.cpp` / `.html`) has NO nav link anywhere in
  `index.html`.** The doc file exists and would render fine if visited directly at
  `/rpc/getrandom.html`, it's just orphaned from navigation. This is the one concrete gap for
  Task 5's "verify all methods have docs" — add a `<li class="rpc_item">` entry for it in the
  "DigiAssets RPCs" section (`web/index.html:907` onward), following the exact pattern of the
  neighboring entries (e.g. `getoldstreamkey` around line 1021).

The 6 overlapping-name methods and where they're actually linked (still under `/rpc/` URLs
but appearing in non-"DigiAssets" sections since the human editor filed them by legacy name):
`getrawtransaction` (line 463), `gettxout` (line 171), `listunspent` (line 775), `send` (line
819), `sendmany` (line 822), `sendtoaddress` (line 827).

### The outdated "wallet support" note — Task 5

Confirmed at **`web/index.html:558-563`** (line numbers shifted by ~-2 vs. prior notes'
estimate of 558-563, actually landed exactly there):

```html
<h3>Wallet RPCs</h3>
<p>
    <strong>Note:</strong> the wallet RPCs are only available if
    DigiByte Core was built with wallet support, which is the
    default.
</p>
```

This is stale/misleading now that wallet support is a hard requirement (not just "the
default") — reword or remove per Task 5.

---

## 3. `readme.md` — issues found (full file is 332 lines)

| Issue | Location | Detail |
|---|---|---|
| Ubuntu-only | Lines 1-30 ("Install Ubuntu" section, TOC has no macOS/Windows entries) | Entire guide assumes Ubuntu 20.04/22.04; states "Ideally this should work on all OS" but gives zero non-Ubuntu instructions. Needs new top-level sections for macOS and Windows. |
| `wget wget` typo | **Line 56** | `wget wget https://github.com/DigiByte-Core/digibyte/releases/download/v7.17.3/digibyte-7.17.3-x86_64-linux-gnu.tar.gz` — literal duplicated `wget`. |
| Old DigiByte version | Lines 56-58 | References `v7.17.3` tarball. Per project convention only DigiByte Core ≤ v8.22.2 is trusted, and v8.22.x is the current target — needs bump to v8.22.2 throughout (URL, filenames, extracted dir name). |
| systemd path version mismatch | **Line 99** | `ExecStart=/home/<your-username>/digibyte-7.17.2/bin/digibyted ...` — says `7.17.2` here even though the download two sections up said `7.17.3`. Internally inconsistent on top of being outdated. |
| No wallet-build note | Whole "Install DigiByte" section | Doesn't mention that DigiAsset_Core now requires a wallet-enabled DigiByte Core build (relevant since default builds may vary; ties into the same underlying fact as the web/index.html note above). |

### Windows compat — prior work exists but is unmerged

Commit `47568e1` ("Add Windows compatibility and GitHub Actions release workflow") exists on
the **`fast` branch only** — confirmed via `git branch --all --contains 47568e1` (returns only
`fast`) and `git merge-base --is-ancestor 47568e1 HEAD` on `last_tasks` returns false (not an
ancestor). It is **not merged into `last_tasks` or `development`**.

What it added (from `git show 47568e1 --stat` + commit message):
- `src/InstanceLock.cpp`: platform-specific implementations behind `#ifdef` — Windows
  (`CreateMutex`), macOS (`_NSGetExecutablePath`), Linux (`/proc/self/exe`).
- `CMakeLists.txt` / `src/CMakeLists.txt` / `tests/CMakeLists.txt`: MSVC flags
  (`/W4 /EHsc`), Windows defines (`NOMINMAX`, `WIN32_LEAN_AND_MEAN`, `_WIN32_WINNT=0x0601`),
  replaced Linux-only `pkg_check_modules` with a cross-platform `find_package` cascade
  (vcpkg config → pkg-config → custom Find module).
- New `vcpkg.json` manifest (boost-asio, curl, openssl, sqlite3, jsoncpp,
  libjson-rpc-cpp) for Windows builds via vcpkg.
- New `.github/workflows/release.yml`: CI matrix building Linux (ubuntu-22.04), macOS
  (macos-14), and Windows (windows-2022 + vcpkg), publishing binaries on `v*` tags.

For Task 5's Windows/macOS readme sections, **check whether this commit should be
cherry-picked/merged into `last_tasks` first** — writing Windows build instructions without
the underlying CMake/vcpkg support landed would describe a build that doesn't actually work
yet on this branch. Worth flagging to the user/task owner rather than silently deciding.

---

## Summary for Task 5 checklist

- [ ] Reword/remove `web/index.html:558-563` wallet-support note
- [ ] Add missing nav link for `getrandom` in `web/index.html` "DigiAssets RPCs" section
- [ ] Fix `readme.md:56` `wget wget` typo
- [ ] Bump DigiByte version refs in `readme.md` from v7.17.3 → v8.22.2 (lines 56-58 and the
      systemd `ExecStart` path on line 99, which is additionally internally inconsistent at
      7.17.2)
- [ ] Add macOS section to `readme.md` (no existing content to adapt — greenfield)
- [ ] Add Windows section to `readme.md` — **first confirm whether commit `47568e1` from the
      `fast` branch (unmerged) should land on `last_tasks`**, since the Windows CMake/vcpkg
      support it adds is a prerequisite for the instructions to be truthful

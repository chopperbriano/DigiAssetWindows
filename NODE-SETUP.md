# Run a DigiAsset node on Windows and earn DGB

Host DigiAsset files, get paid DGB from the DigiStamp pool. The amounts are small:
this is a tip jar for helping keep DigiByte's asset data alive, not a salary (see
[Be realistic about earnings](#be-realistic-about-earnings)).

## 1. One-line install

Open **PowerShell as Administrator** (click Start, type `PowerShell`, right-click
**Windows PowerShell**, choose *Run as administrator*) and paste this single line:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

It asks three things up front, then runs on its own:

1. **Your DGB payout address** — where you want to be paid. Press ENTER and it
   creates one in the local wallet. On a re-run, ENTER keeps the payout address you
   already have.
2. **Windows auto-login** — optional; see [below](#auto-start--running-it-unattended).
3. **Wallet encryption** — a passphrase needed to spend. Receiving works without one.

Then it:

- installs the **DigiByte Core GUI wallet** (9.26.7) into `C:\DigiByte` and writes its config,
- installs **IPFS Desktop** (the tray app) for your user,
- downloads the latest **DigiAsset for Windows** node into `C:\DigiAssetWindows` and writes its config,
- installs the **Visual C++ runtime** the node needs, if it's missing,
- **opens your local firewall** and pre-approves the apps (so you don't get scary popups),
- sets **all three to open when you log in**,
- **tests** whether you're reachable from the internet and tells you what to forward,
- drops the helper scripts into `C:\DigiAssetWindows` (`monitor-node.ps1`,
  `update-binaries.ps1`, `stop-node.ps1`, `update-node.ps1`, `memwatch.ps1`),
- installs a background **maintenance task** that, on every boot and every 6 hours,
  **updates DigiByte Core and the node** and re-checks health — logging to
  `C:\DigiAssetWindows\logs` and alerting you only if something needs your attention.

**Fast-sync is automatic.** On a fresh install it downloads, verifies and extracts
the newest published snapshot (~37 GB download, needs ~90 GB free). If there isn't
enough space, or the snapshot is unavailable, it falls back to a normal sync.
Nothing to configure.

When it finishes, it saves its summary — the router port and backup steps — to
**`DigiAsset - next steps.txt`** on your desktop.

Three apps work together. They open as normal Windows apps — a wallet window, a
tray icon, and a dashboard:

```
DigiByte Core wallet  →  DigiAsset for Windows  →  IPFS Desktop
   (GUI window)            (node + dashboard)        (tray icon)
```

> Want to run the **pool server** (accept nodes and pay hosts) rather than just
> host a node? That's a different job — see **[POOL-SETUP.md](POOL-SETUP.md)**.

### Auto-start & running it unattended

The apps open **when you log in**. Because they're desktop apps, they run while
you're **logged in**.

For an always-on node that comes back by itself after a reboot, accept when the
installer offers **auto-login**. It opens Microsoft's free
**[Sysinternals Autologon](https://learn.microsoft.com/sysinternals/downloads/autologon)**;
check it shows your account and enter your Windows password there (never into the
script). It is optional: type `N` to skip, or pass `-SkipAutologon` so it isn't
asked. Skipped it? Re-run the installer, or run Autologon yourself any time.

> Prefer to launch the apps yourself instead of at logon? Re-run the installer and
> add `-NoStartOnLogon`.

### Thaw Day

DigiByte Core **9.26.6 or newer** is required before mainnet block **24,490,000**
(about Nov 1 2026). The installer pins 9.26.7 and the maintenance task upgrades
older installs, so you normally do nothing. On older DigiByte the node refuses to
index past that block. If `monitor-node.ps1` shows an old DigiByte version,
re-run the installer.

## 2. Let it sync (the one wait)

With the snapshot, the node is usually caught up in **minutes to an hour**. It
takes **a day or more** only if the snapshot can't be used. Watch progress in the
**DigiByte wallet window**, or with the monitor (section 4). Leave the PC on and
logged in. Once DigiByte is synced, the node registers with the pool on its own.

> Already had DigiByte Core installed? The installer detects it and just adds the
> settings — restart DigiByte Core once if it says it wrote a new `digibyte.conf`.

## 3. Open ports on your home router (important)

Your PC's Windows firewall is opened automatically by the installer, but your
**home router** is not. To actually **host incoming connections** — and let the
pool verify you and pay you — forward these to your PC's local IP:

| Port | Protocol | Needed? | Hosts |
|---|---|---|---|
| **4001** | **TCP** | **Required** | DigiAsset / IPFS — how the pool reaches/verifies your node |
| 4001 | UDP | Recommended | DigiAsset / IPFS (QUIC — faster peer connections) |
| 12024 | TCP | Recommended | DigiByte — lets you serve DigiByte peers |

**Do NOT forward 5001, 14022, or 8090** — those are local-only (IPFS API,
DigiByte RPC, and the node's web UI) and must stay private.

> How to forward a port: log into your router (usually `192.168.0.1` or
> `192.168.1.1`), find **Port Forwarding**, and send TCP 4001 to this PC's local
> IP (run `ipconfig` to find it — the "IPv4 Address").

## 4. Check that it's working — the monitor

The easiest way to see everything at a glance is the **monitor script**. From an
Administrator PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\monitor-node.ps1          # one-time status
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\monitor-node.ps1 -Watch   # live, refreshes every 15s
```

It shows one line each for **DigiByte Core** (version and sync %), **IPFS**,
**DigiAsset for Windows**, your **local firewall** + **hosting ports** (4001 and
12024), and **Pool** (are you registered?), plus a plain-English list of anything
to fix.

Other quick checks:

- **In your browser — the Node Console:** open **http://localhost:8090**. It's a
  live dashboard of your node: sync progress, DigiAssets indexed + latest
  issuances, IPFS serving, permanent-storage coverage, **DigiByte network + wallet**
  (balance, peers, verification %), and your **pool status + payout**. A second
  tab is a searchable **RPC Reference** with copy-paste examples for every method.
  (Loopback-only — it stays on your machine; never forward port 8090.)
- **In the app:** in the `DigiAssetWindows.exe` window press **`P`** (re-tests port
  4001) or **`N`** (lists pool nodes; yours is marked `<-- YOU`).
- **From anywhere:** visit https://pool.digistamp.co — your node shows up in the
  count once it's registered and verified.

## Keeping it updated

**It's automatic.** The maintenance task (every 6 hours and at boot) updates
DigiByte Core and the DigiAsset binaries and refreshes the helper scripts.

**Update now** — from an Administrator PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\update-binaries.ps1
```

It downloads the latest release, checks its SHA256, stops the node cleanly, swaps
the binaries and the web console in, and restarts it.

**Repair or update everything** (DigiByte Core, IPFS Desktop, config defaults,
start-up tasks): re-run the one-line install. It keeps your config, payout address
and wallet.

**Memory check** (only if you suspect a leak; run once fully synced):

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\memwatch.ps1
```

It logs Private Bytes over time — leave it ~1 hr at the tip; a flat "stable"
verdict means no leak (growth *during* the initial sync is normal, just caches
filling).

## Editing the config

You normally never edit files by hand. If you must, `config.cfg`
(`C:\DigiAssetWindows`) and `digibyte.conf` (`C:\DigiByte`) are locked to
Administrators: open **Notepad with Run as administrator**, then *File > Open* the
file. Every `config.cfg` key is documented in [example.cfg](example.cfg).

## What "working" looks like

- The **DigiByte wallet** window is open and synced.
- **IPFS Desktop** is running (tray icon).
- The **DigiAsset dashboard** shows **PSP Pool: Hosting pool files** and, once the
  pool has you verified, the **Payment** row goes active.
- Port 4001 tests as **open**.

That's it — leave it running and you'll be paid from the pool for the content you host.

## Be realistic about earnings

Please don't do this to get rich — do it to help keep DigiByte's asset data
alive. A few honest points so there are no surprises:

- **The amounts are small.** This is a tip jar for hosting, not a salary.
- **You're only paid when there's DGB to pay out.** The pool pays from a shared
  treasury funded by asset-creation fees and donations. When the treasury has
  funds, they're split among all verified nodes; when it's empty, nobody is paid
  that period — the pool never pays money it doesn't have.
- **It's a share, not a fixed rate.** What you receive depends on how much is in
  the treasury and how many nodes are sharing it.
- **You must be verified** (reachable — see the port-4001 step) to be included at
  all.

You can watch the live treasury balance and every payout at https://pool.digistamp.co.

## Stopping or removing it

From an Administrator PowerShell (the installer put `stop-node.ps1` in `C:\DigiAssetWindows`):

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\stop-node.ps1                    # stop now (restarts on next boot)
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\stop-node.ps1 -DisableAutostart  # stop + don't restart on boot
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\stop-node.ps1 -Uninstall         # stop + remove boot tasks/firewall + delete C:\DigiAssetWindows
```

It shuts the wallet, IPFS Desktop, and the node down cleanly (not a hard kill).
`-Uninstall` deletes `C:\DigiAssetWindows` but leaves the DigiByte wallet (`C:\DigiByte` +
its blockchain) and IPFS Desktop installed — remove those from **Settings > Apps**
if you want them gone too.

## Troubleshooting

- **Security popups during install (UAC + firewall):** expected — approve them all.
  - **"Do you want to allow this app to make changes to your device?"** (UAC) → **Yes**.
  - **"Allow this app through the firewall?"** (DigiByte / IPFS / node) → **Allow**
    (both Private + Public). The installer pre-approves where it can, but click Allow
    if one still appears. The install can't finish without them.
- **`MSVCP140.dll was not found`:** the node needs the Visual C++ x64 runtime. The
  installer installs it automatically; if you still see this, re-run the one-liner.
- **Node closes with "IPFS Exception: Timeout":** the node needs **IPFS** and
  **DigiByte** running first. The installer waits for both and retries, so this
  should be rare — but if it happens, make sure **IPFS Desktop** (tray icon) is
  running and give it a minute; the node relaunches once IPFS is up. The node keeps
  running while DigiByte finishes syncing (it doesn't need a full sync to start).
- **Windows blue "unknown publisher" / SmartScreen box:** the apps aren't
  code-signed yet, so Windows warns on first run. Click **More info → Run anyway**.
  If your antivirus quarantines `DigiAssetWindows.exe`, allow/restore it.
- **Payment row not active / not verified:** almost always port 4001 isn't
  forwarded on the router. Fix the forward, then press `P` in the app.
- **"DigiByte Core not responding":** the DigiByte wallet hasn't finished syncing
  yet (check with `monitor-node.ps1`), or it isn't open — open the DigiByte wallet.
- **Apps didn't come back after a reboot:** they open at **logon**, so either log
  in, or turn on auto-login (see [above](#auto-start--running-it-unattended)).
- **"Access denied" saving `config.cfg` or `digibyte.conf`:** open Notepad with
  **Run as administrator** (see [Editing the config](#editing-the-config)).
- **PowerShell blocked / "cannot be loaded":** make sure you opened PowerShell
  **as Administrator**; the one-liner already passes `-ExecutionPolicy Bypass`.

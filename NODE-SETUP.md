# Run a DigiAsset node on Windows and earn DGB

Host DigiAsset files, get paid DGB from the DigiStamp pool. This is the simple
version. There are three programs involved — the installer sets up two of them
for you; the third (the DigiByte wallet) you install once.

```
DigiByte Core (wallet)  →  DigiAsset for Windows (this)  →  IPFS (file storage)
   you install once             installer sets up             installer sets up
```

## Be realistic about earnings 🙂

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

Think of it as: contribute a little storage, help the network, and earn a bit of
DGB when the pool has it to give. You can watch the live treasury balance and
every payout at https://pool.digistamp.co.

## 1. One-line install (recommended)

Open **PowerShell as Administrator** (click Start, type `PowerShell`, right-click
**Windows PowerShell**, choose *Run as administrator*) and paste this single line:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/setup-digiasset.ps1 -OutFile "$env:TEMP\setup-digiasset.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\setup-digiasset.ps1"
```

It asks for **one thing — your DigiByte payout address** (where you want to be
paid) — then does everything else automatically:

- installs **DigiByte Core 9.26.4** into `C:\DigiByte` and writes its config,
- installs the current **IPFS (kubo)** release into `C:\DigiAsset`,
- downloads the latest **DigiAsset for Windows** node into `C:\DigiAsset` and writes its config,
- **opens your local firewall** for the right ports,
- sets **all three to start automatically on boot**,
- **tests** whether you're reachable from the internet and tells you what to forward,
- installs a background **maintenance task** that, on every boot and every 6 hours,
  **auto-updates all three components and self-heals** anything broken (restarts
  crashed services, re-downloads missing files, re-opens the firewall) — logging to
  `C:\DigiAsset\logs` and only alerting you if it can't fix something itself.

You do **not** edit any files by hand. The only manual step is one router port
forward (next section).

## 2. Let it sync (the one wait)

DigiByte's blockchain is large, so the first sync takes **several hours**, running
quietly in the background. You don't have to babysit it — just leave the PC on.
Check progress any time with the monitor (see section 4). Once DigiByte is synced,
DigiAsset for Windows registers with the pool on its own.

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
powershell -ExecutionPolicy Bypass -File C:\DigiAsset\monitor-node.ps1          # one-time status
powershell -ExecutionPolicy Bypass -File C:\DigiAsset\monitor-node.ps1 -Watch   # live, refreshes every 15s
```

(The installer drops `monitor-node.ps1` into `C:\DigiAsset` for you.)

It shows one line each for **DigiByte Core** (sync %), **IPFS**, **DigiAsset for
Windows**, your **local firewall** + **hosting ports** (4001 and 12024), and
**Pool** (are you registered?), plus a plain-English list of anything to fix.

Other quick checks:

- **In the app:** in the `DigiAssetWindows.exe` window press **`P`** (re-tests port
  4001) or **`N`** (lists pool nodes; yours is marked `<-- YOU`).
- **From anywhere:** visit https://pool.digistamp.co — your node shows up in the
  count once it's registered and verified.

## What "working" looks like

- IPFS is running (installed as a boot task).
- DigiByte Core is synced.
- `DigiAssetWindows.exe` dashboard shows **PSP Pool: Hosting pool files** and, once
  the pool has you verified, the **Payment** row goes active.
- Port 4001 tests as **open**.

That's it — leave it running and you'll be paid from the pool for the content you host.

## Stopping or removing it

From an Administrator PowerShell (the installer put `stop-node.ps1` in `C:\DigiAsset`):

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAsset\stop-node.ps1                    # stop now (restarts on next boot)
powershell -ExecutionPolicy Bypass -File C:\DigiAsset\stop-node.ps1 -DisableAutostart  # stop + don't restart on boot
powershell -ExecutionPolicy Bypass -File C:\DigiAsset\stop-node.ps1 -Uninstall         # stop + remove boot tasks/firewall + delete C:\DigiAsset
```

It shuts DigiByte + IPFS down cleanly (not a hard kill). `-Uninstall` deletes
`C:\DigiAsset` but leaves DigiByte Core and its blockchain in `C:\DigiByte` — delete
that folder (or use the DigiByte uninstaller in Windows "Apps") if you want it gone too.

## Troubleshooting

- **Windows blue "unknown publisher" / SmartScreen box:** the apps aren't
  code-signed yet, so Windows warns on first run. Click **More info → Run anyway**.
  If your antivirus quarantines `DigiAssetWindows.exe`, allow/restore it.
- **Payment row not active / not verified:** almost always port 4001 isn't
  forwarded on the router. Fix the forward, then press `P` in the app.
- **"DigiByte Core not responding":** it hasn't finished syncing yet (check with
  `monitor-node.ps1`), or you had an old wallet running with different settings —
  restart it once.
- **PowerShell blocked / "cannot be loaded":** make sure you opened PowerShell
  **as Administrator**; the one-liner already passes `-ExecutionPolicy Bypass`.

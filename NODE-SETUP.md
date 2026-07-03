# Run a DigiAsset node on Windows and earn DGB

Host DigiAsset files, get paid DGB from the DigiStamp pool. This is the simple
version. There are three programs involved — the installer sets up two of them
for you; the third (the DigiByte wallet) you install once.

```
DigiByte Core (wallet)  →  DigiAsset Core (this)  →  IPFS (file storage)
   you install once          installer sets up          installer sets up
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

## 1. One-click install

Open **PowerShell as Administrator** and paste this single line:

```powershell
$s="$env:TEMP\install-node.ps1"; iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/install-node.ps1 -OutFile $s; Start-Process powershell "-ExecutionPolicy Bypass -File `"$s`"" -Verb RunAs
```

It asks for **one thing — your DigiByte payout address** (where you want to be
paid) — then does everything else automatically:

- downloads **DigiAsset Core**,
- downloads + installs **DigiByte Core** (the wallet, latest version) and writes its config,
- downloads + starts **IPFS**,
- **opens your local firewall** for the right ports,
- writes the config (pointed at `pool.digistamp.co` with your payout address),
- sets **all three to start automatically on boot**,
- **tests** whether you're reachable from the internet and tells you what to forward.

You do **not** edit any files by hand. The only manual step is one router port
forward (next section).

## 2. Let it sync (the one wait)

DigiByte's blockchain is large, so the first sync takes **several hours**, running
quietly in the background. You don't have to babysit it — just leave the PC on.
Check progress any time with the monitor (see section 4). Once DigiByte is synced,
DigiAsset Core registers with the pool on its own.

> Already had DigiByte Core installed? The installer detects it and just adds the
> settings — restart DigiByte Core once if it says it wrote a new `digibyte.conf`.

## 3. Open ports on your home router (important)

Your PC's firewall is handled automatically, but your **home router** is not.
For the pool to verify you're really hosting (and pay you), forward these to your
PC's local IP:

| Port | Protocol | Needed? | What for |
|---|---|---|---|
| **4001** | **TCP** | **Required** | IPFS — how the pool reaches/verifies your node |
| 4001 | UDP | Recommended | IPFS QUIC (faster connections) |
| 12024 | TCP | Optional | More DigiByte peers |

**Do NOT forward 5001, 14022, or 8090** — those stay on your PC only.

> How to forward a port: log into your router (usually `192.168.0.1` or
> `192.168.1.1`), find **Port Forwarding**, and send TCP 4001 to this PC's local
> IP (run `ipconfig` to find it — the "IPv4 Address").

## 4. Check that it's working — the monitor

The easiest way to see everything at a glance is the **monitor script**. From an
Administrator PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\install-node.ps1  # (installer, if not run yet)
powershell -ExecutionPolicy Bypass -File .\monitor-node.ps1                    # one-time status
powershell -ExecutionPolicy Bypass -File .\monitor-node.ps1 -Watch             # live, refreshes every 15s
```

(Get `monitor-node.ps1` the same way as the installer — it's in the repo root.)

It shows one line each for **DigiByte Core** (sync %), **IPFS**, **DigiAsset
Core**, **Port 4001** (open/closed), and **Pool** (are you registered?), plus a
plain-English list of anything to fix.

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

Grab `stop-node.ps1` (repo root) and, from an Administrator PowerShell:

```powershell
.\stop-node.ps1                     # stop everything now (restarts on next boot)
.\stop-node.ps1 -DisableAutostart   # stop + don't restart on boot
.\stop-node.ps1 -Uninstall          # stop, remove boot tasks/firewall rules, delete the node files
```

It shuts DigiByte + IPFS down cleanly (not a hard kill). `-Uninstall` leaves
DigiByte Core and its blockchain in place — remove those from Windows "Apps" if
you want them gone too.

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

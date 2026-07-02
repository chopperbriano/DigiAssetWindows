# Run a DigiAsset node on Windows and earn DGB

Host DigiAsset files, get paid DGB from the DigiStamp pool. This is the simple
version. There are three programs involved — the installer sets up two of them
for you; the third (the DigiByte wallet) you install once.

```
DigiByte Core (wallet)  →  DigiAsset Core (this)  →  IPFS (file storage)
   you install once          installer sets up          installer sets up
```

## 1. One-click install

Open **PowerShell as Administrator** and paste this single line:

```powershell
$s="$env:TEMP\install-node.ps1"; iwr https://raw.githubusercontent.com/chopperbriano/DigiAsset_Core_Windows/master/install-node.ps1 -OutFile $s; Start-Process powershell "-ExecutionPolicy Bypass -File `"$s`"" -Verb RunAs
```

It will ask for **your DigiByte payout address** (where you want to be paid), then it:

- downloads DigiAsset Core,
- installs and starts **IPFS** (runs automatically on boot),
- **opens your local firewall** for the right ports,
- writes the config (pointed at `pool.digistamp.co` with your payout address),
- **tests** whether you're reachable from the internet and tells you what to forward.

## 2. Install DigiByte Core (one time)

The wallet is a big download the installer can't do for you:

1. Get the **win64 setup** from https://github.com/DigiByte-Core/digibyte/releases
2. Install it, then start it and let it **sync** (this can take a while the first time).
3. If the installer said it wrote a `digibyte.conf`, **restart** DigiByte Core so it picks up the settings.
4. Re-run the one-click command above — it detects the wallet and finishes.

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

## 4. Test that it worked

The installer runs the test automatically, but you can re-check any time:

- **In the app:** start `DigiAssetCore.exe`, press **`P`** — it reports whether
  port 4001 is open. Press **`N`** to confirm your node shows up in the pool's
  node list (marked `<-- YOU`).
- **From anywhere:** visit https://pool.digistamp.co — your node appears in the
  node count once it's registered and verified.

## What "working" looks like

- IPFS is running (installed as a boot task).
- DigiByte Core is synced.
- `DigiAssetCore.exe` dashboard shows **PSP Pool: Hosting pool files** and, once
  the pool has you verified, the **Payment** row goes active.
- Port 4001 tests as **open**.

That's it — leave it running and you'll be paid from the pool for the content you host.

## Troubleshooting

- **Payment row not active / not verified:** almost always port 4001 isn't
  forwarded on the router. Fix the forward, then press `P` in the app.
- **"DigiByte Core not responding":** the wallet isn't running or hasn't finished
  syncing, or you didn't restart it after the config was written.
- **`gh`/PowerShell blocked:** make sure you opened PowerShell **as Administrator**.

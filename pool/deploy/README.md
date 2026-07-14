# Pool server deployment (Caddy reverse proxy + landing page)

This folder sets up a public front end for `DigiAssetPoolServer.exe` using
[Caddy](https://caddyserver.com/):

- a **static landing page** (`site/index.html`) served at `https://<domain>/`
  that explains what the pool does and links to the GitHub repo, and
- an automatic **HTTPS reverse proxy** that forwards the pool's API routes
  (`/permanent/*`, `/list/*`, `/keepalive`, `/nodes.json`, `/map.json`,
  `/bad.json`) to the local pool exe on port 14028.

Caddy obtains and renews the TLS certificate for you, so clients can use a clean
`https://pool.digistamp.co` URL instead of `http://host:14028`.

## Why a reverse proxy at all?

`DigiAssetPoolServer.exe` speaks plain HTTP on port 14028 and has no TLS and no
web page. Clients (`DigiAssetWindows`) default to `https://pool.digistamp.co`, so
something has to terminate TLS on 443 and forward to the exe. Caddy does that
with near-zero config and auto-certificates. It also lets a browser that visits
the domain see a friendly page instead of a 404 from the API.

```
browser / node ──HTTPS :443──► Caddy ──┬── "/" and other paths ──► site/index.html
                                        └── /permanent, /list,  ──► 127.0.0.1:14028
                                            /keepalive, *.json       (pool exe)
```

## Prerequisites

1. **A Windows server** running `DigiAssetPoolServer.exe` (poolport=14028).
2. **DNS**: an `A` record for your domain (e.g. `pool.digistamp.co`) pointing at
   the server's **public IP**.
3. **Ports**: inbound **TCP 80 and 443** reachable from the internet. Caddy needs
   80 for the Let's Encrypt HTTP challenge and 443 to serve HTTPS. (Port 14028
   does **not** need to be public — only Caddy talks to it, locally.)

## Usage

From an **elevated** PowerShell:

```powershell
cd pool\deploy
powershell -ExecutionPolicy Bypass -File .\setup-caddy.ps1
```

Options:

```powershell
.\setup-caddy.ps1 -Domain pool.example.com -PoolPort 14028 -InstallDir C:\DigiStampPool
```

The script will:

1. Download Caddy (windows/amd64) into `-InstallDir` (default `C:\DigiStampPool`).
2. Copy `site/`, create a **fixed cert store** (`<InstallDir>\caddydata`), and
   generate a resolved `Caddyfile` (fills in domain, port, path, storage).
3. `caddy validate` the config.
4. Open firewall rules for TCP 80 and 443.
5. Register a **scheduled task** (`DigiStampCaddy`) that runs Caddy **at boot and
   at logon**, with **no execution time limit** and auto-restart on failure, then
   starts it immediately.

Give it 10-30 seconds on first run to obtain the certificate, then visit
`https://<domain>/`.

> **Why the fixed cert store matters.** Caddy keeps its TLS cert + ACME account
> key in a *per-user* folder by default. The task runs as **SYSTEM**, so without
> a pinned store it uses the (empty) system profile and re-requests a cert every
> boot — the classic "works when I run `caddy` by hand, dies as a task". Pinning
> storage to `<InstallDir>\caddydata` means the SAME cert is reused by both. If
> you set this pool up before this change, **re-run `setup-caddy.ps1` once** to
> regenerate the Caddyfile + re-register the task.

## Managing it

```powershell
Start-ScheduledTask  -TaskName DigiStampCaddy    # start
Stop-ScheduledTask   -TaskName DigiStampCaddy    # stop
Unregister-ScheduledTask -TaskName DigiStampCaddy -Confirm:$false  # remove
```

Logs: Caddy writes to stdout; when run as a scheduled task, check Event Viewer or
run it manually once (`caddy run --config C:\DigiStampPool\Caddyfile`) to watch
certificate issuance live.

### `diagnose-website.ps1` — why is the site down?

If `https://<domain>/` times out or won't come up, run this on the pool box:

```powershell
powershell -ExecutionPolicy Bypass -File .\diagnose-website.ps1
```

It validates the Caddyfile, shows the task's last result, checks for a port
80/443 conflict, checks the inbound firewall, compares this box's public IP to
the domain's DNS A-record, and **runs Caddy in the foreground for ~10 s to
capture the real startup error** the scheduled task hides (usually an ACME/cert
failure because 80/443 aren't reachable from the internet, a bad Caddyfile, or a
port conflict). Fix the cause, then bring the site up with
`start-digistamp.ps1 -WebsiteOnly`.

### `verify-pool-stack.ps1` — full-stack health check

```powershell
powershell -ExecutionPolicy Bypass -File .\verify-pool-stack.ps1
```

Cross-checks that `digibyte.conf`, `config.cfg`, and `pool.cfg` agree on the RPC
handshake, confirms DigiByte Core is reachable + synced, that `txindex` (and any
optional service indexes) are enabled + current, and flags the **reindex trap**
(a persistent `reindex=1`). See the node docs for the full index discussion.

### `verify-peers.ps1` — test the link between peer-aware pools

```powershell
powershell -ExecutionPolicy Bypass -File .\verify-peers.ps1
```

For a **two-independent-pools** setup (see POOL-SETUP.md). Reads `pool.cfg` and
checks: this pool serves `/peer/*` locally, the `poolpeertoken` gates it (wrong
token -> 403), each peer is reachable over its public HTTPS URL **and its Caddy
proxies `/peer/*`** and the token matches, this pool's merged `network` view in
`stats.json`, and that the permanent lists are converging. Exit 1 on any hard
failure. Run it on each pool box after wiring the peers.

## Editing the landing page

Edit `site/index.html` here, re-run `setup-caddy.ps1` (it re-copies `site/`), or
copy the file straight into `<InstallDir>\site\`. Caddy serves changes
immediately — no restart needed.

## Starting the stack + backups

Two helper scripts (also in this folder) run the day-to-day operations. Copy them
next to your data, or run them in place with a `-Root` pointing at the data folder
(default `C:\DigiAssetWindows`, auto-falling-back to `C:\DigiAssetWindows` if that is where
this box's data already lives).

### `update-pool.ps1` — update the whole pool box in one step

Run on the pool box to bring everything current:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/pool/deploy/update-pool.ps1 -OutFile "$env:TEMP\update-pool.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\update-pool.ps1"
```

Self-elevates, then: refreshes the deploy scripts + `Caddyfile` from `master`,
refreshes the **live landing page** into `<CaddyDir>\site\index.html` (default
`C:\DigiStampPool`; Caddy serves it on the next request — no restart), runs
`update-binaries.ps1 -Force -IncludePool` for the latest node + pool exes, and
runs `start-digistamp.ps1`. `-NoStart` updates files only; `-CaddyDir` points at
a non-default Caddy folder.

### `start-digistamp.ps1` — start everything after a reboot

You start the **DigiByte Core Windows client (wallet) yourself**. Then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1
```

Self-elevates, waits for DigiByte Core's RPC, checks the IPFS API, launches
`DigiAssetWindows.exe` and `DigiAssetPoolServer.exe` (each in its own window),
and **always restarts the Caddy website** — it stops the task + any stray
`caddy.exe`, starts the task, and waits for 443 (a task can show "Running" while
`caddy.exe` has actually died, so a plain "is it running?" check isn't enough).
It then **exits**, since everything runs in its own window. Use **`-WebsiteOnly`**
to skip Core/node/pool and just restart the site fast.

### `backup-digistamp.ps1` — rotated data backup

```powershell
powershell -ExecutionPolicy Bypass -File .\backup-digistamp.ps1
```

Copies the small, precious data (`pool.db` ledger/registrations, `config.cfg`,
`pool.cfg`, `local.db`) to `<Root>\backups\backup-<timestamp>\`, backs up the
DigiByte wallet via the `backupwallet` RPC, and keeps the newest 7 (`-Keep N`).
The big re-syncable `chain.db` is excluded unless you pass `-IncludeChain`.

**Best run while the apps are stopped** (SQLite snapshot is cleanest then) — so
the natural pattern is: **on boot, back up first, then start.** Register a
"backup then start" at logon with Task Scheduler:

```powershell
$backup = 'powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\backup-digistamp.ps1'
$start  = 'powershell -ExecutionPolicy Bypass -File C:\DigiAssetWindows\start-digistamp.ps1'
$action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-Command `"& {$backup; $start}`""
$trigger = New-ScheduledTaskTrigger -AtLogOn
Register-ScheduledTask -TaskName DigiStampBoot -Action $action -Trigger $trigger -RunLevel Highest -Force
```

(You still launch the DigiByte Core wallet manually; `start-digistamp.ps1` waits
for it, so ordering is fine either way.)

## Adapting to Linux

The `Caddyfile` itself is cross-platform. On Linux, install Caddy from the
official package, place the resolved config at `/etc/caddy/Caddyfile`, put the
site under e.g. `/var/www/pool`, and use the bundled `caddy` systemd service
instead of the scheduled task. The pool exe is currently Windows-only, so the
usual setup runs Caddy and the exe on the same Windows host.

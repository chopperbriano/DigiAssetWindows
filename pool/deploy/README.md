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
2. Copy `site/` and generate a resolved `Caddyfile` (fills in domain, port, path).
3. `caddy validate` the config.
4. Open firewall rules for TCP 80 and 443.
5. Register a **scheduled task** (`DigiStampCaddy`) that runs Caddy at boot, and
   start it immediately.

Give it 10-30 seconds on first run to obtain the certificate, then visit
`https://<domain>/`.

## Managing it

```powershell
Start-ScheduledTask  -TaskName DigiStampCaddy    # start
Stop-ScheduledTask   -TaskName DigiStampCaddy    # stop
Unregister-ScheduledTask -TaskName DigiStampCaddy -Confirm:$false  # remove
```

Logs: Caddy writes to stdout; when run as a scheduled task, check Event Viewer or
run it manually once (`caddy run --config C:\DigiStampPool\Caddyfile`) to watch
certificate issuance live.

## Editing the landing page

Edit `site/index.html` here, re-run `setup-caddy.ps1` (it re-copies `site/`), or
copy the file straight into `<InstallDir>\site\`. Caddy serves changes
immediately — no restart needed.

## Starting the stack + backups

Two helper scripts (also in this folder) run the day-to-day operations. Copy them
next to your data, or run them in place with a `-Root` pointing at the data
folder (default `C:\DigiAssetWindows`).

### `start-digistamp.ps1` — start everything after a reboot

You start the **DigiByte Core Windows client (wallet) yourself**. Then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\start-digistamp.ps1
```

It waits for DigiByte Core's RPC to respond, checks the IPFS API is up, launches
`DigiAssetWindows.exe` and `DigiAssetPoolServer.exe` (each in its own window, from
the data folder so they read their configs), and makes sure the `DigiStampCaddy`
website task is running. It skips anything already running, so it's safe to
re-run.

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

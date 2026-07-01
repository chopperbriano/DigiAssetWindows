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
web page. Clients (`DigiAssetCore`) default to `https://pool.digistamp.co`, so
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

## Adapting to Linux

The `Caddyfile` itself is cross-platform. On Linux, install Caddy from the
official package, place the resolved config at `/etc/caddy/Caddyfile`, put the
site under e.g. `/var/www/pool`, and use the bundled `caddy` systemd service
instead of the scheduled task. The pool exe is currently Windows-only, so the
usual setup runs Caddy and the exe on the same Windows host.

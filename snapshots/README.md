# Fast-sync snapshots

New installs skip the ~week-long DigiByte sync by downloading a **pre-synced
blockchain + `chain.db`** from Cloudflare R2. These scripts create and publish
that snapshot. **Only the snapshot maintainer runs the publishing scripts** —
regular node operators never do. The one exception is **`seed-digibyte.ps1`**,
which *any* user can run to fast-sync a standalone DigiByte wallet (see
[Seed a standalone DigiByte wallet](#seed-a-standalone-digibyte-wallet-any-user)
at the bottom).

| Script | Purpose |
|---|---|
| `setup-cloudflare-snapshots.ps1` | **Run once.** Installs rclone + configures your Cloudflare R2 remote from your keys, then publishing needs no arguments. |
| `publish-snapshot.ps1` | **The one you run each time.** Build both archives → upload to R2 → rebuild + upload `snapshot.json` → verify. Can schedule itself weekly. |
| `make-snapshot.ps1` | The building block: creates the archives + `*-part.json` (and, with `-Component manifest`, `snapshot.json`). `publish-snapshot.ps1` calls this. |
| `seed-digibyte.ps1` | **Standalone consumer** (any user, no repo needed): seeds *any* DigiByte wallet from the published snapshot. **Prompts for the data directory and validates it before extracting**; `-DataDir` / `-Force` skip the prompts. See [below](#seed-a-standalone-digibyte-wallet-any-user). |
| `snapshot-digibyte-datadir.ps1` | **Local / LAN fleet provisioning.** `-Mode Snapshot` archives one healthy box's `blocks`+`chainstate`+`indexes` (wallet excluded); `-Mode Restore` applies it to another box directly (file or `\\share`) — no R2 round-trip, no reindex, no re-sync. Restored nodes inherit the built indexes. |

## First: one-time Cloudflare R2 setup
Run this **once** on the snapshot box — it installs rclone, configures the R2
remote from your Cloudflare keys, verifies it can reach your bucket, and saves the
defaults so `publish-snapshot.ps1` then needs **no arguments**:
```powershell
powershell -ExecutionPolicy Bypass -File .\setup-cloudflare-snapshots.ps1
```
It prompts for (or takes as `-AccountId` / `-AccessKeyId` / `-SecretAccessKey` /
`-Bucket` / `-PublicUrl`):
- **Account ID** — Cloudflare dashboard → R2.
- **R2 API token** (Access Key ID + Secret Access Key) — R2 → *Manage R2 API
  Tokens* → create one with **Object Read & Write** on your bucket.
- **Public bucket URL** — R2 → your bucket → Settings → enable public r2.dev
  access → the `https://pub-XXXX.r2.dev` URL. **This must match the installer's
  baked-in snapshot URL** (`$DefaultSnapshotUrl`).

Your Secret Access Key goes straight into rclone's own (obfuscated) config and is
**never written into this repo**; only non-secret defaults are saved to
`snapshots/snapshot-config.json` (gitignored). Rotate the key in Cloudflare and
re-run this if it's ever exposed.

## Prerequisites
- One always-on box with a **fully-synced** DigiByte wallet **and** DigiAsset node
  (the "indexed wallet" box). Standard layout: DigiByte in `C:\DigiByte`
  (blockchain in `C:\DigiByte\Data`), node in `C:\DigiAssetWindows`.
- The one-time R2 setup above (installs rclone + configures the `r2` remote).
- `tar.exe` (built into Windows 10 1803+ / 11).

## Publish once (run when the box is fully synced)
In an **Administrator PowerShell**:
```powershell
powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1
```
It stops DigiByte + the node briefly for a clean, **in-sync** snapshot (chain.db
can't drift ahead of the blockchain because both are captured together), restarts
them, uploads everything, refreshes `snapshot.json`, and verifies it's live.

## Schedule it (weekly or daily)
```powershell
# weekly (default: Sundays 03:00)
powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -Schedule

# daily (every day at 03:00)
powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -Schedule -Cadence Daily
```
Registers the **`DigiAssetSnapshotPublish`** task.

**It only runs WHILE YOU ARE LOGGED ON.** The task uses an interactive logon, so
on an always-on box it will silently not fire if the machine is sitting at the
lock screen or was rebooted without a login. Pair with **Autologon** so a session
exists at trigger time. (This is the usual reason a snapshot task "doesn't run.")

**Seeing activity / logs.** Each run opens a **minimized** PowerShell window — open
it from the taskbar to watch live — and writes a full transcript to
`C:\DigiAssetSnapshots\logs\publish-<timestamp>.log` (newest 14 kept). Add
`-Hidden` when scheduling if you'd rather it run with no window (the log is still
written). Check the last result and force a test run anytime:
```powershell
Get-ScheduledTaskInfo -TaskName DigiAssetSnapshotPublish   # LastRunTime / LastTaskResult (0 = success)
Start-ScheduledTask   -TaskName DigiAssetSnapshotPublish   # run it now to confirm
```

**Keep the apps running between snapshots.** The DigiByte wallet + node stay up
24/7 via the installer's autostart tasks (that's what keeps them synced); this job
only *briefly* stops them to take a consistent snapshot, then restarts them. So you
don't run a separate "keep syncing" step — normal autostart is it.

**Cadence is whole-pair only.** Every run snapshots **both** DBs together at the
same height, so daily vs weekly is just how fresh a new node's starting point is.
You **cannot** refresh `chain.db` more often than the DigiByte chain — that would
put `chain.db` ahead of the wallet, which the manifest step rejects as unsafe.
DigiByte is ~30 GB (20–60 min to compress, during which the wallet is down), so
**weekly is the practical sweet spot**; use **daily** only if you want the freshest
possible starting point and don't mind the daily compress window + re-upload.

Change weekly timing with `-ScheduleDay Saturday -ScheduleTime 02:30`
(`-ScheduleDay` is ignored for `-Cadence Daily`). Add `-PruneRemote`
to also delete superseded archives from R2 so the bucket doesn't grow every week.

## Useful options
| Flag | Default | Meaning |
|---|---|---|
| `-BaseUrl` | the R2 public URL | where `snapshot.json` is served from (must match the installer's `$DefaultSnapshotUrl`) |
| `-RcloneRemote` / `-Bucket` | `r2` / `digibyte-snapshots` | rclone remote + bucket to upload to |
| `-Component` | `both` | `digibyte`, `chaindb`, or `both` |
| `-KeepLocal` | `1` | how many local archive sets to keep in `C:\DigiAssetSnapshots` |
| `-PruneRemote` | off | delete old `*.tar.gz` from R2 that the new manifest no longer references |

## How a node consumes it
The installer's `$DefaultSnapshotUrl` points at `…/snapshot.json`. On a fresh
install it downloads + SHA256-verifies + extracts the DigiByte archive into
`C:\DigiByte\Data` and `chain.db` into `C:\DigiAssetWindows` — only if those are
empty. So keep `-BaseUrl` here in sync with the installer's baked-in URL.

## Seed a standalone DigiByte wallet (any user)
`seed-digibyte.ps1` is the only script here a **regular user** runs — it fast-syncs
a plain DigiByte Core wallet (no DigiAsset node required) from the same published
snapshot. Install DigiByte Core, start it once and close it (so the data directory
exists), then run this single line in an **Administrator PowerShell**:

```powershell
iwr https://raw.githubusercontent.com/chopperbriano/DigiAssetWindows/master/snapshots/seed-digibyte.ps1 -OutFile "$env:TEMP\seed-digibyte.ps1" -UseBasicParsing; powershell -ExecutionPolicy Bypass -File "$env:TEMP\seed-digibyte.ps1"
```

The `master` raw URL above always fetches the newest script. For a release-pinned
copy instead, download it from the release:
`https://github.com/chopperbriano/DigiAssetWindows/releases/latest/download/seed-digibyte.ps1`

It:
1. Fetches the manifest and shows the snapshot height / download size.
2. **Prompts for the data directory**, offering a detected default —
   `%APPDATA%\DigiByte` for stock DigiByte Core, `C:\DigiByte\data` for the
   DigiAsset for Windows layout.
3. **Validates the folder before extracting.** If it has no `blocks\`,
   `chainstate\`, `digibyte.conf`, wallet, or `DigiByte` in the path, it warns that
   extracting would drop the chain into that exact folder and offers
   **[O]verride / [R]e-enter / [C]ancel** — so the blockchain never lands in the
   wrong place.
4. Stops a running DigiByte, downloads (resumable, with %/speed/ETA),
   SHA256-verifies, and extracts with a "still working" heartbeat.

**Flags:** `-DataDir <path>` seeds a specific folder and skips the prompt;
`-Force` skips the folder check + overwrite confirmation for unattended use;
`-SnapshotUrl <url>` points at a different manifest (defaults to the official feed).

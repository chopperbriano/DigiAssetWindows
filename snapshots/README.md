# Fast-sync snapshots

New installs skip the ~week-long DigiByte sync by downloading a **pre-synced
blockchain + `chain.db`** from Cloudflare R2. These scripts create and publish
that snapshot. **Only the snapshot maintainer runs these** — regular node
operators never do.

| Script | Purpose |
|---|---|
| `publish-snapshot.ps1` | **The one you want.** Build both archives → upload to R2 → rebuild + upload `snapshot.json` → verify. Can schedule itself weekly. |
| `make-snapshot.ps1` | The building block: creates the archives + `*-part.json` (and, with `-Component manifest`, `snapshot.json`). `publish-snapshot.ps1` calls this. |
| `seed-digibyte.ps1` | Standalone consumer: seeds *any* DigiByte wallet from the published snapshot (install DigiByte, close it, run this). |

## Prerequisites
- One always-on box with a **fully-synced** DigiByte wallet **and** DigiAsset node
  (the "indexed wallet" box). Standard layout: DigiByte in `C:\DigiByte`
  (blockchain in `C:\DigiByte\Data`), node in `C:\DigiAssetWindows`.
- **rclone** installed and a remote (default name `r2`) configured for your R2
  account (`rclone config` → S3 → Cloudflare, using the **S3 API** endpoint
  `https://<ACCOUNT_ID>.r2.cloudflarestorage.com`, not the public `r2.dev` URL).
- `tar.exe` (built into Windows 10 1803+ / 11).

## Publish once (run when the box is fully synced)
In an **Administrator PowerShell**:
```powershell
powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1
```
It stops DigiByte + the node briefly for a clean, **in-sync** snapshot (chain.db
can't drift ahead of the blockchain because both are captured together), restarts
them, uploads everything, refreshes `snapshot.json`, and verifies it's live.

## Schedule it weekly
```powershell
powershell -ExecutionPolicy Bypass -File .\publish-snapshot.ps1 -Schedule
```
Registers the **`DigiAssetSnapshotPublish`** task (default: **Sundays 03:00**),
running as you while logged on — pair with **Autologon** on an always-on box.
Change timing with `-ScheduleDay Saturday -ScheduleTime 02:30`. Add `-PruneRemote`
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

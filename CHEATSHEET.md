# DigiAssetWindows — Script Cheatsheet

Every script is PowerShell. Open an **Administrator PowerShell**, `cd` to the folder, run it.
Defaults assume `C:\DigiAssetWindows` (node/pool), `C:\DigiByte` (blockchain), `C:\DigiStampPool` (Caddy/site).
Pick your role below — you only need the scripts in your section.

---

## 1. I run a NODE (host DigiAssets, earn payouts)

| Do this | Command |
|---|---|
| **Install / re-run everything** | `.\setup-digiasset.ps1 -PayoutAddress D_YOUR_ADDR` |
| First install, fast-sync from snapshot | `.\setup-digiasset.ps1 -PayoutAddress D_YOUR_ADDR -SnapshotUrl https://pub-bd3f441e6b464d499ba583016accfa01.r2.dev/snapshot.json` |
| **Update the binaries** to latest release | `node\update-node.ps1` |
| **Is it healthy?** (one look) | `node\monitor-node.ps1` |
| Watch health live | `node\monitor-node.ps1 -Watch` |
| **Stop** the node stack | `node\stop-node.ps1` |
| Stop + never auto-start again | `node\stop-node.ps1 -DisableAutostart` |
| Stop + remove everything | `node\stop-node.ps1 -Uninstall` |

---

## 2. I run a POOL (the main pool, or my own)

| Do this | Command |
|---|---|
| **Build a whole pool host from scratch** | `.\setup-pool.ps1 -PayoutAddress D_YOUR_ADDR -TreasuryAddress D_POOL_ADDR -Domain pool.example.com` |
| **Put the website up** (Caddy + HTTPS + landing page) | `pool\deploy\setup-caddy.ps1 -Domain pool.example.com` |
| **Start the whole stack** (node + pool + always restart the site) | `pool\deploy\start-digistamp.ps1` |
| Restart JUST the website | `pool\deploy\start-digistamp.ps1 -WebsiteOnly` |
| **Update a running pool box** (scripts + site + binaries) | `pool\deploy\update-pool.ps1` |
| **Full stack health check** | `pool\deploy\verify-pool-stack.ps1` |
| Health check + auto-fix config | `pool\deploy\verify-pool-stack.ps1 -Fix` |
| **Website is down — why?** | `pool\deploy\diagnose-website.ps1 -Domain pool.example.com` |
| **Back up** ledger/config/wallet | `pool\deploy\backup-digistamp.ps1` |

---

## 3. I want a NEW PEER POOL that joins the network

| Do this | Command |
|---|---|
| **All-in-one: stand up a new peer pool** | `pool\deploy\provision-peer-pool.ps1 -Domain fred.example.com -PeerUrl https://pool.digistamp.co -Token SHARED_TOKEN -TreasuryAddress D_POOL_ADDR` |
| **Link an existing pool to a peer** | `pool\deploy\add-peer.ps1 -PeerUrl https://pool.digistamp.co -Token SHARED_TOKEN` |
| **Test the peer link** (both directions) | `pool\deploy\verify-peers.ps1` |
| Force one on-chain announce (spends a tiny fee) | `pool\deploy\verify-peers.ps1 -TestAnnounce` |

> `-Token` is the shared `poolpeertoken` — both pools must use the **same** value.
> Pools also auto-discover each other over the network; the token only gates trusted collaboration (ledger/payout dedup).

---

## 4. I run the SNAPSHOT box (fast-sync for everyone)

| Do this | Command |
|---|---|
| **One-time R2 setup** | `snapshots\setup-cloudflare-snapshots.ps1 -AccountId XXX -AccessKeyId XXX -SecretAccessKey XXX` |
| **Publish a fresh snapshot** (build + upload + manifest) | `snapshots\publish-snapshot.ps1` |
| Publish ONLY the DigiByte chain (no chain.db on this box) | `snapshots\publish-snapshot.ps1 -Component digibyte` |
| Schedule it weekly (Sun 3am) | `snapshots\publish-snapshot.ps1 -Schedule` |
| **Seed a NEW node** from the published snapshot | `snapshots\seed-digibyte.ps1` |
| Copy a synced data dir across your own LAN | `snapshots\snapshot-digibyte-datadir.ps1 -Mode Snapshot` / `-Mode Restore` |

> `make-snapshot.ps1` is the low-level piece-builder — `publish-snapshot.ps1` calls it for you. Only run it directly to debug.

---

## 5. Dev / troubleshooting

| Do this | Command |
|---|---|
| Update binaries, incl. pool exe | `node\update-binaries.ps1 -IncludePool` |
| Install exes you just compiled | `node\update-binaries.ps1 -FromBuild -IncludePool` |
| Force reinstall even if up to date | `node\update-binaries.ps1 -Force` |
| **Check for a memory leak** (log Private Bytes over time) | `node\memwatch.ps1 -OutCsv C:\temp\mem.csv` |
| Benchmark sync speed (prep, then run) | `node\bench-sync.ps1 -Prepare` then `node\bench-sync.ps1 -Pipeline 1` |

---

### The 5 commands you'll actually use most
```powershell
node\monitor-node.ps1                 # node OK?
node\update-node.ps1                  # update a node
pool\deploy\start-digistamp.ps1       # (re)start a pool + site
pool\deploy\verify-pool-stack.ps1     # pool OK?
snapshots\publish-snapshot.ps1        # refresh the fast-sync snapshot
```

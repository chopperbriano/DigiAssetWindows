# Fable Cheat Sheet — Index

*Written by another agent (not Fable) while DigiByte Core was resyncing, to help
Fable pick back up on the `last_tasks` branch after running out of credits.
This is all research notes — pointers to where things live and how they're
structured. No code was changed while writing these notes.*

**Start here, in this order:**

1. Read `/Users/mc/Desktop/DigiAsset_Core/LAST_TASKS_NOTES.md` first (repo root) —
   it's Fable's own prior session notes: the 5 tasks, status table, and a work log.
2. Read `/Users/mc/Desktop/DigiAsset_Core/TODO_TESTS.md` (repo root) — carry-over
   test bugs (5-8 unfixed) and tests still to write. Referenced, not duplicated, in
   `05_tests_and_psp.md`.
3. Then use this folder's files as a map into the codebase for each task:

| File | Covers | Relevant to task # |
|---|---|---|
| [01_architecture_overview.md](01_architecture_overview.md) | Big-picture: how `src/`, `cli/`, `qt/`, `web/` fit together | all |
| [02_rpc_cli_and_build.md](02_rpc_cli_and_build.md) | Every RPC method, how new ones get registered, CLI passthrough, CMake targets | 1, 2, 3 |
| [03_transaction_encoding_and_database.md](03_transaction_encoding_and_database.md) | `DigiByteTransaction` encode/decode, DigiAsset v3 wire format, Database asset/balance queries, wallet RPC integration | 1, 2, 3 |
| [04_qt_gui_and_web_docs.md](04_qt_gui_and_web_docs.md) | Qt tab structure/RPCLoader pattern, web docs structure, readme.md issues | 1, 2, 3 (GUI), 5 |
| [05_tests_and_psp.md](05_tests_and_psp.md) | Full test file map, test gaps, testFiles/ fixture, PermanentStoragePool module context | 4 |
| [06_task_checklist.md](06_task_checklist.md) | All 5 tasks mapped to specific files/functions, suggested order of attack | all |
| [07_corrections_and_gotchas.md](07_corrections_and_gotchas.md) | **Read this before trusting LAST_TASKS_NOTES.md/TODO_TESTS.md** — several of their claims turned out stale | all |
| [08_remote_synced_node.md](08_remote_synced_node.md) | **Read this before assuming live-chain testing is still blocked** — a second, fully-synced node exists now (no wallet funds, but unblocks the replay test / chain-query tests) | 4 |

**Other repo-root docs worth knowing about (not duplicated here):**
- `DATABASE_OPTIMIZATION_PLAN.md` — a separate, mostly independent proposal for
  SQLite tuning (WAL mode, cache size, etc). Not part of the 5 tasks; ignore unless
  asked, it was written by yet another session.
- `INVESTIGATION_SIGILL.md` — postmortem on the SIGILL crash bug (bug #1 in
  TODO_TESTS.md, already fixed). Background reading only if curious.
- `build_and_test.sh` — a Linux one-shot build+test script (apt-get deps, cmake
  configure with `-DBUILD_TEST=ON -DBUILD_CLI=ON -DBUILD_WEB=ON`, build, run tests).
  macOS/local dev doesn't need this — see `01_architecture_overview.md` for the
  local build commands actually used on this Mac.

**Constraint reminder, updated 2026-07-18 (originally written 2026-07-13):** the
LOCAL Mac node (`/Volumes/external/digibyte-data`) may still be resyncing — check
current status before assuming it's usable. **However, a second fully-synced node
is now available on a different machine** — see `08_remote_synced_node.md` before
assuming any live-chain test is still blocked. It has no wallet funds (so
balance/send/issue tests still need the local node), but it does unblock the
replay test and any pure chain-query RPC test. The `tests/testFiles/`
partial-chain fixture (see `05_tests_and_psp.md`) does NOT depend on either node
and can be used regardless.

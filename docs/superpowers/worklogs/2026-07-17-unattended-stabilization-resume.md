# Unattended stabilization resume — 2026-07-17 (afternoon)

Resumed per `2026-07-17-stabilization-pause-state.md`. User directives for this run:
deep systematic debugging on every bug (no handwaving / early conclusions), RCA document
per bug (`docs/superpowers/reports/`), fix from the RCA and mark fixed; if a fix does not
land in 3 attempts — park the bug for discussion. Watchdog cron re-armed at 20-minute
cadence (checks for hung commands/subagents). This file is the unattended-work log.

## State found on resume {#state}

- Branch `cas-gc-rebuild`, HEAD moved from pause point `75dfccc8e3a` to `927ea142c9c`:
  user merged `altinity/antalya-26.6` (PR #2061 line) and landed a cleanup sweep
  (namespace merge `DB::ContentAddressed` → `DB::Cas`, blob-hash/digest file reshape,
  dead-code removal, README rewrite, GC-lease heartbeat-seq fix `927ea142c9c`).
- Uncommitted tail: BACKLOG.md GREEN-DEBT entry (build-dir localize drift) — committed
  as `34c2d615874`.
- Compose cluster (`ca-soak-*`, multidisk) UP 7h with the PRE-MERGE fixed binary; `build/`
  binary equally pre-merge. Submodule pointers consistent with HEAD.
- Consequence: B199 RCA and R5 must run on a binary built from CURRENT HEAD (renames
  touched the RefWriter file family). `build/` needs a full cmake reconfigure first
  (GREEN-DEBT: all `localize_rust_c_*` rules in build.ninja argless).

## Queue (resume order) {#queue}

1. [in progress] Full cmake reconfigure of `build/` (forensic snapshot of the argless
   rules taken first) + full rebuild of current HEAD in background.
2. [in progress] GREEN-DEBT tails on the live cluster/old fixed binary (card-level fixes,
   binary-independent): S39 `--scale ci` run (expect 11/11), S37 dev run on hardened card.
3. B199 RCA (`RefWriterRecoverySeal.SealPutConflictThrowPropagatesAndDoesNotWedgeRecovery`)
   on the NEW binary — full systematic debugging, RCA doc.
4. R5 (#38): RESTART from S01 on the new binary; deliverable = full results table
   (№ / description / findings / fixed) across S01–S40.
5. R6: ASan/TSan pass.

## Log {#log}

- 15:2x — resumed; committed BACKLOG tail `34c2d615874`; watchdog cron re-armed (20 min).
- 19:36 — GREEN-DEBT CLOSED: S39 `--scale ci` PASS 11/11 + S37 dev (committed hardened card)
  PASS 23/23, both against the user's post-merge binary (26.6.1.20000.altinityantalya,
  built 15:12). Tasks #22/#23 completed. Build-dir localize drift: self-healed — the user's
  17:44 reconfigure regenerated build.ninja with reference libs present (forensic snapshot
  in tmp/forensics/).
- ~22:0x — **PRIORITY SWITCH (user)**: make CI/CD run for PR
  https://github.com/Altinity/ClickHouse/pull/2073 (push to altinity authorized for this
  branch only). Done so far:
  - Config Workflow "Workflows are outdated" → regenerated 4 YAMLs via `praktika yaml`
    (`835251f81cb`); the regeneration adds both CAS stateless lanes to the generated
    workflows. Pushed → Config Workflow GREEN 12/12, pipeline started.
  - rustfs CI debt: `start_rustfs` expected a hand-extracted binary at `ci/tmp/rustfs`
    (CI wipes ci/tmp per run) → added `download_rustfs` (GitHub release
    1.0.0-beta.9, musl, arch-aware) `00c8727632f`. Validated: beta.9 passes the
    conditional-op probe (second `If-None-Match: *` PUT → 412, wrong-etag DELETE → 412),
    provisioning tested end-to-end, local stand upgraded beta.8→beta.9.
  - Fast test red: `dbms` linked dead `ch_contrib::crc32c` (phase1a leftover; fast test
    doesn't init that submodule) → dropped the link, crc32c back under
    google-cloud-cpp-only (`4212f275e92`); local reconfigure clean. Pushed.
  - Poller armed on `result_fast_test.json` for the new sha.
  - Fast test round 2: cmake passed (crc32c fix worked), compile failed on unconditional
    `openssl/evp.h` in `CasBlobHashingWriteBuffer.cpp` → sha256 backend now `#if USE_SSL`
    (fail-closed `SUPPORT_IS_DISABLED` otherwise), pushed `5365b20b604`; preemptive sweep of
    the whole CAS diff found no other unguarded external-lib includes. Poller re-armed.
- 22:42 watchdog — unit_tests_dbms build at 3661/3663 (B199 RCA next); fast-test poller
  waiting; codex-review triage methodology presented to user, awaiting approval.

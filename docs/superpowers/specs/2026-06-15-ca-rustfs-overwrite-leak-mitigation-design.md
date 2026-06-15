# CA soak: RustFS overwrite-leak mitigation (design)

**Date:** 2026-06-15. **Branch:** `cas-mergetree-poc`. **Status:** brainstormed (self, unattended) → spec. **Scope: test-harness only** (RustFS defect, not CA/server code).

## Problem
RustFS `1.0.0-beta.8` does NOT reclaim the previous data dir on an un-versioned overwrite (E3: a 256 KB key overwritten 2000× → 2001 `<uuid>/part.1` dirs, scanner ON or OFF). The CA design overwrites each `roots/<t>/<ns>/<shard>` manifest on every commit, so `roots/` grew to **74 GB / 18 live keys** in ~1 h (findings §A, B158). Over a 12 h soak this fills the disk. It is a RustFS defect × CA's overwrite-heavy manifest pattern; widening `root_shards` (#4) only **distributes** the leak (fixes hot-key congestion/listability), it does not reclaim bytes.

## Design — an orphan-reaper sidecar (deterministic, scoped, safe)

A small periodic reaper running against the RustFS data volume, **scoped to `roots/`** (the only overwritten prefix — blobs/trees are immutable, written once, and are never touched).

### Mechanism
For each manifest object dir `…/roots/<t>/<ns>/<shard>/`:
- it contains `xl.meta` (the current-version marker) + one-or-more `<uuid>/` data dirs.
- the **newest `<uuid>/` by mtime is the current incarnation** (an un-versioned overwrite writes a new `<uuid>/`, repoints `xl.meta`, and leaks the old ones).
- the reaper **keeps `xl.meta` and every `<uuid>/` whose mtime is within a grace window** (default 120 s), and **deletes the rest** (`<uuid>/` dirs older than grace, excluding the single newest). Anything older than the grace that is not newest is a *confirmed* superseded orphan — a later write has since repointed `xl.meta`.

### Safety
- **Scoped to `roots/`** → cannot touch immutable blobs/trees (which have exactly one data dir anyway).
- **Grace window** (120 s) → never races an in-flight overwrite (a just-written dir is younger than grace and kept).
- **Keep-newest** → never deletes the current incarnation even if its mtime is old (a rarely-overwritten shard keeps its one dir).
- **Read-only on `xl.meta`** → never edits RustFS metadata; only removes confirmed-dead data dirs.
- Runs on the RustFS container's local FS (bypasses the S3 API), so it adds no S3 load / no 503 pressure.

### Wiring
- Script `utils/ca-soak/scripts/orphan_reaper.sh` (or a sidecar in `docker-compose.yml`): every `REAP_INTERVAL` (default 300 s), walk `/data/<bucket>/<pool>/roots`, apply the keep-newest+grace rule, log reclaimed bytes/dirs.
- Launched by `run_24h.sh` alongside the soak (and torn down with it). Logs to `logs/orphan_reaper_<ts>.log`.

### Secondary (cheap, complementary)
Leave `RUSTFS_SCANNER_ENABLED=false` (re-enabling caused 503 bursts and E3 showed it doesn't reclaim these orphans anyway) — the reaper is the reclaim mechanism. #4 (widen `root_shards`) keeps the *current* set listable so the reaper's per-object dirs stay small.

## Testing / validation
- **Unit-ish (script test):** create a fake `roots/<t>/<ns>/0/` tree with `xl.meta` + several aged `<uuid>/part.1` dirs + one fresh; run the reaper; assert only the aged-non-newest dirs are removed, `xl.meta` + newest + within-grace survive. Pure filesystem, no RustFS.
- **Live validation (in the soak):** with the reaper running, `du roots/` stays bounded (does not grow to tens of GB), and the pool stays correct — `system.parts` queries + checker SYNC REPLICA + `fsck` (quiesced) pass; zero `no ref`/broken parts attributable to the reaper.

## Fallback (if the reaper proves unsafe in testing → backlog)
If the live validation shows ANY reaper-induced read failure / loss, disable it immediately, fall back to "provision a large RustFS volume + document the leak as a RustFS defect," and backlog a cleaner mitigation (real S3, or a RustFS version that reclaims, or the deferred publish-path/op-count reduction that lowers the overwrite rate). Fail-closed: the reaper must never delete a current or within-grace dir.

## Scope
In: the reaper script + soak wiring + its tests. Out: any CA/server change; reducing the overwrite rate (GC/op-count, [[B160]]); production RustFS behavior.

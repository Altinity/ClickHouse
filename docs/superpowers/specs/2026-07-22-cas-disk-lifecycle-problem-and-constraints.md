# CAS disk lifecycle — problem statement, requirements, and constraints

Status: **NOT a design.** This is the problem definition + the full constraint set + the record of every
design we tried and why each failed, so a future, careful lifecycle design starts from the accumulated
evidence instead of re-deriving the dead-ends. Written 2026-07-22 after four successive designs (Dormant
husk, eject, auto-mount, self-quiesce/"Path B") were each shown unsound — the last by an adversarial
review (codex gpt-5.6-sol, high) that found the core assumption false.

Related (all superseded / partial): `2026-07-21-cas-mount-lease-abort-and-disk-lifecycle-design.md`
(rev.2 — Part 1 & Part 6 stand; Parts 2-4 rolled back), `2026-07-22-cas-disk-lifecycle-automount-design.md`
(eject/auto-mount, no code), `2026-07-22-cas-disk-self-quiesce-simplification-design.md` (Path B, blocked).

---

## 1. What a CAS disk is (the substrate that makes this hard)

A content-addressed (CAS) disk is not a passive blob store. A **mounted** CAS disk owns:

- **Background threads:** a GC round scheduler (+ advisory heartbeat), a mount-lease renewal thread, a
  self-remount recovery thread, and short-lived one-shot workers (snapshot-publish, anomaly-diagnostics).
- **A mount lease** in the shared backing store: a single-writer lease (writer epoch), GC leadership, and
  a heartbeat proving this node's liveness/participation. **Using the pool — reads included — is coupled
  to a live lease/heartbeat; using it without them violates the mount contract** (another node may
  fence/reclaim; incarnation/GC coordination assumes a live mount).
- **A pool object** (`cas_store`) holding the backend client, ref ledger, caches.
- **Shared, untracked ownership:** the disk is a `DiskPtr` (shared_ptr) handed out by the process-wide
  `Context` disk registry (`DiskSelector` + `StoragePolicySelector`) and copied into many transient and
  long-lived holders (loaded tables' parts, reservations, backups, all-disks-sweep snapshots). **No one
  tracks who holds it or for how long.**

The multi-node reality: a pool can be shared across servers; there is **no cluster-wide lock**; another
node's GC can be deleting objects concurrently at any moment.

## 2. The problem we are trying to solve

**Origin (real, shipped bug):** CI PR #2073 aborted the server. A test `rm -rf`'d a CAS pool dir while
the disk was still mounted and its background threads were running; the mount-lease renewal thread hit the
vanished backing, threw `LOGICAL_ERROR`, and in debug/ASan builds the exception *constructor* aborts the
process. Collateral: unrelated tests failed.

**Broader problem:** a CAS disk, once created (config disk at server start, or an inline `disk(...)` on
first table use), is **cached forever** in the disk registry with no teardown (`Context::getOrCreateDisk`,
the "disk-lifecycle-leak"). There is no clean, safe way to:

1. Stop a CAS disk's background work (so a pool dir can be removed / a disk decommissioned) without an
   abort or an endless error/retry spam.
2. Let generic ClickHouse code (all-disks sweeps, table load) cope when a CAS disk's data is not
   currently reachable, **without either hanging or silently losing/skipping data.**
3. Reuse / reset a CAS disk's state between uses (the motivating case: stateless tests reusing a disk
   name across retries in one long-lived server).
4. Verify a pool's integrity (`FSCK`) without a false verdict.

## 3. Goals (what a solution must achieve)

Ranked by how real the need is:

- **G1 — no abort.** A background CAS thread must never abort the server because its backing vanished or a
  transient error occurred. *(Part 1, already landed — sound.)*
- **G2 — no unbounded spam / zombie.** After a pool dir is removed under a mount, the disk's background
  work must stop (or become bounded), not spin forever logging errors every interval.
- **G3 — generic code must be correct against a not-fully-live CAS disk.** All-disks sweeps
  (`DROP TABLE` finalizer, orphaned-parts search, `Freeze`/`UNFREEZE`, `system.*`, `clearTransactionMetadata`)
  and table load/attach must neither **hang** (throw that a retry loop spins on) nor **silently skip/empty**
  (treat "can't reach the disk now" as "nothing here" → skipped reclaim = permanent ref leak, or empty part
  enumeration = silent empty table).
- **G4 — tests get deterministic isolation.** The CA stateless tests must pass on retry in a shared,
  long-lived server. *(Achievable independently — see §7.)*
- **G5 — `FSCK` gives no false verdicts** on a pool that may be concurrently mutated by another node.
- **G6 — operator decommission / maintenance** (nice-to-have): a way to quiesce/retire a disk on purpose.
- **G7 — runtime re-use of the *same* disk after a stop** (nice-to-have; **turned out to be the source of
  most of the pain** — see §5/§6). Every design that pursued this created a fragility cascade.

## 4. Hard constraints (invariants any solution MUST respect)

These are the load-bearing facts, most discovered the hard way:

- **C1 — "disk not mounted" ≠ "data gone".** Absence of the mount key, loss of the lease, a transient
  outage, mid-startup, `<readonly>` mode, and terminal shutdown are all not-fully-Mounted states in which
  **the refs/manifests/blobs may be fully intact.** Treating any of them as "data erased" and answering
  probes benign-absent silently skips real reclaim (permanent ref leak) or loads an empty table.
  *(The false assumption that sank Path B.)*
- **C2 — benign-absent is only safe for a POSITIVELY-established "data erased" state.** There is no cheap,
  non-racy positive signal for "the pool's data is really gone" on an arbitrary backend (a LIST can be
  transiently empty or eventually-consistent). Any not-mounted state that is not provably data-erased must
  **fail loud**, not benign-absent. *(Refined 2026-07-22, rev.6: a full-prefix LIST CAN serve as an erasure
  proof, but ONLY on backends with a documented strongly-consistent prefix LIST (AWS S3, GCS), only after
  the ref-lane settle/materialization-grace window, and only with spaced repeated samples; on all other
  backends (Local, Emulated, unqualified S3-compatible gateways, RustFS until evidence is provided) no
  natural proof exists and erasure requires an explicit operator assertion — `FORGET`.)*
- **C3 — the lease/heartbeat is coupled to ANY pool use, including reads.** You cannot stop the
  lease/heartbeat while anything still uses the pool without violating the mount contract. Therefore you
  cannot quiesce a disk that is in use. *(Refined 2026-07-22, rev.6: the lease gates operation ADMISSION
  and every DURABLE-EFFECT finalization (fence-generation checked at plain-object CAS writes and staging
  finalize); an already-admitted, non-durable in-flight read may complete on its existing pipeline — its
  failure modes are the backend's own errors.)*
- **C4 — usage is via untracked `shared_ptr`; you cannot reliably detect "unused".** `use_count` is racy,
  does not distinguish a millisecond sweep snapshot from a week-long backup, and never says *who*. The only
  clean signals are **semantic**: enumerate loaded `IStorage`s (the live-table guard) and any explicitly
  registered in-flight operation.
- **C5 — long operations can outlive a table and last arbitrarily long.** A backup can run for a week and
  hold the pool; you cannot "drain and wait" for it, and you cannot stop the lease under it (C3).
- **C6 — teardown must not run on the thread that is inside the disk's own callback.** The mount-lease
  keeper's background thread executes the lease-loss callback; if that callback tears down the last `Pool`,
  `~Pool` → `stopBackground` joins that same thread → self-join / UAF. Teardown/`~Pool` must happen on
  another thread, and scheduler shutdown must serialize through `gc_scheduler_mutex`.
- **C7 — lease loss has multiple distinct causes that must be typed.** Confirmed mismatch (superseded /
  foreign / gc-fenced) vs deadline-expiry from a transient outage vs mount-key-absent-but-data-intact vs
  confirmed-backing-destruction. The current `on_lost` callback is **untyped** and fires identically for
  all of them — so any action keyed off it (e.g. "go inert") misfires on a transient outage and can
  permanently disable a healthy disk.
- **C8 — the registry is keyed by disk NAME, not path.** `DiskFromAST` derives the key from the explicit
  `name`; `getOrCreateDisk` returns the existing object for a repeated name (and may reject changed custom
  settings). A unique *path* alone does NOT produce a fresh disk object — the **name** must be unique.
- **C9 — `MergeTreeData` does not cache a `DiskPtr`/`StoragePolicyPtr`.** `getStoragePolicy()` re-resolves
  every call via the selector caches. (This makes registry eviction *lifetime*-safe — but does not make it
  *reachability*-safe, see C1/C10.)
- **C10 — MergeTree part loading enumerates via `iterateDirectory`.** If that is benign-absent on a
  not-live disk, ATTACH/load reads **zero parts** and silently attaches an EMPTY table over intact data.
  A not-live disk must make data-bearing enumeration fail loud (C2), not merely make `store()` throw
  (part *load* reaches `iterateDirectory` before it reaches `store()`).
- **C11 — vanished-backing detection is incomplete via the lease alone.** GC-round failures do not notify
  the disk lifecycle; a `<readonly>` pool has no mount keeper at all. Loss of `_pool_meta` / GC state /
  blobs with the mount key intact is invisible to the lease path.
- **C12 — `runFsck` is a multi-phase scan with no coherent snapshot, racy against ANY node's GC.** Its
  hard findings (missing manifest at recovery; `meta_without_body` from separate raw LISTs) can
  false-positive under concurrent GC (body-delete-precedes-meta-delete) and under incoherent/eventually-
  consistent listings. `Dormant` on the local node does NOT protect it (other nodes still GC). A "still
  referenced" blob check alone is insufficient — it does not distinguish "body genuinely lost" from
  "body exists but LIST omitted it", and does not cover manifest findings. Every hard finding must be
  re-validated against the **authoritative current ref** and the exact body **HEAD**ed before it is
  reported.
- **C13 — upstream/generic-code minimization.** Prefer solutions inside `ContentAddressed*` + the
  sanctioned `SYSTEM`-verb family. Generic edits (disk-registry eviction across `DiskSelector` +
  `StoragePolicySelector`, or a hook in `MergeTreeData`) are high-scrutiny and were repeatedly the point
  where a "clean" idea turned murky (C4) or hacky.
- **C14 — no cluster-wide lock is introduced.** `ON CLUSTER` is the only pool-wide mechanism.
- **C15 — lock order** is `lifecycle_mutex → gc_scheduler_mutex → pointer_mutex`; the `poolAccess()`
  coherent snapshot (one acquisition of `pointer_mutex` returning `{pool, part-access}`) must be preserved
  (it fixed a real straddle bug). Any auto-mount-from-`store()` inverts this and deadlocks.

## 5. Designs tried, and why each is unsound (each dead-end encodes a constraint)

1. **Dormant husk (landed, Tasks 4-8):** `UNMOUNT` drains + quiesces into a persistent `Dormant` state,
   `MOUNT` re-mounts, `FSCK` dormant-only. → A persistent not-live-but-reachable disk forces every generic
   sweep into throw (hang — the original bug) or benign-absent (silent-skip). Part 6 chose benign-absent →
   **silent-skip reclaim leaks (H1/#1a/#2), ATTACH empty-load** (violates C1/C2/C3/C10). *Landed; its
   benign-absent hazard is latent in the shipped branch.*
2. **Explicit MOUNT/UNMOUNT reuse lifecycle:** same as (1) — reuse *requires* the persistent Dormant state.
3. **Auto-mount (lazy remount on access, then table-bind hook):** re-mount a Dormant disk on use. → Lazy
   `ensureMounted()` in `store()` inverts the lock order and deadlocks (C15); a benign probe can't tell a
   table-bind from a sweep, so the trigger becomes a `MergeTreeData` hook (C13 generic edit) that is
   honest but still rests on incidental call order; and even then ATTACH's benign `iterateDirectory`
   empty-loads (C10). Judged a hack.
4. **Eject (remove the disk from both registries on UNMOUNT):** make it Mounted-or-gone. → Audit found
   eviction is *lifetime*-safe (C9) — but it does not solve *reachability*: a straggler holding a
   pre-eviction `shared_ptr` still sees a quiesced disk (C4), you still cannot stop the lease under a
   week-long backup (C3/C5), and it reduces to "Dormant that throws". Also needs generic disk-registry
   surface (C13).
5. **Generic `SYSTEM UNMOUNT DISK`:** cleaner placement of (4), same C3/C4/C5 walls.
6. **Self-quiesce + inert (Path B):** on vanished-backing, stop all background work → inert; keep Part 6
   benign-absent (claimed "now safe" because "no Dormant-with-data"). → **Blocked by codex:** the trigger
   (lease-loss `on_lost`) is untyped and fires on transient outages → permanently inert a healthy disk
   (C7); mount-key-absent ≠ data-gone (C1) → benign-absent still silent-skips reclaim (C2); inert ATTACH
   empty-loads via `iterateDirectory` (C10); self-quiesce self-joins the keeper thread (C6); lease is an
   incomplete detector (C11); FSCK-on-mounted false-positives beyond `meta_without_body` (C12).

**The through-line:** every design reduces to *"a not-fully-live disk is reachable by generic code; what
does access do?"* — and the only two blunt answers (throw → hang; benign-absent → silent-loss) are both
wrong. The real requirement (C1/C2) is a **typed, state-aware answer**: fail-loud unless data is provably
erased.

## 6. What a sound solution MUST provide (requirements for the future design)

- **R1 — typed lease-loss / disk-health.** Distinguish: `transient` (retryable outage — do NOT change
  durable state), `fenced/superseded/foreign` (another node owns it — this node fail-loud, data may be
  intact elsewhere), `mount-key-absent-data-intact`, and `confirmed-data-erased`. Only the last enables
  benign-absent. (Addresses C1/C7.)
- **R2 — an explicit state model,** e.g. `Constructing → Mounted → {Unavailable/Fenced (fail-loud, intact
  data possible), Vanished (data provably erased — benign-absent ONLY here), Stopped (terminal shutdown,
  fail-loud)}`. `poolAccess`/`throwNotMounted` branch on state; the probe surface answers benign-absent
  ONLY in `Vanished`. (Addresses C1/C2, and the shutdown gap.)
- **R3 — data-bearing enumeration fails loud outside `Vanished`.** `iterateDirectory`/`listDirectory` on a
  not-live disk must throw (with a clear message) unless the disk is provably `Vanished`, so MergeTree
  never interprets an empty iterator as an empty table. (Addresses C10.)
- **R4 — generic reclaim/sweeps handle a fail-loud CAS disk correctly.** The `DROP TABLE` finalizer and
  peers must catch a not-live/`Unavailable` CAS disk and **defer/reschedule** (not conclude "already
  removed", not hang forever) — distinguishing transient-retry from give-up-with-a-loud-log; a `Vanished`
  disk's reclaim is a no-op (nothing to reclaim) but logged. Per-disk isolation (try/catch) so one disk
  never poisons a whole fan-out. (Addresses G3.)
- **R5 — positive vanished detection, covering all paths.** Establish `Vanished` from a positive check
  (pool meta / roots gone), reachable from the GC path and read-only pools, not only the writable mount
  keeper; guard against a transient LIST-empty false positive. (Addresses C11, C7.)
- **R6 — lifetime-safe stop.** Stop background work without tearing down the `Pool` on the keeper thread;
  hand teardown to another thread; serialize scheduler stop via `gc_scheduler_mutex`. (Addresses C6/C15.)
- **R7 — quiesce requires quiescence (if runtime stop is offered at all).** Do not stop the lease under an
  in-flight user; use the semantic guard (loaded `IStorage`s — widened past `MergeTreeData` to
  `StorageSet/Log/StripeLog/Join`, and in-flight backups) + refuse-when-busy (never wait a week, never
  violate the mount contract). (Addresses C3/C4/C5.)
- **R8 — FSCK re-validates every hard finding** against the authoritative current ref and a direct body
  HEAD, not raw LISTs + a reference-only check; then it may run on a mounted/concurrently-GC'd pool.
  (Addresses C5/C12.)
- **R9 — tests use unique disk NAMES** (and paths), updating all matching SQL predicates. (Addresses C8.)
- **R10 — minimize generic/upstream edits** (C13); no cluster lock (C14); preserve `poolAccess` snapshot
  and lock order (C15).

## 7. What is sound today (do independently of the hard redesign)

- **Part 1** (abort-hardening: vanished/absent lease → `FILE_DOESNT_EXIST`/no-op, never `LOGICAL_ERROR`)
  — landed, confirmed sound. It correctly treats a missing key as a recoverable lease-loss rather than
  aborting; the error was only in *promoting* that observation to "all data vanished" (C1).
- **The `05020` + CA-stateless test fix (G4):** the real cause is fixed-name reuse across retries in one
  server (C8). The fix — **unique disk NAMES** (`${CLICKHOUSE_TEST_UNIQUE_NAME}`) and unique paths
  (`${CLICKHOUSE_USER_FILES_UNIQUE}`), plus updated predicates — is independent, sound, and unblocks CI.
  This does NOT require any lifecycle redesign.
- **`poolAccess()` coherent snapshot, atomic-publish `startup()`, `ca-fsck` rename** — landed, independently
  good, keep.

## 8. The genuinely-hard open core (for the future design to answer)

1. How to **positively and cheaply** establish "this pool's data is really erased" (`Vanished`) without a
   racy/eventually-consistent LIST giving a false positive?
2. How should generic reclaim treat a CAS disk whose data may be intact but is **transiently unreachable**
   — reschedule (risk: infinite retry if it never returns) vs give-up-and-log (risk: leak) — and how to
   tell the two apart without a positive signal (see 1)?
3. Is **runtime reuse of the same disk (G7)** worth supporting at all, given it was the source of the
   Dormant/eject/auto-mount cascade? If tests use unique names (§7) and decommission is config+restart,
   G7 may be unnecessary — which removes the persistent not-live state entirely and collapses the problem
   to G1/G2/G3/G5.

The recommended framing for the next attempt: **do not build reuse.** Solve G1 (done), G2 (bounded/typed
stop of background work on *positively-confirmed* vanish, lifetime-safe — R5/R6), G3 (fail-loud state +
sweep handling — R2/R3/R4), and G5 (FSCK re-validation — R8), and treat any not-live-with-possibly-intact
data as **fail-loud**, never benign-absent. Reconsider whether Part 6's benign-absent should exist at all,
or be replaced by fail-loud + sweep-side handling (R4).

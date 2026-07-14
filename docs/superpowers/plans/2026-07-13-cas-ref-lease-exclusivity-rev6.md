---
description: 'Implementation plan for CAS ref protocol rev.6: lease-boundary exclusivity (observation-based liveness, conditional T_mat, epoch-closing seal, grace removal)'
sidebar_label: 'CAS Ref Lease Exclusivity rev.6 (plan)'
sidebar_position: 20260713
slug: /superpowers/plans/cas-ref-lease-exclusivity-rev6
title: 'CAS Ref Lease Exclusivity rev.6 Implementation Plan'
doc_type: 'reference'
---

# CAS Ref Lease Exclusivity rev.6 Implementation Plan {#rev6-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the approved spec
`docs/superpowers/specs/2026-07-13-cas-ref-lease-exclusivity-rev6-design.md`: solve writer
exclusivity once at the mount-lease boundary (clock-free observation, conditional T_mat,
epoch-closing recovery seal) and delete the ref protocol's grace machinery.

**Architecture:** TLA+ gates first (extend the ref model to prove `NoDivergentFold` under the new
mount rule; extend the mount model to prove observation-based reclaim). Then lease-side hardening
(observation reclaim, clean-release drain, T_mat), then the seal, then the hot-path simplification
(publish-from-live), then anomaly policy and detectors. Recovery is lazy per namespace in this
codebase, so "seal before writable" is realized as "seal inside `ensureRefTableRecovered`, before
the table's state is exposed" — equivalent for `NoDivergentFold` because an unrecovered table has
no writer-side state to diverge from.

**Tech Stack:** C++ (ClickHouse), gtest (`unit_tests_dbms`), TLA+/TLC (`docs/superpowers/models/`,
runner scripts + `tmp/tla2tools.jar`).

## Global Constraints {#global-constraints}

- Branch: `cas-gc-rebuild`. New commits only — never rebase or amend. Never commit to `master`.
- C++: Allman braces. Never `sleep` to fix a race. CAS is pre-release: **no compat scaffolding** —
  codec format versions may be bumped fail-closed, old versions rejected.
- Builds: `ninja -C <build_dir> unit_tests_dbms` with output redirected to a log file in the build
  directory; never pass `-j`; use a subagent to summarize the log. Some negative tests need a
  `chassert`-active build (debug/ASan).
- Tests: redirect each run to a unique log file in the build directory; use a subagent to summarize.
- gtest run: `./<build_dir>/src/unit_tests_dbms --gtest_filter='<F>'`.
- TLC run (models dir `docs/superpowers/models/`): `./run_refsnaplog.sh`, `./run_mount.sh <cfg>`;
  logs land in repo-root `tmp/`.
- Core code dir (abbreviated `Core/` below):
  `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`.
- Spec constants: observation threshold = `mount_lease_ttl_ms + mount_lease_ttl_ms/20 +
  poll_interval_ms`; `materialization_grace_ms` default `30000`; seal id =
  `RefTxnId{my_epoch - 1, UINT64_MAX}`; snapshot codec format version bumps `1 → 2` adding
  `sealed_from`.
- Commit messages: `cas: <what>`, ending with the `Co-Authored-By: Claude Fable 5
  <noreply@anthropic.com>` trailer.

## Execution order {#execution-order}

Tasks 1–2 (TLA+ gates) first and independent of each other. Then 3 → 4 → 5 → 6 → 7 → 8 (each
consumes the previous task's interfaces). 9 independent after 2. 10 after 8. 11–12 after 10.
13–14 last.

---

### Task 1: TLA+ gate — ref model rev.6 (`NoDivergentFold`) {#task-1}

(amended 2026-07-14: `CoveredFold` fix + `_rev6_latedelivery` expectation corrected to GREEN —
in-flight transient inexpressible under the reader-freeze abstraction; falsifiability held by
`rev6_freshreader`. See the amendment note after Step 5 below for the full explanation and the
verified guard names — do not re-derive the "expected VIOLATION" framing below at face value, it
was superseded.)

**Files:**
- Modify: `docs/superpowers/models/CaRefTableSnapshotLogCore.tla`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_safe.cfg`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_latedelivery.cfg`
- Create: `docs/superpowers/models/CaRefTableSnapshotLogCore_rev6_freshreader.cfg`
- Modify: `docs/superpowers/models/run_refsnaplog.sh`

**Interfaces:**
- Consumes: existing model (vars `op, writtenEver, logs, snaps, publishedEver, snapCov, nextId,
  completed, badRecreate`, reader vars, actions `LatePredecessorPut`, invariant `INV_RECOVERY`;
  constant `LatePred`).
- Produces: constant `Rev6MountRule`, ghost var `droppedEver`, oracle `WStateRev6`, invariants
  `NoDivergentFold` (strict) and `INV_FRESH_READER` (weak, for the post-T_mat violation case);
  runner expectation table updated. Later tasks cite these names in code comments only.

- [ ] **Step 1: Add the rev.6 semantics to the model**

In `CaRefTableSnapshotLogCore.tla`:

```tla
CONSTANTS ..., LatePred, Rev6MountRule    \* Rev6MountRule: coverage-at-birth drops a late PUT

VARIABLES ..., droppedEver,   \* ghost: late PUTs that landed under an existing snapshot
          rStartedAfterDrop   \* ghost: reader started after the last late delivery

\* Init adds: droppedEver = {} /\ rStartedAfterDrop = TRUE

\* rev.6 oracle: contract-clean truth excludes never-ACKed writes dropped by coverage-at-birth
WStateRev6 == FoldIds(EmptyState, writtenEver \ droppedEver)

LatePredecessorPut ==
    /\ LatePred = TRUE
    /\ \E L \in Seqs :
        /\ op[L] = "none" /\ L \notin writtenEver
        /\ \E X \in snaps : X > L          \* lands under an existing snapshot
        /\ LifeBelow(L) = "live"
        /\ op' = [op EXCEPT ![L] = "mut"]
        /\ writtenEver' = writtenEver \cup {L}
        /\ logs' = logs \cup {L}
        /\ droppedEver' = IF Rev6MountRule THEN droppedEver \cup {L} ELSE droppedEver
        /\ rStartedAfterDrop' = FALSE      \* any in-flight reader may transiently see L
        /\ UNCHANGED <<snaps, publishedEver, snapCov, nextId, completed, badRecreate,
                       rPhase, rScanPos, rSeenLogs, rSeenSnaps, rPickedSnap, rRestarts>>

\* ReaderStart additionally sets rStartedAfterDrop' = TRUE

\* Strict: every finished reader reconstructs the rev.6 oracle.
NoDivergentFold == (rPhase = "done" /\ Rev6MountRule) => (Reconstruct = WStateRev6)

\* Weak (post-T_mat violation containment): a reader that STARTED after the last late
\* delivery always agrees. In-flight readers may transiently include the dropped log;
\* folds re-derive each round, error direction is spare-not-delete.
INV_FRESH_READER == (rPhase = "done" /\ Rev6MountRule /\ rStartedAfterDrop)
                        => (Reconstruct = WStateRev6)

\* Snapshot byte-determinism under rev.6: every published body equals the oracle fold below it.
INV_SNAP_DETERMINISTIC == Rev6MountRule =>
    \A X \in snaps : snapCov[X] = FoldIds(EmptyState, {i \in (writtenEver \ droppedEver) : i <= X})
```

**AMENDED 2026-07-14 — load-bearing, do not omit:** the snippet above is not sufficient by itself.
`WriterPublishSnapshot`'s own fold formula must ALSO exclude `droppedEver` under `Rev6MountRule`, or
`INV_SNAP_DETERMINISTIC` is unsatisfiable by construction (any snapshot published at/above a dropped
id, after the drop landed, would silently re-include it — this was caught by the gate itself, not by
inspection, the first time this task ran):
```tla
CoveredFold(X) == IF Rev6MountRule
                   THEN FoldIds(EmptyState, { i \in (writtenEver \ droppedEver) : i <= X })
                   ELSE FoldIds(EmptyState, { i \in writtenEver : i <= X })
\* WriterPublishSnapshot: snapCov' = [snapCov EXCEPT ![X] = CoveredFold(X)]   (was: raw refold)
```

Keep `INV_RECOVERY` for the legacy configs (guard it with `~Rev6MountRule` where the two would
conflict is NOT needed — legacy configs set `Rev6MountRule = FALSE` so `NoDivergentFold` is
vacuous there and `INV_RECOVERY` keeps its old meaning).

- [ ] **Step 2: Write the three new configs**

`CaRefTableSnapshotLogCore_rev6_safe.cfg` — deliveries only pre-coverage (T_mat honored). Model
this by keeping `LatePred = FALSE` (no under-snapshot delivery can occur) and asserting all
invariants:

```text
SPECIFICATION Spec
CONSTANTS MaxSeq = 4  MaxRestarts = 2
  SabotageDeleteBeforeSnapshot = FALSE  SabotageVanishIsCorruption = FALSE
  SabotageRecreateBeforeCompleted = FALSE  SabotageRemountKeepsOldEpoch = FALSE
  LatePred = FALSE  Rev6MountRule = TRUE
INVARIANTS TypeOK INV_RECOVERY INV_NOFAIL INV_RECREATE NoDivergentFold INV_SNAP_DETERMINISTIC INV_FRESH_READER
```

`_rev6_latedelivery.cfg` — the late-delivery-under-an-existing-snapshot demo: `LatePred = TRUE,
Rev6MountRule = TRUE`, INVARIANTS `TypeOK NoDivergentFold` → **AMENDED 2026-07-14: expected GREEN**,
`MaxSeq = 5` (not 4 — see amendment note after Step 5 for why 5, not 4, is the bound to use). The
original brief expected a VIOLATION here ("an in-flight reader transiently sees the dropped log");
that premise does not hold once `WriterPublishSnapshot` correctly excludes `droppedEver` (see the
`CoveredFold` amendment to Step 1 above) — do not re-introduce the raw refold to manufacture a
violation.

`_rev6_freshreader.cfg` — same constants, `MaxSeq = 5`, INVARIANTS
`TypeOK INV_FRESH_READER INV_SNAP_DETERMINISTIC` → expected **GREEN** (fresh observers are
deterministic; snapshots stay byte-deterministic). This config is the regression guard for the
`CoveredFold` fix: it was RED on `INV_SNAP_DETERMINISTIC` before the fix, proving the invariant is
falsifiable, not vacuously green.

Legacy configs are untouched except each existing `.cfg` gains `Rev6MountRule = FALSE`.

- [ ] **Step 3: Run the legacy expectation table (must be unchanged)**

Run: `cd docs/superpowers/models && ./run_refsnaplog.sh > ../../../tmp/tlc_refsnaplog_legacy.log 2>&1`
Expected: same PASS table as before the change (safe green, each sab red on its invariant,
`latepred` red = PASS). Use a subagent to summarize the log.

- [ ] **Step 4: Add the three rev.6 configs to `run_refsnaplog.sh` with their expectations**
(`rev6_safe` → GREEN, `rev6_latedelivery` → **AMENDED: GREEN, not violation-is-PASS**,
`rev6_freshreader` → GREEN) and run again: `./run_refsnaplog.sh > ../../../tmp/tlc_refsnaplog_rev6.log
2>&1`. Expected: full PASS table. If `rev6_freshreader` is red, the model (or the design) has a hole
— STOP and report; do not proceed to implementation tasks. (This is exactly what happened the first
time this task ran, with the original un-amended snippet — see the amendment note below.)

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/models/CaRefTableSnapshotLogCore.tla docs/superpowers/models/*.cfg docs/superpowers/models/run_refsnaplog.sh
git commit -m "cas: TLA ref model rev.6 — NoDivergentFold under coverage-at-birth seal"
```

#### Amendment (2026-07-14): `CoveredFold` fix and the `_rev6_latedelivery` GREEN {#task-1-amendment}

Running this task as originally written produces `rev6_freshreader` RED on
`INV_SNAP_DETERMINISTIC` — the Step-1 snippet transcribed above never propagates `droppedEver` into
`WriterPublishSnapshot`'s own fold, so any snapshot published at/above a dropped id, after the drop
landed, silently re-includes it. The Step 1 section above has been amended in place with the
`CoveredFold` fix; apply that, not the original unfixed formula.

With `CoveredFold` in place, `_rev6_latedelivery` (checking `NoDivergentFold` alone) unexpectedly also
came back GREEN rather than the originally-expected violation, at every bound tested:
- `MaxSeq = 5`, exhaustive: no error, 35,656,456 states generated, 0 left on queue (8s) — a clean
  proof, not a timeout.
- `MaxSeq = 6`, exhaustive: no violation found before being stopped at 426,892,469 states generated /
  13,277,931 states left on queue (still climbing) after ~5 minutes wall time / 10+ GB resident —
  abandoned as impractical rather than left to explore an apparently-empty region indefinitely.
- `MaxSeq = 12`, random simulation (`-simulate num=1000000 -depth 60`, `MaxRestarts=3`): 183,514,671
  states checked across 1,000,000 traces in ~100s, zero violations.

This is not "the bound was too small" — re-tracing the original (pre-fix) `MaxSeq=6` counterexample
showed its violating reader had picked a snapshot whose body wrongly included the dropped id, i.e.
that counterexample was itself the `INV_SNAP_DETERMINISTIC` defect surfacing through a different
invariant. Once the fold is correct, no distinct counterexample has been found.

**Verified guard (do not assume, check `CaRefTableSnapshotLogCore.tla` directly if this drifts):**
`ReaderInactive == rPhase = "idle"` gates exactly six actions — `WriterBirth`, `WriterMut`,
`WriterRemove`, `WriterRebirth`, `WriterFail`, `GcComplete` — the ordinary namespace-lifecycle writes.
Once the model's single global reader starts (`rPhase` only ever moves forward,
idle→scan→fetch→{done,failed,stuck}, never back to idle), none of those six can fire again for the
rest of the run. `ReaderInactive` does **not** gate `WriterPublishSnapshot` (explicitly commented
"off-lane; may run during a reader's recovery"), nor `GcCleanupLog`, `GcCleanupSnap`, or
`LatePredecessorPut` — all four remain concurrent with an in-flight reader, by design. **The green
result does not depend on publish being frozen** (it isn't); it depends on the conjunction of:
1. ordinary writes freezing on reader-start (so any id later dropped must have been created either
   before the reader started, or by the drop action itself — never by a concurrent ordinary write);
2. the model's key space sorting all `_log` keys before all `_snap` keys regardless of id
   (`KeyLt`: kind-major), so the reader's single ordered scan can only fail to enumerate a currently-
   present snapshot if that snapshot did not yet exist at the instant its scan completed;
3. `GcCleanupSnap` never deleting the sole/newest present snapshot (its guard requires a strictly
   greater one to already coexist);
4. `LatePredecessorPut`'s own precondition requiring an existing covering snapshot `X > L` before a
   drop can happen at all — and, because that snapshot was present before or at `L`'s creation, (2)+(3)
   guarantee the reader's scan (whether it started before or after `X` published) will always still
   enumerate `X` or a still-newer replacement before its own scan can complete;
5. `CoveredFold` ensuring every published `snapCov[X]` already excludes `droppedEver` as of its own
   publish time (whether because a not-yet-landed `L` simply wasn't `writtenEver` yet, or because `L`
   was already dropped by the time a later `X` published) — and both `droppedEver` and `writtenEver`
   only grow, never shrink, so this stays true going forward.

Chained together: whatever snapshot a reader ultimately picks is always `>= X > L` for any `L` that
could possibly be dropped during its lifetime, so `L` can only ever land in `Reconstruct`'s *base*
(already excluded by (5)), never its *tail* (which structurally excludes anything `<= rPickedSnap`).
`NoDivergentFold` only constrains `rPhase = "done"` states, so the "in-flight reader transiently sees
it" case the original brief described was never actually reachable in a way this invariant could
observe — the only real failure mode was the `CoveredFold`/`INV_SNAP_DETERMINISTIC` bug, now fixed.

**This is an abstraction-limit finding, not a stronger-than-designed system property.** The real
system's in-flight T_mat-violation transient (accepted, spec's T_mat/seal sections — Tasks 6-8) is not
expressible in this model, because this model serializes the reader against ordinary writes
(property 1 above) in a way the real system does not: a real reader observes a live, concurrently-
mutating table, not a frozen one. `_rev6_latedelivery` is therefore GREEN here as a fact about this
model's reader-freeze abstraction, not a proof that the real system's transient cannot happen.
`INV_FRESH_READER` remains the fresh-observer guarantee the design actually needs; `rev6_freshreader`
is the regression guard for the `CoveredFold` fix (proven falsifiable — it was RED before the fix).

---

### Task 2: TLA+ gate — mount model observation-based reclaim {#task-2}

**Files:**
- Modify: `docs/superpowers/models/CaCasMountCore.tla`
- Create: `docs/superpowers/models/CaCasMountCore_rev6_observe.cfg`
- Create: `docs/superpowers/models/CaCasMountCore_sab_wallclockreclaim.cfg`

**Interfaces:**
- Consumes: existing vars (`owner, epoch, mount, mtoken, clock, ...`), invariants
  (`SupersededWriterMakesNoMutation`, `WriterEpochMonotoneUnique`), `TTL` constant.
- Produces: constants `Drift`, `SabWallClockReclaim`; vars `fenceUntil`, `obsToken`, `obsSince`;
  action `ObservedReclaim`; witness `W_ObservedReclaim`.

- [ ] **Step 1: Model clock-rate drift and the two reclaim rules**

In `CaCasMountCore.tla`:

```tla
CONSTANTS ..., Drift,               \* max extra ticks the holder's true fence outlives the stamp
          SabWallClockReclaim       \* TRUE: reclaim trusts the stamped deadline (the old bug)

VARIABLES ..., fenceUntil,  \* holder's TRUE local-fence expiry (stamp + nondet skew <= Drift)
          obsToken, obsSince \* reclaimer's observation: token first seen, at which clock tick

\* On every renew/claim by the holder:
\*   mount.deadline' = clock + TTL                      (the stamp others read)
\*   \E d \in 0..Drift : fenceUntil' = clock + TTL + d  (what physics actually guarantees)
\* Holder write actions are guarded by clock <= fenceUntil (not the stamp).

StartObservation ==
    /\ mount # None /\ obsToken = None
    /\ obsToken' = mtoken /\ obsSince' = clock
    /\ UNCHANGED <<...>>

\* Observation restarts implicitly: any mount write bumps mtoken; ObservedReclaim requires match.
ObservedReclaim ==
    /\ ~SabWallClockReclaim
    /\ obsToken # None /\ obsToken = mtoken            \* token stable since obsSince
    /\ clock - obsSince >= TTL + Drift                 \* full rate-bound wait on OUR clock
    /\ <take over: bump epoch, write mount, reset obsToken' = None>

WallClockReclaim ==   \* the sabotage: trust the stamp
    /\ SabWallClockReclaim
    /\ mount # None /\ clock > mount.deadline
    /\ <take over: bump epoch, write mount>

W_ObservedReclaim == ~(<some state where a same-uuid successor completed ObservedReclaim>)
```

- [ ] **Step 2: Configs.** `_rev6_observe.cfg`: `SabWallClockReclaim = FALSE, Drift = 2, TTL = 3`,
INVARIANTS include `SupersededWriterMakesNoMutation`, plus witness config expectation
(`W_ObservedReclaim` violated = reachable). `_sab_wallclockreclaim.cfg`:
`SabWallClockReclaim = TRUE` → expected **VIOLATION of `SupersededWriterMakesNoMutation`** (the
holder, whose true fence outlives the stamp by `Drift`, writes after a stamp-trusting takeover).

- [ ] **Step 3: Run** `./run_mount.sh CaCasMountCore_rev6_observe > ../../../tmp/tlc_mount_rev6.log 2>&1`
and the sabotage config; also re-run the existing `CaCasMountCore_stage1` and `_sab_*` configs
(with `Drift = 0, SabWallClockReclaim = FALSE` added) — legacy table unchanged. Subagent
summarizes.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaCasMountCore.tla docs/superpowers/models/CaCasMountCore_*.cfg
git commit -m "cas: TLA mount model — observation-based reclaim vs wall-clock sabotage"
```

---

### Task 3: `sealed_from` in the snapshot codec (format v2) {#task-3}

**Files:**
- Modify: `Core/CasRefSnapshotCodec.h` (struct at `:41-51`, format doc `:58-62`)
- Modify: `Core/CasRefSnapshotCodec.cpp` (`kRefTableSnapshotFormatVersion` `:27`,
  `encodeRefTableSnapshot` `:184`, `decodeRefTableSnapshot` `:211`, `checkSnapshotInvariants` `:161`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp` (round-trip tests near the existing codec tests)

**Interfaces:**
- Consumes: `RefTxnId` (`Core/CasRefIds.h:27-30`).
- Produces: `RefTableSnapshot::sealed_from` — `std::optional<RefTxnId>`; format version 2 (v1
  rejected fail-closed, CAS is pre-release); invariant: if `sealed_from` present then
  `*sealed_from <= snapshot_id`.

- [ ] **Step 1: Write the failing round-trip test**

```cpp
TEST(RefSnapshotCodec, SealedFromRoundTripsAndOrderingEnforced)
{
    DB::Cas::RefTableSnapshot s;
    s.ns = "ns1";
    s.snapshot_id = DB::Cas::RefTxnId{2, UINT64_MAX};       /// a seal id: (epoch-1, MAX)
    s.lifecycle = DB::Cas::RefLifecycle::Live;
    s.sealed_from = DB::Cas::RefTxnId{2, 17};               /// greatest listed id
    const auto bytes = DB::Cas::encodeRefTableSnapshot(s);
    const auto back = DB::Cas::decodeRefTableSnapshot(bytes, "ns1", s.snapshot_id);
    EXPECT_EQ(back, s);

    s.sealed_from = DB::Cas::RefTxnId{3, 1};                /// > snapshot_id: must throw
    EXPECT_ANY_THROW(DB::Cas::encodeRefTableSnapshot(s));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `ninja -C build_debug unit_tests_dbms > build_debug/build_t3.log 2>&1` — fails to compile
(`sealed_from` not a member). Subagent confirms the error is the expected one.

- [ ] **Step 3: Implement**

`CasRefSnapshotCodec.h`: add `std::optional<RefTxnId> sealed_from;` to `RefTableSnapshot` (keep
`operator== = default`); extend the wire-format doc comment: `... u8 lifecycle |
[remove_txn_id if Removed] | u8 has_sealed_from | [sealed_from] | u32 n_committed | ...`.
`CasRefSnapshotCodec.cpp`: `kRefTableSnapshotFormatVersion = 2`; encode/decode the presence byte +
id; `checkSnapshotInvariants`: throw `LOGICAL_ERROR` if `sealed_from && snapshot_id < *sealed_from`;
decoder rejects any version `!= 2` (fail-closed, no v1 acceptance).

- [ ] **Step 4: Run to verify it passes**

Run: `./build_debug/src/unit_tests_dbms --gtest_filter='RefSnapshotCodec.*' > build_debug/test_t3.log 2>&1`
Expected: PASS. Also run the full existing ref suite (`--gtest_filter='RefWriter*'`) — the version
bump must not break any test (helpers re-encode with v2 automatically).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefSnapshotCodec.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: snapshot codec v2 — sealed_from upper-bound metadata for the recovery seal"
```

---

### Task 4: Observation-based lease reclaim {#task-4}

**Files:**
- Modify: `Core/CasServerRoot.h` (`MountClaimResult` `:160-175`, `claimMount` decl `:190-192`,
  `claimMountAwaitingExpiry` decl `:212-218`, decision-table comment `:143-159`)
- Modify: `Core/CasServerRoot.cpp` (`claimMount` `:337-415`, `claimMountAwaitingExpiry` `:437-475`)
- Modify: `Core/CasStore.cpp` (call sites `:381` in open, `:635` in remount — pass the new args)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `Backend::get` returning `{body, token}`; `putOverwrite(key, body, token)`.
- Produces:
  ```cpp
  enum class MountPriorState { None, Clean, Fenced, UncleanObserved };
  struct MountClaimResult { Kind kind; MountLease body; MountPriorState prior; };
  MountClaimResult claimMount(Backend &, const Layout &, const String & srid,
      UInt128 our_uuid, uint64_t our_epoch, uint64_t now_ms, uint64_t ttl_ms,
      const std::optional<Token> & proven_dead_token = {}, const CasEventSink & sink = {});
  MountClaimResult claimMountAwaitingExpiry(Backend &, const Layout &, const String & srid,
      UInt128 our_uuid, uint64_t our_epoch,
      const std::function<uint64_t()> & now_ms_fn,          // wall: stamping/diagnostics only
      const std::function<uint64_t()> & mono_ms_fn,         // observation clock
      uint64_t ttl_ms, uint64_t poll_interval_ms,
      const std::function<void(uint64_t)> & sleep_ms_fn,
      const std::function<void(const MountLease &, uint64_t)> & on_wait_start = {},
      const CasEventSink & sink = {});
  ```
  Semantics: same-uuid different-epoch **not fenced, not clean-marked** lease is never reclaimed by
  wall clock; `claimMount` returns `LiveDoubleStart` for it unless `proven_dead_token` matches the
  current token (then token-guarded reclaim). `gc_fenced` and clean-marker (`min_active ==
  UINT64_MAX`) branches reclaim immediately as today, with `prior = Fenced` / `Clean`.
  `claimMountAwaitingExpiry` implements the observation loop and stamps `prior = UncleanObserved`
  on an observed takeover. Observation threshold: `ttl_ms + ttl_ms / 20 + poll_interval_ms` on
  `mono_ms_fn`. Foreign uuid: unchanged (`ForeignOwner`, never waited).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasMountObservation, ExpiredLookingLeaseIsNotReclaimedByWallClock)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout l{"p"};
    /// Predecessor epoch 7 stamped expires_at_ms = 1000; our wall clock says 999999 (long past).
    auto first = DB::Cas::claimMount(*b, l, "r", UInt128(1), 7, /*now_ms=*/500, /*ttl_ms=*/500);
    ASSERT_EQ(first.kind, DB::Cas::MountClaimResult::Claimed);
    auto r = DB::Cas::claimMount(*b, l, "r", UInt128(1), /*our_epoch=*/8, /*now_ms=*/999999, 500);
    EXPECT_EQ(r.kind, DB::Cas::MountClaimResult::LiveDoubleStart);  /// no wall-clock trust
}

TEST(CasMountObservation, TokenStableForThresholdThenReclaimed)
{
    auto b = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout l{"p"};
    ASSERT_EQ(DB::Cas::claimMount(*b, l, "r", UInt128(1), 7, 500, 500).kind,
              DB::Cas::MountClaimResult::Claimed);
    uint64_t mono = 0;
    std::vector<uint64_t> sleeps;
    auto r = DB::Cas::claimMountAwaitingExpiry(*b, l, "r", UInt128(1), 8,
        []{ return uint64_t{999999}; },                 /// wall clock: irrelevant
        [&]{ return mono; },                            /// observation clock
        /*ttl_ms=*/500, /*poll_interval_ms=*/50,
        [&](uint64_t ms){ sleeps.push_back(ms); mono += ms; });
    EXPECT_EQ(r.kind, DB::Cas::MountClaimResult::Claimed);
    EXPECT_EQ(r.prior, DB::Cas::MountPriorState::UncleanObserved);
    EXPECT_GE(mono, 500 + 500 / 20 + 50);               /// full threshold actually waited
}

TEST(CasMountObservation, RenewalDuringObservationRestartsIt)
{
    /// Same setup; a MountLeaseKeeper for epoch 7 renews once mid-observation (token bump).
    /// Expect: the successor does NOT take at the first threshold from t0; it needs a full
    /// threshold of stability AFTER the renewal. Assert via total mono elapsed >= 2 windows - poll.
}

TEST(CasMountObservation, GcFencedIsReclaimedInstantlyWithPriorFenced)
{
    /// Fence the lease via computeHeartbeatFloor's fence write (or a manual putOverwrite with
    /// gc_fenced=true, seq+1), then claimMount with our_epoch=8: Claimed immediately,
    /// prior == MountPriorState::Fenced, no observation sleep.
}
```

(Write the two sketched bodies fully in the test file, following the pattern of the first two —
lease bodies via `claimMount`, keeper via the `MountLeaseKeeper` ctor as in
`gtest_cas_mount.cpp:117-130`.)

- [ ] **Step 2: Run to verify failure**

`ninja -C build_debug unit_tests_dbms > build_debug/build_t4.log 2>&1` — compile fails
(`MountPriorState` unknown / signature mismatch). Then after stubbing, behavioral tests fail on
the old wall-clock reclaim. Subagent confirms.

- [ ] **Step 3: Implement**

`claimMount`: replace the `expires_at_ms <= now_ms` reclaim condition (`cpp:397-414`). New branch
logic for same-uuid different-epoch:

```cpp
const bool clean_marker = got_lease.min_active == UINT64_MAX;
if (got_lease.gc_fenced || clean_marker
    || (proven_dead_token && *proven_dead_token == got->token))
{
    /// token-guarded reclaim exactly as before (putOverwrite with got->token, seq+1)
    result.prior = got_lease.gc_fenced ? MountPriorState::Fenced
                 : clean_marker        ? MountPriorState::Clean
                                       : MountPriorState::UncleanObserved;
    ...
}
else
    return {MountClaimResult::LiveDoubleStart, got_lease, MountPriorState::None};
```

`claimMountAwaitingExpiry`: observation loop replacing the wall-deadline wait (`cpp:437-475`):

```cpp
const uint64_t threshold_ms = ttl_ms + ttl_ms / 20 + poll_interval_ms;
std::optional<Token> observed;
uint64_t observed_since = 0;
size_t restarts = 0;
while (true)
{
    auto r = claimMount(b, l, srid, our_uuid, our_epoch, now_ms_fn(), ttl_ms,
        (observed && mono_ms_fn() - observed_since >= threshold_ms) ? observed : std::nullopt,
        sink);
    if (r.kind != MountClaimResult::LiveDoubleStart)
        return r;
    const auto got = b.get(l.mountKey(srid));
    if (!got) continue;                            /// vanished: next claimMount claims fresh
    if (!observed || *observed != got->token)
    {
        if (observed && ++restarts > kMaxObservationRestarts)   /// = 3, holder is alive
            return r;                                            /// LiveDoubleStart to caller
        observed = got->token;
        observed_since = mono_ms_fn();
        if (on_wait_start)
            on_wait_start(r.body, threshold_ms);
        LOG_INFO(getLogger("CasMountLease"),
            "Attempting to mount content-addressed server root {} after node change or hard "
            "restart; waiting ~{} ms (token-stability observation) to confirm the previous "
            "incarnation's operations are all finalized", srid, threshold_ms);
    }
    sleep_ms_fn(poll_interval_ms);
}
```

Update both call sites in `CasStore.cpp` (`:381`, `:635`) to pass
`mono_ms_fn = [this]{ return bootMsNow(); }` and drop the old `margin_ms` argument. Update the
decision-table comment at `CasServerRoot.h:143-159` to the new rules.

- [ ] **Step 4: Run to verify pass**

`./build_debug/src/unit_tests_dbms --gtest_filter='CasMountObservation.*:CasMount*:CasHeartbeat*:CasStore*' > build_debug/test_t4.log 2>&1`
Expected: new tests PASS; pre-existing suites PASS except tests that asserted wall-clock reclaim —
rewrite those to the observation semantics (e.g. a test claiming instantly on expired stamp now
asserts `LiveDoubleStart`). Subagent lists every pre-existing test it had to touch, with reasons.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m "cas: observation-based lease reclaim — no cross-node wall-clock trust on takeover"
```

---

### Task 5: Clean-release drain gates the farewell marker {#task-5}

**Files:**
- Modify: `Core/CasStore.h` (Store members; near `wedgedRefLaneCount` decl `:819`)
- Modify: `Core/CasStore.cpp` (`~Store` `:461-504`, `mutateRef` entry, new drain method)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `RefTableRuntime::{pending, leader_active, cv, wedge, state_mutex}`; `ref_queue_mutex`;
  `mount_keeper->stop()` (farewell) vs `mount_keeper->stopBackground()` (no terminal op).
- Produces:
  ```cpp
  /// Stops admitting new ref mutations, waits (bounded) for queues to empty and leaders to
  /// exit, then reports whether any unresolved conditional PUT (wedge) remains.
  /// true = drained: no in-flight ref-log PUT can exist; farewell marker is safe.
  bool Store::drainRefLanesForShutdown(uint64_t wait_budget_ms);
  std::atomic<bool> Store::shutting_down{false};   // checked at mutateRef admission
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasStoreShutdown, CleanStopDrainsAndWritesFarewell)
{
    /// Open a writable Store over InMemoryBackend, append one committed ref txn,
    /// destroy the Store; then decode the mount lease body:
    /// EXPECT min_active == UINT64_MAX (farewell written — drain succeeded).
}

TEST(CasStoreShutdown, UnresolvedWedgeSkipsFarewell)
{
    /// Use failNextCasPut / a ThrowingSingleAttemptBackend-style subclass so the ref-log PUT
    /// ends Unresolved (wedge set, as in existing wedge tests), then destroy the Store:
    /// EXPECT the lease body's min_active != UINT64_MAX (no farewell), and gc_fenced == false.
    /// A successor claimMount on this body must return LiveDoubleStart (unclean path).
}
```

- [ ] **Step 2: Run to verify failure** — first test may already pass (stop() writes farewell
unconditionally today); the second MUST fail (farewell currently written despite the wedge).
`--gtest_filter='CasStoreShutdown.*'`, log to `build_debug/test_t5_fail.log`.

- [ ] **Step 3: Implement**

`drainRefLanesForShutdown`: set `shutting_down = true` (new ref mutations throw
`ABORTED` "store is shutting down"); snapshot runtimes under `ref_queue_mutex`; for each, wait on
`cv` until `pending.empty() && !leader_active`, bounded overall by `wait_budget_ms` (use
`cv.wait_for` slices; no sleeps); then under each `state_mutex` collect `wedge.has_value()`.
Return `!any_wedge && !timed_out`. In `~Store` (`:461-504`), before `mount_keeper->stop()`:

```cpp
const bool drained = drainRefLanesForShutdown(
    config.cas_request_budget.attempt_timeout_ms + config.cas_request_budget.lease_safety_margin_ms);
if (mount_keeper)
{
    if (drained)
        mount_keeper->stop();            /// terminal farewell: released clean
    else
    {
        LOG_WARNING(log, "CAS store shutdown with an unresolved ref-log PUT: skipping the "
                         "clean-release marker; the next mount will treat this end as unclean");
        mount_keeper->stopBackground();  /// no terminal op — successor observes + waits T_mat
    }
}
```

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasStoreShutdown.*:CasStore*'`, log
`build_debug/test_t5.log`, subagent summary.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: clean release drains ref lanes; farewell marker only after a successful drain"
```

---

### Task 6: `materialization_grace_ms` (T_mat) at open {#task-6}

**Files:**
- Modify: `Core/CasStore.h` (`PoolConfig` `:99-239`: new fields)
- Modify: `Core/CasStore.cpp` (`Store::open` `:290-456`)
- Modify: `Core/CasRequestControl.h` (`validateCasRequestBudget` doc `:109-118` — comment only)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
  (`:420-440` PoolConfig assembly)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` (~`:247-305`
  config-key reads)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `MountClaimResult::prior` (Task 4), `boot_ms_fn`.
- Produces:
  ```cpp
  /// PoolConfig additions:
  uint64_t materialization_grace_ms = 30000;   /// T_mat; lowering increases the risk that a
                                               /// late-materializing predecessor write is dropped
  std::function<void(uint64_t)> wait_sleep_fn = {};   /// test hook for open/remount waits
  /// Store member:
  std::atomic<bool> unclean_epoch_boundary_seen{false};   /// sticky; Task 8 reads it
  ```
  XML key: `<materialization_grace_ms>` under the disk's config prefix, read in
  `MetadataStorageFactory.cpp` alongside the existing keys and assigned in
  `ContentAddressedMetadataStorage.cpp:420-440`.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasMountTmat, UncleanOpenWaitsMaterializationGrace)
{
    /// Predecessor: claim epoch 7, no farewell (simulate crash: just drop the keeper).
    /// Successor Store::open with config.materialization_grace_ms = 30000,
    /// config.wait_sleep_fn collecting sleep amounts, boot_ms_fn fake clock.
    /// EXPECT the collected waits to include the observation window AND a 30000 T_mat wait.
}

TEST(CasMountTmat, CleanOpenSkipsAllWaits)
{
    /// Predecessor released cleanly (drain + farewell from Task 5).
    /// Successor open: EXPECT zero recorded waits (no observation, no T_mat).
}

TEST(CasMountTmat, FencedPriorPaysOnlyTmat)
{
    /// Predecessor lease carries gc_fenced=true. Successor open:
    /// EXPECT exactly one wait == materialization_grace_ms, no observation window.
}
```

- [ ] **Step 2: Run to verify failure** — waits absent / fields unknown. Log
`build_debug/test_t6_fail.log`.

- [ ] **Step 3: Implement**

In `Store::open`, after the claim loop succeeds (`cpp:381-440`) and before `armMountFence`
(`:444`):

```cpp
if (claim.prior == MountPriorState::Fenced || claim.prior == MountPriorState::UncleanObserved)
{
    store->unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
    const uint64_t t_mat = store->config.materialization_grace_ms;
    LOG_INFO(log, "Content-addressed mount {} follows an unclean predecessor; waiting {} ms "
                  "(materialization grace) for the store to finalize or drop its accepted "
                  "requests before trusting recovery listings", srid, t_mat);
    store->waitSleep(t_mat);   /// wait_sleep_fn if set, else interruptible sleep helper
}
```

Extend the `validateCasRequestBudget` doc comment: the handover-wait invariant
`observation (>= ttl) + T_mat >= all client timeouts + T_mat` holds by construction because
`attempt_timeout_ms + lease_safety_margin_ms < mount_lease_ttl_ms` is already enforced. Plumb the
XML key: in `MetadataStorageFactory.cpp` read
`config.getUInt64(config_prefix + ".materialization_grace_ms", 30000)`, pass it through to the
`PoolConfig` assembly in `ContentAddressedMetadataStorage.cpp` (follow how `server_root_id` flows
at `:285-287` → `:420-440`).

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasMountTmat.*:CasStore*'`, log
`build_debug/test_t6.log`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRequestControl.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: materialization_grace_ms (T_mat) wait on unclean mount open"
```

---

### Task 7: Conditional T_mat on self-remount; delete the wedge→LatePredecessor conversion {#task-7}

**Files:**
- Modify: `Core/CasStore.cpp` (`tryRemountOnce` `:597-719`, `quiesceRefTablesForRemount`
  `:1241-1285`)
- Modify: `Core/CasStore.h` (`quiesceRefTablesForRemount` doc `:746-755`)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `drainRefLanesForShutdown`-style waiting (Task 5), `wait_sleep_fn`,
  `unclean_epoch_boundary_seen`, `materialization_grace_ms`.
- Produces:
  ```cpp
  /// Waits (bounded by attempt budget) for lane leaders to conclude their current attempt,
  /// then reports whether any ref-log conditional PUT remains unresolved (wedge present).
  bool Store::refLanesSettledForRemount();
  ```
  `quiesceRefTablesForRemount` no longer "converts an in-flight wedged PUT into the accepted Late
  Predecessor case" — the doc comment at `CasStore.h:746-755` is rewritten: the seal (Task 8)
  makes the dropped-epoch region uniformly invisible; remount pays T_mat only when unsettled.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasRemountTmat, DrainedRemountSkipsGrace)
{
    /// Trip the fence (fake boot clock past deadline as in WriteFenceUsesInjectedBootClock,
    /// gtest_cas_store.cpp:1151), with NO in-flight ref PUT. Drive tryRemountOnce.
    /// EXPECT: remount succeeds and wait_sleep_fn recorded NO materialization_grace_ms wait;
    /// unclean_epoch_boundary_seen stays false (drained boundary is clean for sealing).
}

TEST(CasRemountTmat, UnresolvedWedgePaysGraceAndMarksBoundaryUnclean)
{
    /// Wedge the lane first (Unresolved PUT via the throwing backend), trip fence, remount.
    /// EXPECT: exactly one recorded wait == materialization_grace_ms, and
    /// unclean_epoch_boundary_seen == true.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t7_fail.log`.

- [ ] **Step 3: Implement**

In `tryRemountOnce`, after `claimMountAwaitingExpiry` returns `Claimed` (`:635`) and before the new
keeper start (`:658`):

```cpp
const bool settled = refLanesSettledForRemount();
if (!settled)
{
    unclean_epoch_boundary_seen.store(true, std::memory_order_relaxed);
    LOG_INFO(log, "Self-remount of content-addressed mount {} with an unresolved ref-log PUT; "
                  "waiting {} ms (materialization grace) before re-listing", srid,
                  config.materialization_grace_ms);
    waitSleep(config.materialization_grace_ms);
}
```

`refLanesSettledForRemount`: same wait mechanics as `drainRefLanesForShutdown` but without the
admission latch (mutations already fail via the tripped fence), budget =
`attempt_timeout_ms + lease_safety_margin_ms`; returns `!any_wedge`. In
`quiesceRefTablesForRemount`, keep the publish-settle + `superseded_by_remount` +
`ref_tables.clear()` mechanics; delete the Late-Predecessor conversion language and behavior notes.

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasRemountTmat.*:CasStore*'`, log
`build_debug/test_t7.log`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: conditional T_mat on self-remount; retire the wedge-to-LatePredecessor case"
```

---

### Task 8: The recovery seal {#task-8}

**Files:**
- Modify: `Core/CasStore.cpp` (`ensureRefTableRecovered` `:1016-1165`)
- Modify: `Core/CasStore.h` (doc near `kRefRecoveryMaxRestarts` `:691`)
- Modify: `src/Common/ProfileEvents.cpp` (add `CasRefRecoverySealPublished` next to `:768`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`

**Interfaces:**
- Consumes: `RefTableSnapshot::sealed_from` + codec v2 (Task 3);
  `unclean_epoch_boundary_seen` (Tasks 6/7); `liveWriterEpoch()`;
  `ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok)`;
  test helpers `writeRefLogTxnRaw` / `writeRefSnapshotRaw` / `minimalLiveSnapshot`
  (`src/Disks/tests/cas_test_helpers.h:744-770`).
- Produces: seal semantics inside recovery —
  `seal_id = RefTxnId{liveWriterEpoch() - 1, UINT64_MAX}`, body = replay of everything listed,
  `snapshot_id = seal_id`, `sealed_from = greatest listed txn id`; published before the table is
  marked `recovered`; failure throws (recovery fails closed, bounded retries by the existing
  restart mechanism). Skipped when: boundary not unclean, `liveWriterEpoch() < 2`, dead-epoch
  region empty, or `newest_snapshot_id >= seal_id` (already sealed).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(RefWriterRecoverySeal, UncleanBoundarySealsAllDeadEpochsBeforeExposingState)
{
    /// Inject predecessor debris out-of-band: logs from TWO dead epochs, no snapshot:
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=1, seq=1, birth "a"));
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=2, seq=1, mut  "b"));
    /// Open the store so liveWriterEpoch() == 3 and unclean_epoch_boundary_seen == true
    /// (crash-style predecessor from Task 6 test setup). Touch the namespace (any read).
    /// EXPECT: backend now holds _snap at RefTxnId{2, UINT64_MAX}; decoded body has
    /// sealed_from == RefTxnId{2, 1}, contains "a" and "b";
    /// ProfileEvents CasRefRecoverySealPublished incremented by 1.
}

TEST(RefWriterRecoverySeal, LateLogBelowSealIsInvisibleToRecoveryAndFold)
{
    /// Continue from the sealed state; now inject a LATE dead-epoch log out-of-band:
    ///   writeRefLogTxnRaw(backend, layout, txn(epoch=2, seq=2, mut "c"));
    /// A fresh recoverRefTable (free function, CasRefIntake.h:132) over the namespace:
    /// EXPECT state equals the sealed body — "c" is not applied (id <= seal_id is covered).
}

TEST(RefWriterRecoverySeal, CleanBoundaryDoesNotSeal)
{
    /// Same debris, but unclean_epoch_boundary_seen == false (clean predecessor):
    /// EXPECT: no _snap object at {epoch-1, UINT64_MAX}; recovery state still correct.
}

TEST(RefWriterRecoverySeal, SealPutFailureFailsRecoveryClosed)
{
    /// failNextCasPut(seal key) so the seal PUT definitively fails:
    /// EXPECT the namespace touch throws; a second touch (PUT now succeeding) recovers and seals.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t8_fail.log`.

- [ ] **Step 3: Implement**

In `ensureRefTableRecovered`, after the replay of listed logs and before the seeding block
(`:1127`):

```cpp
const uint64_t my_epoch = liveWriterEpoch();
const RefTxnId seal_id{my_epoch - 1, UINT64_MAX};
const bool dead_region_nonempty = greatest_listed_id.has_value()
    && greatest_listed_id->writer_epoch < my_epoch;
const bool already_sealed = rt.newest_snapshot_id && !(*rt.newest_snapshot_id < seal_id);
if (unclean_epoch_boundary_seen.load(std::memory_order_relaxed)
    && my_epoch >= 2 && dead_region_nonempty && !already_sealed
    && recovered_state.lifecycle == RefLifecycle::Live)
{
    RefTableSnapshot seal = snapshotOf(recovered_state, ns.string());
    seal.snapshot_id = seal_id;                       /// upper bound of the covered region
    seal.sealed_from = recovered_state.greatest_applied;
    const String bytes = encodeRefTableSnapshot(seal);
    const auto fence_ok = [this] { return refAppendFenceOk(); };
    const auto outcome = ref_request_controller->putIfAbsentControlled(
        pool_layout.refSnapshotKey(ns, seal_id), bytes, fence_ok);
    if (outcome != CasWriteOutcome::Committed)
        throw Exception(ErrorCodes::ABORTED,
            "CAS recovery seal PUT for namespace {} did not commit; failing recovery closed "
            "(the mount stays non-writable for this table; retry will re-seal)", ns.string());
    ProfileEvents::increment(ProfileEvents::CasRefRecoverySealPublished);
    rt.newest_snapshot_id = seal_id;
    rt.base_snapshot_bytes.store(bytes.size(), std::memory_order_relaxed);
}
```

Capture `greatest_listed_id` in the existing LIST loop (`:1052-1083`). Note `state.greatest_applied`
after replay is the greatest listed id, so `sealed_from` needs no extra bookkeeping. A `Removed`
lifecycle table keeps its existing removed-snapshot path (no seal). Since a same-key retry after an
`Unresolved` outcome must re-PUT byte-identical content, the seal bytes must be deterministic:
they are — replay of the same listed set (the controller's resolve-before-reissue handles the
ambiguous case with the exact same bytes; a *changed* listed set after a failed envelope goes
through a fresh recovery restart, which re-LISTs and rebuilds both the state and the seal bytes —
still the same key only if the epoch did not change; on epoch change the key moves, which is the
safe cross-attempt move described in the spec).

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='RefWriterRecoverySeal.*:RefWriterRecovery.*:RefWriterSnapshotPublish.*' > build_debug/test_t8.log 2>&1`.
The `MountTimeTriggerPublishesAfterRecoveryReplaysLargeTail` test (`:970`) may need its expectation
adjusted if its scenario now seals first — subagent reports what changed and why.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: recovery seal — epoch-closing snapshot published before a table goes writable"
```

---

### Task 9: Observation-based GC fence-out {#task-9}

**Files:**
- Modify: `Core/CasServerRoot.h` (`computeHeartbeatFloor` decl `:251-252`, `HeartbeatFloor`
  `:241-249`)
- Modify: `Core/CasServerRoot.cpp` (`computeHeartbeatFloor` `:477-556`)
- Modify: `Core/CasGc.h` (cross-round members near `:376-381`)
- Modify: `Core/CasGc.cpp` (call sites `:240-242`, `:2120-2122`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (suite `CasHeartbeatFloor`)

**Interfaces:**
- Consumes: existing fence mechanics (token-guarded `putOverwrite`, `max_reclassify = 4`); the
  cross-round observation precedent `rememberObservation` (`CasGc.h:354`); `Cas::Gc` longevity via
  `CasGcScheduler.h:87`.
- Produces:
  ```cpp
  struct MountTokenObservation { Token token; uint64_t first_seen_mono_ms = 0; };
  using MountObservationMap = std::map<String /*srid*/, MountTokenObservation>;
  HeartbeatFloor computeHeartbeatFloor(Backend &, const Layout &, uint64_t now_ms /*audit only*/,
      uint64_t mono_now_ms, uint64_t stable_threshold_ms, MountObservationMap & obs);
  ```
  `Cas::Gc` gains `MountObservationMap mount_obs;` (in-memory; a new leader starts empty →
  fencing delayed one round, safe). Fence condition: not terminated, not fenced, token unchanged
  in `obs` for `>= stable_threshold_ms` of the leader's monotonic clock. `skew_margin_ms` is gone
  from the fence decision; `listMounts`' display heuristic stays untouched.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasHeartbeatFloor, FirstSightNeverFencesEvenIfStampLooksExpired)
{
    /// Lease stamped expires_at_ms = 10, GC wall now = 999999. Empty obs map.
    /// EXPECT fenced_now == 0 and obs now contains the srid (observation started).
}

TEST(CasHeartbeatFloor, StableTokenPastThresholdIsFenced)
{
    /// Same lease; call twice with mono 0 then mono = threshold. EXPECT fenced_now == 1 on
    /// the second call, gc_fenced set in the body, seq bumped.
}

TEST(CasHeartbeatFloor, RenewalBetweenRoundsRestartsObservation)
{
    /// Observe at mono 0; renew the keeper (token bump); call at mono = threshold:
    /// EXPECT fenced_now == 0 and the obs entry rebased to the new token.
}
```

Adapt the existing `ClassifiesAndFencesOut` (`gtest_cas_mount.cpp:494`) to the two-call pattern.

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t9_fail.log`.

- [ ] **Step 3: Implement** — in `computeHeartbeatFloor`, replace the
`now_ms > expires_at_ms + skew_margin_ms` test (`:525`) with the observation lookup/update; keep
everything else (classification counters, token-guarded fence write, reclassify loop, farewell/
fenced short-circuits). Both `CasGc.cpp` call sites pass
`mono_now_ms = clock_fn_mono()` (add a monotonic clock member next to the existing wall
`clock_fn`, default `bootMs`-equivalent, test-injectable) and
`stable_threshold_ms = ttl + ttl / 20 + round_period_hint` where `round_period_hint =
mount_renew_period.count()` (one beat of slack); delete both `skew_margin_ms` derivations
(`:240-241`, `:2120-2121`).

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='CasHeartbeatFloor.*:CasGc*' > build_debug/test_t9.log 2>&1`; subagent lists
adjusted pre-existing tests.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_mount.cpp
git commit -m "cas: GC fence-out by token-stability observation on the leader's own clock"
```

---

### Task 10: Publish-from-live; delete the grace machinery {#task-10}

**Files:**
- Modify: `Core/CasStore.h` (delete `snapshot_min_log_age_ms` `:217` + doc `:199-216`; replace
  `TailLogEntry`/`tail_since_snapshot`/`snapshot_base_state` `:630-647` with counters)
- Modify: `Core/CasStore.cpp` (`maybeScheduleSnapshotPublish` `:1813-1926`,
  `trySnapshotPublishOnce` `:1977-2091`, `flushRefBatch` push `:1744-1745`,
  `ensureRefTableRecovered` seeding `:1127-1155`, `publishRemovedSnapshotNow` `:2176-2206`,
  cache-budget accounting reading `base_snapshot_bytes`/`tail_bytes_since_snapshot`)
- Modify: `src/Common/ProfileEvents.cpp` (delete `CasRefLatePredecessorObserved` `:770`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`

**Interfaces:**
- Consumes: T11 monotonic adoption (kept); backoff machinery (kept); Task 8 seal (sets
  `newest_snapshot_id` before any publish).
- Produces (RefTableRuntime replacement fields):
  ```cpp
  std::atomic<uint64_t> tail_count_since_snapshot{0};   /// applied txns above newest_snapshot_id
  std::atomic<uint64_t> tail_bytes_since_snapshot{0};   /// kept, same meaning
  /// snapshot_base_state and tail_since_snapshot are DELETED.
  ```
  Publish algorithm: copy `rt->state` once under `state_mutex` (candidate_x =
  `state.greatest_applied`), encode + conditional PUT off the lock; adoption under T11 guard
  subtracts the captured count/bytes.

- [ ] **Step 1: Write the failing tests first (behavioral targets of the new scheme)**

```cpp
TEST(RefWriterPublishFromLive, YoungTxnIsCoveredImmediately)
{
    /// Append ONE committed txn; force a publish (drive trySnapshotPublishOnce directly).
    /// OLD behavior: nothing published (grace window). NEW: snapshot at that txn id exists
    /// and its decoded body contains the row. No time manipulation at all.
}

TEST(RefWriterPublishFromLive, TriggerFiresOnCountAboveThresholdWithoutAging)
{
    /// Append snapshot_log_count_threshold + 1 txns with a boot clock that NEVER advances:
    /// EXPECT CasRefSnapshotPublishDispatched incremented (old code required aging).
}

TEST(RefWriterPublishFromLive, AdoptionSubtractsCapturedCountersUnderConcurrentAppends)
{
    /// Seed counters, run a publish; while its PUT is "in flight" (use the counting backend's
    /// hook), append more txns; after adoption EXPECT tail_count_since_snapshot equals the
    /// number appended after the copy (not zero, not negative).
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t10_fail.log`.

- [ ] **Step 3: Implement the new publish path**

`trySnapshotPublishOnce` core replacement (`:1982-2019` collapses to):

```cpp
RefTableState candidate_state;
RefTxnId candidate_x;
uint64_t captured_count, captured_bytes;
{
    std::lock_guard lock(rt->state_mutex);
    if (rt->state.lifecycle != RefLifecycle::Live)
        return false;
    if (rt->newest_snapshot_id && !(*rt->newest_snapshot_id < rt->state.greatest_applied))
        return false;                       /// nothing above the newest snapshot
    candidate_state = rt->state;            /// ONE copy, at a txn boundary
    candidate_x = rt->state.greatest_applied;
    captured_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
    captured_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
}
```

Encode/PUT/backoff/T11 stay as today (`:2021-2067`); the adoption block (`:2069-2088`) replaces the
prune loop with counter subtraction (`fetch_sub` of captured values, clamped via a CAS loop or
`fetch_sub` + assert-no-underflow in debug) and drops `snapshot_base_state` entirely.
`maybeScheduleSnapshotPublish`: the fused walk (`:1857-1876`) becomes

```cpp
const uint64_t publishable_count = rt->tail_count_since_snapshot.load(std::memory_order_relaxed);
const uint64_t publishable_bytes = rt->tail_bytes_since_snapshot.load(std::memory_order_relaxed);
const bool over_threshold = publishable_count > config.snapshot_log_count_threshold
    || publishable_bytes > config.snapshot_log_bytes_threshold;
if (over_threshold) { rt->pending_snapshot_publishes.fetch_add(1, ...); dispatch = true; }
```

`flushRefBatch` commit path (`:1744-1745`): increment the two counters instead of `push_back`.
`ensureRefTableRecovered` seeding (`:1127-1155`): seed counters from the listed tail
(count of logs above the newest snapshot, sum of their body sizes) — the mount-time trigger
semantics survive; delete `snapshot_base_state` seeding. `publishRemovedSnapshotNow`
(`:2201-2205`): zero both counters instead of clearing the vector/base. Cache-budget accounting:
`base_snapshot_bytes + tail_bytes_since_snapshot` formula unchanged. Delete
`snapshot_min_log_age_ms`, `TailLogEntry`, `tail_since_snapshot`, `snapshot_base_state`, the
`CasRefLatePredecessorObserved` ProfileEvent, and these tests wholesale:
`GraceAgeRespectedYoungLogNotCovered` (`:1008`), `LatePredecessorCounterCountsGraceWindowHoldback`
(`:1040`), `TriggerIgnoresYoungTailAboveCountThreshold` (`:1364`),
`TriggerNotLatchedBySustainedLoadYoungFloor` (`:1398`). Adapt
`TriggerIgnoresEntriesCoveredByNewestSnapshot` (`:1443`) to counters (covered entries simply are
not counted — seed via recovery). Keep and re-verify: threshold/counter tests, C4 backoff suite
(`:1202-1330`), `ConcurrentOutOfOrderPublishDoesNotRegressBaseNorDropCommittedTxns` (`:1138` —
now asserts counters instead of base state).

- [ ] **Step 4: Run the full ref suite** —
`--gtest_filter='RefWriter*:RefSnapshotCodec.*' > build_debug/test_t10.log 2>&1`; subagent
summarizes every deleted/adapted test with a one-line reason each.

- [ ] **Step 5: Grep-gate the deletion**

Run: `grep -rn "snapshot_min_log_age_ms\|tail_since_snapshot\|snapshot_base_state\|CasRefLatePredecessorObserved\|TailLogEntry" src/ | grep -v test` — expected: empty output.

- [ ] **Step 6: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/ src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_ref_writer.cpp
git commit -m "cas: publish-from-live — grace window, base+tail replay and LatePredecessor counter removed"
```

---

### Task 11: Wedge hard contract + anomaly policy helper {#task-11}

**Files:**
- Modify: `Core/CasStore.h` (helper decl), `Core/CasStore.cpp` (`flushRefBatch` id-allocation site
  `:1706` area, wedge-resolution `CORRUPTED_DATA` branch `:1518-1526`)
- Modify: `Core/CasEvent.h` (add `CasEventType::ForeignInterference`)
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`, `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: `tripMountLost()` + `scheduleRemount()` (existing `on_lost` mechanics,
  `CasStore.cpp:418-423`); `EventEmitter` idiom (`CasEvent.h:72-93`); `emitEvent`
  (`CasStore.h:548-552`).
- Produces:
  ```cpp
  /// Incidental-detection reaction (spec §anomaly-policy): LOGICAL_ERROR + fail-closed remount
  /// + background diagnostics strictly off the critical path.
  void Store::reportImpossibleInterference(
      const String & key, const String & reason,
      const std::optional<String> & offending_ns = {});
  ```
  Behavior: LOG_ERROR with full context; `EventEmitter{*this}.emit` a `ForeignInterference` event;
  `tripMountLost(); scheduleRemount();` then schedule ONE background `ThreadFromGlobalPool` task
  that GETs `key` (single attempt), decodes what it can (writer epoch/uuid if the body carries
  one), and LOG_ERRORs a rich diagnostic line — never on the caller's thread; finally the caller
  throws `LOGICAL_ERROR`. The wedge contract: `chassert(!rt->wedge)` at the new-id allocation
  point plus a release-mode guard that fails the batch with `LOGICAL_ERROR` through this helper
  instead of allocating.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasAnomalyPolicy, ForeignBytesAtWedgeKeyTripFenceAndRemount)
{
    /// Wedge the lane (Unresolved PUT), then overwrite the wedge key out-of-band with
    /// DIFFERENT bytes via backend->putOverwrite. Next flush resolves the wedge:
    /// EXPECT the mutation fails with LOGICAL_ERROR (was CORRUPTED_DATA), mayMutate() == false
    /// (fence tripped), and a ForeignInterference CasEvent captured by a test event sink.
}

TEST(CasAnomalyPolicy, WedgeContractReleaseFailClosed)
{
    /// Force rt->wedge while injecting a batch that would allocate a new id (simulate the
    /// impossible state by setting the wedge directly under state_mutex in the test):
    /// EXPECT the batch fails with LOGICAL_ERROR and NO new _log object appears.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t11_fail.log` (debug build so
`chassert` is active; the release-guard test asserts the outcome, not the assert).

- [ ] **Step 3: Implement** — helper as specified; rewire the `CORRUPTED_DATA` different-bytes
branch (`:1518-1526`) through it (keep the wedge in place — fail-closed); add the id-allocation
guard before `allocateRefTxnId()` in `flushRefBatch`; add the enum value + its `String` name in
the event-type table in `CasEvent.h`/`.cpp`.

- [ ] **Step 4: Run to verify pass** —
`--gtest_filter='CasAnomalyPolicy.*:RefWriter*' > build_debug/test_t11.log 2>&1`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp src/Disks/tests/gtest_cas_ref_writer.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "cas: anomaly policy — LOGICAL_ERROR + fail-closed remount on impossible interference; wedge hard contract"
```

---

### Task 12: Sweep detector for T_mat violations (late log below the seal) {#task-12}

**Files:**
- Modify: `Core/CasRefIntake.h`/`.cpp` (`recoverRefTable` `:132`/`:139-210` — surface the newest
  snapshot's `snapshot_id` + `sealed_from`)
- Modify: `Core/CasOrphanManifestSweep.cpp` (log loop `:129-140`)
- Modify: `Core/CasEvent.h` (add `CasEventType::RefLateLogDetected`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (or the sweep's existing gtest home — locate
  `sweepNamespace` tests and add alongside)

**Interfaces:**
- Consumes: `sealed_from` (Task 3), seal objects (Task 8), `EventEmitter`.
- Produces:
  ```cpp
  struct RecoveredRefTable { RefTableState state;
      std::optional<RefTxnId> newest_snapshot_id;
      std::optional<RefTxnId> sealed_from; };
  RecoveredRefTable recoverRefTableDetailed(Backend &, const Layout &, const RootNamespace &,
      std::function<void()> on_page_fetched = {},
      unsigned max_restarts = 3);   /// recoverRefTable stays as a thin wrapper returning .state
  ```
  CARRY-FORWARD (added 2026-07-14 after the §0 introspection plan landed; final-review finding I1):
  `recoverRefTable` NOW has an `on_page_fetched` parameter (4th, before `max_restarts`) and the GC
  callers at `CasGc.cpp` / `CasOrphanManifestSweep.cpp` pass a callback that is the sole emit path
  for `CasGcEnumerationPages` on the recovery-LIST scans. The wrapper refactor MUST preserve that
  parameter and keep the two GC call sites passing it (fsck callers stay without it). Likewise the
  `CasOrphanManifestSweep.cpp` log loop this task modifies already invokes `onGcEnumerationPage`
  per fetched page — preserve that wiring while adding the late-log detector. Line anchors in this
  task drifted vs the §0 commits (5edd9b39cec..de42a89ab87); re-locate by name.
  Detector rule: a listed `_log` id `L` with `sealed_from < L <= newest_snapshot_id` (when
  `sealed_from` is present) provably materialized after the recovery LIST → `LOG_WARNING` + one
  `RefLateLogDetected` event. Never GET the log body to "revive" it (resurrect invariant); GC's
  ordinary covered-log cleanup deletes it later.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasSweepLateLog, LogBetweenSealedFromAndSealIdIsReportedNotRevived)
{
    /// Build a namespace with a seal (via writeRefSnapshotRaw: snapshot_id={2,UINT64_MAX},
    /// sealed_from={2,3}), then inject a late log at {2,7} via writeRefLogTxnRaw.
    /// Run the sweep page over the namespace with a test event sink:
    /// EXPECT one RefLateLogDetected event naming {2,7}; the log object still present
    /// (sweep does not delete it); no state change derived from it.
}
```

- [ ] **Step 2: Run to verify failure** — log `build_debug/test_t12_fail.log`.

- [ ] **Step 3: Implement** — extend the free recovery to return the detail struct; in the sweep's
LIST-classification (it already parses `_log` ids), add the range check + emission before the
above-cursor protection loop; wire the event type.

- [ ] **Step 4: Run to verify pass** — `--gtest_filter='CasSweepLateLog.*:*Sweep*' >
build_debug/test_t12.log 2>&1`.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRefIntake.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasOrphanManifestSweep.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/tests/
git commit -m "cas: sweep reports a late log below the seal as a T_mat violation (report, never revive)"
```

---

### Task 13: GC-defense compliance audit {#task-13}

**Files:**
- Create: `docs/superpowers/reports/2026-07-XX-cas-gc-defense-audit.md` (stamp the actual date)
- Possibly modify: only sites the audit finds non-compliant.

**Interfaces:** none produced; consumes the spec's §gc-defense-audit checklist.

- [ ] **Step 1:** For each site, read the code and write a compliance note (site, what it defends
against, request cost on the hot path, verdict): `gc/state` round-ownership re-read
(`Core/CasGc.cpp:1433` area), zombie-steal committed-pair threading (`Core/CasGc.h:243`),
deposed-leader debris handling (`Core/CasGc.cpp:1682-1695`), orphan-sweep epoch gate
(`Core/CasOrphanManifestSweep.cpp:146-163`), TokenMismatch/404 delete tolerance (`:200-206`,
`:283-302`), fold-lag clamps (`Core/CasGc.cpp:986-1052`, `:1222-1236`). The principle to check:
zero S3 requests spent *fighting* a foreign writer on hot paths; incidental checks and
CAS-linearized commits are compliant by definition.
- [ ] **Step 2:** If a site spends non-incidental requests on foreign-writer defense, fix it in a
separate commit with a test; otherwise record "compliant, keep" with a one-line justification.
- [ ] **Step 3:** Commit the report (`git add docs/superpowers/reports/... && git commit -m "cas:
GC-defense compliance audit vs rev.6 anomaly policy"`).

---

### Task 14: End-to-end checks and soak scenario {#task-14}

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s31_late_put_injection.py` (follow the structure of an
  existing card, e.g. `utils/ca-soak/scenarios/cards/s15_s18_shards_lifecycle.py`)
- Modify: `utils/ca-soak/scenarios/RUN_HISTORY.md` (append the run entry when executed)

**Interfaces:** consumes everything above via the built server binary.

- [ ] **Step 1:** Scenario card: boot a 2-node cluster (framework `cluster_boot.py`), kill -9 the
writer mid-append-storm, restart it, assert from logs: the observation + T_mat wait lines appear,
the seal publish happens (grep `CasRefRecoverySealPublished` in `system.events` /
`content_addressed_log`), no `sparing` warnings of the `delete_pending retired entry recovered
in-degree` class, and a clean stop/start pays no wait (grep absence).
- [ ] **Step 2:** Late-PUT injection: while the successor is inside its T_mat wait (long
`materialization_grace_ms` for the test), inject a dead-epoch `_log` object directly to the object
store (the scenario has S3 credentials); after mount, assert the sweep emits `RefLateLogDetected`
and queries return only sealed truth.
- [ ] **Step 3:** Run the card against a fresh debug build; triage with the existing scenario
framework conventions (host logs survive teardown; `down -v` for clean data). Record in
`RUN_HISTORY.md`.
- [ ] **Step 4:** Re-run the S13/S15/S18 cards once as a regression sweep (they exercise the
fence/remount and shard lifecycle paths this plan touched).
- [ ] **Step 5:** Commit the card + history entry
(`git commit -m "cas: soak card s31 — unclean handover, seal, late-PUT injection"`).

---

## Self-review results {#self-review}

- **Spec coverage:** decision log 1→Tasks 4/9; 2→4/9; 3→6; 4→7; 5/6→8; 7→8 (clean skip);
  8→11; 9 already landed; 10→13. §lease-acquisition→4/5/6; §gc-fence-out→9; §t-mat→6/7;
  §recovery-seal→3/8; §ref-simplification→10; §anomaly-policy→11/12; §gc-defense-audit→13;
  §tla→1/2; §configuration→6/10; §testing→per-task + 14.
- **Known deliberate mappings:** "seal before writable" realized per-namespace inside lazy
  recovery (architecture note at top); "T_mat violation invariant" split into strict
  (expected-red demo) + fresh-reader (green) configs in Task 1 — matching the spec's
  "deterministic-invisible for observers from birth" wording honestly.
- **Type consistency:** `MountPriorState` produced in Task 4, consumed 6/7;
  `sealed_from` produced 3, consumed 8/12; counters produced 10 only after seal (8) no longer
  needs the tail vector — verified Task 8 uses only `newest_snapshot_id`/`base_snapshot_bytes`,
  which survive Task 10.
- **Placeholder scan:** two TLA action bodies in Task 2 are elided with `<take over: ...>` — they
  copy the existing model's claim/write mechanics verbatim from adjacent actions; acceptable
  because the implementer edits that file with the original in front of them. No other
  placeholders.

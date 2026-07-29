# CAS disk lifecycle rev.8: throw-when-uncertain, truth-when-asserted (FORGET-only)

Status: **DESIGN rev.8 — final, FORGET-only.** Rewrites rev.7 in place (git history holds rev.1–rev.7).

**rev.7 → rev.8 (owner decision, 2026-07-23, after Tasks 1–12 of the implementation were landed and
reviewed):** the natural `Vanished(erased)` proof is **EXCISED, not shipped dormant** — "чем меньше кода,
тем лучше". Two days of implementation and four adversarial reviews made the proof stack sound, but also
revealed its true cost: the probe-outcome taxonomy depth, the outstanding-durable-request counter, the
grace arithmetic, the strong-LIST backend capability — all serving a path whose production switch was OFF
indefinitely anyway (capability unwired, Local/Emulated never eligible, the test teardown using `FORGET`).
**Erasure is never proven by the system; it is only asserted by the operator.** The excised implementation
(reviewed, tested, and partially TLA-modeled) remains in git history as the v2 door if automatic erasure
detection is ever genuinely needed.

**The core, now one sentence:** a CA disk that is not certainly live **throws to everyone**; the only paths
to truthful benign answers are the operator's `FORGET` (`Vanished(forgotten)`) and authoritative proof of a
*foreign* pool at our prefix (`Vanished(replaced)` — one comparison, no machinery).

**Part 6 vs the terminal states — the honesty distinction (unchanged):**

| | Part 6 (rolled back) | `Vanished` (rev.8) |
|---|---|---|
| Gate condition | "not Mounted" — ambiguous, data possibly intact | operator-asserted (`FORGET`) or foreign-`pool_id`-proven (replaced) |
| exists/list answers | absent/empty — **a lie** when data is intact | absent/empty — **the truth** (asserted/proven) |
| Content reads | absent-ish | **typed throw** naming the actual reason |
| Writes/creates | benign | **typed throw** |
| Transient outage / `IdentityLost` | lied "absent" | **throws** (no benign answer while uncertain) |

Supersedes: rev.1–rev.7 (git history), the Path-B spec, the automount/eject spec, the 2026-07-21
Dormant/UNMOUNT spec (Task 4–8 rollback per §9; Part 1 KEPT). Companion:
`2026-07-22-cas-disk-lifecycle-problem-and-constraints.md` (its C2 gains a rev.8 disposition note).

---

## 1. States and the operation-class gate

```
[Running/live] ──lease lost──► [transient-not-live] ──┬─ _pool_meta present, foreign pool_id ──► [Vanished(replaced)]  (truth)
      ▲                             │ (all access throws;  └─ BOTH sentinels authoritatively absent ─► [IdentityLost]
      └── self-remount succeeds ────┘  remount retries)        (fail-loud terminal; threads exit;
                                                                cure = FORGET or restart)
                       FORGET (from any state) ──────────────► [Vanished(forgotten)] (truth)
```

- **`IdentityLost` is now a fail-loud TERMINAL state** (rev.8 change): entered on an authoritative
  `KeyAbsent` of BOTH `_pool_meta` and the owner-claim key (errors/`ContainerAbsent`/`AccessDenied` never
  conclude — stay transient and retry). The remount thread **exits** on entering it (rev.7's demoted
  low-rate observer existed only to run the erased proof — gone). No auto-revival ([D3] trivially holds:
  nothing runs); a matching-sentinel restore does not revive; restart re-runs the full mount chain,
  `FORGET` converts to truthful `Vanished(forgotten)`.
- **`Vanished(replaced)`** stays: proven by one identity comparison during a transient recovery attempt
  (`_pool_meta` present, immutable identity fields differ). Zero extra machinery.
- The minimal storage-level lifecycle `Constructing → Started → ShutDown` stays (null-pool fail-loud).

**Operation classes — SIX (unchanged from the as-built T8 gate):** Factory (always works) / Probe /
ContentRead / Write / Remove / Admin. Live → all pass; transient/`IdentityLost` → all but Factory throw,
in DIFFERENT classes (transient: `throwCasTransientUnavailable`, i.e. `NETWORK_ERROR`, "mount lease not
held — backing may be temporarily unreachable", so no consumer reads unavailability as damage;
`IdentityLost`: 668 and its own message — no auto-recovery, cure = restart or `FORGET`); `Vanished` →
Probe answers truthful absent/empty, Remove is no-op success (a vanished-disk table's `DROP` completes),
ContentRead/Write/Admin throw the typed [D5] message.

**Error messages tell the truth [D5]:**
- `Vanished(replaced)` — "data root replaced by a foreign pool (pool_id mismatch)".
- `Vanished(forgotten)` — "disk decommissioned by SYSTEM CONTENT ADDRESSED FORGET at <timestamp> — erasure
  was NOT verified; if this was a mistake the data may be intact (restart re-registers the name)".
- `IdentityLost` — "pool identity lost (sentinels absent) — recover by restart or SYSTEM CONTENT ADDRESSED
  FORGET; a matching-sentinel restore does not auto-revive".
The same strings appear in the lifecycle snapshot (`lifecycle_detail`) and the transition WARN.

**The empty-proof rule (T9, KEPT):** pre-terminal and read-only pools — an enumeration about to answer
empty at a `TableDir`/`DetachedContainer` root first confirms `_pool_meta` with one authoritative uncached
`probeSentinel`; absent ⇒ typed throw. This (plus write-class gating) remains the silent-empty-ATTACH
killer, and needs only the single-sentinel probe — none of the excised proof machinery.

## 2. The identity gate (FORGET-only form)

Step 0 of `tryRemountOnce` (the existing fresh-incarnation recovery stays verbatim; `claimOwnerOrThrow`'s
empty-root bootstrap unreachable from recovery):

1. `probeSentinel(_pool_meta)` — authoritative, typed (`Present`/`KeyAbsent`/`ContainerAbsent`/
   `AccessDenied`/`Indeterminate`; the T3 probe with its S3 raw-error classification stays — it serves this
   gate, the empty-proof rule, and bootstrap).
2. **Present + immutable identity matches** (`pool_id` + `blob_header_len` [B6]; format gate = successful
   compatible decode; `algos_used`/`min_reader_generation` legally mutate — refreshed, never compared) →
   proceed with recovery. Applies ONLY in transient.
3. **Present + foreign `pool_id`** → `enterVanished(Replaced)`.
4. **`KeyAbsent` for `_pool_meta` AND the owner-claim key** → `enterIdentityLost()`; the remount thread
   exits after the transition.
5. Anything else (`ContainerAbsent`, `AccessDenied`, `Indeterminate`, mixed) → stay transient, return
   false, retry at the loop's cadence. **Terminal conclusions never come from errors.**

EXCISED from rev.7 §2 (see §9 rollback additions): the pool-wide emptiness proof and its 2-sample/spacing
protocol, `probePrefixEmptiness`, the outstanding-durable-request counter and `DurableRequestGuard` (the
**fence-generation checks stay** — they close the real [C2] late-PUT hole and are independent of the
proof), the `supportsErasureProof` capability, the grace arithmetic, `gc_quiescent_fn`.

**Startup bootstrap ordering (T7, KEPT — [C4][D2]):** zero-write residual check (its own `Backend::list`
helper, not the excised prefix-emptiness probe) BEFORE the `_probe` battery; missing `_pool_meta` over a
non-empty prefix ⇒ typed refusal, zero writes; mint gated by `allow_mint=false` default; `pool_prefix`
exclusively CAS-owned. **Contract [A1] (KEPT):** erase-and-recreate at the same prefix under live
processes is out of contract; `FORGET` is node-local.

## 3. Terminal transitions; serialization (simplified)

- `enterIdentityLost` / `enterVanished`: reason+`since` stamped before the state publish (as-built T12c);
  one WARN + ProfileEvent (`CasIdentityLost` / `CasDataRootVanished`); idempotent via the intent latch.
- **Thread exits on terminal states:** the keeper already exits on terminal renewal failure; the remount
  thread exits on `IdentityLost` and `Vanished`; **the GC scheduler self-exits at its next wake on
  `Vanished`, on the published FORGET-intent (`vanished_intent`, still pre-terminal — earliest-signal
  discipline), AND (rev.8) on `IdentityLost`** (as built: `isVanished() || vanishedIntentPublished() ||
  lifecycle()==IdentityLost` in both `loop()` and `heartbeatLoop()`) — rev.7 kept it ticking through
  `IdentityLost` solely because the erased proof ran from there (rev-t8 adjudication); with the proof
  excised, the last G2-zombie case (eternal `CORRUPTED_DATA` retries against a half-erased pool) closes too.
  Self-exit at the loop's own wake — no join from any callback (C6).
- The §3 serialization protocol (terminal-intent latch published first; checked by the keeper callback,
  the remount loop, the GC scheduler wake, and — [M1] — the remount step-0; joins outside `remount_mutex`;
  FORGET joins synchronously, natural transitions defer joins to `~Pool`) — unchanged.
- The disk stays registered; the T12 snapshot (`lifecycle`/`lifecycle_reason` enum-clean/`lifecycle_detail`
  full [D5]/`lifecycle_since`) shows every state.

## 4. Blast radius (unchanged rows, minus the erased column)

Transient: honest failures, auto-drain via the existing per-table `DROP` re-queue; fence-out recovery
≈36.5 s. `IdentityLost`: stalls eligible all-disk operations until `FORGET`/restart — accepted (origin is
an out-of-contract erase or a broken backing; cure is one verb). `Vanished`: sweeps skip on truth,
vanished-table `DROP` completes, `SELECT`/`BACKUP` fail with the typed reason, never silent-empty.
`search_orphaned_parts_disks=ANY` + AsyncLoader-no-retry exposure stands accepted with `LOCAL` guidance.

## 5–7. Verbs and introspection (as built, unchanged)

`SYSTEM CONTENT ADDRESSED FORGET` (§5 protocol as landed in T10 incl. the trip#2 race step), `GC
STOP/START` (§6 as landed in T11), the non-gated lifecycle snapshot and FSCK-on-running with
`meta_without_body` advisory (§7 as landed in T12/T13). FORGET is now not merely the escape hatch
but **the** decommission/teardown story — which the test teardown (T14) and 04290/04295/05020 already
assumed.

## 8. Known limits

- **Erasure is never concluded automatically** (rev.8, by decision): a genuinely erased pool presents as
  `IdentityLost` (fail-loud) until an operator `FORGET`s or recreates it. This is the deliberate trade:
  ~2000 lines of proof machinery for a capability production had switched off anyway.
- Cross-session: restart over a FULLY erased root re-bootstraps empty only after T7's empty-prefix proof;
  partial erase fails loud at startup. Partial tampering with `_pool_meta` intact: FSCK's domain.
- Erase-and-recreate under live processes: out of contract. Permanently-unconfirmable disk: stalls until
  `FORGET`/restart.

## 9. Rollback and tests

The Task 4–8 (old Dormant/UNMOUNT lifecycle) rollback list is unchanged from rev.7 §9 (MountState enum,
UNMOUNT/MOUNT verbs + AccessTypes + AST + `unmountSynchronously`/`mountExplicitly`, Part-6 branches
subsumed by the gate, FSCK-dormant-only, 4 lifecycle gtests, old teardown pattern; KEEP Part 1, `poolAccess`
snapshot, atomic `startup()`, `ca-fsck` rename, `pending_*` columns, gated GC entry points).

**NEW (rev.8) — the erased-proof excision list** (a dedicated task before T13 resumes):
1. The erasure-proof machinery in `CasPool.cpp` (`evaluateErasureProofEmptySample`, streak state, grace
   computation) + the `SentinelsGoneEmptyPrefix` verdict path → the §2 FORGET-only verdict switch.
2. `probePrefixEmptiness` (CasSentinelProbe) + its backend virtuals/forwarding.
3. `DurableRequestGuard` / `outstandingDurableRequests` / `beginDurableRequest` (counter only — every
   fence-generation CHECK stays).
4. `supportsErasureProof` + `setStrongPrefixListCapable` (+ the test-only capability backend).
5. `PoolConfig::gc_quiescent_fn` + `gcQuiescentForErasureProof` (+ the T8 wiring).
6. `PoolLifecycle::VanishedErased` enum value (`-Wswitch` sweeps every mapping: `casLifecycleToString`,
   `casLifecycleReasonWord`, `lifecycleReasonDetail`, the gate, the snapshot).
7. The remount observer's demoted-loop machinery (IdentityLost → thread exits instead).
8. GC scheduler self-exit condition widens to `IdentityLost || Vanished` (extends the C1 fix).
9. Tests: `gtest_cas_erasure_proof.cpp` removed; lifecycle/gate/forget suites adjusted (IdentityLost
   terminal-exit assertions replace observer assertions; counter tests removed; fence-generation tests
   KEPT).
10. Docs: BACKLOG `{#erased-capability-operator-assertion}` → OBSOLETE (this decision).

Tests otherwise per rev.7 §9 (unique names, fail-closed `DROP SYNC → FORGET → verify vanished(forgotten)
→ rm -rf` teardown — unchanged, it never used the proof).

## 10. Review ledger

- rev.1–rev.7: see git history (4 codex design reviews; architecture stable since rev.3).
- Implementation Tasks 1–12 landed under rev.7 with per-task opus reviews + a whole-increment Fable-xhigh
  review (C1 GC-zombie found; I1/I2/I3 resolved) + TLA modeling (Model 2, lifecycle/FORGET, continues as
  the T15 gate; Model 1, erasure proof, stopped — moot after this revision).
- rev.8: this document — the owner's FORGET-only decision after observing the implemented complexity
  ("лекарство хуже болезни" for the natural-proof wing): erasure is asserted, never proven. The excised
  stack remains reviewable at git history (v2 door).

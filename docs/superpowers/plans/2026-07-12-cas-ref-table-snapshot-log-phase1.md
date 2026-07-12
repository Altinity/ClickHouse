# CAS Ref Table Snapshot+Log — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to
> implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the mutable `RootShardManifest` ref storage with the immutable snapshot+log
protocol of the spec (rev.4), Phase 1 scope only, and validate with a 1-hour soak.

**Architecture:** Per-table append-only `_log/<txn-id>` objects + writer-published `_snap/<id>.proto`
snapshots; `GC` folds per-table cursors and manifest-edge deltas from one global `LIST`, cleans
covered objects, and never reconstructs table state. A CAS-owned S3 retry controller (single-attempt
conditional writes, exact-key resolution, lease gating) is the prerequisite.

**Tech Stack:** C++ (ClickHouse tree), TLA+/TLC gates, gtest, rustfs integration, utils/ca-soak.

**Normative source:** `docs/superpowers/specs/2026-07-11-cas-ref-table-snapshot-log-design.md`
(rev.4). Where this plan and the spec disagree, the spec governs; report the conflict. The companion
RFC `docs/superpowers/specs/2026-07-12-cas-s3-timeout-retry-control-rfc.md` governs Tasks 4-5.

## Global Constraints

- Branch: `cas-gc-rebuild`. New commits only; never rebase/amend; never commit to master.
- Commit trailers (every commit):
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` +
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.
- **Phase 1 only. Explicitly out of scope** (do not implement, do not scaffold): inline zero-byte
  log keys; `GC`-side fallback compaction; indexed/chunked multi-object snapshots; lazy snapshot
  blocks / row-level cache eviction; per-round ref index; chunked namespace removal; Keeper-based
  cross-epoch fencing. The ONLY permitted optimizations are those the spec's Phase 1 mandates
  (writer-local batching queue, whole-table cache, body reuse within one fold batch).
- **No compatibility scaffolding**: `RootShardManifest`/`CasRootShardCodec` and every caller are
  DELETED by the end (grep gate in Task 12). No dual-format readers, no migration. A pool written by
  the old format must fail closed via the pool format bump (Task 12).
- `GC` never reads/GETs a condemned object to revive it; revival = fresh re-upload only.
- `GC` must never throw/fail-closed on a 404 during fold in a way that wedges rounds: record + continue
  where the spec allows; abort the attempt (not the process) where the spec demands fail-closed.
- TLA gates (Tasks 1-3) must be GREEN before any dependent C++ task starts.
- Builds: `ninja -C build_debug <target> > build_debug/build_<task>.log 2>&1` (no `-j`); analyze the
  log with a subagent; unit tests run via
  `build_debug/src/unit_tests_dbms --gtest_filter='<Filter>*' > build_debug/test_<task>.log 2>&1`.
- Temp files in `tmp/` under repo root. NEVER delete `ci/tmp/rustfs`. Monitor disk; >85% → clean
  build artifacts of unrelated build dirs only.
- Style: Allman braces; no `sleep` for races; comments state constraints, not narration; say
  "exception" not "crash" for logical errors.

## File Structure (target)

```
docs/superpowers/models/CaRefTableSnapshotLogCore.tla|_safe.cfg|_latepred.cfg|run_refsnaplog.sh
docs/superpowers/models/CaRefDeltaIntakeCore.tla|_safe.cfg|_latepred.cfg|run_refintake.sh
docs/superpowers/models/CaRefWriterCleanupCore.tla|_safe.cfg|run_refwcleanup.sh
src/.../ContentAddressed/Core/CasRefIds.h                (RefTxnId, canonical hex render/parse)
src/.../ContentAddressed/Core/CasRefLogCodec.{h,cpp}     (RefLogTxn, RefOp, OwnerBinding, wire)
src/.../ContentAddressed/Core/CasRefSnapshotCodec.{h,cpp}(RefTableSnapshot wire, canonical sort)
src/.../ContentAddressed/Core/CasRefStateMachine.{h,cpp} (TableState, apply, admission budget)
src/.../ContentAddressed/Core/CasRequestControl.{h,cpp}  (retry controller, outcome classes)
src/.../ContentAddressed/Core/CasLayout.h                (+_cleanup/_log/_snap, hex manifests)
src/.../ContentAddressed/Core/CasStore.{h,cpp}           (writer: recovery/lane/queue/snapshots)
src/.../ContentAddressed/Core/CasGc.{h,cpp}              (ref intake, cursors, cleanup, ns item)
src/.../ContentAddressed/Core/CasOrphanManifestSweep.cpp (snapshot+tail protection view)
src/Disks/tests/gtest_cas_ref_codecs.cpp                 (new)
src/Disks/tests/gtest_cas_ref_statemachine.cpp           (new)
src/Disks/tests/gtest_cas_request_control.cpp            (new)
src/Disks/tests/gtest_cas_ref_writer.cpp                 (new)
src/Disks/tests/gtest_cas_ref_intake.cpp                 (new)
DELETED: Core/CasRootShardCodec.{h,cpp} and all references
```

Interfaces referenced across tasks (exact, defined in Task 6 unless noted):

```cpp
struct RefTxnId { uint64_t writer_epoch = 0; uint64_t ref_sequence = 0;
                  auto operator<=>(const RefTxnId &) const = default; };  /// both nonzero when valid
String renderRefTxnId(const RefTxnId &);                       /// "0000000000000007-000000000000008e"
std::optional<RefTxnId> parseRefTxnId(std::string_view);       /// canonical-only, reject otherwise

enum class RefOpKind : uint8_t { NamespaceBirth = 1, OwnerTransition = 2, SetPayload = 3,
                                 RemoveNamespace = 4 };
enum class RefOwnerKind : uint8_t { Committed = 1, Precommit = 2 };
struct RefOwnerBinding { RefOwnerKind kind; String ref_name; PartManifestRef manifest_ref; };
struct RefOp { RefOpKind kind;
               std::optional<RefOwnerBinding> old_binding;   /// OwnerTransition
               std::optional<RefOwnerBinding> new_binding;   /// OwnerTransition
               String ref_name; PartManifestRef expected_manifest_ref;  /// SetPayload
               String payload; uint64_t published_at_ms = 0; };         /// SetPayload
struct RefLogTxn { String ns; RefTxnId txn_id; std::vector<RefOp> ops; };

enum class RefLifecycle : uint8_t { Live = 1, Removed = 2 };
struct RefCommittedRow { String ref_name; PartManifestRef manifest_ref; String payload;
                         uint64_t published_at_ms = 0; };
struct RefTableSnapshot { String ns; RefTxnId snapshot_id; RefLifecycle lifecycle;
                          std::optional<RefTxnId> remove_txn_id;
                          std::vector<RefCommittedRow> committed;      /// sorted by ref_name
                          std::vector<RefOwnerBinding> precommits; };  /// sorted (ref_name, mref)

enum class CasWriteOutcome : uint8_t { Committed, DefiniteFailure, Unresolved };  /// Task 5
```

`PartManifestRef` is the existing manifest identity struct in `Core/CasIds.h` (writer_epoch,
build_sequence, ordinal fields — reuse it verbatim; if the current name differs, use the current
name everywhere and report it in the task report). Wire encodings use fixed-width little-endian
integers and u32-length-prefixed byte strings; every message starts with `u32 format_version = 1`;
unknown versions fail closed.

---

### Task 1: TLA+ gate — CaRefTableSnapshotLogCore

**Files:** Create `docs/superpowers/models/CaRefTableSnapshotLogCore.tla`,
`CaRefTableSnapshotLogCore_safe.cfg`, `CaRefTableSnapshotLogCore_latepred.cfg`,
`run_refsnaplog.sh` (follow `run_retiredinrun.sh` as the runner template).

**Model scope (spec §tla-models):** one table; writer appends immutable logs in strictly increasing
ids with at most one unresolved append; writer publishes snapshots (grace age abstracted as "may not
cover the newest log"); reader recovery = one ordered scan (`_log` before `_snap`) + body fetches
with restart-on-vanish; cleanup deletes logs `<= X` and snapshots `< X` only for an observed durable
snapshot `X`; namespace removal + recreation gated on a `Completed` marker.

**Invariants:** (1) `Replay(newest visible valid snapshot, surviving later logs) = Replay(full
history)` for every reader that completes (possibly after restarts); (2) cleanup never removes an
object a completing reader still needs; (3) recreation never observes a stale delete.
**Sabotage configs (each must FAIL when enabled):** delete a log before its covering snapshot is
observed durable; reader treats vanished-selected-object as corruption instead of restart; recreation
before `Completed`. **Adversarial config `_latepred`:** `LatePredecessorPut` action — an old-epoch
log materializes after the successor's scan; the complete-recovery invariant MUST produce the known
counterexample (expected-fail, kept as documentation).

- [ ] Write model + safe cfg; TLC green on safe config
- [ ] Each sabotage toggle produces a violation trace
- [ ] `_latepred` produces the expected counterexample
- [ ] Commit: `tla(cas): CaRefTableSnapshotLogCore gate for ref snapshot-log Phase 1`

### Task 2: TLA+ gate — CaRefDeltaIntakeCore

**Files:** Create `docs/superpowers/models/CaRefDeltaIntakeCore.tla`, `_safe.cfg`, `_latepred.cfg`,
`run_refintake.sh`.

**Model scope:** two tables interleaved in key space; paginated enumeration with
resume-after-last-returned-key; concurrent strictly-ordered writer appends (with id gaps); per-table
candidate cursors adopted atomically by a single fold commit that can lose; deterministic `+1/-1`
edge events; cleanup requires cursor coverage AND observed snapshot coverage; no content deletion in
intake. **Invariants:** every durable log's delta is adopted exactly once (event-id idempotent);
cursor never passes an unreturned durable log (the three-premise proof of spec
§gc-step-enumerate-once); losing commits adopt nothing. **Sabotage:** resume from a position beyond
the last returned key (must violate); advance cursor before adoption; clean a log covered by
snapshot but not cursor. **Adversarial `_latepred`:** late old-epoch insert behind the scan → missed
delta counterexample retained (expected-fail).

- [ ] Model + safe cfg green; sabotage toggles fail; `_latepred` expected-fail
- [ ] Commit: `tla(cas): CaRefDeltaIntakeCore gate (pagination premises, cursor safety)`

### Task 3: TLA+ gate — CaRefWriterCleanupCore (small)

**Files:** Create `docs/superpowers/models/CaRefWriterCleanupCore.tla`, `_safe.cfg`,
`run_refwcleanup.sh`.

**Model scope (spec §tla-models):** active builds + exact precommit ownership;
removal-before-retirement for current-writer failures; fenced successor removes predecessor-epoch
precommits in bounded batches with interruption; namespace removal cancels local builds only after
its transaction is durable. **Invariants:** no owner loss (delay only over-protects); weak fairness
on successor maintenance ⇒ eventual cleanup (`pending ~> cleaned` liveness property).

- [ ] Model + cfg green (safety + liveness); commit `tla(cas): CaRefWriterCleanupCore gate`

### Task 4: Single-attempt conditional writes + outcome classification

**Files:** Modify the S3 write path used by `ObjectStorageBackend::nativeConditionalPut` (locate via
`grep -rn "nativeConditionalPut" src/`); Create `Core/CasRequestControl.h` (outcome enum only, full
controller in Task 5); Test `src/Disks/tests/gtest_cas_request_control.cpp`.

**Requirements (RFC §required-retry-policy):** conditional (`If-None-Match`/`If-Match`) CAS writes
perform exactly ONE HTTP attempt — request-scoped override, not a process-wide retry change; the
raw result is classified into `CasWriteOutcome`: `Committed` (2xx), `DefiniteFailure` (whitelist:
malformed-request / entity-too-large / access-denied classes — NEVER `PreconditionFailed`),
`Unresolved` (timeout, connection loss, 5xx, `PreconditionFailed` before resolution — the caller
resolves via exact-key `GET`). Add per-class counters (attempts, sdk_retries_must_stay_zero,
outcomes).

- [ ] Failing test: fault-injecting backend asserts exactly 1 HTTP attempt per conditional write and
      the classification table above (each row one test case)
- [ ] Implement; tests green; unrelated S3 disks keep their retry config (test with a plain disk)
- [ ] Commit: `cas: single-attempt conditional writes with explicit outcome classification`

### Task 5: CAS retry controller

**Files:** Create `Core/CasRequestControl.{h,cpp}`; Test `gtest_cas_request_control.cpp` (extend).

**Interface (consumed by Tasks 8-11):**

```cpp
struct CasRequestBudget { uint64_t attempt_timeout_ms; uint64_t operation_deadline_ms;
                          uint32_t max_attempts; uint64_t lease_safety_margin_ms; };
class CasRequestController {
public:
    /// putIfAbsent with resolve-before-reissue. fence() checked before every attempt and before
    /// returning Committed. GET-absent NEVER yields DefiniteFailure (spec §writer-side-linearization).
    CasWriteOutcome putIfAbsentControlled(std::string_view key, std::string_view bytes,
                                          const std::function<bool()> & fence_ok);
    CasWriteOutcome resolveByExactGet(std::string_view key, std::string_view expected_bytes);
};
```

Startup validation: `attempt_timeout + lease_safety_margin < mount_lease_ttl` else refuse writable
mount (RFC §required-timeout-model). Config keys per RFC §configuration (derive defaults from lease
TTL; expose effective values in logs).

- [ ] Failing tests: uncertain→GET-identical=Committed; uncertain→GET-different=throws corruption;
      uncertain→GET-absent=Unresolved (retry same key within budget, never a new key); budget
      exhaustion returns Unresolved; fence-lost before attempt → no attempt sent; fence-lost after
      write → no Committed; invalid config rejected at writable open
- [ ] Implement; green; commit: `cas: CAS-owned retry controller (deadlines, fence gating, exact-key resolution)`

### Task 6: RefTxnId + RefLogTxn codec

**Files:** Create `Core/CasRefIds.h`, `Core/CasRefLogCodec.{h,cpp}`; Test
`src/Disks/tests/gtest_cas_ref_codecs.cpp`; register in `src/Disks/tests/` build like the existing
`gtest_cas_codecs.cpp`.

Types verbatim from **Interfaces** above. Wire (`encodeRefLogTxn`/`decodeRefLogTxn`):
`u32 ver=1 | u32 ns_len+bytes | u64 epoch | u64 seq | u32 op_count | ops...`; op:
`u8 kind | kind-specific` with OwnerTransition = `u8 has_old [binding] u8 has_new [binding]`,
binding = `u8 owner_kind | u32 name_len+bytes | u64 m_epoch | u64 m_seq | u32 m_ordinal`,
SetPayload = `u32 name_len+bytes | manifest_ref | u32 payload_len+bytes | u64 published_at_ms`.
Validation on decode: nonzero txn id fields; op count/byte limits (`ref_txn_max_ops`,
`ref_txn_max_bytes` — plain constants with a removal-class exception flag); ref names canonical
clean relative paths (reject empty, `.`, `..`, repeated separators); key-derived fields must match
body (`ns`, `txn_id` repeated in body per spec §object-layout); unknown version/op kind fail closed.

- [ ] Failing tests: hex render/parse canonical + rejects (short, upper, zero, overflow, garbage);
      tuple order == lexical order of renders (property over a sample grid); round-trip each op
      kind; every validation rejection listed above
- [ ] Implement; green; commit: `cas: RefTxnId and RefLogTxn deterministic codec`

### Task 7: RefTableSnapshot codec

**Files:** Create `Core/CasRefSnapshotCodec.{h,cpp}`; Test `gtest_cas_ref_codecs.cpp` (extend).

Wire: `u32 ver=1 | ns | snapshot_id | u8 lifecycle | [remove_txn_id if Removed] | u32 n_committed |
rows | u32 n_precommits | rows`. Encoder REQUIRES sorted input (bytewise `ref_name`; precommits then
by manifest ref) and throws on unsorted/duplicate; decoder validates sortedness, canonical names,
nonzero ids; a `Removed` snapshot must have zero rows and a `remove_txn_id`. Deterministic: no
timestamps, no map iteration.

- [ ] Failing tests: round-trip Live and Removed; byte-identical re-encode of the same logical
      state; rejects unsorted/duplicate/non-canonical/oversized (`ref_snapshot_max_bytes`)
- [ ] Implement; green; commit: `cas: RefTableSnapshot deterministic codec`

### Task 8: CasLayout — ref object keys + hex manifest paths

**Files:** Modify `Core/CasLayout.h`; Test `gtest_cas_layout.cpp` (extend).

Add: `refLogKey(ns, RefTxnId)` → `<prefix>/cas/refs/<ns>/_log/<render>`;
`refSnapshotKey(ns, RefTxnId)` → `.../_snap/<render>.proto`;
`refCleanupMarkerKey(ns, RefTxnId)` → `.../_cleanup/<render>`;
`parseRefObjectKey(key) -> optional<{ns, kind∈{Cleanup,Log,Snap}, RefTxnId}>` (strict; non-canonical
rejected). Change the manifest path to spec §manifest-identifier canonical hex:
`cas/manifests/<ns>/<epoch-hex>-<seq-hex>/<ordinal-6hex>.proto`; update the single shared manifest
render/parse and its callers (sweep, fsck, tests). Keep the old `rootShardKey` functions compiling
for now (deleted in Task 12).

- [ ] Failing tests: key round-trips for all three kinds; `_cleanup` < `_log` < `_snap` lexical
      order asserted; manifest hex path round-trip; non-canonical parse rejections
- [ ] Implement; green; commit: `cas: ref-object key layout and canonical hex manifest paths`

### Task 9: Shared transition validator (CasRefStateMachine)

**Files:** Create `Core/CasRefStateMachine.{h,cpp}`; Test `gtest_cas_ref_statemachine.cpp`.

```cpp
struct RefTableState { RefLifecycle lifecycle = RefLifecycle::Removed;  /// empty==never born
                       std::optional<RefTxnId> remove_txn_id; RefTxnId greatest_applied{};
                       std::map<String, RefCommittedRow> committed;
                       std::set<std::pair<String, PartManifestRef>> precommits; };
/// Applies the COMPLETE txn or throws (spec §state-transitions, all preconditions); used verbatim
/// by writer, recovery, fsck, and snapshot construction. Never mutates state on throw.
void applyRefLogTxn(RefTableState &, const RefLogTxn &);
RefTableSnapshot snapshotOf(const RefTableState &, const String & ns);      /// canonical sort
RefTableState replay(const std::optional<RefTableSnapshot> &, std::span<const RefLogTxn>);
/// Admission budget (spec §snapshot-format): both bounds, estimated incrementally.
bool admits(const RefTableState &, const RefOp &, uint64_t snapshot_budget, uint64_t removal_budget);
```

- [ ] Failing tests: every transition precondition from spec §state-transitions (birth on
      non-Removed rejected; exact-binding checks; promote atomicity — no ownerless moment observable
      in the post-state; payload expected-ref check; removal requires empty owners after its own
      transitions; ops after `Removed` rejected except birth; strictly increasing txn ids); replay
      equation: random op sequences → `replay(snapshotOf(state), tail) == full replay`; `admits`
      rejects growth past either budget (`owner_transition`, `set_payload`, promote-with-payload)
- [ ] Implement; green; commit: `cas: shared ref transition validator and replay`

### Task 10: Writer — recovery, cached state, append lane, queue

**Files:** Modify `Core/CasStore.{h,cpp}` (the ref mutation + read paths currently built on
`CasRootShardCodec` — locate via `grep -n "RootShard" Core/CasStore.cpp`); Test
`gtest_cas_ref_writer.cpp`.

Scope (spec §writer-algorithms): (a) recovery = one `LIST` of `cas/refs/<ns>/` via
`parseRefObjectKey`, greatest snapshot, `GET`+validate, tail `GET`s in id order through
`applyRefLogTxn`, restart-on-vanish (bounded, counted), `_cleanup` markers retained for the
recreation gate; (b) whole-table cache keyed by ns (evict = drop whole object); (c) append lane:
`ref_sequence` from the store-wide counter, at most one unresolved `PUT` per table via
`CasRequestController::putIfAbsentControlled` with fence callback, wedge semantics (Unresolved
blocks the lane; observed-durable applies to cache before unwedging); (d) the existing batching
queue re-targeted: bounded compatible batch → one `RefLogTxn` (validate with per-request undo
against cached state; invalid requests get their exceptions; birth/removal run alone; one op per
ref name per batch); (e) admission budget enforced pre-encode. All public `CasStore` ref entry
points (`publishRef`/`dropRef`/`updateRefPayload`/precommit add/remove/promote — use current names)
keep their signatures; only persistence changes.

- [ ] Failing tests first (fake backend): empty+birth recovery; snapshot+tail recovery; recovery
      restart when snapshot vanishes between LIST and GET (converges on newer); wedged lane blocks
      later same-table ids while another table proceeds; wedged append observed durable → cache
      applied before next id; failed queue entry returns its own exception, batch survives; warm
      isolated mutation = exactly 1 backend create, 0 ref reads (count requests); `B` compatible
      mutations share 1 create
- [ ] Implement; green; existing `gtest_cas_store` ref tests adapted (rewrite assertions to the new
      persistence, keep the behavioral contracts)
- [ ] Commit: `cas: writer ref persistence on snapshot+log (recovery, lane, queue)`

### Task 11: Writer — snapshot publication, stale-precommit cleanup, namespace removal

**Files:** Modify `Core/CasStore.{h,cpp}`; Test `gtest_cas_ref_writer.cpp` (extend).

Scope: (a) snapshot publication (spec §writer-snapshot-publication): thresholds
`snapshot_log_count_threshold` / `snapshot_log_bytes_threshold` + mount-time trigger + grace age
`snapshot_min_log_age_ms` (own appends timed locally; mount-replayed logs via LIST metadata);
background, off-lane, `putIfAbsentControlled` + byte-compare resolve; (b) failed-build exact
precommit removal before retirement (current writer) and fenced successor cleanup: after recovery,
bounded `RefLogTxn` batches removing all precommits with `manifest_ref.writer_epoch < current`;
(c) `dropNamespace` → one body txn (exact transitions for every owner + `remove_namespace`), apply
to memory, reject further ops, publish constant-size `Removed` snapshot after durability;
recreation (`namespace_birth` while `Removed`) requires the exact `_cleanup` marker from recovery.

- [ ] Failing tests: threshold + mount-time snapshot triggers; grace age respected (young log not
      covered); snapshot never blocks a concurrent append; cache-replay equivalence (published bytes
      == replay of logs); successor cleanup emits exact removals in bounded batches, interruption
      resumes; removal txn names every owner then `remove_namespace`; repeated drop returns success
      without a second txn; birth without marker rejected even with empty prefix; birth with
      matching marker accepted, continues the id timeline
- [ ] Implement; green; commit: `cas: writer snapshots, successor precommit cleanup, namespace removal`

### Task 12: GC — ref intake, cursors, cleanup, namespace item; delete old format

**Files:** Modify `Core/CasGc.{h,cpp}`, `Core/CasOrphanManifestSweep.cpp`; DELETE
`Core/CasRootShardCodec.{h,cpp}`; Tests `gtest_cas_ref_intake.cpp` (new) + adapt
`gtest_cas_gc_round.cpp` / `gtest_cas_gc_resume.cpp` / `gtest_cas_gc_ack_floor.cpp` fixtures to
write ref logs instead of shard manifests (behavioral assertions unchanged).

Scope (spec §gc-round-algorithm): (a) one global paginated `LIST cas/refs/` with
resume-after-last-returned-key (explicit `start-after`); (b) per-table `last_folded_ref_id` cursors
persisted in the existing fold artifacts, adopted by the existing single `gc/state` CAS; (c) edge
delta: `+1/-1` per spec §gc-step-produce-manifest-edge-delta with
`event_id={ns, txn_id, op_ordinal, edge_ordinal}`, promote-same-manifest = no net change,
add+remove within one unfolded batch cancel; malformed key/body → abort ref folding for the round
(no partial delta); (d) cleanup: observed-snapshot + cursor coverage (+item durable for removal
txns), exact keys, batches ≤1000, snapshots < newest observed deletable; (e) namespace-cleanup item
`{ns, remove_txn_id}` `Pending→Completed` in fold artifacts; exact-key enumerate-and-delete passes;
after `Completed`: publish `_cleanup` marker + republish `Removed` snapshot (both idempotent);
(f) orphan sweep protection view = newest snapshot + complete tail (owners + manifests removed
anywhere in tail) + young-manifest window unchanged + `last_folded_ref_id` condition; (g) pool
format: bump the CAS pool schema constant (locate: `grep -rn "kSourceEdgeKeySchema\|pool_meta" Core/`)
so old-format pools fail closed with a clear message; (h) delete `CasRootShardCodec` and every
reference — grep gate: `grep -rn "RootShard" src/ | grep -v build` returns ZERO lines.

- [ ] Failing tests: >1000-key scan folds every pre-existing log exactly once; concurrent append
      behind the scan is not skipped (cursor stays below); later-page append included; cursor never
      advances past an unreturned log (fault-injected pagination); edge cancellation; losing commit
      adopts nothing and deletes nothing; cleanup respects all three conditions; item passes
      re-executed after leader change; marker+Removed-snapshot republication; sweep protects
      tail-removed manifests and skips (not deletes) on any missing/invalid input
- [ ] Implement; green; grep gate zero; full unit sweep
      (`--gtest_filter='*Cas*'`) green
- [ ] Commit: `cas: GC ref intake on snapshot+log; remove RootShardManifest format`

### Task 13: Consumers — fsck/inspect, counters, e2e integration

**Files:** Modify `CasInspect.cpp`, fsck path (locate via `grep -rn "fsck" Core/`), counters;
Test: existing fsck/inspect gtests adapted; new e2e in `gtest_cas_ref_intake.cpp`.

Scope: read-only consumers use `replay` from Task 9 with restart-on-vanish; fsck adds the
cache-replay/snapshot byte-compare oracle; counters from spec §implementation-impact (LIST pages,
body GETs, logs-per-table-after-snapshot, snapshot PUT bytes, `H`, emitted edges, cleanup backlog,
restart/resolution/wedge counters from earlier tasks). E2e (rustfs, existing integration harness):
create pool → inserts/drops/renames across 2 tables → force GC rounds → assert fold+cleanup+snapshot
lifecycle → `fsck` clean → `ca-gc-dryrun` empty.

- [ ] Tests green; commit: `cas: read-only consumers, counters, e2e for ref snapshot+log`

### Task 14: 1-hour soak

Rebuild release binary; 2-node ca-soak cluster on rustfs (`utils/ca-soak`, compose from
`scenarios/framework/cluster_boot.py`); run phase 3 `--duration 60m` with the standard fault mix;
during the run monitor disk and node logs for `Logical error`/`Exception` storms. Success criteria:
run completes; final converge `fsck dangling=0 unreachable=0` held for 2 consecutive reads;
`ca-gc-dryrun` empty; no logical-error exceptions; snapshot/log counters show bounded tails.
Archive host logs to `tmp/refsnaplog_soak/` before teardown (`down -v` only after archiving).

- [ ] Soak green; record verdict in ledger + worklog; commit any harness fixes separately

---

## Self-Review Notes

- Spec coverage: Tasks 1-3 = §tla-models; 4-5 = RFC; 6-8 = §common-identifiers/§object-layout/
  §snapshot-format/§transaction-log-format; 9 = §state-transitions/§table-state; 10-11 =
  §writer-algorithms (+§read-only-consumers via Task 13); 12 = §gc-round-algorithm/
  §concurrent-startup-and-cleanup/§orphan-manifest-protection + format gate; 13 = counters +
  consumers; 14 = soak. Unit-test bullets in the spec map onto the per-task failing-test lists.
- Deliberate deviations: none. Phase 2 items excluded per Global Constraints.
- Execution: subagent-driven; TLA tasks and Task 10/12 need a capable model; codec tasks (6-9) are
  transcription-class. Controller augments each dispatch with current symbol names discovered at
  dispatch time (existing entry-point names in `CasStore`/`CasGc`).

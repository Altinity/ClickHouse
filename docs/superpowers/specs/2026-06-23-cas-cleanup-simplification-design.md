# CAS cleanup / simplification — design (behavior-preserving)

**Status:** design for unattended execution
**Date:** 2026-06-23
**Branch:** `cas-vfs-path-mapping`
**Inputs:** the two read-only analyses provided by the operator — the *architecture analysis*
(17-step refactor sequence; clusters C1–C10) and the *consistency review* (findings F1–F9) for
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (the CAS feature).

## Goal

Reduce noise and duplication in the CAS feature **without changing any runtime behavior or any
on-disk byte format**. Every change is a refactor: the code must do exactly what it does today.

## Hard constraints (the contract for every task)

1. **No behavior change.** Same reads, same writes, same GC decisions, same exceptions, same logs
   (event *types/fields* preserved), same metrics (except the throwaway `CasDbg*` we intentionally
   delete). When in doubt, preserve.
2. **No on-disk format change.** Every codec must produce **byte-for-byte identical** output and
   accept exactly the same inputs. UInt128 byte orders (LE binary / BE protobuf / lowercase hex) are
   frozen on-disk formats — we may only *name and route* them through typed helpers, never re-byte.
3. **No new bugs.** Prefer the smallest change that removes the duplication. Reject a refactor that
   cannot be proven equivalent by the existing tests.
4. **Verification (the oracle), per task:**
   - the existing gtest suite (`Cas*`/`Ca*`, round-trip-through-real-codecs fixtures) — must stay
     green except the known-unrelated baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow`;
   - for codec tasks: an explicit **byte-equality** assertion (encode before == encode after) added
     in the task and then removed or kept as a golden test;
   - for event tasks: the event-assertion tests (event *type/field* equality) plus the soak's
     regression-watch;
   - a final **6h chaos soak** with `dangling=0`, `unreachable` draining, clean regression-watch.

## Scope — items to do this round (each behavior-preserving, individually shippable)

Ordered safest→riskier. Each names its analysis source, the change, and *why it preserves behavior*.

### Group 1 — mechanical (near-zero risk)

- **R1 (F1 / step 1): remove the throwaway `CasDbg*` instrumentation.** Delete the 4
  `CasDbgExistsHit/Miss`,`CasDbgMetaHit/Miss` ProfileEvents (`ProfileEvents.cpp`) and
  `casDbgSampleHeadMiss` + its call sites in `S3ObjectStorage.cpp` (`exists`,`tryGetObjectMetadata`).
  *Preserves behavior:* these are self-described "DBG(CA soak)" debug counters + a 404 stack
  sampler; removing them changes no functional path (only drops debug telemetry). **Risk:** confirm
  no dashboard depends on `CasDbg*` (operator has none — feature in dev).

- **R2 (step 2): `toEventKind(ObjectKind)` free function.** Replace the ≥7 duplicated
  `kind == Tree ? CasEventObjectKind::Tree : …` ternaries with one mapping fn.
  *Preserves behavior:* pure extraction of an identical mapping.

- **R3 (step 4): `cursorKey(ns,shard)` / `parseCursorKey` helpers in `CasGc`.** Route the ~5
  `ns.string()+"/"+to_string(shard)` format sites and the inverse parse through one pair.
  *Preserves behavior:* the helper must emit the identical string (golden-assert in a unit test).

- **R4 (step 16 / docs): documentation-only reconciliations.** Reconcile the `RootsRegistry` type
  name vs its `gc/registry` storage key (add a clarifying comment naming both; do **not** rename the
  key — that would be a format change); confirm the `CasRootShardCodec.h` JSON→protobuf doc-comment
  is corrected (already done this branch); add a comment marking the test-only `EnvelopeHeader` knobs.
  *Preserves behavior:* comments only.

### Group 2 — semantic, pure refactor (compile/test-checked)

- **R5 (step 10 / C7): a `Placement` visitor / `forEachPlacement`.** Route the 5–6 duplicated
  `Blob/PackSlice/Subtree/Inline` switches through one exhaustiveness-checked visitor.
  *Preserves behavior:* identical arms; the compiler enforces exhaustiveness (no silent default).

- **R6 (step 3 / C6): `EventEmitter` helper.** Seed once with `(identity = build_id|gc_id, round,
  store-sink)`; collapse the ~24 (`Build`) + ~30 (`Gc`) `if(hasEventSink){CasEvent _evN; …; emit}`
  blocks into single `emit(type, kind, hash, outcome, reason, detail={})` calls.
  *Preserves behavior:* must emit the **identical** event (every field, incl. the REQUIRED `reason`),
  and stay zero-cost when no sink is set. Oracle: event-assertion gtests + soak regression-watch.
  Events are audit-only — they never feed read/write/GC decisions, so this cannot corrupt data; the
  only failure mode is a changed/dropped audit field, which the assertions catch.

- **R7 (step 5): one `persistGenerationProbingUpward` helper.** Extract the duplicated
  "probe-upward generation persist" loop (the fold copy `CasGc.cpp:~657-698` and the cascade copy
  `~1755-1795`, same `max_generation_probes=1000` brake) into one function called from both.
  *Preserves behavior:* identical loop body; assert via existing GC gtests.

### Group 3 — codec substrate, **byte-identical** (unit byte-equality oracle)

- **R8 (step 9 / C7 / F5-adjacent): `checkVersion(current, seen, what)` helper.** Route the 4
  hand-written `seen > CURRENT → throw` gates (`CasEnvelope`, `CasTreeCodec`, `CasRootShardCodec`,
  `CasCodecUtil`) through one helper. *Preserves behavior:* same comparison, same throw — except the
  error **code** is updated by R12 (below); keep the message text equivalent.

- **R9 (step 9 / C7): named `writeU128`/`readU128` wire helpers.** Give each of the three frozen
  byte orders a named, typed helper (`writeU128LE/readU128LE`, `writeU128BE/readU128BE`, the hex pair
  already exist) and route the ad-hoc sites through them. *Preserves behavior:* **zero byte change** —
  the helpers reproduce the exact current bytes; assert byte-for-byte on every codec round-trip.

- **R10 (step 9 / F9): `JsonObjectWriter` for the 4 strict-JSON encoders.** `ServerWatermark`,
  `Heartbeat`, `PoolMeta`, `RootsRegistry` encode sides currently copy the `{format,version,…}`
  brace/comma boilerplate. Introduce one small RAII object-writer that emits the identical JSON.
  *Preserves behavior:* **byte-identical** JSON (golden-assert each encoder before==after). Decode is
  already DRY via `CasCodecUtil` and is untouched.

### Group 4 — consistency alignment (user-facing; pre-release, so safe to settle now)

- **R11 (F4): unify the config keys.** Collapse `cas_scratch_path` + the six `content_addressed_*`
  keys to one scheme — **bare keys under the per-disk node** (`gc_enabled`, `gc_interval_sec`,
  `root_shards`, `dedup_cache_bytes`, `dedup_head_first_min_bytes`, `gc_snap_generations_to_keep`,
  `scratch_path`), matching the disk-node norm (`object_metadata_cache_size`, etc.). This is an
  **atomic** change: `MetadataStorageFactory.cpp` **and** every config that sets these
  (`utils/ca-soak/configs/storage_conf.xml`, any test disk configs) **and** any docs, in one task.
  *Preserves behavior:* same tunables, same defaults, same effect — only the key spelling changes.
  *Risk:* a missed config reference makes the disk fail to configure → caught at soak bringup + the
  factory's own defaults. The task must grep the whole tree for the old keys and update all.

- **R12 (F5): `UNKNOWN_FORMAT_VERSION` for a future on-disk version.** At the 3 future-version gates
  (`CasEnvelope.cpp:192,330`, `CasRootShardCodec.cpp:183`) throw `UNKNOWN_FORMAT_VERSION` (287)
  instead of `NOT_IMPLEMENTED`; keep `CORRUPTED_DATA` for malformed and reserve `NOT_IMPLEMENTED` for
  unsupported *operations*. Update the test contract (`cas_test_helpers.h::expectThrowsCode` and any
  test asserting the old code). *Preserves behavior:* the only observable change is a more accurate
  error code on a branch that **cannot trigger in the current single-version deployment** (no
  future-version objects exist), so the soak/tests never hit it in practice.

### Group 5 — backend seam (mechanical, touches 4 impls uniformly)

- **R13 (step 15): `WriteResult{outcome, token}` + reuse `casRemoveObject`.** Replace the
  `out_token` out-parameter on the 3 backend write methods (× 4 impls) with a returned
  `WriteResult`, removing the `if(out_token)*out_token=…` branch everywhere; and make
  `Store::dropNamespace` reuse the verified `casRemoveObject` helper instead of re-inlining the
  head+conditional-delete body. *Preserves behavior:* same outcomes, same tokens, same deletes.

### Group 6 — higher-value, higher-touch (do last; gated by review + soak)

- **R14 (step 8 / C3): merge `WatermarkKeeper` + `HeartbeatKeeper` → `SingleWriterSlotKeeper`.** One
  base/template for the durable single-writer slot (anchor → async renew → terminal op; fail-closed
  on foreign token); the delta becomes an **explicit policy**: watermark = epoch-equality +
  `min_active` + retire-via-`farewell`(sets `min_active=UINT64_MAX`); heartbeat = `created_at_ms` +
  discard-via-`deleteExact`. *Preserves behavior:* every observable op (anchor/renew/stop/terminal,
  the foreign-touch fail-close, the exact JSON bodies) is identical; the merge is a code-sharing
  change only. **Dead-code note:** `farewell()` has **no production caller** today (`~Store` calls
  `stopBackground`, never `farewell`); we **preserve** it as a policy hook (removing it is a separate
  decision) and record the observation. *Risk (liveness/GC-lease safety):* this is the one item whose
  bug could escape unit tests, so it is **last**, gated on both review stages, and validated by the
  6h chaos soak (which stresses lease/heartbeat/watermark under kills). If spec/quality review finds
  the two policies do not factor cleanly without blurring the safety contract, **defer R14** and ship
  R1–R13.

## Explicitly deferred (NOT this round) — with rationale

These are real and worthwhile but carry behavior-change/safety risk disproportionate to an unattended
no-regression round, or are wide mechanical churn better done with focused attention:

- **RefId/RefMove (step 6/C1), ObjectRef (step 7/C5):** wide signature churn across many files /
  hot-path identity type. Mechanical but large; defer to a dedicated round.
- **RoundContext (step 11/C4) + CasGc file split (step 12):** must preserve the "thread, never
  re-read" anti-zombie invariant (`CasGc.h:168-172`); high-touch on the most safety-critical file.
- **PoolContext/PoolTuning (step 13/C2), RoutedRead (step 14/C8):** ctor/threading reshape; defer.
- **F2 (caches → `CacheBase`):** changes eviction policy (wholesale-clear → LRU) and weight model;
  transparent but subtly behavior-affecting — needs its own validation.
- **F3 (`BackgroundSchedulePool` for GC/keeper threads):** a threading-model change — exactly where
  timing/lifecycle bugs hide; not for an unattended round.
- **F6 (`Backend` vs `IObjectStorage`):** the review itself says "don't necessarily collapse"; the
  parallel seam is justified by test isolation. **Document the rationale only.**
- **F7 (`PartPathParser` constants):** layering-constrained (constants live in `Storages/MergeTree`);
  needs a shared low-level header — design separately. **Document + add a drift test only if cheap.**
- **F8 (CityHash vs SipHash for tree identity):** changing the hash is an **on-disk format change** —
  forbidden here. **Document the rationale only.**

## Testing strategy

- Per task: build (`ninja -C build`), run the touched `Cas*`/`Ca*` gtests, confirm only the known
  baseline red remains. Codec tasks add a byte-equality assertion as the oracle.
- After all tasks: full `Cas*:Ca*` gtest sweep green (baseline red only); then rebuild the
  `clickhouse` binary and run the **6h phase-3 chaos soak** (`utils/ca-soak`, `--duration 6h`) with a
  **30-minute** status+regression report cadence. Success = `dangling=0` throughout, `unreachable`
  drains toward ~0, replicas converge, `Code 499`/`<Fatal>`/`workload_failures`/`cannot reuse` all 0.

## Out of scope

- Any change to on-disk bytes, wire formats, GC algorithm, read/write semantics, or the public
  `IMetadataStorage`/`IObjectStorage`/`IContentAddressedExchange` contracts.
- The deferred items above.

# CAS Disk Lifecycle rev.7 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the rev.7 CAS disk lifecycle — transient-not-live throws to everyone, proven-erased `Vanished` answers the truth, `FORGET`/`GC STOP/START` verbs, identity-gated recovery — and roll back the landed Dormant/UNMOUNT lifecycle, with an immediately-landable CI-unblock test fix as Phase A.

**Architecture:** Everything lives inside the CAS subsystem (zero generic-code changes): a typed erasure-evidence probe below `Backend`, a fence-generation check on every durable-effect path, a four-condition pool lifecycle (`live / transient / IdentityLost / Vanished{erased,replaced,forgotten}`) driven by an identity gate in `tryRemountOnce`, one central operation-class gate at every metadata/transaction entry replacing the Part-6 benign-absent branches, plus verb plumbing (`FORGET`, `GC STOP/START`) following the existing `FSCK` pattern. Tasks are ordered so the tree stays green at every commit: Phase A first (works with today's landed semantics), then additive machinery, then the teardown switch, then the rollback of the old lifecycle.

**Tech Stack:** C++ (CAS subsystem under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`; SYSTEM-verb wiring in `src/Parsers` + `src/Interpreters` + `src/Access`), GoogleTest (`src/Disks/tests/`), bash stateless tests.

**Spec (the single source of truth):** `docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md` (rev.7). Each task names the spec §§ it implements — the implementer MUST read those sections before coding. Companion (constraints C1–C15, requirements R1–R10): `docs/superpowers/specs/2026-07-22-cas-disk-lifecycle-problem-and-constraints.md`.

## Global Constraints

- Branch `cas-gc-rebuild`. Never rebase or amend; new commits only. Never commit to `master`. NO `git push` without fresh explicit per-instance authorization.
- Shared worktree: another session may interleave docs-only commits — verify your commit landed with `git log -1` after each commit; do not halt on foreign docs commits.
- Allman braces. Never use `sleep` in C++ to fix a race (a bounded wait for an EXTERNAL condition with an explicit comment is allowed; bash test polling loops are fine).
- Zero generic-code changes: nothing outside `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`, the SYSTEM-verb plumbing files (`src/Parsers/ASTSystemQuery.{h,cpp}`, `src/Parsers/ParserSystemQuery.cpp`, `src/Interpreters/InterpreterSystemQuery.cpp`, `src/Access/Common/AccessType.h`), `src/Storages/System/StorageSystemContentAddressedMounts.cpp`, and tests. If a task seems to need a generic edit, STOP and escalate.
- Gate throws use error code 668 (`INVALID_STATE`) with the exact [D5] message shapes from spec §1 ("Error messages tell the truth about the reason").
- Builds: run `ninja` from the build dir with NO `-j`, redirect output to `<build_dir>/build_<task>.log`, analyze the log with a subagent returning a concise summary. One ninja at a time. Primary build dir: `build_asan` (its `DEBUG_OR_SANITIZER_BUILD` + `abort_on_logical_error` catch `LOGICAL_ERROR`-class bugs).
- Tests: redirect output to `<build_dir>/test_<name>.log` (unique per test), analyze via subagent. The definitive CA gtest gate filter: `--gtest_filter='Cas*:CaLifecycle*:CaWiring*:ContentAddressed*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*'`.
- Stateless runs: `python3 -m ci.praktika run "Stateless tests (amd_binary, parallel)" --test <space-separated-names>` from the repo root (`--test` takes ONE space-separated flag; repeats collapse to last). Binary symlinked at `ci/tmp/clickhouse`.
- Temp files in `tmp/` under the repo root, not `/tmp`.
- Say "exception" not "crash" for logical errors; write "ASan" not "ASAN".
- Commit messages end with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01HKgdqVjZwkpWPxLyHzduPb` trailers.
- CA invariants that bind every task: never GET a condemned object to revive (revival = fresh re-upload only); GC must never throw on a 404 during fold (record + continue); no CA-specific fields in generic Replicated/Keeper code or formats.

---

## Phase A — CI-unblock test fix (Tasks 1–2)

Independently landable NOW; works with the currently-landed Dormant/UNMOUNT semantics. Root cause being fixed: the disk registry keys custom disks by NAME forever (`Context::getOrCreateDisk`), so a fixed disk name makes a test retry (same long-lived server) reuse the previous invocation's cached disk — for `05020` a Dormant husk whose `GC RUN` trips `throwNotMounted`. Spec §9 "Tests". `CLICKHOUSE_TEST_UNIQUE_NAME` = `${CLICKHOUSE_TEST_NAME}_${CLICKHOUSE_DATABASE}` (`tests/queries/shell_config.sh:19`) — unique per normal run but NOT under a runner-supplied fixed `--database`, hence the extra `$RANDOM` suffix everywhere below.

### Task 1: `05020_content_addressed_fsck.sh` — unique disk name + unique pool path + normalized reference

**Files:**
- Modify: `tests/queries/0_stateless/05020_content_addressed_fsck.sh` (lines 16–23 define the fixed name/path; line 49 prints the disk name)
- Modify: `tests/queries/0_stateless/05020_content_addressed_fsck.reference` (row 5 contains the literal name `05020_content_addressed_fsck`)

**Interfaces:**
- Consumes: `shell_config.sh` env (`CLICKHOUSE_TEST_UNIQUE_NAME`, `CLICKHOUSE_USER_FILES_UNIQUE`).
- Produces: the naming pattern Tasks 2 and 14 reuse: `DISK_NAME="ca_<short>_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"`, `POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_<short>_${RANDOM}"`.

- [ ] **Step 1: Replace the fixed identifiers**

Replace lines 16–23 with:

```bash
DISK_NAME="ca_fsck_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_fsck_${RANDOM}"
rm -rf "${POOL_DIR:?}"
mkdir -p "${POOL_DIR}"
DISK_CA="disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = content_addressed,
    server_root_id = '05020',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/')"
```

(`server_root_id` stays `'05020'` — it is the node identity inside the pool, not a registry key.)

- [ ] **Step 2: Normalize the disk name in printed output**

The FSCK summary (current line 49) prints the disk name column. Pipe it through sed:

```bash
${CLICKHOUSE_CLIENT} --format TSVWithNames --query "SYSTEM CONTENT ADDRESSED FSCK '${DISK_NAME}'" \
    | sed "s/${DISK_NAME}/<disk>/"
```

Update `.reference` row 5: replace `05020_content_addressed_fsck` with `<disk>`. Verify no OTHER reference row contains the old literal (`grep -n 05020_content_addressed 05020_content_addressed_fsck.reference` → only the row you just fixed).

- [ ] **Step 3: Add teardown cleanup**

After the final query in the script, append (UNMOUNT at current line 45 already joined every CAS thread for this disk under the landed semantics, so the `rm` is safe):

```bash
rm -rf "${POOL_DIR:?}"
```

- [ ] **Step 4: Run the test twice (retry emulation)**

```bash
python3 -m ci.praktika run "Stateless tests (amd_binary, parallel)" --test 05020_content_addressed_fsck > build_asan/test_05020_a.log 2>&1
python3 -m ci.praktika run "Stateless tests (amd_binary, parallel)" --test 05020_content_addressed_fsck > build_asan/test_05020_b.log 2>&1
```

If the praktika harness reuses a running server between invocations, the second run IS the retry scenario. If it tears the server down each time, additionally emulate the retry in one run: temporarily duplicate the script body (source-level, not committed) or run `ci/tmp/clickhouse` client by hand executing the script twice against the same server; the second execution previously failed with `throwNotMounted` on `GC RUN` and must now pass (fresh unique disk each time). Analyze both logs via a subagent; expected: OK / OK.

- [ ] **Step 5: Commit**

```bash
git add tests/queries/0_stateless/05020_content_addressed_fsck.sh tests/queries/0_stateless/05020_content_addressed_fsck.reference
git commit -m "ca: 05020 — unique per-invocation disk name and pool path

A retry in the same long-lived server reused the name-keyed cached Dormant
disk and failed GC RUN with throwNotMounted. Unique name + path + normalized
reference output make every invocation a fresh disk. CI-unblock (Phase A of
the rev.7 lifecycle plan)."
```

(Append the standard trailers. Verify with `git log -1`.)

### Task 2: `04290`/`04295` — unique disk names (same latent mine)

**Files:**
- Modify: `tests/queries/0_stateless/04290_content_addressed_no_leftovers.sh` (line 44: `DISK_NAME='04290_content_addressed'`; line 29: `POOL_DIR`)
- Modify: `tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.sh` (line 32: `DISK_NAME='04295_content_addressed_mut'`; line 20: `POOL_DIR`)
- Possibly modify their `.reference` files (only if the disk name leaks into printed output — check).

**Interfaces:**
- Consumes: the Task 1 naming pattern.

- [ ] **Step 1: Make the names unique**

In `04290`: replace `DISK_NAME='04290_content_addressed'` with `DISK_NAME="ca_04290_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"` and `POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}/04290_content_addressed_pool"` with `POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_04290_${RANDOM}"`. Same for `04295` (`ca_04295_...`, `..._04295_${RANDOM}`). The paths were already per-run-unique in the normal case; the `$RANDOM` protects the fixed-`--database` rerun case; the NAME is the real fix (the registry keys by name — a rerun's `CREATE TABLE` with the same name would get the cached disk with the OLD path, and `getOrCreateDisk` may reject the changed settings).

- [ ] **Step 2: Check for name leaks into output**

```bash
grep -n '04290_content_addressed\|04295_content_addressed_mut' tests/queries/0_stateless/04290_content_addressed_no_leftovers.reference tests/queries/0_stateless/04295_content_addressed_mutation_no_leftovers.reference
```

If any row prints the disk name, sed-normalize exactly as in Task 1 Step 2 (`sed "s/${DISK_NAME}/<disk>/"`) and update the reference.

- [ ] **Step 3: Run both tests**

```bash
python3 -m ci.praktika run "Stateless tests (amd_binary, parallel)" --test "04290_content_addressed_no_leftovers 04295_content_addressed_mutation_no_leftovers" > build_asan/test_0429x.log 2>&1
```

Subagent-analyze; expected OK/OK.

- [ ] **Step 4: Commit** (message: `ca: 04290/04295 — unique per-invocation disk names` + why + trailers; verify `git log -1`.)

---

## Phase B — rev.7 machinery (Tasks 3–13), teardown switch (14), rollback (15), acceptance (16–17)

Ordering keeps every commit green: Tasks 3–13 are additive (the landed `MountState`/UNMOUNT keeps working — during the transition `poolAccess` checks BOTH the old `MountState == Mounted` AND the new lifecycle gate); Task 14 switches the tests to the new teardown; only then Task 15 removes the old machinery.

### Task 3: Typed erasure evidence — `SentinelProbe` below `Backend` (spec §2 [B5][D2-adjacent])

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasSentinelProbe.h` and `.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp` (the S3/native path currently flattens `NO_SUCH_KEY`/`NO_SUCH_BUCKET`/`RESOURCE_NOT_FOUND` into one `nullopt` via `tryGetObjectMetadata` ~`:599`; the Local path maps missing-parent to `nullopt`)
- Test: `src/Disks/tests/gtest_cas_sentinel_probe.cpp` (new; use the Emulated backend the existing `Cas*` gtests use — grep `CountingBackendShape` tests for the harness pattern)

**Interfaces (Produces — Tasks 5, 6, 7 consume these exactly):**

```cpp
namespace DB::ContentAddressed
{
enum class ProbeOutcome : uint8_t { Present, KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate };

struct SentinelProbeResult
{
    ProbeOutcome outcome;
    std::optional<Bytes> body;   /// set only when outcome == Present
};

/// Authoritative, cache-bypassing probe of one key. NEVER conflates transport errors with absence:
/// timeouts / 5xx / connection errors => Indeterminate; permission errors => AccessDenied;
/// missing container/bucket/prefix-parent => ContainerAbsent; a clean authoritative miss => KeyAbsent.
SentinelProbeResult probeSentinel(Backend & backend, const String & key);

/// Container proof: ListObjectsV2(max-keys=1, prefix=pool_root) — NOT bucket HEAD.
/// Returns Present when the LIST succeeded and found >=1 object, KeyAbsent when it succeeded and
/// found ZERO objects (the pool-wide emptiness observation), AccessDenied / ContainerAbsent /
/// Indeterminate otherwise.
SentinelProbeResult probePrefixEmptiness(Backend & backend, const String & pool_root_prefix);
}
```

- [ ] **Step 1: Write failing gtests** — Emulated backend: (a) present key → `Present` with body; (b) deleted key, container alive → `KeyAbsent`; (c) whole container/prefix-parent removed → `ContainerAbsent` (Local semantics: an explicit stat of the configured container distinguishes deleted-prefix from missing-key — assert the distinction); (d) a backend forced to throw a transport error → `Indeterminate` (never `KeyAbsent`); (e) `probePrefixEmptiness` on an empty prefix → `KeyAbsent`, on a prefix with one object → `Present`.
- [ ] **Step 2: Run** the new filter `--gtest_filter='CasSentinelProbe*'` → all FAIL (missing symbols).
- [ ] **Step 3: Implement.** For the S3/native path the raw error must be preserved BEFORE the flattening at `src/IO/S3/getObjectInfo.cpp:90/:135` discards it — do NOT edit `getObjectInfo.cpp` (generic!); instead call the object-storage API that surfaces the error (e.g. perform the HEAD via the backend's existing raw-request path and classify the caught exception by S3 error code: `NO_SUCH_KEY` → `KeyAbsent`, `NO_SUCH_BUCKET` → `ContainerAbsent`, HTTP 403 → `AccessDenied`, everything else → `Indeterminate`). Local: `stat` the configured container dir first (`ContainerAbsent` if gone), then the key. Emulated: exact map lookup under its mutex.
- [ ] **Step 4: Run** the new tests → PASS; run the full CA gtest gate filter → no regressions.
- [ ] **Step 5: Commit** (`ca: typed sentinel probe — KeyAbsent vs ContainerAbsent vs AccessDenied vs Indeterminate`).

### Task 4: Fence-generation token + outstanding-durable-request counter (spec §1 "Gate lifetime [C2]", §2 [D1])

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.{h,cpp}` (owns the fence: `mayMutate`, `tripMountLost` `~:68`; add the generation + counter here)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.cpp` (`casPutObject` `~:21` and its delete sibling — today they write with NO `mayMutate` check)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (staging-buffer finalize `~:824`; plain-object commit sites `~:748`, rename copy/remove `~:1312`)
- Test: `src/Disks/tests/gtest_cas_fence_generation.cpp` (new)

**Interfaces (Produces):**

```cpp
/// CasMountRuntime:
uint64_t fenceGeneration() const;          /// bumped on every trip AND every re-arm
class DurableRequestGuard                   /// RAII: increments outstanding_durable_requests in ctor,
{ ... };                                    /// decrements in dtor; constructed at ADMISSION of any
                                            /// durable-effect op, alive until the request resolves
uint64_t outstandingDurableRequests() const;
/// Every durable CAS/PUT/DELETE on the plain-object surface and every staging finalize:
///   capture gen = fenceGeneration() at admission; immediately before the durable backend call,
///   if (!mayMutate() || fenceGeneration() != gen) -> throw typed transient error (668). The check
///   must also run before every conditional-retry iteration, not just the first attempt.
```

- [ ] **Step 1: Failing gtests** — (a) `casPutObject` with the fence tripped between admission and the durable call → typed 668 exception, NO object written (assert via Emulated backend listing); (b) staging finalize racing a trip → same; (c) `DurableRequestGuard` counting: two guards alive → `outstandingDurableRequests()==2`, destruction → 0; (d) happy path unchanged.
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** (generation = atomic incremented in `tripMountLost` and in `armMountFence`; guard = atomic counter; thread the check into the three files' durable sites — enumerate them by grepping `casPutObject|casDeleteObject|finalize` in the two files and cover EVERY durable site, this is [C2]'s whole point).
- [ ] **Step 4: Run new + full CA gate → PASS.**
- [ ] **Step 5: Commit** (`ca: fence-generation check on every durable-effect path + outstanding-durable counter`).

### Task 5: Pool lifecycle condition + identity gate in `tryRemountOnce` (spec §1 states, §2 verdict rules, [C1][D3][B6])

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.{h,cpp}` (`tryRemountOnce` `~:640-725` — add step 0 BEFORE `claimOwnerOrThrow`; the remount loop cadence `~:632`; `PoolMeta` in-memory identity from `open`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.{h,cpp}` (host the lifecycle atomic + observer mode)
- Test: `src/Disks/tests/gtest_cas_lifecycle_condition.cpp` (new)

**Interfaces (Produces — Tasks 6, 8, 9, 10, 12 consume):**

```cpp
enum class PoolLifecycle : uint8_t { Live, TransientNotLive, IdentityLost, VanishedErased, VanishedReplaced, VanishedForgotten };
PoolLifecycle lifecycle() const;                       /// atomic read
bool isVanished() const;                               /// any of the three Vanished values
/// One-way setters, serialized under remount_mutex + the terminal-intent flag:
void enterIdentityLost();                              /// from TransientNotLive only
void enterVanished(PoolLifecycle which, String reason); /// one WARN + ProfileEvent, threads exit per spec §3
```

Identity comparison (spec [B6]): **`pool_id` + `blob_header_len` ONLY**; format gate = the decode itself succeeding; `algos_used`/`min_reader_generation` are refreshed, never compared.

- [ ] **Step 1: Failing gtests** — with an Emulated pool opened, then manipulated behind the pool's back:
  - (a) `_pool_meta` + owner deleted, other objects REMAIN → after the gate runs: `IdentityLost`, `enterVanished` NOT called, all access throws (assert on `store()`), remount loop demoted to observer (no further `claimOwnerOrThrow` calls — count them via the Emulated backend's op counter);
  - (b) `_pool_meta` present but `pool_id` differs → `VanishedReplaced` immediately;
  - (c) `_pool_meta` present, identity matches, `algos_used` DIFFERS → NOT treated as replacement; existing recovery proceeds (this is [B6] — the mutable-fields trap);
  - (d) from `IdentityLost`, restore matching `_pool_meta`+owner → state STAYS `IdentityLost` (no auto-revival, [D3]);
  - (e) transport errors from the probe → stays `TransientNotLive`, retries continue.
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** step 0 of `tryRemountOnce`: `probeSentinel(_pool_meta)` → dispatch per the spec §2 verdict table; `Present`+match rule applies ONLY in `TransientNotLive` (never revives `IdentityLost`); on `IdentityLost` the remount thread demotes: same loop, but it skips claim/epoch/mutation and only runs the probe at renewal-period cadence with backoff.
- [ ] **Step 4: Run new + full CA gate → PASS.** Also run the existing self-remount/fence gtests (they exercise `tryRemountOnce` — the happy fence-out path must be byte-identical in behavior).
- [ ] **Step 5: Commit** (`ca: pool lifecycle condition + identity gate as step 0 of tryRemountOnce`).

### Task 6: `Vanished(erased)` proof — strong-LIST capability + full quiescence preconditions (spec §2 proof [C3][D1])

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h` (+ the concrete backends): add `virtual bool supportsErasureProof() const { return false; }` — override `true` ONLY for the S3-native backend when NOT a gateway (documented strong LIST: AWS S3, GCS); Local/Emulated stay `false`; RustFS stays `false` (pending evidence — leave a comment saying exactly that).
- Modify: `CasPool.cpp` observer loop (from Task 5) — the proof runs there.
- Test: extend `src/Disks/tests/gtest_cas_lifecycle_condition.cpp`.

**Proof preconditions (ALL, from spec §2 item 2 — copy into a comment at the implementation site):** backend `supportsErasureProof()`; ref lanes settled; `outstandingDurableRequests() == 0` and stays 0 across both samples; GC scheduler thread + any in-flight round fully exited; elapsed-since-fence-trip ≥ max(materialization grace, backend total request-timeout budget); then ≥2 `probePrefixEmptiness == KeyAbsent` samples spaced ≥ the mount renewal period; any error or `Present` resets the counter.

- [ ] **Step 1: Failing gtests** — Emulated with a test-only `supportsErasureProof()==true` override: (a) full erase → after two spaced empty samples: `VanishedErased`; (b) one sample empty, second `Present` (an object re-appears) → counter resets, stays `IdentityLost`; (c) `outstandingDurableRequests() > 0` (hold a `DurableRequestGuard`) → proof never starts; (d) Emulated WITHOUT the override → proof never runs, terminal natural state is `IdentityLost`.
- [ ] **Step 2: Run → FAIL.  Step 3: Implement.  Step 4: New + full CA gate → PASS.  Step 5: Commit** (`ca: Vanished(erased) proof — strong-LIST capability + full durable-lane quiescence`).

### Task 7: Startup bootstrap ordering (spec §2 "Startup [C4][D2]")

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp` (`open`: the `_probe/<random>/` capability battery currently runs at `~:230` BEFORE `PoolMeta::createOrValidate` at `~:263`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPoolMeta.cpp` (`createOrValidate` `~:105/:117`)
- Test: `src/Disks/tests/gtest_cas_bootstrap_ordering.cpp` (new)

**Contract:** bootstrap sequence = (0) zero-write residual check FIRST — a prefix LIST that ignores structurally-valid `_probe/` debris when deciding whether CAS state exists; (1) only then the mutating capability battery; (2) then `createOrValidate`. Missing `_pool_meta` over a non-empty (non-`_probe`) prefix ⇒ typed startup failure, zero non-probe writes. `pool_prefix` is exclusively CAS-owned — bootstrapping over unrelated foreign objects is rejected.

- [ ] **Step 1: Failing gtests** — (a) empty prefix → open succeeds, meta created (normal first mount unchanged); (b) prefix containing `ca/refs/...` residue but NO `_pool_meta` → open fails typed, and the Emulated op-log shows ZERO writes except (none — the battery must not have run); (c) prefix containing ONLY stale `_probe/xxx/` debris → treated as empty → open succeeds; (d) existing healthy pool → open unchanged.
- [ ] **Step 2–5:** FAIL → implement → PASS (+ full CA gate; the existing open/startup gtests must stay green) → commit (`ca: zero-write residual check before the probe battery; exclusive pool prefix`).

### Task 8: The central operation-class gate + `Vanished` truth semantics + [D5] messages (spec §1 table + "Error messages tell the truth")

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.{h,cpp}` (`poolAccess` `~:1548` / `throwNotMounted` `~:805`; the short-circuit offenders: `liveTreeDirHasChildren` hardcoded-true `~:883`, `isDirectoryEmpty` part-dir short-circuit `~:1385`, `tryGetInManifestBytes` catch-all `~:1488`; the Part-6 `isMounted()` sites `~:557-564`, `:1114`, `:1370`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` (no-op `createDirectory*` `~:924`, empty-txn commit `~:408`, `setLastModified`/`setReadOnly` `~:1109`)
- Test: `src/Disks/tests/gtest_cas_operation_gate.cpp` (new)

**Interfaces (Produces):**

```cpp
enum class CasOpClass : uint8_t { Factory, Probe, ContentRead, Write, Remove, Admin };
/// ONE helper consulted at EVERY public metadata/transaction entry:
///   Factory: never gated (createTransaction is I/O-free — verified; getType/getPath/capabilities/
///            gcHealth/lifecycle snapshot).
///   Live: all classes pass.  Constructing/ShutDown (storage lifecycle): null-pool fail-loud (existing).
///   TransientNotLive / IdentityLost: Probe/ContentRead/Write/Remove/Admin ALL throw 668
///     ("mount lease not held — backing may be temporarily unreachable").
///   Vanished: Probe -> truthful absent/empty; ContentRead/Write/Admin -> typed 668 with the [D5]
///     per-reason message (erased: "data root erased (verified: pool prefix empty)…"; replaced:
///     "data root replaced by a foreign pool (pool_id mismatch)"; forgotten: "disk decommissioned by
///     SYSTEM CONTENT ADDRESSED FORGET at <timestamp> — erasure was NOT verified…");
///     Remove -> no-op success.
void checkOpAdmitted(CasOpClass op) const;   /// on ContentAddressedMetadataStorage
```

- [ ] **Step 1: Build the method→class inventory.** List EVERY public method of `ContentAddressedMetadataStorage` and `ContentAddressedTransaction` with its class in a table comment at the top of the `.cpp` (the spec demands the complete inventory — grep the headers; do not skip the offenders listed above). This inventory is the review artifact for this task.
- [ ] **Step 2: Failing gtests** — for each class × state cell of the spec §1 table (use the Task 5 setters to force states): probes on Vanished answer absent/empty; removes on Vanished no-op-succeed (and an empty-directory `removeRecursive` completes); content read on Vanished throws with the exact "data root erased (verified: pool prefix empty)" substring; the same read on `VanishedForgotten` throws with "erasure was NOT verified"; every class except Factory throws on `TransientNotLive`; `createTransaction` constructs fine on Vanished; `tryGetInManifestBytes` no longer converts the typed 668 into `FILE_DOESNT_EXIST` (narrowed catch — assert the typed exception propagates).
- [ ] **Step 3: Run → FAIL.  Step 4: Implement** — transitional rule: `poolAccess` requires (`MountState == Mounted` — the OLD landed check, kept until Task 15) AND (`checkOpAdmitted` for the op class). The Part-6 `isMounted()` benign branches are REPLACED by `checkOpAdmitted(Probe)` (their truth-answer path now fires only on `isVanished()`).
- [ ] **Step 5: Run new + full CA gate + the four landed lifecycle gtests (must still pass — UNMOUNT semantics unchanged until Task 15) → PASS.  Step 6: Commit** (`ca: central operation-class gate; Vanished truth semantics; typed per-reason messages`).

### Task 9: The empty-proof rule (spec §1 "empty-proof rule [B3]")

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (the enumeration routes for `TableDir` / `DetachedContainer` `DirShape`s — routing exists at `~:1303`)
- Test: extend `src/Disks/tests/gtest_cas_operation_gate.cpp`

**Contract:** pre-terminal (Live/Transient) and on read-only pools ONLY (never in Vanished — it answers truth-empty directly): an enumeration about to answer EMPTY at a `TableDir`/`DetachedContainer` root first confirms `_pool_meta` with `probeSentinel` (authoritative, UNCACHED — a cached positive never authorizes an empty answer); `KeyAbsent`/`ContainerAbsent` ⇒ typed 668 throw instead of the empty answer; `Indeterminate`/`AccessDenied` ⇒ typed transient throw.

- [ ] **Step 1: Failing gtests** — (a) read-only pool over an erased backing: `iterateDirectory` on a table root throws typed (NOT empty) — this is the RO-ATTACH silent-empty killer; (b) live pool, genuinely empty table dir, `_pool_meta` present: answers empty and the Emulated op-log shows exactly one uncached sentinel GET; (c) Vanished pool: answers empty WITHOUT any probe (terminal truth path).
- [ ] **Step 2–5:** FAIL → implement → PASS (+ full gate) → commit (`ca: empty-proof rule — empty data-root answers require an authoritative pool-meta positive`).

### Task 10: `SYSTEM CONTENT ADDRESSED FORGET '<disk>'` (spec §5, §3 serialization)

**Files:**
- Modify: `src/Access/Common/AccessType.h` (add `SYSTEM_CONTENT_ADDRESSED_FORGET` next to the existing `SYSTEM_CONTENT_ADDRESSED_FSCK` row — copy that row's pattern exactly)
- Modify: `src/Parsers/ASTSystemQuery.{h,cpp}`, `src/Parsers/ParserSystemQuery.cpp` (follow the FSCK verb's parse/format pattern — grep `CONTENT ADDRESSED FSCK`)
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp` (the CONTENT ADDRESSED family dispatch `~:2456`)
- Modify: `CasMountRuntime.{h,cpp}` / `CasPool.{h,cpp}` (the protocol body)
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference` (+1 row: `SYSTEM CONTENT ADDRESSED FORGET ['SYSTEM CONTENT ADDRESSED FORGET'] GLOBAL SYSTEM` — match the FSCK row's format)
- Test: `src/Disks/tests/gtest_cas_forget.cpp` (new) + the stateless coverage arrives in Task 14

**Protocol (spec §5 — implement in this exact order):** (1) publish terminal-intent (an atomic the keeper callback checks before scheduling a remount and the remount loop checks at every step boundary); (2) trip the fence (`tripMountLost` — allowed on a live disk); (3) stop keeper renewal WITHOUT the clean terminal release unless ref lanes provably drained (the `~Pool` rule `CasPool.cpp:565`) — otherwise leave the lease to expire by observation; (4) stop the GC scheduler, clear `i_am_leader`; (5) join keeper/remount/GC threads OUTSIDE `remount_mutex`; (6) `enterVanished(VanishedForgotten, ...)` with the timestamp for the [D5] message.

- [ ] **Step 1: Failing gtests** — (a) FORGET on a live Emulated pool: fence tripped, all three thread kinds joined (assert via the runtime's thread handles), state `VanishedForgotten`, NO terminal clean-release write in the Emulated op-log (undrained lanes case); (b) FORGET racing an in-flight `tryRemountOnce` completes without deadlock (start a remount against a slow/blocked backend, issue FORGET, assert bounded completion — bounded wait on a condition variable with a generous timeout and an explicit comment, no sleeps); (c) double FORGET is idempotent.
- [ ] **Step 2–5:** FAIL → implement (verb plumbing + protocol) → PASS (+ full gate; `01271` locally: `python3 -m ci.praktika run "Stateless tests (amd_binary, parallel)" --test 01271_show_privileges > build_asan/test_01271.log 2>&1`) → commit (`ca: SYSTEM CONTENT ADDRESSED FORGET — operator force-Vanish with fence-first protocol`).

### Task 11: `SYSTEM CONTENT ADDRESSED GC STOP/START '<disk>'` (spec §6)

**Files:**
- Modify: the same four verb-plumbing files as Task 10 (two more AccessTypes: `SYSTEM_CONTENT_ADDRESSED_GC_STOP`, `SYSTEM_CONTENT_ADDRESSED_GC_START`; two more `01271` rows)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcScheduler.{h,cpp}` (`stop` `~:74` currently leaves `i_am_leader` true; `start` must be re-enterable on the same instance with `gc_id` preserved)
- Test: `src/Disks/tests/gtest_cas_gc_stop_start.cpp` (new)

- [ ] **Step 1: Failing gtests** — (a) `stop()` → no further rounds fire (observe via the round counter), `i_am_leader` reads false; (b) `start()` after `stop()` → rounds resume, same `gc_id`; (c) concurrent `stop`/`start` calls serialize (no torn state — drive from two threads, assert final state matches the last call); (d) disk reads/writes unaffected while GC stopped.
- [ ] **Step 2–5:** FAIL → implement → PASS → commit (`ca: GC STOP/START verbs — restartable scheduler, leader flag cleared on stop`).

### Task 12: Introspection — non-gated lifecycle snapshot (spec §7, [C5]-visibility)

**Files:**
- Modify: `src/Storages/System/StorageSystemContentAddressedMounts.cpp` (`~:95-101` currently calls `ca->store()`, catches, and SKIPS the disk — a not-live disk vanishes from the table)
- Modify: `ContentAddressedMetadataStorage.{h,cpp}` (expose `struct CasLifecycleSnapshot { String lifecycle; String reason; time_t since; }` — Factory class, no backing I/O, readable in every state)
- Test: stateless assertions arrive in Task 14 (the teardown verifies `vanished(forgotten)` through this table); add a gtest asserting the snapshot is readable on a Vanished pool.

- [ ] **Steps:** failing gtest → implement (the system table synthesizes a row from the snapshot when `store()` is refused: columns it cannot know read NULL/defaults; new/changed columns: `lifecycle` (`live`/`not_live`/`identity_lost`/`vanished`), `lifecycle_reason` (`erased`/`replaced`/`forgotten`/empty), `lifecycle_since`) → PASS → commit (`ca: content_addressed_mounts shows not-live and vanished disks via a non-gated snapshot`).

### Task 13: FSCK — revalidation + `meta_without_body` advisory (spec §7)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.{h,cpp}` (missing-committed-manifest `~:290` — add the exact-ref-re-resolve + exact-object-HEAD revalidation, same pattern as the sound blob-`Dangling` recheck at `~:370`; `meta_without_body` `~:563` — reclassify advisory: exclude from `FsckReport::clean` `CasFsck.h:113`)
- Modify: the FSCK verb path in `src/Interpreters/InterpreterSystemQuery.cpp` / the CAS-side dispatch — **remove the mounted-refusal** ("run SYSTEM CONTENT ADDRESSED UNMOUNT first"): FSCK now ALSO runs on a Running disk via `store()`. The dormant observe-only path stays working (both modes accepted) until Task 15 removes it — Task 14's teardown switch needs running-FSCK, while the not-yet-switched tests still exercise the dormant path in between.
- Modify: `src/Disks/tests/gtest_cas_fsck.cpp` (`~:217` asserts `meta_without_body` is hard — invert: it must NOT fail `clean`; the refusal gtest, if any, inverts to "FSCK on a mounted disk succeeds")
- Test: extend `gtest_cas_fsck.cpp`

- [ ] **Step 1: Failing gtests** — (a) a manifest missing in a STALE recovered view but present in a fresh re-resolve → NOT reported (revalidation catches the race); (b) genuinely missing manifest → still hard; (c) `meta_without_body` present → `clean` stays true, the count still reported in the full report; (d) FSCK on a mounted (Running) disk succeeds with the one-row summary.
- [ ] **Step 2–5:** FAIL → implement → PASS → commit (`ca: fsck — runs on a running disk; manifest revalidation; meta_without_body advisory (no finite GC horizon exists)`).

### Task 14: Switch the test teardown to `DROP SYNC → FORGET → verify → rm -rf` (spec §9 tests; fail-closed)

**Files:**
- Modify: `tests/queries/0_stateless/05020_content_addressed_fsck.sh` + `.reference` (drop the UNMOUNT-refusal sections — but NOT yet the UNMOUNT verb itself; FSCK now runs on the RUNNING disk: reorder so FSCK runs before DROP, remove the `fsck_refused_while_mounted` block)
- Modify: `tests/queries/0_stateless/04290_content_addressed_no_leftovers.sh` (`:103-121`) + `04295_...sh` (`:104-121`) + references

**The fail-closed teardown block (verbatim, parameterized by `$DISK_NAME` / `$POOL_DIR` / table name):**

```bash
${CLICKHOUSE_CLIENT} --query "DROP TABLE <table> SYNC"
${CLICKHOUSE_CLIENT} --query "SYSTEM CONTENT ADDRESSED FORGET '${DISK_NAME}'" || {
    echo "FORGET failed — leaving pool dir in place (fail-closed)"; exit 1; }
LIFECYCLE=$(${CLICKHOUSE_CLIENT} --query "
    SELECT lifecycle || '(' || lifecycle_reason || ')' FROM system.content_addressed_mounts
    WHERE disk = '${DISK_NAME}'")
[ "${LIFECYCLE}" = "vanished(forgotten)" ] || {
    echo "unexpected lifecycle after FORGET: ${LIFECYCLE}"; exit 1; }
rm -rf "${POOL_DIR:?}"   # safe: FORGET stopped and joined every CAS thread for this disk
```

- [ ] **Step 1:** Apply to all three tests; update references (the removed UNMOUNT/refusal lines disappear; FSCK's row unchanged apart from ordering).
- [ ] **Step 2:** Run all three via praktika (one `--test` flag, space-separated) twice; subagent-analyze → OK×2 each.
- [ ] **Step 3: Commit** (`ca: tests — fail-closed DROP SYNC -> FORGET -> verify -> rm teardown`).

### Task 15: Rollback of the landed Task 4–8 lifecycle (spec §9 rollback list — ALL seven items)

**Files (discover the full set by grep, this list is the starting anchor):**
- Modify: `ContentAddressedMetadataStorage.{h,cpp}` — remove the `MountState` enum + every branch (grep `MountState|Mounted|Unmounting|Dormant` within the CAS dir); remove `unmountSynchronously`, `mountExplicitly`, the drain loop; drop the transitional `MountState==Mounted` conjunct from `poolAccess` (Task 8) — the lifecycle gate + storage `Constructing/Started/ShutDown` null-pool check now stand alone.
- Modify: the four verb-plumbing files — remove `SYSTEM CONTENT ADDRESSED UNMOUNT`/`MOUNT` (AccessTypes, AST fields, parser/formatter cases, interpreter dispatch).
- Modify: `CasFsck` call path — remove the now-unused dormant observe-only pool path (the mounted-refusal itself was already removed in Task 13; after this task FSCK is running-disk-only).
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference` — remove the UNMOUNT/MOUNT rows (FORGET/GC rows stay from Tasks 10–11).
- Modify: `src/Disks/tests/gtest_ca_transaction.cpp` (`~:725` ff.) — delete the four Dormant/UNMOUNT lifecycle gtests (their replacements exist since Tasks 5–10).
- KEEP (do not touch): Part 1 abort-hardening in `CasServerRoot.cpp`, `poolAccess` snapshot, atomic `startup()`, `ca-fsck` applet rename, the `GC RUN` `pending_*` columns, the GC-round entry-point gating (now via the lifecycle protocol — verify `runGarbageCollectionRoundNow`/`runOneGcRoundForTest` at `ContentAddressedMetadataStorage.cpp:~407` are gated by `checkOpAdmitted(Admin)`).

- [ ] **Step 1:** `grep -rn 'MountState\|unmountSynchronously\|mountExplicitly\|UNMOUNT\|Dormant' src/ tests/queries/0_stateless/*.sh` — build the complete removal list; anything referencing these outside the expected set → STOP and escalate (a hidden coupling).
- [ ] **Step 2:** Remove; build (`ninja` → log → subagent).
- [ ] **Step 3:** Full CA gtest gate + all three stateless CA tests + `01271` → green.
- [ ] **Step 4:** Whole-tree check: `git status` the WHOLE tree, confirm committed HEAD builds (the reorg-sweep rule).
- [ ] **Step 5: Commit** (`ca: roll back the Dormant/UNMOUNT reuse lifecycle (spec rev.7 §9)`).

### Task 16: Acceptance-matrix tests not yet covered (spec §9 matrix)

**Files:**
- Test: extend `src/Disks/tests/gtest_cas_lifecycle_condition.cpp` / create `gtest_cas_acceptance.cpp` as fits.

Add the matrix rows not already produced by Tasks 5–14 (audit first — do not duplicate):
- [ ] **Transient auto-recovery:** lease key removed then restored on Emulated → access throws in the gap (668), succeeds after the keeper's next successful renewal; no restart, no operator action.
- [ ] **DROP-drain:** a removal issued during the gap re-queues (simulate at the metadata layer: `Remove`-class op throws during transient, succeeds after recovery).
- [ ] **No-silent-empty:** a content read on `VanishedErased` throws the typed message (assert the "verified: pool prefix empty" substring); an enumeration on `TransientNotLive` throws (never empty).
- [ ] Run full gate → PASS → commit (`ca: rev.7 acceptance-matrix gtests`).

### Task 17: Final gates + docs alignment

- [ ] **Step 1:** Full CA gtest gate on `build_asan` (log + subagent).
- [ ] **Step 2:** Stateless CA family (`05020 04290 04295 01271`) via praktika, twice (log + subagent).
- [ ] **Step 3:** Update `docs/superpowers/cas/BACKLOG.md`: mark the disk-lifecycle-leak / 05020 items done; add follow-ups discovered during implementation (each with a pointer to this plan).
- [ ] **Step 4:** Re-read spec rev.7 §§1–9 against the shipped code — any place the implementation was forced to deviate gets a spec amendment commit (the spec is the source of truth; do not leave silent drift).
- [ ] **Step 5: Commit** (`ca: rev.7 implementation complete — gates green, backlog updated`).

---

## Self-review notes (spec coverage)

- §1 states/gate/classes/[D5] → Tasks 5, 8; empty-proof [B3] → Task 9; fence lifetime [C2] → Task 4.
- §2 probe/verdicts/[B6]/[C3]/[D1] → Tasks 3, 5, 6; startup [C4][D2] → Task 7; [A1] contract → doc-only (no code).
- §3 serialization/terminal-intent/joins → Tasks 5, 10; bounded-abort GC → Task 10 (b-test).
- §4 blast radius → behavior emerges from Tasks 8–9; no per-caller code (zero-generic).
- §5 FORGET → Task 10. §6 GC STOP/START → Task 11. §7 introspection/FSCK → Tasks 12, 13.
- §8 known limits → no code by design. §9 rollback/tests → Tasks 14, 15, 16; Phase A → Tasks 1–2.
- Deliberately NOT planned (spec "Not doing"): runtime STOP/START, UNMOUNT ALL, inline-disk MOUNT, registry eviction, per-caller sweep guards.

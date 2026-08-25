---
description: 'Implementation plan for bounded ambiguity-aware CAS mount renewal retries, stable runtime worker ownership, recovery observability, and snapshot refusal backoff.'
sidebar_label: 'CAS mount renewal retries plan'
sidebar_position: 5
slug: /superpowers/plans/cas-mount-renewal-retries
title: 'CAS Mount Renewal Retries Implementation Plan'
doc_type: 'plan'
---

# CAS Mount Renewal Retries Implementation Plan {#cas-mount-renewal-retries-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development`
> (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Use
> `superpowers:test-driven-development` for every production-code task and
> `superpowers:verification-before-completion` before claiming a gate passed. Steps use checkbox
> (`- [ ]`) syntax for tracking.

**Goal:** Keep a writable CAS mount live across transient object-store failures that fit inside its
last confirmed lease, resolve ambiguous renewals without weakening single-writer safety, and make
renewal/remount recovery observable without log storms.

**Architecture:** `MountLeaseKeeper` becomes a synchronous durable-slot state machine and
`CasMountRuntime` becomes the stable owner of separate long-lived renewal and remount workers. One
immutable renewal body, identified by `write_attempt_id`, is retried through an extended
`CasRequestController::putOverwriteControlled` under an absolute BOOTTIME deadline. Terminal results
cross one explicit runtime boundary; they can fence and request one recovery generation only after no
keeper call is in flight.

**Tech Stack:** TLA+/TLC, C++23, ClickHouse CAS backend and pool runtime, GoogleTest,
pytest/Praktika integration tests, Ninja, RustFS fault proxy, and `ca-soak`.

**Spec:** `docs/superpowers/specs/2026-08-23-cas-mount-renewal-retry-design.md` revision 4 or later.

## Hard gates and global constraints {#hard-gates-and-global-constraints}

- Tasks 1 and 2 are a hard formal-methods gate. Do not edit production C++, production headers,
  C++ tests, integration tests, or soak code until both tasks pass exactly and their result documents
  are committed.
- A sabotage passes only when TLC reports its exact named invariant violation. A different violation,
  timeout, deadlock, parse error, state-space error, or arbitrary nonzero exit is a failed gate.
- A witness passes only when its exact negated reachability property is violated while every safety
  invariant remains enabled and green. An unreachable witness is vacuous and fails the gate.
- If the focused model exposes a protocol defect, stop production work. Revise the specification,
  obtain design review, and then update this plan.
- Read the specification and `docs/superpowers/cas/AGENTS.md` before every implementation task. The
  specification wins if this plan diverges.
- Preserve the durable owner/epoch allocation, mount key, reclaim qualification, ref-runtime
  quiescence, conditional dialect, authentication, and every non-CAS object-storage path.
- Add no user-facing retry, timeout, logging, or snapshot-backoff setting. Reuse `CasRequestBudget`
  and the existing per-table snapshot backoff values.
- CAS is pre-release. Generation-9 pools are recreate-only; do not add a dual decoder, migration,
  compatibility alias, or fallback body without `write_attempt_id`.
- One logical renewal has one immutable `(key, bytes, expected token, write_attempt_id)` tuple across
  every physical `PUT`. A resolving `GET`, retry, response timestamp, and local optimism never extend
  authority.
- `max_attempts` gates only a new physical `PUT`. It never suppresses the exact resolving `GET` or
  acceptance checks for the final sent attempt.
- Every terminal keeper result carries a non-null typed exception. Owner cancellation before any
  request is the only `NotAttempted` result; cancellation after a sent request is terminal and forbids
  farewell.
- One stable `CasMountRuntime` owns both workers and all lifecycle effects. A replaceable keeper owns
  no thread, condition variable, owner callback, or runtime pointer.
- Follow Allman braces. Refer to functions without call parentheses in prose/comments. Say
  “exception” rather than “crash” for `LOGICAL_ERROR` behavior.
- Follow TDD. Each production task starts with a focused failing test. Rebuild `unit_tests_dbms`
  after adding the test; running the pre-change binary is not red evidence.
- An exact compile failure caused by a newly referenced interface is acceptable red evidence. If the
  test compiles, list it from the rebuilt binary, run it, require a nonzero test exit, and match the
  intended failed assertion.
- Before every filtered GoogleTest run, capture `--gtest_list_tests`. After the run, require a
  nonzero execution count:

```bash
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' <log>
```

- Build with `ninja` without `-j` or `nproc`. Redirect every build and test command to a unique log
  under the selected build directory and use a subagent to summarize the log, as required by the
  repository instructions.
- Use `apply_patch` for edits. Preserve unrelated worktree changes. Before each commit run
  `git diff --check`, inspect the staged diff, and stage only files named by that task. Never amend or
  rebase.
- Every intermediate commit must build and be behaviorally coherent. Do not leave a pure virtual
  interface without all implementations, a removed method with a later-task caller, or a test filter
  that runs zero tests.
- The object-store fault lane and `S39` chaos validation are release gates. An unavailable Docker
  environment may defer release readiness, but mock/unit evidence cannot replace them.

## Target interfaces and ownership {#target-interfaces-and-ownership}

`CasServerRootFormats.h` adds exact logical-attempt identity:

```cpp
struct MountLease
{
    UInt128 server_uuid{};
    uint64_t writer_epoch = 0;
    UInt128 write_attempt_id{};
    String hostname;
    uint64_t pid = 0;
    uint64_t started_at_ms = 0;
    uint64_t seq = 0;
    uint64_t expires_at_ms = 0;
    uint64_t min_active = 0;
    bool gc_fenced = false;
};
```

Every holder-originated body uses a fresh nonzero UUID-v4-derived `UInt128`; retry reuses it. GC
fencing copies it, while reclaim and successor bodies replace it.

`CasRequestControl.h` adds an operation context without changing the existing overload:

```cpp
enum class CasOverwriteDeadlineSource : uint8_t
{
    RequestBudget,
    ExternalLeaseSafety,
};

enum class CasOverwriteStopCause : uint8_t
{
    Continue,
    Cancelled,
    FenceOrLifecycleLost,
};

enum class CasOverwriteProgressKind : uint8_t
{
    PutStarted,
    BecameAmbiguous,
    ResolveStarted,
    RetryStarted,
    ResolvedByGet,
};

struct CasOverwriteProgress
{
    CasOverwriteProgressKind kind;
    uint32_t attempt_no;
};

struct CasOverwriteOperationContext
{
    uint64_t absolute_deadline_ms;
    CasOverwriteDeadlineSource deadline_source;
    std::function<CasOverwriteStopCause()> stop_cause;
    std::function<bool(uint64_t)> wait_before_retry;
    std::function<void(const CasOverwriteProgress &)> observe;
};

struct CasOverwriteDiagnostics
{
    uint32_t attempts_sent = 0;
    bool resolved_by_get = false;
    CasUnresolvedReason unresolved_reason = CasUnresolvedReason::NotUnresolved;
    CasOverwriteDeadlineSource deadline_source = CasOverwriteDeadlineSource::RequestBudget;
    CasOverwriteStopCause stop_cause = CasOverwriteStopCause::Continue;
};

struct CasOverwriteResult
{
    CasOverwriteOutcome outcome = CasOverwriteOutcome::Unresolved;
    Token token;
    CasOverwriteDiagnostics diagnostics;
};

CasOverwriteResult putOverwriteControlled(
    std::string_view key,
    std::string_view bytes,
    const Token & expected,
    const CasOverwriteOperationContext & context);
```

The existing `fence_ok` overload builds the legacy relative-deadline context and preserves existing
callers. The new overload applies stop/deadline checks before every backend request and acceptance,
but attempt count only before a new `PUT`.

`CasServerRoot.h` replaces `SingleWriterSlot` with the synchronous keeper protocol:

```cpp
enum class MountLeaseKeeperState : uint8_t { New, Active, RenewalTerminal, Released };
enum class MountRenewOutcome : uint8_t { Committed, NotAttempted, Terminal };

struct MountRenewResult
{
    MountRenewOutcome outcome;
    uint64_t attempt_start_boot_ms;
    CasOverwriteDiagnostics diagnostics;
    std::exception_ptr failure; // non-null exactly for Terminal
};

struct MountRenewOperationEnvironment
{
    std::function<uint64_t()> boot_ms;
    std::function<CasOverwriteStopCause()> stop_cause;
    std::function<bool(uint64_t)> wait_before_retry;
    std::function<void(const CasOverwriteProgress &)> observe;
};

uint64_t start();
MountRenewResult renew(
    const CasRequestBudget & budget,
    const MountRenewOperationEnvironment & environment);
void release();
```

`CasMountRuntime.h` owns one driver state and both workers:

```cpp
enum class RenewalDriverState : uint8_t
{
    Dormant,
    StartupCall,
    DirectCall,
    RemountCall,
    WorkerIdle,
    WorkerCall,
    ParkRequested,
    Parked,
    Stopping,
};

uint64_t renewKeeperForStartupOnce();
uint64_t renewKeeperForRemountOnce();
void renewWatermarkOnce();
void startBackgroundWorkers(std::chrono::milliseconds period);
void stopBackgroundWorkers();
```

The runtime uses a private RAII driver lease to perform the state transition under one mutex, drops
the mutex before keeper/backend work, and restores/reports `WorkerIdle`, `Parked`, `Dormant`, or
`Stopping` before consuming the result. `scheduleRemount` increments a requested-generation counter
and wakes the persistent remount worker; it never constructs a thread during an incident.

---

### Task 1: Prove split-phase mount renewal in focused TLA+ {#task-1-prove-split-phase-mount-renewal-in-focused-tla}

**Gate:** No production work may start until this task passes and is committed.

**Files:**

- Create: `docs/superpowers/models/CaMountRenewRetryCore.tla`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_safe.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_ignore_attempt_id.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_refresh_deadline_from_response.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_retry_with_new_body.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_accept_after_terminal.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_accept_successor.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_drop_pending_on_terminal.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_late_rearm.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_response_relative_cadence.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_send_after_deadline.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_sab_double_conditional_landing.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_direct_retry.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_read_adoption.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_exhaustion_fences.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_late_before_reclaim.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_late_after_successor.cfg`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_witness_catchup.cfg`
- Create: `docs/superpowers/models/run_mountrenewretry.sh`
- Create: `docs/superpowers/models/CaMountRenewRetryCore_RESULTS.md`

**Interfaces:**

- Consumes: the exact state and invariants in the specification's `{#tla-gate}` section.
- Produces: an asserted sabotage-first runner and a recorded formal gate that Tasks 3–9 may cite.

- [ ] **Step 1: Define finite identities, durable state, local state, and pending delivery**

Model a finite `AttemptIds` set, predecessor/successor tokens, holder identities, confirmed body and
deadline, local authority, one immutable logical request, retry count, cadence anchor, and outstanding
physical copies. Represent outstanding copies as a count plus the immutable logical tuple; do not
model different bodies inside that count.

- [ ] **Step 2: Encode split request actions and adversarial observations**

Add separate actions for send, land, response loss, exact resolve, retry wait, retry send, local
terminalization, late delivery, GC fence, same-pair twin, successor claim, foreign holder, time
advance, cancellation, and next-beat scheduling. Local terminalization must leave pending copies
deliverable. Conditional landing must consume the predecessor token, so at most one identical copy
can land honestly.

- [ ] **Step 3: Add the stable safety invariants**

Use these exact names in the model, configs, runner, and results:

```text
ExactAttemptOnly
ForeignOrSuccessorNeverAdopted
ConfirmedDeadlineNeverExtendedByResponse
NoRequestAfterSafeDeadline
TerminalNeverRearmsAuthority
OneLogicalBodyPerExpectedToken
AcknowledgedRenewalIsDurable
LateDeliveryCannotOverwriteSuccessor
PendingSurvivesLocalTerminal
OneIncarnationPerPredecessor
CadenceAnchoredAtAttemptStart
```

- [ ] **Step 4: Map every sabotage to one exact invariant**

| Config | Exact expected violation |
|---|---|
| `sab_ignore_attempt_id` | `ExactAttemptOnly` |
| `sab_refresh_deadline_from_response` | `ConfirmedDeadlineNeverExtendedByResponse` |
| `sab_retry_with_new_body` | `OneLogicalBodyPerExpectedToken` |
| `sab_accept_after_terminal` | `TerminalNeverRearmsAuthority` |
| `sab_accept_successor` | `ForeignOrSuccessorNeverAdopted` |
| `sab_drop_pending_on_terminal` | `PendingSurvivesLocalTerminal` |
| `sab_late_rearm` | `TerminalNeverRearmsAuthority` |
| `sab_response_relative_cadence` | `CadenceAnchoredAtAttemptStart` |
| `sab_send_after_deadline` | `NoRequestAfterSafeDeadline` |
| `sab_double_conditional_landing` | `OneIncarnationPerPredecessor` |

Each config enables exactly one sabotage. `sab_double_conditional_landing` must start with at least
two outstanding copies and permit both to replace the same predecessor; this proves the count
reduction relies on conditional atomicity. `PendingSurvivesLocalTerminal` is a transition-history
guard: local terminalization itself may not decrement an already-positive outstanding count. It does
not claim that every unresolved outcome necessarily has a pending message.

- [ ] **Step 5: Add six non-vacuity witnesses**

Define negated reachability properties with these exact names:

```text
WitnessDirectRetry
WitnessReadAdoption
WitnessExhaustionFences
WitnessLateBeforeReclaim
WitnessLateAfterSuccessor
WitnessImmediateCatchup
```

Each witness config lists all safety invariants plus its one witness property. The two late witnesses
must reach local `Unresolved` before delivery; the successor witness must show the old conditional
message being refused, not silently deleted.

- [ ] **Step 6: Write the asserted sabotage-first runner**

Follow `run_blobpublish.sh`: pin `tmp/tla2tools.jar`, use a private metadir per row, default to
`TLC_WORKERS=1`, reject timeout/deadlock/parse errors, require the exact named violation, and assert
that at least one selected row executed. Run sabotages first, then `safe`, then witnesses. Mark the
runner executable with `chmod +x docs/superpowers/models/run_mountrenewretry.sh`.

- [ ] **Step 7: Run the complete focused gate**

Run:

```bash
docs/superpowers/models/run_mountrenewretry.sh > tmp/run_mountrenewretry.log 2>&1
```

Expected: 10 exact sabotage violations, one green safe configuration, six exact witness violations,
17 executed rows, and exit status 0. Verify the runner's final row count; a truncated or partial run
is not a pass.

- [ ] **Step 8: Record reproducible evidence**

Write `CaMountRenewRetryCore_RESULTS.md` with the TLC jar checksum/version, worker count, command,
per-row result, generated/distinct states, depth, elapsed time, exact invariant, and short narration
of both late-delivery witnesses. State explicitly that no formal refinement to atomic
`CaCasMountCore.Renew` is claimed. Because this is a new documentation file, add the required
frontmatter and explicit `{#...}` anchor to every heading.

- [ ] **Step 9: Commit the focused formal gate**

```bash
git add docs/superpowers/models/CaMountRenewRetryCore.tla \
  docs/superpowers/models/CaMountRenewRetryCore_*.cfg \
  docs/superpowers/models/run_mountrenewretry.sh \
  docs/superpowers/models/CaMountRenewRetryCore_RESULTS.md
git diff --cached --check
git commit -m "tla: model bounded CAS mount renewal retries"
```

### Task 2: Audit and rerun the atomic mount model {#task-2-audit-and-rerun-the-atomic-mount-model}

**Gate:** No production work may start until this task passes and is committed.

**Files:**

- Modify: `docs/superpowers/models/CaCasMountCore_RESULTS.md`
- Modify only if the audit finds stale atomic prose: `docs/superpowers/models/CaCasMountCore.tla`
- Modify only if an expectation is missing or incorrect: `docs/superpowers/models/run_mount.sh`

**Interfaces:**

- Consumes: Task 1's split-phase postcondition and the existing complete `run_mount.sh` expectation
  table.
- Produces: recorded evidence that the established mount/remount safety battery is unchanged.

- [ ] **Step 1: Audit the abstraction boundary before running**

Confirm that `CaCasMountCore.Renew` remains an atomic successful-renewal abstraction. Document this
correspondence: focused `Committed` states satisfy its durable body/token and local-authority
postconditions; focused terminal states enter its already-modelled fenced/remount boundary. Do not
encode landed-but-unconfirmed messages as atomic stuttering and do not add transport state to this
model.

- [ ] **Step 2: Inventory the runner's complete expectation table**

Compare every tracked `CaCasMountCore_*.cfg` with `run_mount.sh`. The intentionally excluded
`CaCasMountCore_rev6_observe.cfg` must remain documented and may run only with `SLOW=1`; no other
tracked green, sabotage, or witness config may be absent.

- [ ] **Step 3: Run the complete committed battery**

Run:

```bash
docs/superpowers/models/run_mount.sh > tmp/run_mount_2244.log 2>&1
```

Expected: every named row matches its exact green/violation/witness expectation and the runner exits
0. Compare the executed-row count against its `CONFIGS` table, not against a hand-written expected
number.

- [ ] **Step 4: Record the new regression run**

Append one dated section to `CaCasMountCore_RESULTS.md` containing command, jar identity, worker count,
per-row state/depth totals, elapsed time, and the explicit non-refinement boundary. If an existing
finding/incomplete row remains, preserve it verbatim and distinguish it from a regression.

- [ ] **Step 5: Commit the atomic-model gate**

```bash
git add docs/superpowers/models/CaCasMountCore_RESULTS.md \
  docs/superpowers/models/CaCasMountCore.tla \
  docs/superpowers/models/run_mount.sh
git diff --cached --check
git commit -m "docs: record CAS mount model regression gate"
```

### Task 3: Add generation-10 mount attempt identity {#task-3-add-generation-10-mount-attempt-identity}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.cpp`
- Test: `src/Disks/tests/gtest_cas_format.cpp`
- Test: `src/Disks/tests/gtest_cas_server_root_format.cpp`
- Test: `src/Disks/tests/gtest_cas_encoding_pins.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`
- Test: `src/Disks/tests/gtest_cas_observability.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_contiguous_alloc.cpp`
- Test: `src/Disks/tests/cas_format_test_battery.h`

**Interfaces:**

- Consumes: Task 1's `ExactAttemptOnly` invariant.
- Produces: `kMountWriteAttemptIdGeneration = 10`, `G_BUILD = 10`, required canonical field
  `write_attempt_id`, and fresh IDs from every holder-originated body constructor.

- [ ] **Step 1: Write failing format and producer tests**

Add tests proving:

```cpp
TEST(CASMountLeaseFormat, WriteAttemptIdIsRequiredAndCanonical)
TEST(CASMountLeaseFormat, UnknownFieldsRemainTolerated)
TEST(CASFormat, MountAttemptIdentityIsARecreateOnlyGenerationTenChange)
TEST(CASPoolMeta, GenerationNinePoolIsRejectedAtReaderFloor)
TEST(CASMountLease, HolderBodiesMintFreshAttemptIdsAndFenceCopiesIt)
TEST(CASMountLease, ReclaimAndSuccessorBodiesMintNewAttemptIds)
```

The first test must check the literal JSON key `"write_attempt_id"`, exact round-trip of a nonzero
`UInt128`, and `CORRUPTED_DATA` when the field is absent. The producer tests must distinguish two
separate holder writes, verify a GC fence preserves the observed ID, and verify reclaim/successor
bodies replace it.

- [ ] **Step 2: Rebuild and capture red evidence**

Run:

```bash
ninja -C build src/unit_tests_dbms > build/t3_format_red_build.log 2>&1
```

Expected: nonzero compile status naming the missing member/constant, or a rebuilt binary whose
focused tests fail for the absent field/floor. If it compiles, list and run:

```bash
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASMountLeaseFormat*:CASFormat*:CASPoolMeta*:CASMountLease*:CASRefContiguousAlloc*' \
  > build/t3_format_red_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASMountLeaseFormat*:CASFormat*:CASPoolMeta*:CASMountLease*:CASRefContiguousAlloc*' \
  > build/t3_format_red_test.log 2>&1
```

Require at least one listed new test and a failed assertion naming generation 10 or
`write_attempt_id`.

- [ ] **Step 3: Add the durable field and canonical codec**

Add `UInt128 write_attempt_id{}` to `MountLease`. Encode it as the full-word key
`write_attempt_id` with the existing hex-128 codec. Decode it into a separate `saw_write_attempt_id`
flag and reject a body lacking it together with the existing identity-field validation. Do not accept
zero as an implicit legacy value on a decoded durable body.

- [ ] **Step 4: Bump the recreate-only format floor**

Set `G_BUILD = 10`, add `kMountWriteAttemptIdGeneration = 10`, append breaking generation-10 change
points to both `MOUNT_LEASE` and `POOL_META`, and make `decodePoolMeta` reject generation 9 before
interpreting its body. Update the rejection message from the generation-9 frontier wording to the
generation-10 mount-attempt-identity floor.

- [ ] **Step 5: Mint and preserve IDs at every production writer**

Introduce one private `newMountWriteAttemptId` helper using `UUIDHelpers::generateV4`, returning a
nonzero `UInt128`. Route `makeMountBody`, initial claim, certified reclaim, keeper adoption/renewal,
and clean farewell through a fresh ID. When GC sets `gc_fenced`, copy the observed body and retain its
ID. Do not change reader-only sites in `CasDecommission` or `CasInspect`.

- [ ] **Step 6: Update golden and battery expectations**

Update canonical encoded strings and field-presence checks in the three format-test files. Preserve
the test battery's unknown-field and trailing-byte cases. Add a discriminator showing the decoder
rejects a correctly headed generation-10 body that contains every old field but lacks only
`write_attempt_id`. Update the exact stale-floor diagnostics in `gtest_cas_ref_contiguous_alloc.cpp`
from the generation-9 committed-frontier requirement to the generation-10 mount-attempt-identity
requirement. Add `write_attempt_id` to `ca-inspect`'s mount JSON and extend
`CASObservability.CaInspectDecodesMountLeaseToJson` to pin it.

- [ ] **Step 7: Build and run focused green tests**

```bash
ninja -C build src/unit_tests_dbms > build/t3_format_green_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASMountLeaseFormat*:CASFormat*:CASFormatBattery*:CASPoolMeta*:CASMountLease*:CASRefContiguousAlloc*' \
  > build/t3_format_green_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASMountLeaseFormat*:CASFormat*:CASFormatBattery*:CASPoolMeta*:CASMountLease*:CASRefContiguousAlloc*' \
  > build/t3_format_green_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t3_format_green_test.log
```

Expected: build status 0, nonzero test count, all focused tests pass.

- [ ] **Step 8: Commit the format break**

```bash
git add \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasInspect.cpp \
  src/Disks/tests/gtest_cas_format.cpp \
  src/Disks/tests/gtest_cas_server_root_format.cpp \
  src/Disks/tests/gtest_cas_encoding_pins.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_observability.cpp \
  src/Disks/tests/gtest_cas_ref_contiguous_alloc.cpp \
  src/Disks/tests/cas_format_test_battery.h
git diff --cached --check
git commit -m "feat: identify CAS mount lease write attempts"
```

### Task 4: Add absolute overwrite gates and diagnostics {#task-4-add-absolute-overwrite-gates-and-diagnostics}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp`
- Test: `src/Disks/tests/gtest_cas_request_control.cpp`

**Interfaces:**

- Consumes: existing `CasRequestBudget`, `CasUnresolvedReason`, backend `putOverwrite`/`get`, and the
  target context/diagnostic types declared above.
- Produces: the context overload of `putOverwriteControlled`; Task 5 consumes it directly.

- [ ] **Step 1: Write failing gate-precedence and retry-resolution tests**

Add deterministic fake-clock/backend tests for:

```cpp
TEST(CASRequestController, AbsoluteDeadlineCannotBeReanchoredAfterPreemption)
TEST(CASRequestController, MaxAttemptsOneStillResolvesLostResponseByGet)
TEST(CASRequestController, StopBeforeFirstPutReportsExactCause)
TEST(CASRequestController, StopAfterPutSuppressesResolveAndReportsMidWay)
TEST(CASRequestController, StopAfterResolvedCommitReportsPostWrite)
TEST(CASRequestController, FenceWinsCancellationAndExternalDeadlineWinsDeadlineTie)
TEST(CASRequestController, InterruptedWaitResamplesStopCause)
TEST(CASRequestController, ObserverFailureCannotChangeOutcome)
TEST(CASRequestController, ResolveFailuresExhaustDeadlineWithoutSendingLatePut)
TEST(CASRequestController, EveryTerminalShapeReportsExactDiagnostics)
```

Use call counters, injected integer clocks, and callbacks; do not use real sleeps. In the
`max_attempts = 1` test, the first `PUT` must land and throw, then the exact `GET` must prove the same
bytes and return the new token. The diagnostic-table test must cover every pre-send, mid-way,
post-write, deadline, attempt-exhaustion, cancellation, and fence row from the specification rather
than sampling one outcome per enum.

- [ ] **Step 2: Rebuild and prove the new contract is red**

```bash
ninja -C build src/unit_tests_dbms > build/t4_request_control_red_build.log 2>&1
```

Expected: compile failure on the missing context/diagnostics types, or rebuilt focused tests that
fail the stated assertions. If it compiles, run the rebuilt binary:

```bash
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASRequestController*' \
  > build/t4_request_control_red_list.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASRequestController*' \
  > build/t4_request_control_red_test.log 2>&1
```

Require at least one exact new test name in the list and a nonzero test status caused by the new
deadline/diagnostic contract. Never use the old binary.

- [ ] **Step 3: Add context, progress, and diagnostic types**

Add the exact types in `{#target-interfaces-and-ownership}`. Append diagnostics to
`CasOverwriteResult` with defaults so existing aggregate/legacy callers remain source-compatible.
Keep `CasUnresolvedReason`; do not introduce a parallel protocol-reason enum.

- [ ] **Step 4: Implement one sampled gate helper**

The helper samples stop cause and clock once per gate and applies:

```text
FenceOrLifecycleLost > Cancelled > absolute deadline > backend/result transition
```

Classify a deadline tie as `ExternalLeaseSafety`. A false wait while stop cause remains `Continue`
is a programming exception. Map pre-first refusal to `NoAttemptSent`, mid-operation stop to
`FenceLostMidWay`, post-proof refusal to `FenceLostPostWrite`, elapsed deadline after send to
`DeadlineMidWay`, and inconclusive final resolution to `AttemptsExhausted`.

- [ ] **Step 5: Preserve resolve-before-reissue at the final attempt**

Increment `attempts_sent` and emit `PutStarted` only immediately before `putOverwrite`. Always admit
the exact resolving `GET` after an ambiguous sent attempt when stop/deadline gates allow it, even if
the `PUT` consumed `max_attempts`. Evaluate the attempt count only when another `PUT` would be sent.
Set `resolved_by_get` and emit `ResolvedByGet` only when exact bytes prove commit.

- [ ] **Step 6: Contain diagnostic observers**

Wrap every observer call so it cannot change protocol control flow. Suppress observer exceptions and
report at most one `DEBUG` line. Keep the final result derived from backend state and gates alone.

- [ ] **Step 7: Preserve the existing overload behavior**

Implement the current `fence_ok` overload as a legacy adapter with its call-relative operation
deadline, default sleeper, and fence-to-`FenceOrLifecycleLost` mapping. Run existing request-control
tests unchanged; the new path must not alter `putIfAbsentControlled`, mutable create, or slot occupy.

- [ ] **Step 8: Build and run the focused controller gate**

```bash
ninja -C build src/unit_tests_dbms > build/t4_request_control_green_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASRequestControl*:CASRequestController*:CASRequestControllerBackoff*' \
  > build/t4_request_control_green_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASRequestControl*:CASRequestController*:CASRequestControllerBackoff*' \
  > build/t4_request_control_green_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t4_request_control_green_test.log
```

Expected: all focused tests pass and the legacy test count is not lower than before the change.

- [ ] **Step 9: Commit the controller surface**

```bash
git add \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp \
  src/Disks/tests/gtest_cas_request_control.cpp
git diff --cached --check
git commit -m "feat: bound CAS overwrite retries by absolute deadlines"
```

### Task 5: Move keeper and worker ownership into the stable runtime {#task-5-move-keeper-and-worker-ownership-into-the-stable-runtime}

**Atomic ownership task:** Do not commit after deleting `SingleWriterSlot` until every runtime caller
uses the new synchronous keeper and both worker loops. This is one reviewer gate because a temporary
mixed ownership model would be harder to prove than either endpoint.

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`
- Test: `src/Disks/tests/gtest_cas_pool.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_ack_floor.cpp`
- Test support: `src/Disks/tests/cas_test_helpers.h`

**Interfaces:**

- Consumes: Task 3's required attempt ID and Task 4's context overload/diagnostics.
- Produces: the exact keeper/runtime APIs in `{#target-interfaces-and-ownership}`, two persistent
  workers, generation-latched remount requests, and no remaining `SingleWriterSlot` symbol.

- [ ] **Step 1: Add failing synchronous-keeper state tests**

Replace subclass-hook tests with public-protocol tests proving:

```cpp
TEST(CASHeartbeat, KeeperStateAllowsOnlyActiveReleaseOrTerminal)
TEST(CASHeartbeat, RenewalRetriesOneImmutableBodyAndAdoptsLostResponse)
TEST(CASHeartbeat, DeadlineBeforeSendTerminalizesWithTypedFailure)
TEST(CASHeartbeat, CancellationBeforeSendIsNotAttemptedAndAllowsRelease)
TEST(CASHeartbeat, CancellationAfterSendIsTerminalAndForbidsRelease)
TEST(CASHeartbeat, SlowResolvedSuccessKeepsAttemptStartAnchor)
TEST(CASHeartbeat, SamePairTwinAndForeignOrSuccessorStayTerminal)
TEST(CASHeartbeat, ExpectedPredecessorThenLateLandingIsAdoptedExactly)
TEST(CASHeartbeat, GcFenceAndVanishedMountStayTerminal)
TEST(CASHeartbeat, LateDeliveryAfterTerminalCannotRearmOrOverwriteSuccessor)
TEST(CASHeartbeat, WallClockStepsAndBootSuspendCannotExtendAuthority)
```

The retry backend must save every `(key, bytes, expected token)` and assert equality across physical
attempts, including the same `write_attempt_id`. Every `Terminal` assertion checks `failure != nullptr`
and the exact non-`LOGICAL_ERROR` code/message. The predecessor test must return the expected token
from the first resolving `GET`, land the delayed first `PUT`, then make the retry conflict and adopt
only the exact body. The late-delivery test must exercise both landing before fresh-epoch reclaim and
conditional refusal after successor claim. Wall-clock jumps must be inert; injected BOOTTIME
advancement, including a suspend-sized overshoot, must close admission.

- [ ] **Step 2: Add failing runtime admission and lifecycle tests**

Add deterministic barriers, not sleeps, for:

```cpp
TEST(CASPoolRemount, DirectRenewCannotRaceWorkerStartOrKeeperReplacement)
TEST(CASPoolRemount, RemountWaitsForRenewalParkedBeforeReplacement)
TEST(CASPoolRemount, TeardownJoinsBothWorkersBeforeRelease)
TEST(CASPoolRemount, WorkerConstructionRollbackFailsOpenClosed)
TEST(CASPoolRemount, ExternalLossDuringRenewalUsesOneRecoveryGeneration)
TEST(CASPoolRemount, ConcurrentRemountRequestIsProcessedAfterActiveGeneration)
TEST(CASPoolRemount, ImmediatePostRemountRenewalFailureIsNotDropped)
TEST(CASPoolRemount, StaleRemountAnchorPerformsParkedRedo)
TEST(CASPoolRemount, ThrowingEventSinkAfterCommitLeavesRuntimeLive)
TEST(CASPoolShutdown, PreSendCancellationAllowsFarewellButAmbiguityDoesNot)
TEST(CASPool, DirectAndStartupTerminalFailuresRethrowTypedExceptions)
TEST(CASPool, DeterministicWorkerFailureFencesWithoutWaitingForCadence)
TEST(CASPool, RenewWatermarkOnceRefreshesFenceAndDepositsOneFailure)
```

`ExternalLossDuringRenewalUsesOneRecoveryGeneration` must assert one loss metric, one successful
remount callback, and one fresh epoch. `ImmediatePostRemountRenewalFailureIsNotDropped` must advance
the injected BOOTTIME during ref-runtime quiescence and make the catch-up renewal fail before the
remount worker returns to wait. The direct/startup test must cover `max_attempts = 1`, pre-attempt
deadline, cancellation after send, and post-write refusal as typed `NETWORK_ERROR` propagation; the
startup leg must prove open rollback happens before a live fence/runtime is published.

- [ ] **Step 3: Rebuild and capture ownership red evidence**

```bash
ninja -C build src/unit_tests_dbms > build/t5_runtime_red_build.log 2>&1
```

Expected: compile failure naming new state/result/runtime APIs, or rebuilt tests failing their
barrier/state assertions. If compiled, run:

```bash
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASHeartbeat*:CASPoolRemount*:CASPoolShutdown*:CASPool*' \
  > build/t5_runtime_red_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASHeartbeat*:CASPoolRemount*:CASPoolShutdown*:CASPool*' \
  > build/t5_runtime_red_test.log 2>&1
```

Require the exact new test names in the list and a nonzero test status caused by a new state,
barrier, retry, or lifecycle assertion.

- [ ] **Step 4: Replace the slot base with a synchronous keeper**

Delete `SingleWriterSlot` and all base hooks, thread members, callback fields, mismatch side-channel,
and derived-destructor join workaround. Move `seq`, held token, explicit keeper state, durable claim,
renew body creation, mismatch classification, terminal factory, and clean release directly into
`MountLeaseKeeper`.

`start` performs `New -> Active` and returns its pre-I/O BOOTTIME anchor. `renew` is admitted only in
`Active`; it fixes wall/boot timestamps, `min_active`, `seq + 1`, fresh attempt ID, bytes, expected
token, and absolute deadline before controller entry. `release` performs only `Active -> Released`.
Destruction performs no durable write.

Compute the operation boundary with checked/saturating arithmetic exactly once:

```text
lease_retry_deadline = confirmed_deadline_boot_ms - lease_safety_margin_ms
request_deadline = attempt_start_boot_ms + budget.operation_deadline_ms
absolute_deadline = min(lease_retry_deadline, request_deadline)
```

No request is admitted unless `now + attempt_timeout_ms <= absolute_deadline`.

- [ ] **Step 5: Implement terminal-result construction once**

Create one private factory that stores deposition, changes `Active -> RenewalTerminal`, and requires
a non-null exception. Preserve original exceptions for conflict/deterministic failures. Synthesize a
`NETWORK_ERROR` exception, without logging, for unresolved deadline, cancellation-after-send,
post-write refusal, and attempt exhaustion. Both factory and runtime consumer assert the non-null
invariant.

- [ ] **Step 6: Add RAII renewal-driver admission**

Implement `RenewalDriverState` under one mutex/condition pair. A small private driver lease performs
the transition under lock, releases the lock before keeper/backend I/O, and restores/reports the
destination state before result consumption. No runtime mutex may be held across backend I/O,
keeper calls, failure handling, remount scheduling, waits for another thread, or joins.

- [ ] **Step 7: Move renewal cadence and cancellation into `CasMountRuntime`**

Implement `renewKeeperForStartupOnce`, `renewKeeperForRemountOnce`, `renewWatermarkOnce`, and
`renewalLoop`. Startup uses `Dormant -> StartupCall -> Dormant` and never schedules remount. Remount
redo uses `Parked -> RemountCall -> Parked` and fails only its current attempt. Background renewal is
the sole caller after workers start. Direct renewal refuses if background operation is configured or
either worker exists.

Schedule the next beat from `last_committed_attempt_start_boot_ms + period`; when overdue, run it
immediately. Use interruptible condition-variable backoff plus post-wait BOOTTIME gates; do not use
sleep to establish correctness.

- [ ] **Step 8: Add persistent remount worker and generation latch**

Replace the on-demand remount thread and `remount_running` boolean with one long-lived worker,
`remount_requested_generation`, and `remount_handled_generation`. `scheduleRemount` increments the
requested generation and notifies even when recovery is active. The worker snapshots a generation,
requests `ParkRequested`, waits for `Parked`, and executes the existing whole-chain callback.

After success, mark only the snapshot handled. If a newer request exists, remain parked and process
it; otherwise resume renewal only after the fresh epoch, keeper, ref-runtime quiescence, fence, and
`Live` state are published.

- [ ] **Step 9: Make worker creation and remount commit fail closed**

Construct both workers in parked startup state before the writable pool is externally visible; only
release their loops after both handles exist. If either construction fails, signal/join the partial
worker, trip the fence, and fail open. No incident path constructs a thread.

Make the final remount fence/lifecycle publication a no-throw commit section. Contain event/log sink
exceptions after the local runtime has committed. If the keeper's start anchor no longer passes the
same safety/attempt-fit gate after quiescence, perform one synchronous parked redo before fence arm.

Use an internal-only worker factory in `CasMountRuntimeConfig`:

```cpp
using RuntimeWorkerFactory
    = std::function<ThreadFromGlobalPool(std::function<void()>)>;

RuntimeWorkerFactory worker_factory;
```

An empty factory selects ordinary `ThreadFromGlobalPool` construction. Tests inject a factory that
throws on call one or two; this is not parsed from disk configuration and is not user-facing.

- [ ] **Step 10: Consume terminal results by RAII return state**

For a terminal returning to `WorkerIdle`, record the first operational loss, trip the fence, and
request one recovery generation. For a terminal returning to `Parked`, record deposition but neither
duplicate loss accounting nor increment the request generation; the request that caused parking owns
recovery. For `Stopping`, do not count operational loss or schedule recovery. Any sent ambiguity
forbids farewell in all three cases.

- [ ] **Step 11: Rewire `Pool` startup, remount, direct seam, and teardown**

Replace `keeperStart`, `keeperStartBackground`, `keeperStopBackground`, and callback wiring with the
new runtime entry points. Keep `Pool::tryRemountOnce`'s owner/catalog/epoch allocation,
`claimMountAwaitingExpiry`, ref-recovery cancellation, ref-table quiescence, fence arm, and
`noteRemounted` order unchanged. Teardown stops/joins both workers, then releases only an `Active`
keeper; it destroys a `RenewalTerminal` keeper without farewell.

- [ ] **Step 12: Update all direct keeper fixtures and comments**

Update the four named test/support files in this task. Remove `TestableMountLeaseKeeper`, protected
hook exposure, callback assumptions, and comments about base-owned renewal. Where a test needs one
beat, call synchronous `renew` with an explicit environment; where it tests the production loop, use
`CasMountRuntime` and barriers.

- [ ] **Step 13: Prove the old ownership surface is gone**

```bash
! rg -n 'SingleWriterSlot|onRenewCommitted|onRenewSucceeded|onRenewFailed|setFenceCallbacks|keeperStartBackground|keeperStopBackground' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed \
  src/Disks/tests/gtest_cas_heartbeat.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_pool.cpp \
  src/Disks/tests/gtest_cas_gc_ack_floor.cpp
```

Any remaining match must be removed or rewritten in this task; do not defer a dangling caller.

- [ ] **Step 14: Build and run the ownership/retry gate**

```bash
ninja -C build src/unit_tests_dbms > build/t5_runtime_green_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASHeartbeat*:CASMount*:CASPool*:CASPoolRemount*:CASPoolShutdown*:CASMountFence*:CASMountStartup*:CASGCAckFloor*' \
  > build/t5_runtime_green_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASHeartbeat*:CASMount*:CASPool*:CASPoolRemount*:CASPoolShutdown*:CASMountFence*:CASMountStartup*:CASGCAckFloor*' \
  > build/t5_runtime_green_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t5_runtime_green_test.log
```

Before accepting green, confirm the list contains every new test name; wildcard suite matches alone
are insufficient.

- [ ] **Step 15: Commit the atomic ownership migration**

```bash
git add \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_heartbeat.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_pool.cpp \
  src/Disks/tests/gtest_cas_gc_ack_floor.cpp \
  src/Disks/tests/cas_test_helpers.h
git diff --cached --check
git commit -m "refactor: centralize CAS mount renewal ownership"
```

### Task 6: Add bounded renewal and remount observability {#task-6-add-bounded-renewal-and-remount-observability}

**Files:**

- Modify: `src/Common/ProfileEvents.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Test: `src/Disks/tests/gtest_cas_observability.cpp`
- Test: `src/Disks/tests/gtest_cas_event_log.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`
- Test: `src/Disks/tests/gtest_cas_pool.cpp`

**Interfaces:**

- Consumes: Task 4's progress/diagnostic dimensions and Task 5's single result-consumption boundary.
- Produces: eight new ProfileEvents, bounded text logs, structured `watermark_renew` events, and
  step-labelled `mount_remount` outcomes.

- [ ] **Step 1: Write failing counter and event tests**

Add deterministic tests proving exact deltas for:

```text
CASMountRenewalAttempts
CASMountRenewalRetries
CASMountRenewalResolved
CASMountRenewalRecovered
CASMountRenewalDeadlineExceeded
CASRemountAttempts
CASRemountSucceeded
CASRemountFailed
```

Test first-attempt success, direct retry success, read-resolved success, deadline terminalization,
remount success, and one named-step failure. Assert `CASMountLeaseLost` increments once per
operational `Live -> TransientNotLive` recovery generation: either the initiating external loss or
ordinary worker terminal owns it; a parked terminal, classification branch, and shutdown do not.

- [ ] **Step 2: Write failing bounded-log tests**

Capture the logger/event sink and assert one logical renewal emits at default levels at most one
`WARNING` entering retry and then one `INFO` recovered, or one final `WARNING` fenced. Ordinary
first-attempt success emits neither a default-level line nor a `watermark_renew` event. Every remount
attempt emits exactly one final result naming attempt number and last/current step.

- [ ] **Step 3: Rebuild and verify the new observability surface is red**

```bash
ninja -C build src/unit_tests_dbms > build/t6_observability_red_build.log 2>&1
```

Expected: compile failure on new ProfileEvents or a rebuilt test failure on missing counters/details.
If it compiles, run:

```bash
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASObservability*:CASEvent*:CASContentAddressedLog*:CASMountAudit*:CASPoolRemount*' \
  > build/t6_observability_red_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASObservability*:CASEvent*:CASContentAddressedLog*:CASMountAudit*:CASPoolRemount*' \
  > build/t6_observability_red_test.log 2>&1
```

Require the exact new counter/log test names in the list and a nonzero test status caused by missing
observability behavior.

- [ ] **Step 4: Add ProfileEvents with exact ownership semantics**

Add the eight events and update the `CASMountLeaseLost` description to say it counts once per
operational loss/recovery generation. Increment attempt/retry/resolved progress at the physical
observer transition; increment recovered/deadline once at logical completion. Do not reconstruct
attempt counts from a terminal return.

- [ ] **Step 5: Emit structured renewal events from one runtime boundary**

Use `CasEventType::WatermarkRenew` outcomes `retrying`, `recovered`, and `failed`. Populate detail with
`server_root_id`, `writer_epoch`, `seq`, shortened attempt ID, attempts sent, elapsed milliseconds,
remaining confirmed budget, unresolved reason, deadline source, stop cause, and final classification.
Contain sink/allocation failures so observability cannot change protocol outcome.

- [ ] **Step 6: Label whole-chain remount attempts without changing their protocol**

In `Pool::tryRemountOnce`, keep an explicit local step label updated before each existing
state-changing/probing step. Increment `CASRemountAttempts` at entry. On success increment succeeded;
on any return/exception increment failed and emit/log one final result with attempt number and step.
Do not add per-step retry, backend reads, or persistent progress.

- [ ] **Step 7: Implement bounded text levels**

Individual physical retries remain `DEBUG`. Emit the first transition to retry at `WARNING`, recovery
at `INFO`, and final fence at `WARNING`. Emit one default-level remount result per whole-chain attempt.
Keep polling iterations inside `claimMountAwaitingExpiry` below warning level.

- [ ] **Step 8: Build and run the observability gate**

```bash
ninja -C build src/unit_tests_dbms > build/t6_observability_green_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASObservability*:CASEvent*:CASContentAddressedLog*:CASMountAudit*:CASMountLease*:CASPoolRemount*' \
  > build/t6_observability_green_list.log 2>&1
build/src/unit_tests_dbms \
  --gtest_filter='CASObservability*:CASEvent*:CASContentAddressedLog*:CASMountAudit*:CASMountLease*:CASPoolRemount*' \
  > build/t6_observability_green_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t6_observability_green_test.log
```

Confirm from the list that the added tests ran; do not accept a wildcard whose new test name is
absent even when sibling suites executed.

- [ ] **Step 9: Commit observability**

```bash
git add \
  src/Common/ProfileEvents.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
  src/Disks/tests/gtest_cas_observability.cpp \
  src/Disks/tests/gtest_cas_event_log.cpp \
  src/Disks/tests/gtest_cas_mount.cpp \
  src/Disks/tests/gtest_cas_pool.cpp
git diff --cached --check
git commit -m "feat: expose CAS mount renewal recovery"
```

### Task 7: Rate-limit snapshot publication refusal {#task-7-rate-limit-snapshot-publication-refusal}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp`

**Interfaces:**

- Consumes: existing `advancePublishBackoff`, `resetPublishBackoff`,
  `publish_backoff_until_ms`, and `settleSnapshotPublish` re-evaluation.
- Produces: `lane_state != Ready` refusal participates in the same bounded per-table backoff as every
  other non-committed snapshot attempt.

- [ ] **Step 1: Add a failing `NotReady` refusal-loop test**

Extend `CASRefSnapshotPublishOrdering` with a fake BOOTTIME test that:

1. raises the table above snapshot threshold;
2. puts the append lane in a non-`Ready` state;
3. invokes one production settlement/dispatch cycle;
4. proves `CASRefSnapshotPublishBackoff` increases once;
5. proves immediate settlement does not redispatch;
6. advances to 200 ms and proves one retry is admitted;
7. advances successive failures through 400 ms and the 30-second cap;
8. completes one durable publish and proves backoff resets.

- [ ] **Step 2: Rebuild and capture the refusal-loop red**

```bash
ninja -C build src/unit_tests_dbms > build/t7_snapshot_red_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASRefSnapshotPublishOrdering*' > build/t7_snapshot_red_list.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASRefSnapshotPublishOrdering*' \
  > build/t7_snapshot_red_test.log 2>&1
```

Expected: nonzero test count and failure showing an immediate redispatch or unchanged backoff count.

- [ ] **Step 3: Arm backoff under the existing state lock**

When `tryPublishSnapshotAndAdvanceCheckpointOnceOnRuntime` observes
`lane_state != RefLaneState::Ready`, call `advancePublishBackoff` while holding `state_mutex`, then
copy the state needed for the warning and return `false`. Do not move correctness state, snapshot
capture, or threshold checks. The existing settlement re-evaluation must see the armed deadline.

- [ ] **Step 4: Keep warning and direct-call semantics precise**

Emit the refusal warning only for the actual backoff-spaced attempt. A direct test call remains one
attempt per invocation; only production redispatch is suppressed. Reset remains restricted to a
durable snapshot publication.

- [ ] **Step 5: Build and run focused green tests**

```bash
ninja -C build src/unit_tests_dbms > build/t7_snapshot_green_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests \
  --gtest_filter='CASRefSnapshotPublishOrdering*' > build/t7_snapshot_green_list.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASRefSnapshotPublishOrdering*' \
  > build/t7_snapshot_green_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t7_snapshot_green_test.log
```

- [ ] **Step 6: Commit the bounded refusal loop**

```bash
git add \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
  src/Disks/tests/gtest_cas_ref_snapshot_publish_ordering.cpp
git diff --cached --check
git commit -m "fix: back off refused CAS snapshot publication"
```

### Task 8: Validate retry recovery against an object store and chaos {#task-8-validate-retry-recovery-against-an-object-store-and-chaos}

**Files:**

- Create: `tests/integration/test_cas_mount_renewal_retry/__init__.py`
- Create: `tests/integration/test_cas_mount_renewal_retry/test.py`
- Create: `tests/integration/test_cas_mount_renewal_retry/configs/storage_conf.xml`
- Create: `tests/integration/test_cas_mount_renewal_retry/docker_compose_proxy.yml`
- Modify: `utils/ca-soak/proxy/s3_fault_proxy.py`
- Modify: `utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py`
- Create: `utils/ca-soak/tests/test_s3_fault_proxy.py`

**Interfaces:**

- Consumes: the existing RustFS XML API lane, the request-preserving fault proxy behavior from
  `utils/ca-soak/proxy/s3_fault_proxy.py`, and the Task 6 ProfileEvents/log contracts.
- Produces: a deterministic integration fault and a lease-sensitive release soak oracle.

- [ ] **Step 1: Create an isolated proxy-backed integration fixture**

Start one ClickHouse node, RustFS, and a test-local forwarding proxy. The storage config must point
the CAS S3 endpoint at the proxy, keep normal authentication/signing, and use test-only lease values
that leave room for at least two bounded attempts. The compose file starts faults disarmed. The proxy
must support a rule scoped to the exact mount-key conditional `PUT`; do not fault all data-plane PUTs
for the focused recovery test.

Reuse the shared proxy implementation by bind-mounting
`../../../utils/ca-soak/proxy/s3_fault_proxy.py`; do not fork a second copy. Append the test compose
file to `cluster.base_cmd` before `cluster.start`. Publish the control port with an ephemeral host
port (`127.0.0.1::8474`) and obtain it with
`subprocess.check_output(cluster.base_cmd + ["port", "s3proxy", "8474"], text=True)`.

- [ ] **Step 2: Write failing scoped-proxy unit tests**

Create `test_s3_fault_proxy.py` with local upstream/proxy servers and tests for path-mismatch
pass-through, exact remaining-fault count, reset to disarmed state, and a request that reaches the
upstream but loses its downstream response while statistics retain method/path/body hash/status.

Run:

```bash
pytest -q utils/ca-soak/tests/test_s3_fault_proxy.py \
  > build/t8_proxy_red.log 2>&1
```

Expected: nonzero pytest count and failures naming the absent scoped fields/mode/statistics.

- [ ] **Step 3: Extend the shared proxy with deterministic scoped faults**

Add optional control fields `path_substring` and `remaining_faults`. A request is eligible only when
method and path both match and the remaining count is positive; select the fault and decrement the
remaining count atomically under `_cfg_lock`, so concurrent requests cannot exceed the configured
count. Add mode `drop_after_forward`: buffer and forward the complete request, read the upstream
response, record path, method, request-body SHA-256 and upstream status in `/stats`, then close the
downstream connection without sending the response. Existing configurations that omit the new
fields retain byte-for-byte decision behavior.

Add pure HTTP-server tests in `test_s3_fault_proxy.py` for path mismatch pass-through, exact fault
count, reset to disarmed state, and landed-response-drop statistics.

- [ ] **Step 4: Add a non-vacuous short-disruption test**

Record the current mount sequence/token and the eight Task 6 counters. Arm a finite sequence that
causes the next renewal `PUT` to return a transient failure while RustFS remains healthy, then lets a
retry land. Poll `system.cas_mounts` and counters until the sequence advances. Assert:

- `CASMountRenewalAttempts` delta is greater than one;
- `CASMountRenewalRetries` and `CASMountRenewalRecovered` increase;
- lifecycle remains `Live` and `CASRemountAttempts` does not increase;
- a mutation succeeds during/after recovery;
- the aggregate `watermark_renew` retry/recovered sequence exists;
- the proxy recorded at least one targeted request, so the test cannot pass without exercising the
  fault.

- [ ] **Step 5: Add a landed-response-lost integration leg**

Have the proxy forward one mount renewal to RustFS and drop only its response. Assert the resolving
read adopts the exact `write_attempt_id`/bytes and the observed incarnation token,
`CASMountRenewalResolved` increases, no second logical body is created, and the mount remains `Live`.
Use proxy request/body hashes plus `system.cas_log` details; do not infer adoption only from eventual
availability.

- [ ] **Step 6: Update `S39` to the new operational contract**

Replace stale `SingleWriterSlot` log needles with Task 6 counters and aggregate events. The short leg
must inject repeated sub-TTL disruptions and require zero unexpected `TransientNotLive`, zero
remounts, continued mutations, and positive retry/recovery counts. The long leg remains fail-closed:
it must observe one lease-loss recovery generation, eventual remount, successful post-clear write,
and clean fsck. Keep the fault-window arithmetic derived from TTL/period and record it in observations.

- [ ] **Step 7: Run proxy and scenario import checks**

```bash
pytest -q utils/ca-soak/tests/test_s3_fault_proxy.py \
  > build/t8_proxy_unit.log 2>&1
python3 -m py_compile utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py
```

Expected: status 0 and a nonzero pytest test count.

- [ ] **Step 8: Run the integration lane through Praktika**

Do not overlap this with a live `ca-soak` because Praktika prunes containers/volumes.

```bash
python3 -m ci.praktika run "integration" --test test_cas_mount_renewal_retry \
  > build/t8_cas_mount_renewal_integration.log 2>&1
```

Expected: both fault legs pass, no skipped test, and the log names a positive targeted request count.

- [ ] **Step 9: Run focused `S39` at development scale**

Run:

```bash
(cd utils/ca-soak && python3 -m scenarios.run \
  --scenario S39 --seed 2244 --duration 15m --scale dev) \
  > build/t8_s39_dev.log 2>&1
```

Expected: every S39 verdict passes, short-fault recovery has no remount, long-fault recovery
remounts once or more as permitted by the existing whole-chain protocol, and final fsck reports no
dangling content.

- [ ] **Step 10: Commit the integration and chaos oracle**

```bash
git add \
  tests/integration/test_cas_mount_renewal_retry/__init__.py \
  tests/integration/test_cas_mount_renewal_retry/test.py \
  tests/integration/test_cas_mount_renewal_retry/configs/storage_conf.xml \
  tests/integration/test_cas_mount_renewal_retry/docker_compose_proxy.yml \
  utils/ca-soak/proxy/s3_fault_proxy.py \
  utils/ca-soak/scenarios/cards/s39_lease_fault_tolerance.py \
  utils/ca-soak/tests/test_s3_fault_proxy.py
git diff --cached --check
git commit -m "test: exercise CAS mount renewal recovery"
```

### Task 9: Update documentation and run the release gate {#task-9-update-documentation-and-run-the-release-gate}

**Files:**

- Modify: `docs/en/antalya/cas/architecture/mounts-and-leases.md`
- Modify: `docs/en/antalya/cas/operations/monitoring.md`
- Modify: `docs/en/antalya/cas/operations/debugging.md`
- Modify: `docs/en/antalya/cas/operations/troubleshooting.md`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md`
- Modify: `docs/superpowers/cas/final-checks-todo.md`
- Modify: `docs/superpowers/cas/BACKLOG.md`
- Modify only if an executed soak result is being recorded: `utils/ca-soak/scenarios/RUN_HISTORY.md`
- Audit and modify when stale: comments in every production/test file changed by Tasks 3–8

**Interfaces:**

- Consumes: every implemented behavior and recorded test/model result from Tasks 1–8.
- Produces: user/operations documentation, completed minimum `#2244` bookkeeping, preserved remount
  follow-up, and final release evidence.

- [ ] **Step 1: Document the exact renewal protocol and ownership**

Update `mounts-and-leases.md` with `write_attempt_id`, immutable retry tuple, exact-`GET` adoption,
BOOTTIME deadline/cadence anchoring, terminal fail-closed behavior, synchronous keeper, runtime-owned
workers, parking before replacement, and graceful-shutdown distinction. Remove the current claim that
a transient exception merely waits for a later ordinary cadence.

- [ ] **Step 2: Document counters, events, and operator diagnosis**

Add the eight ProfileEvents and their dimensions to monitoring/debugging. Add a troubleshooting flow
that distinguishes recovered blip, external lease-safety exhaustion, cancellation, confirmed
conflict, terminal lifecycle/fence loss, and whole-chain remount step failure. State the bounded
default-level log policy and how to correlate `watermark_renew` with `mount_remount`.

- [ ] **Step 3: Document the generation-10 format break**

Update `Formats/README.md` with the required full-word `write_attempt_id`, generation-10 change points
for `MountLease` and `PoolMeta`, and recreate-only generation-9 policy. Do not add migration guidance
for unreleased pools beyond recreation.

- [ ] **Step 4: Close only the minimum `#2244` cut**

In `final-checks-todo.md`, mark renewal retries, observability, and snapshot backoff complete only
after their gates pass. In `BACKLOG.md`, retain
`{#issue-2244-remount-retry-follow-up}` unchanged as live work: per-step remount recovery, ambiguous
claim observation, persistent step progress, and its focused TLA+ gate remain deferred.

- [ ] **Step 5: Audit TLA+ models and stale prose globally**

Run untruncated searches:

```bash
rg -n -i 'SingleWriterSlot|one attempt per cadence|one physical attempt|no in-period retry|never re-armed|background renewal failed transiently|keeperStartBackground|keeperStopBackground' \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed \
  src/Disks/tests docs/en/antalya/cas docs/superpowers/cas docs/superpowers/models utils/ca-soak
```

Classify every match. Rewrite stale current-contract prose/comments; preserve explicitly historical
RCA/result text when it is clearly dated and labelled. Recheck every model that mentions renewal and
record why it remains valid or update it in the same task. Do not edit old result numbers to look
current.

- [ ] **Step 6: Run Release and Debug focused builds/tests**

```bash
ninja -C build src/unit_tests_dbms > build/t9_release_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests --gtest_filter='CAS*' \
  > build/t9_release_cas_list.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CAS*' \
  > build/t9_release_cas_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build/t9_release_cas_test.log

ninja -C build_debug src/unit_tests_dbms > build_debug/t9_debug_build.log 2>&1
build_debug/src/unit_tests_dbms --gtest_list_tests --gtest_filter='CAS*' \
  > build_debug/t9_debug_cas_list.log 2>&1
build_debug/src/unit_tests_dbms --gtest_filter='CAS*' \
  > build_debug/t9_debug_cas_test.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' \
  build_debug/t9_debug_cas_test.log
```

Expected: both builds succeed with zero warnings introduced by this work; every `CAS*` test passes;
no `<Fatal>` or `LOGICAL_ERROR` appears in either test log. Compare listed and executed suite counts.

- [ ] **Step 7: Rerun formal and object-store release gates**

```bash
docs/superpowers/models/run_mountrenewretry.sh > tmp/t9_mountrenewretry.log 2>&1
docs/superpowers/models/run_mount.sh > tmp/t9_mount_model.log 2>&1
python3 -m ci.praktika run "integration" --test test_cas_mount_renewal_retry \
  > build/t9_integration.log 2>&1
```

Expected: both model runners reproduce exact expectations; integration passes without skips. Run the
release-scale `S39`/lease-sensitive chaos campaign required by the specification:

```bash
(cd utils/ca-soak && python3 -m scenarios.run \
  --scenario S39 --seed 2244 --duration 2h --scale full) \
  > build/t9_s39_release.log 2>&1
```

Record duration, commit, scenario verdicts, counter deltas, remount count, and fsck result. A missing
Docker lane remains an explicit release blocker.

- [ ] **Step 8: Inspect upstream intersection and final diff**

```bash
base=$(git merge-base altinity/antalya-26.6 HEAD)
git diff --stat "$base"..HEAD -- \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed \
  src/Common/ProfileEvents.cpp \
  docs/en/antalya/cas \
  docs/superpowers/models \
  utils/ca-soak tests/integration/test_cas_mount_renewal_retry
git diff --check "$base"..HEAD
```

Confirm the only generic ClickHouse production intersection added by this feature is the new
ProfileEvents declarations. No generic S3/GCS, MergeTree, replication, Keeper, or user-facing
configuration file may appear.

- [ ] **Step 9: Commit documentation and bookkeeping only**

Stage only files actually changed in this task. If `RUN_HISTORY.md` already contains unrelated local
work, stage only the new verified row or leave the result in the task report; never overwrite or
co-commit the existing change.

```bash
git add \
  docs/en/antalya/cas/architecture/mounts-and-leases.md \
  docs/en/antalya/cas/operations/monitoring.md \
  docs/en/antalya/cas/operations/debugging.md \
  docs/en/antalya/cas/operations/troubleshooting.md \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md \
  docs/superpowers/cas/final-checks-todo.md \
  docs/superpowers/cas/BACKLOG.md
git diff --cached --check
git commit -m "docs: document CAS mount renewal recovery"
```

## Completion criteria {#completion-criteria}

The work is complete only when:

1. the focused split-phase TLA+ runner reports all exact sabotage, safe, and witness verdicts;
2. the complete existing `CaCasMountCore` battery reproduces its expected results;
3. generation-10 lease bodies require and round-trip `write_attempt_id`, while generation-9 pools
   fail before old mount bodies are interpreted;
4. the final-attempt exact `GET` can still prove a `max_attempts = 1` lost-response commit;
5. every accepted renewal remains inside the last confirmed lease's absolute BOOTTIME budget;
6. terminalization prevents another body and clean farewell, except pre-send owner cancellation;
7. keeper replacement happens only in `Parked`, release only after both workers join, and neither
   worker is created on the incident path;
8. external loss during in-flight renewal produces one loss/recovery generation, not two;
9. remount request generations cannot be dropped during an active remount or immediate catch-up
   failure;
10. snapshot `NotReady` refusal uses the existing capped per-table backoff and resets on durable
    success;
11. counters plus aggregate events reconstruct renewal/remount outcomes without per-attempt warning
    spam;
12. focused Release/Debug tests, full `CAS*`, object-store integration, and release-scale `S39` pass;
13. the remount redesign remains explicitly live in the backlog; and
14. no non-CAS object-storage or user-facing API/configuration behavior changes.

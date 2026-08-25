---
description: 'Implementation plan for replacing conditional CAS blob creation with one HEAD-first, unconditional publication protocol across object-storage providers.'
sidebar_label: 'CAS unconditional blob publication plan'
sidebar_position: 2
slug: /superpowers/plans/cas-unconditional-blob-publication
title: 'CAS Unconditional Blob Publication Implementation Plan'
doc_type: 'plan'
---

# CAS Unconditional Blob Publication Implementation Plan {#cas-unconditional-blob-publication-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Use `superpowers:test-driven-development` for every production-code task and `superpowers:verification-before-completion` before claiming a gate passed. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every CAS blob writer one provider-independent protocol—durable precommit, blob `HEAD`, unconditional publication only when absent or condemned, metadata reconciliation, explicit proof—while preserving mutable conditional operations and all non-CAS object-storage behavior.

**Architecture:** `PartWriteTxn::ensureBlobPresent` owns the state machine. `Backend::publishBlob` owns only unconditional streaming or native same-store-copy transport and returns no incarnation token. A shared monotonic bit on `BlobSource` prevents reuse of a staged envelope after any publication attempt. Writer dependencies record `Materialized` or `TrustedManifest` proof instead of tokens. A small provider-neutral `NativeOnly` copy capability replaces the fork-specific conditional-copy surface without changing ordinary `copyObject` fallback behavior.

**Tech Stack:** TLA+/TLC, C++23, ClickHouse object-storage abstractions, AWS SDK for C++, Google Cloud Storage XML API, GoogleTest, pytest/Praktika integration tests, Ninja, `ca-soak`.

**Spec:** `docs/superpowers/specs/2026-08-21-cas-unconditional-blob-publication-design.md` revision 3 or later.

## Hard gates and global constraints {#hard-gates-and-global-constraints}

- Tasks 1 and 2 are a hard formal-methods gate. Do not edit production C++, production headers, C++ tests, settings, or integration tests until both TLA+ tasks pass exactly as specified and their current result documents are committed.
- A sabotage passes only when TLC reports the named invariant violation. Any nonzero exit, different invariant, deadlock, timeout, parse error, or state-space error is a failed gate.
- A witness passes only when its required action/state is reached and the safe invariants remain green. A green but unreachable witness is vacuous and fails the gate.
- If the focused model reveals a protocol defect, stop. Revise the design specification, obtain review, and only then update this plan. Do not patch C++ around an unproved protocol.
- Read the specification and `docs/superpowers/cas/AGENTS.md` before implementation. The approved specification defines semantics and wins over this plan if they diverge.
- Preserve the public configuration and wire behavior of non-CAS S3 and GCS users. In particular, ordinary `IObjectStorage::copyObject`, `gcp_oauth`, `gcs_hmac`, ETag handling, request headers, retry behavior, and client-side copy fallback remain unchanged.
- Conditional mode remains required for mutable CAS objects, native-token `HEAD`, and exact-token deletion. Only blob-body publication leaves that mode.
- Do not add compatibility aliases for pre-release CAS-only APIs or settings removed by the specification.
- Use Allman braces. Refer to functions without call parentheses in prose and comments. Say “exception” rather than “crash” for `LOGICAL_ERROR` behavior.
- Follow TDD. Each production task starts with a focused failing test whose failure names the missing contract, not an unrelated compile/environment problem.
- After adding a red test, rebuild `unit_tests_dbms` before listing or running it. An exact compiler failure on the newly referenced API is valid red evidence; record the nonzero build status and match the missing symbol/type in the build log. If the test compiles, list it from the rebuilt binary, run it, require a nonzero test exit, and match the intended failed assertion. Never run a newly added test from the pre-change binary.
- Before a filtered GoogleTest red or green run, capture `--gtest_list_tests`; after the run, assert a nonzero execution count with:

```bash
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' <log>
```

- Build with `ninja` without `-j`. Redirect every build and test command to a unique log under `build/`; use the repository build/test skills and a subagent to summarize each log as required by repository instructions.
- Use `apply_patch` for edits. Preserve unrelated worktree changes. Before every commit, run `git diff --check`, inspect the staged diff, and stage only files named by the task. Never amend or rebase.
- Every intermediate commit must compile and must be behaviorally coherent. New APIs are introduced beside old ones, the writer switches atomically, and only then are dead APIs removed.
- The real-GCS lane and the performance acceptance in Tasks 10 and 11 are release gates. Missing credentials may defer release readiness but may not be replaced with mock evidence.

## Target interfaces and ownership {#target-interfaces-and-ownership}

`CasBackend.h` owns the transport-only publication request:

```cpp
struct StreamingBlobPublication
{
    uint64_t payload_size;
    String fresh_envelope;
    std::function<std::unique_ptr<ReadBuffer>()> open_payload;
};

struct VerbatimStagedBlobPublication
{
    String object_key;
    uint64_t object_size;
};

using BlobPublication = std::variant<StreamingBlobPublication, VerbatimStagedBlobPublication>;

struct BlobPublishRequest
{
    String destination_key;
    BlobPublication publication;
};

virtual void publishBlob(const BlobPublishRequest & request) = 0;
```

`CasPartWriteTxn.h` owns logical source state and dependency proof:

```cpp
struct BlobSource
{
    uint64_t size;
    std::function<std::unique_ptr<ReadBuffer>()> open;
    std::optional<String> server_side_copy_from;
    std::shared_ptr<std::atomic<bool>> publication_attempted
        = std::make_shared<std::atomic<bool>>(false);

    bool beginPublication() const
    {
        return !publication_attempted->exchange(true, std::memory_order_acq_rel);
    }
};

enum class BlobDependencyProof : uint8_t
{
    Materialized,
    TrustedManifest,
};

struct BlobDepRecord
{
    ObjectKind kind;
    uint64_t size;
    BlobDependencyProof proof;
};
```

The exact staged-source representation may use a named descriptor rather than `optional<String>`, but every copied/moved `BlobSource` and `BlobUploadRequest` must share the same atomic state.

`WriteSettings.h` owns a narrow native-copy requirement:

```cpp
enum class ObjectStorageCopyMode : uint8_t
{
    Default,
    NativeOnly,
};

ObjectStorageCopyMode object_storage_copy_mode = ObjectStorageCopyMode::Default;
```

`IObjectStorage` adds `supportsCopyMode`. `Default` preserves existing behavior. `NativeOnly` means that `copyObject` must use provider-native same-store copy or throw before any client-side fallback. CAS sets it only for `VerbatimStagedBlobPublication`; no user-facing setting is added.

Diagnostics use independent dimensions rather than a branch-product enum:

```cpp
enum class BlobMaterializationAction : uint8_t { Observed, Published };
enum class BlobPublicationReason : uint8_t { Absent, Condemned };
enum class BlobPublicationTransport : uint8_t { Streaming, ServerSideCopy };

struct BlobUploadDiagnostics
{
    BlobMaterializationAction action;
    std::optional<BlobPublicationReason> reason;
    std::optional<BlobPublicationTransport> transport;
};
```

Only `PartWriteTxn` chooses `Observed` versus `Published`, reason, and transport. `Backend` never performs `HEAD`, reads CAS metadata, decides whether the destination is condemned, or returns a token.

---

### Task 1: Prove the focused blob-publication protocol in TLA+ {#task-1-prove-the-focused-blob-publication-protocol-in-tla}

**Gate:** No production work may start until this task is green and committed.

**Files:**

- Create: `docs/superpowers/models/CaBlobPublishCore.tla`
- Create: `docs/superpowers/models/CaBlobPublishCore_safe.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_adopt_condemned.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_reuse_condemned_envelope.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_recopy_after_condemned.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_recopy_after_absent.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_first_condemned_then_copy.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_unconditional_delete.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_ready_without_reobserve.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_publish_before_precommit.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_skip_meta_clean.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_commit_after_fence.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_sab_wrong_payload.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_witness_racing_publishers.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_witness_staged_retag.cfg`
- Create: `docs/superpowers/models/CaBlobPublishCore_witness_late_landing.cfg`
- Create: `docs/superpowers/models/run_blobpublish.sh`
- Create: `docs/superpowers/models/CaBlobPublishCore_RESULTS.md`

- [ ] **Step 1: Define the finite state and explicit identities**

Model two writers and one GC. Include writer phase, durable precommit, observed body/meta/token, fence generation, readiness and commit state; body presence, logical payload, envelope, token and next generation; metadata state/version; queued exact deletes; fixed staged envelope; and per-writer `publication_attempted`.

Model both token families without provider control flow: ETag-style tokens are a deterministic function of complete envelope plus payload, while generation-style tokens consume `nextToken` on every landed write. Both feed the same exact-delete action and invariants.

- [ ] **Step 2: Encode the production decision boundary**

Make `HEAD` and metadata observation explicit. The publication action must atomically change `publication_attempted` from false to true before a backend landing can occur. Permit verbatim staged copy only on that false-to-true transition after an absent observation. A first `Condemned` observation and every later attempt must select a fresh envelope. Model response loss separately from landing, including a late landing after the writer moves to recovery.

- [ ] **Step 3: State named safety invariants**

Use these stable names in the model, configs, runner, and results:

```text
CommittedRefHasContent
ReadyRequiresObservedMaterialization
CondemnedNeedsFreshPublication
FreshAfterCondemned
PublicationAttemptIsMonotonic
VerbatimCopyOnlyFirstAbsent
ExactDeleteCannotRemoveFreshIncarnation
PublicationRequiresDurablePrecommit
ReadyRequiresCleanMeta
FencedWriterCannotCommit
KeyNamesPayload
```

`CommittedRefHasContent` is logical: commit implies a present body whose payload matches the content-addressed key. It must not require the current token to equal one previously observed by the writer.

- [ ] **Step 4: Add sabotage configs before the safe config**

Each sabotage enables exactly one defect and must violate exactly the mapped invariant:

| Config | Expected invariant |
|---|---|
| `sab_adopt_condemned` | `CondemnedNeedsFreshPublication` |
| `sab_reuse_condemned_envelope` | `FreshAfterCondemned` |
| `sab_recopy_after_condemned` | `ExactDeleteCannotRemoveFreshIncarnation` |
| `sab_recopy_after_absent` | `ExactDeleteCannotRemoveFreshIncarnation` |
| `sab_first_condemned_then_copy` | `VerbatimCopyOnlyFirstAbsent` |
| `sab_unconditional_delete` | `ExactDeleteCannotRemoveFreshIncarnation` |
| `sab_ready_without_reobserve` | `ReadyRequiresObservedMaterialization` |
| `sab_publish_before_precommit` | `PublicationRequiresDurablePrecommit` |
| `sab_skip_meta_clean` | `ReadyRequiresCleanMeta` |
| `sab_commit_after_fence` | `FencedWriterCannotCommit` |
| `sab_wrong_payload` | `KeyNamesPayload` |

The three staged-copy sabotages must include byte-identical ETag reproduction and a queued old-token delete. `sab_first_condemned_then_copy` must exercise: initial `Condemned` → ambiguous retagged PUT → retry `HEAD` miss → forbidden original staged copy.

- [ ] **Step 5: Add non-vacuity witnesses**

Add action predicates and configs proving that the explored safe state space reaches:

- two equivalent writers publishing after the same miss;
- absent staged copy followed by `Condemned` re-observation and retagged streaming;
- response loss, recovery `HEAD`, and a late landing.

The runner must reject a witness config unless TLC reports the witness action reached and all safety invariants remain green.

- [ ] **Step 6: Write the exact-result runner**

Follow `run_condemnmarker.sh`: pin `tla2tools.jar`, run sabotages first, parse the exact violated invariant, then run safe and witness configs, and print `ALL EXPECTATIONS MET` only when every row matches. Store TLC scratch state under repository `tmp`; never accept timeout as evidence.

- [ ] **Step 7: Run the entire battery**

```bash
mkdir -p build tmp
docs/superpowers/models/run_blobpublish.sh > build/task1_blobpublish_tla.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task1_blobpublish_tla.log
```

Review the complete log. Record TLC version, command, config table, state counts, elapsed times, exact sabotage invariants, witness reachability, and safe result in `CaBlobPublishCore_RESULTS.md`. Because this is a new documentation file, give it the complete required frontmatter and explicit anchors on every heading.

- [ ] **Step 8: Re-run from the committed inputs and commit the formal gate**

```bash
docs/superpowers/models/run_blobpublish.sh > build/task1_blobpublish_tla_rerun.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task1_blobpublish_tla_rerun.log
git diff --check
git add docs/superpowers/models/CaBlobPublishCore.tla docs/superpowers/models/CaBlobPublishCore_*.cfg docs/superpowers/models/run_blobpublish.sh docs/superpowers/models/CaBlobPublishCore_RESULTS.md
git commit -m 'Model unconditional CAS blob publication in `CaBlobPublishCore`'
```

Stop here if any result differs. Do not begin Task 2 or production work.

---

### Task 2: Audit affected TLA+ models and close the formal gate {#task-2-audit-affected-tla-models-and-close-the-formal-gate}

**Gate:** No production work may start until this task is green and committed.

**Files:**

- Modify: `docs/superpowers/models/CaGcCondemnMarkerGate.tla`
- Create: `docs/superpowers/models/CaGcCondemnMarkerGate_RESULTS.md`
- Modify: `docs/superpowers/models/CaIncarnationCore.tla`
- Modify: `docs/superpowers/models/README.md`

- [ ] **Step 1: Inventory every potentially stale formal artifact**

```bash
rg -n 'WCreate|WOverwrite|[Rr]esurrect|tokened|adopted.*tok|NoDangle|conditional create|write-once blob|promoteStaged|publication' docs/superpowers/models > build/task2_tla_semantic_inventory.log
```

Classify every hit. Historical result files remain historical unless their input changed; current runners, configs, model comments, and current result documents must describe the new protocol.

- [ ] **Step 2: Correct the general `NoDangle` statement**

In `CaGcCondemnMarkerGate.tla`, replace the general token-equality implication with logical presence/content identity. Equivalent replacement is allowed to change the token. Confirm that the missing-marker sabotage still makes the body absent and therefore still violates `NoDangle`; do not weaken the model until the original bug disappears.

- [ ] **Step 3: Narrow the broad model's comments**

Keep `CaIncarnationCore` as the broader incarnation/GC model. Update comments around `WCreate`, `WOverwrite`, and resurrection to point to `CaBlobPublishCore` for split `HEAD`/publication, staged-envelope reuse, and `publication_attempted`. Do not duplicate the focused transition system into the broad model.

- [ ] **Step 4: Re-run changed and canonical batteries**

```bash
docs/superpowers/models/run_condemnmarker.sh > build/task2_condemnmarker_tla.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task2_condemnmarker_tla.log
docs/superpowers/models/run_tlc.sh > build/task2_incarnation_tla.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task2_incarnation_tla.log
```

If `run_tlc.sh` has documented bounded/incomplete rows, require its own exact expected-result parser to accept them; do not replace it with a loose “no error” grep.

- [ ] **Step 5: Document current results and model routing**

Write `CaGcCondemnMarkerGate_RESULTS.md` from the new run, with complete required frontmatter and explicit anchors on every heading. Update `README.md` to make `CaBlobPublishCore` authoritative for blob publication and to state the narrower role of `CaIncarnationCore`. Record why `CaGcRoundDeferCore` needs no semantic change if its `NoDangle` already uses logical presence.

- [ ] **Step 6: Re-run both formal gates together and commit**

```bash
docs/superpowers/models/run_blobpublish.sh > build/task2_blobpublish_regression.log 2>&1
docs/superpowers/models/run_condemnmarker.sh > build/task2_condemnmarker_rerun.log 2>&1
docs/superpowers/models/run_tlc.sh > build/task2_incarnation_rerun.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task2_blobpublish_regression.log
grep -q '^ALL EXPECTATIONS MET$' build/task2_condemnmarker_rerun.log
grep -q '^ALL EXPECTATIONS MET$' build/task2_incarnation_rerun.log
git diff --check
git add docs/superpowers/models/CaGcCondemnMarkerGate.tla docs/superpowers/models/CaGcCondemnMarkerGate_RESULTS.md docs/superpowers/models/CaIncarnationCore.tla docs/superpowers/models/README.md
git commit -m 'Align CAS TLA+ models with unconditional blob publication'
```

Only this successful commit opens the production-code gate.

---

### Task 3: Add a native-only copy transport capability {#task-3-add-a-native-only-copy-transport-capability}

**Files:**

- Modify: `src/IO/WriteSettings.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp`
- Modify: `src/IO/S3/copyS3File.h`
- Modify: `src/IO/S3/copyS3File.cpp`
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp`
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`

- [ ] **Step 1: Add failing native-copy tests**

Add `S3ObjectStorageConditionalOpsTest.DefaultCopyMayFallback`, `S3ObjectStorageConditionalOpsTest.NativeOnlyCopyUsesNativeTransport`, and `S3ObjectStorageConditionalOpsTest.NativeOnlyCopyNeverFallsBack`. They prove `Default` preserves ordinary native-copy-or-client-fallback behavior, `NativeOnly` performs native same-store copy, and `NativeOnly` throws before a client-side fallback when native copy is unavailable. Assert ordinary copy carries no conditional request mode, `If-None-Match`, or destination precondition.

- [ ] **Step 2: Run the red tests with a non-vacuity check**

```bash
set +e
ninja -C build unit_tests_dbms > build/task3_native_copy_red_build.log 2>&1
red_build_rc=$?
set -e
if [[ $red_build_rc -ne 0 ]]; then
    grep -Eq 'error:.*(ObjectStorageCopyMode|NativeOnly|supportsCopyMode)|(ObjectStorageCopyMode|NativeOnly|supportsCopyMode).*was not declared' build/task3_native_copy_red_build.log
else
    build/src/unit_tests_dbms --gtest_list_tests > build/task3_native_copy_list.log 2>&1
    grep -q 'DefaultCopyMayFallback' build/task3_native_copy_list.log
    grep -q 'NativeOnlyCopyUsesNativeTransport' build/task3_native_copy_list.log
    grep -q 'NativeOnlyCopyNeverFallsBack' build/task3_native_copy_list.log
    set +e
    build/src/unit_tests_dbms --gtest_filter='S3ObjectStorageConditionalOpsTest.DefaultCopyMayFallback:S3ObjectStorageConditionalOpsTest.NativeOnlyCopyUsesNativeTransport:S3ObjectStorageConditionalOpsTest.NativeOnlyCopyNeverFallsBack' > build/task3_native_copy_red.log 2>&1
    red_test_rc=$?
    set -e
    test "$red_test_rc" -ne 0
    grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task3_native_copy_red.log
fi
```

The expected failure is missing copy-mode enforcement.

- [ ] **Step 3: Add the provider-neutral mode and capability**

Add `ObjectStorageCopyMode` and `WriteSettings::object_storage_copy_mode`. Add:

```cpp
virtual bool supportsCopyMode(ObjectStorageCopyMode mode) const
{
    return mode == ObjectStorageCopyMode::Default;
}
```

Override it in `S3ObjectStorage`: `Default` is always supported; `NativeOnly` reflects the existing `allow_native_copy` decision. This predicate is transport capability, not CAS capability and not a provider-name check.

- [ ] **Step 4: Make native-only failure explicit without changing default copy**

Pass the copy mode through `S3ObjectStorage::copyObject` to `copyS3File`. When `NativeOnly` is requested, reject disabled/inapplicable native copy and set `allow_fallback=false`. Keep every existing `Default` branch byte-for-byte equivalent, including client-side read/write fallback.

- [ ] **Step 5: Run focused and ordinary-copy regression suites**

```bash
ninja -C build unit_tests_dbms > build/task3_native_copy_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='*CopyObject*:*NativeOnly*:*CopyS3File*' > build/task3_native_copy_green.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task3_native_copy_green.log
```

- [ ] **Step 6: Commit Task 3**

```bash
git add src/IO/WriteSettings.h src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp src/IO/S3/copyS3File.h src/IO/S3/copyS3File.cpp src/IO/tests/gtest_writebuffer_s3.cpp src/Disks/tests/gtest_cas_s3_staging.cpp
git commit -m 'Add native-only object-storage copy mode'
```

---

### Task 4: Add the transport-only `Backend::publishBlob` API {#task-4-add-the-transport-only-backend-publishblob-api}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.cpp`
- Update direct `Backend` test implementations in: `src/Disks/tests/gtest_cas_backend.cpp`, `src/Disks/tests/gtest_cas_decommission.cpp`, `src/Disks/tests/gtest_cas_mount.cpp`, `src/Disks/tests/gtest_cas_part_write.cpp`, `src/Disks/tests/gtest_cas_pool.cpp`, `src/Disks/tests/gtest_cas_probe.cpp`
- Update the temporary cache test double in: `src/Disks/tests/gtest_ca_dedup_cache.cpp`
- Test: `src/Disks/tests/gtest_cas_backend.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`

- [ ] **Step 1: Freeze the direct-subclass inventory and add failing backend-contract tests**

Before editing `Backend`, run and save:

```bash
rg -n 'public [A-Za-z0-9_:]*Backend\b|struct [A-Za-z0-9_]+ final : Backend' src/Disks --glob '*.{h,cpp}' > build/task4_backend_subclasses.log
```

Classify direct `Backend` subclasses separately from subclasses of `InMemoryBackend` or test helpers. The expected direct implementation files are exactly the three production headers plus the seven test files named above. A new unclassified direct subclass blocks the task.

Cover streaming `[fresh_envelope][payload]`, exact payload count, short and long source cancellation before visibility, atomic in-memory visibility, verbatim staged bytes, no returned token, Default request mode, ordinary retry profile, and a streaming body above the former GCS cap selecting multipart rather than throwing. Assert a response without ETag/generation still succeeds.

- [ ] **Step 2: Run the focused red tests**

```bash
set +e
ninja -C build unit_tests_dbms > build/task4_publish_blob_red_build.log 2>&1
red_build_rc=$?
set -e
if [[ $red_build_rc -ne 0 ]]; then
    grep -Eq 'error:.*(publishBlob|BlobPublishRequest|StreamingBlobPublication)|(publishBlob|BlobPublishRequest|StreamingBlobPublication).*was not declared' build/task4_publish_blob_red_build.log
else
    build/src/unit_tests_dbms --gtest_list_tests > build/task4_publish_blob_list.log 2>&1
    grep -Eq 'PublishBlob' build/task4_publish_blob_list.log
    set +e
    build/src/unit_tests_dbms --gtest_filter='CASBackend.*PublishBlob*:CASInMemory.*PublishBlob*:CASObjectStorageBackend.*PublishBlob*:CASBackendGeneration.*PublishBlob*' > build/task4_publish_blob_red.log 2>&1
    red_test_rc=$?
    set -e
    test "$red_test_rc" -ne 0
    grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task4_publish_blob_red.log
fi
```

- [ ] **Step 3: Add the publication variant and virtual method**

Implement the target interfaces from this plan in `CasBackend.h`. Do not make `publishBlob` a wrapper over `putIfAbsentStream`, `promoteStaged`, or `resurrect`; their preconditions and return contracts are intentionally different. Keep the old virtuals temporarily so this commit does not switch the writer.

- [ ] **Step 4: Implement streaming publication**

For native object storage, call ordinary `writeObject` with `WriteMode::Rewrite`, Default request mode, and ordinary retry settings. Write the fresh envelope, stream exactly `payload_size`, cancel before finalize on mismatch, and finalize without extracting a response token. Generation stores must be eligible for normal multipart.

For emulated/local mode, preserve complete-object atomic visibility and the existing one-body memory bound. Do not broaden this task into a spill-to-disk redesign; Task 12 updates the corresponding backlog item.

- [ ] **Step 5: Implement verbatim staged publication**

Use ordinary same-store `copyObject` with `ObjectStorageCopyMode::NativeOnly`. The staged object is already size-verified and includes its envelope. Never silently fall back to streaming or client-side copy inside the backend; transport selection belongs to `PartWriteTxn`.

- [ ] **Step 6: Instrument physical publication**

Make `InstrumentedBackend::publishBlob` delegate once and record a physical blob write. Do not infer absent/condemned reason in the backend. Keep decision diagnostics in the writer; remove obsolete instrumentation only after the writer switches.

- [ ] **Step 7: Build and run backend suites**

```bash
ninja -C build unit_tests_dbms > build/task4_publish_blob_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASBackend.*:CASInMemory.*:CASInstrumentedBackend.*:CASObjectStorageBackend.*:CASBackendGeneration.*' > build/task4_publish_blob_green.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task4_publish_blob_green.log
```

- [ ] **Step 8: Commit Task 4**

Stage only the files listed above and commit:

```bash
git commit -m 'Add unconditional `Backend::publishBlob` transport'
```

---

### Task 5: Make writer dependency proof explicit {#task-5-make-writer-dependency-proof-explicit}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Test: `src/Disks/tests/gtest_cas_part_write.cpp`
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`

- [ ] **Step 1: Add failing proof-state tests**

Prove that observed and successfully published blobs produce `Materialized`, `adoptEvidence` produces `TrustedManifest` without I/O, failed/pending uploads produce no dependency, promotion accepts exactly the two proofs, and a missing dependency fails closed.

- [ ] **Step 2: Rebuild and establish the proof-state red result**

```bash
set +e
ninja -C build unit_tests_dbms > build/task5_dependency_proof_red_build.log 2>&1
red_build_rc=$?
set -e
if [[ $red_build_rc -ne 0 ]]; then
    grep -Eq 'error:.*(BlobDependencyProof|Materialized|TrustedManifest)|(BlobDependencyProof|Materialized|TrustedManifest).*was not declared' build/task5_dependency_proof_red_build.log
else
    build/src/unit_tests_dbms --gtest_list_tests > build/task5_dependency_proof_list.log 2>&1
    grep -Eq 'DependencyProof|Pending.*Dependency' build/task5_dependency_proof_list.log
    set +e
    build/src/unit_tests_dbms --gtest_filter='CASPartWrite.*DependencyProof*:CASPartWrite.*Pending*Dependency*:CASUploadFanout.*DependencyProof*' > build/task5_dependency_proof_red.log 2>&1
    red_test_rc=$?
    set -e
    test "$red_test_rc" -ne 0
    grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task5_dependency_proof_red.log
fi
```

- [ ] **Step 3: Replace token/boolean control state**

Add `BlobDependencyProof` and replace `BlobDepRecord::token` plus `adopted`. Keep tokens in `HeadResult`, metadata CAS, GC, and exact deletion. Replace `depIsTokened`/`isTrustedAdopt` with a proof query used by production and tests.

- [ ] **Step 4: Remove pending dependency recording**

Delete `recordPendingBlobDep` and both calls in `ContentAddressedTransaction.cpp`. A fan-out result enters `deps` only after success. Promotion of an incomplete fan-out observes a missing proof and throws.

- [ ] **Step 5: Convert promotion to the enum**

`Materialized` is edge-protected physical evidence. `TrustedManifest` follows the existing trusted-manifest branch and event accounting. No token value participates in writer readiness.

- [ ] **Step 6: Run proof and promotion suites**

```bash
ninja -C build unit_tests_dbms > build/task5_dependency_proof_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASPartWrite.*:CASUploadFanout.*' > build/task5_dependency_proof_tests.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task5_dependency_proof_tests.log
```

- [ ] **Step 7: Commit Task 5**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp src/Disks/tests/gtest_cas_part_write.cpp src/Disks/tests/gtest_cas_upload_fanout.cpp
git commit -m 'Represent CAS blob dependencies with explicit proof'
```

---

### Task 6: Switch the writer atomically to `HEAD` then unconditional publication {#task-6-switch-the-writer-atomically-to-head-then-unconditional-publication}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Test: `src/Disks/tests/gtest_cas_upload_detached.cpp`
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`
- Test: `src/Disks/tests/gtest_cas_part_write.cpp`
- Test: `src/Disks/tests/gtest_cas_fence_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`

- [ ] **Step 1: Add the complete failing state-machine matrix**

Add deterministic tests for all cases required by the specification: fresh miss with no pre-publication meta GET; existing `Clean`; absent metadata backfill; absent body with stale `Condemned`; present `Condemned`; two racing writers; landed and non-landed ambiguity; fence loss before and after publication; wrong-size source; and equal proof for streaming/copy.

Add three separate ETag-faithful staged regressions:

1. ambiguous verbatim copy → observed `Condemned` → retagged publication → queued old `deleteExact` cannot remove it;
2. ambiguous verbatim copy lands → its old `deleteExact` removes it before retry `HEAD` → retry observes absence while another delete for the same ETag remains queued → replacement must retag/stream and the queued retry must miss;
3. initial `Condemned` → ambiguous retagged PUT → retry `HEAD` miss → original staged copy remains forbidden.

Name the tests `StagedCopyCondemnedRetryRetagsBeforeQueuedDelete`, `StagedCopyDeletedBeforeAbsentRetryRetagsBeforeQueuedDelete`, and `FirstCondemnedAttemptThenAbsentRetryNeverRecopies`. Give them distinct fault scripts. The second test must prove both that native verbatim copy was not called again and that the second queued exact delete cannot remove the replacement; it is not covered by the first test's present-`Condemned` observation.

Copy and move `BlobSource` through the real fan-out request shape and assert all copies share `publication_attempted`.

- [ ] **Step 2: Run the red matrix and confirm old branch behavior**

```bash
set +e
ninja -C build unit_tests_dbms > build/task6_blob_state_red_build.log 2>&1
red_build_rc=$?
set -e
test "$red_build_rc" -eq 0
build/src/unit_tests_dbms --gtest_list_tests > build/task6_blob_state_list.log 2>&1
grep -Eq 'CASUploadDetached|CASUploadFanout|CASPartWrite|CASFenceGeneration|CASS3Staging' build/task6_blob_state_list.log
grep -q 'StagedCopyCondemnedRetryRetagsBeforeQueuedDelete' build/task6_blob_state_list.log
grep -q 'StagedCopyDeletedBeforeAbsentRetryRetagsBeforeQueuedDelete' build/task6_blob_state_list.log
grep -q 'FirstCondemnedAttemptThenAbsentRetryNeverRecopies' build/task6_blob_state_list.log
set +e
build/src/unit_tests_dbms --gtest_filter='CASUploadDetached.*:CASUploadFanout.*:CASPartWrite.*:CASFenceGeneration.*:CASS3Staging.*' > build/task6_blob_state_red.log 2>&1
red_test_rc=$?
set -e
test "$red_test_rc" -ne 0
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task6_blob_state_red.log
```

These tests use existing public seams and must compile before the behavior switch. If they do not, fix the test fixture rather than implementing production behavior before obtaining the runtime red result.

- [ ] **Step 3: Add shared monotonic source state**

Add `publication_attempted` and `BlobSource::beginPublication` as shown above. `beginPublication` executes after the final fence check and immediately before any backend publication I/O. It consumes first-attempt status even when the selected transport is streaming, the backend throws, the response is lost, or no object lands.

For S3 staging, make the source re-readable as payload: open the staging object, validate/skip the existing fixed envelope, and expose the payload to a retagged streaming publication. Do not read a condemned destination body.

- [ ] **Step 4: Implement `PartWriteTxn::ensureBlobPresent`**

Replace the split `observeAndAdmit`/`uploadFromSource` decision tree with one bounded outer loop:

1. require live transaction and durable precommit;
2. `HEAD` the blob;
3. if present, validate logical size and load metadata;
4. adopt a non-condemned body, including absent-meta backfill;
5. for absent or `Condemned`, capture/check fence, call `beginPublication`, select transport, and call `publishBlob`;
6. reconcile metadata to compatible `Clean`;
7. return `Materialized` proof;
8. on an ambiguous retryable transport outcome, restart from `HEAD`; propagate deterministic failures.

Do not perform metadata GET before publication on the absent path. Reuse a loaded `Condemned` marker/version when possible. Do not reconstruct `conditionalCreateControlled` under another name.

- [ ] **Step 5: Select staged transport by state, never by fallback**

Select `VerbatimStagedBlobPublication` only when `beginPublication` reports first, the just-observed body is absent, and a staged object exists. Select `StreamingBlobPublication` for first-plus-`Condemned` and every later publication, including later absence. A native copy failure propagates; it does not select streaming as a fallback.

- [ ] **Step 6: Replace branch-product diagnostics**

Return/action-log the independent action, optional reason, and optional transport dimensions. Keep `CASBlobBodyPutAvoided` only for a safe present-body observation. Do not increment it on a condemned `HEAD` that later publishes.

- [ ] **Step 7: Build and run the full deterministic writer matrix**

```bash
ninja -C build unit_tests_dbms > build/task6_blob_state_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASUploadDetached.*:CASUploadFanout.*:CASPartWrite.*:CASFenceGeneration.*:CASS3Staging.*' > build/task6_blob_state_green.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task6_blob_state_green.log
```

- [ ] **Step 8: Commit the atomic behavior switch**

Before committing, search production call sites and confirm all blob writers enter `ensureBlobPresent`, while the old backend/controller APIs are merely dead and retained for the next cleanup tasks.

```bash
git commit -m 'Publish CAS blobs after mandatory `HEAD`'
```

---

### Task 7: Remove conditional blob APIs, controller state, and cache {#task-7-remove-conditional-blob-apis-controller-state-and-cache}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInstrumentedBackend.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
- Modify: `src/IO/WriteBufferFromS3.cpp`
- Modify: `src/Common/CurrentMetrics.cpp`
- Modify: `src/Common/ProfileEvents.cpp`
- Delete: `src/Disks/tests/gtest_ca_dedup_cache.cpp`
- Modify: `src/Disks/tests/cas_test_helpers.h`
- Test: `src/Disks/tests/gtest_ca_wiring.cpp`
- Test: `src/Disks/tests/gtest_cas_backend.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_contract.cpp`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_bootstrap_ordering.cpp`
- Test: `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`
- Test: `src/Disks/tests/gtest_cas_decommission.cpp`
- Test: `src/Disks/tests/gtest_cas_fence_generation.cpp`
- Test: `src/Disks/tests/gtest_cas_forget.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_ack_floor.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_frontier_gate.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_leak.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`
- Test: `src/Disks/tests/gtest_cas_lifecycle_condition.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`
- Test: `src/Disks/tests/gtest_cas_observability.cpp`
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`
- Test: `src/Disks/tests/gtest_cas_part_write.cpp`
- Test: `src/Disks/tests/gtest_cas_pool.cpp`
- Test: `src/Disks/tests/gtest_cas_probe.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_writer.cpp`
- Test: `src/Disks/tests/gtest_cas_request_control.cpp`
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`
- Test: `src/Disks/tests/gtest_cas_sentinel_probe.cpp`
- Test: `src/Disks/tests/gtest_cas_settings.cpp`
- Test: `src/Disks/tests/gtest_cas_blob_meta.cpp`
- Test: `src/Disks/tests/gtest_cas_upload_detached.cpp`
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`
- Modify: `tests/queries/0_stateless/05025_cas_attach_partition_cross_disk.sh`
- Delete: `utils/ca-soak/configs/storage_conf_small_dedup_cache_ch1.xml`
- Delete: `utils/ca-soak/configs/storage_conf_small_dedup_cache_ch2.xml`
- Delete: `utils/ca-soak/docker-compose-small_dedup_cache.yml`
- Modify: `utils/ca-soak/docker-compose-tuned.yml`
- Modify: `utils/ca-soak/scenarios/framework/cluster_boot.py`
- Modify: `utils/ca-soak/scenarios/tests/test_render_tuned_config.py`
- Modify: `utils/ca-soak/scenarios/cards/s01_s02_huge_blob.py`
- Modify: `utils/ca-soak/scenarios/cards/s09_s11_mutations.py`
- Modify: `utils/ca-soak/scenarios/cards/s12_s14_faults.py`
- Modify: `utils/ca-soak/scenarios/cards/s19_s22_clone_fetch.py`
- Modify: `utils/ca-soak/scenarios/cards/s23_s27_misc.py`
- Modify: `utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py`
- Modify: `utils/ca-soak/scenarios/ASSUMPTIONS.md`
- Modify: `utils/ca-soak/scenarios/README.md`
- Modify with an explicit historical supersession note: `utils/ca-soak/scenarios/BACKLOG.md`
- Modify with an explicit historical supersession note: `utils/ca-soak/scenarios/RUN_HISTORY.md`

- [ ] **Step 1: Freeze the complete old-surface inventory and add reduced-surface tests**

Generate both inventories before editing:

```bash
rg -l '\b(putIfAbsentStream|promoteStaged|resurrect)\b' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed src/Disks/tests --glob '*.{h,cpp}' | sort > build/task7_old_backend_surface.log
rg -l 'deduplication_cache_bytes|deduplication_head_first_min_bytes|CASDeduplicationCache|CASBlobDeduplicationCacheHit|CASBlobHeadFirst' src tests utils/ca-soak docs/en/antalya/cas docs/en/operations/storing-data.md --glob '!utils/ca-soak/logs/**' --glob '!utils/ca-soak/logs_archive/**' | sort > build/task7_cache_surface.log
```

The file list above is the classified live-code/config/test inventory on the plan revision. A newly found live file must be added to this task before editing or staging it; historical docs are handled in Task 12 with explicit supersession markers.

Pin that mutable `putIfAbsent`, `casPut`, native-token `HEAD`, and exact delete remain. Add `CASContentAddressedSettings.RemovedCacheSettingsAreRejected`; before the implementation it must fail because the two settings are still accepted. The Task 6 writer tests already prove blob publication does not enter the conditional-create controller. Rename the GCS cap test to `gcs_max_conditional_put_bytes` and assert the old name is rejected.

- [ ] **Step 2: Rebuild and establish the reduced-surface red result**

```bash
set +e
ninja -C build unit_tests_dbms > build/task7_reduced_surface_red_build.log 2>&1
red_build_rc=$?
set -e
test "$red_build_rc" -eq 0
build/src/unit_tests_dbms --gtest_list_tests > build/task7_reduced_surface_list.log 2>&1
grep -q 'RemovedCacheSettingsAreRejected' build/task7_reduced_surface_list.log
set +e
build/src/unit_tests_dbms --gtest_filter='CASContentAddressedSettings.RemovedCacheSettingsAreRejected' > build/task7_reduced_surface_red.log 2>&1
red_test_rc=$?
set -e
test "$red_test_rc" -ne 0
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task7_reduced_surface_red.log
```

- [ ] **Step 3: Remove blob-only backend operations**

Delete `putIfAbsentStream`, `promoteStaged`, and `resurrect` from `Backend`, all implementations, instrumentation, overload imports, comments, and test doubles. Keep small-object `putIfAbsent` because manifests/control objects still require write-once semantics.

- [ ] **Step 4: Remove the generic conditional-create seam**

Delete `CasCreateOutcome`, `CasCreateResult`, and `conditionalCreateControlled` if no non-blob caller remains. Delete `stagingConditionalCreate` from `CasRefLedger` and `CasPool`. Remove their tests rather than preserving compatibility wrappers.

- [ ] **Step 5: Remove the presence cache and head-first tuning**

Delete the dedup cache object, config plumbing, lookup/insert methods, `deduplication_cache_bytes`, and `deduplication_head_first_min_bytes`. Delete cache gauges and cache-only ProfileEvents. Remove tautological `CASBlobHeadFirst`; retain `CASBlobBodyPutAvoided` with an accurate “safe present observation avoided a physical body publication” description.

Remove the settings from the active stateless configuration. Delete the S24 small-dedup-cache configs/compose variant and its `smalldedupcache` selector; remove the S24 cache-eviction scenario because the optimized cache no longer exists. Replace the tuned-config renderer test's removed key with the still-supported `manifest_decode_cache_bytes`. Remove deleted counters and threshold assumptions from every named active `ca-soak` card. Keep old run history only with an explicit statement that the cache experiment describes the superseded protocol.

- [ ] **Step 6: Narrow conditional write settings**

Delete `tokenProducingWriteSettings`. Make `conditionalWriteSettings` directly apply `NativeConditional`, single-attempt retry, and the GCS single-PUT cap only to genuine mutable conditional operations. Rename constructor fields and the setting to `gcs_max_conditional_put_bytes` with no alias. Blob `publishBlob` continues to use Default settings and ordinary multipart.

- [ ] **Step 7: Build and run controller/settings/backend/writer suites**

```bash
ninja -C build unit_tests_dbms > build/task7_cleanup_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests > build/task7_cas_list.log 2>&1
grep -Eq 'CASBackend|CASBackendContract|CASMount|CASPool|CASPartFolderAccess|CASRequestControl|CASContentAddressedSettings|CASUploadDetached|CASUploadFanout|CASPartWrite' build/task7_cas_list.log
build/src/unit_tests_dbms --gtest_filter='CAS*:*CAS*' > build/task7_cleanup_tests.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task7_cleanup_tests.log
python3 -m ci.praktika run functional --test 05025_cas_attach_partition_cross_disk --path "$(pwd)/build/programs" > build/task7_05025_stateless.log 2>&1
python3 -m pytest utils/ca-soak/scenarios/tests/test_render_tuned_config.py -q > build/task7_ca_soak_config_tests.log 2>&1
rg -n 'deduplication_cache_bytes|deduplication_head_first_min_bytes|CASDeduplicationCache|CASBlobDeduplicationCacheHit|CASBlobHeadFirst' src tests utils/ca-soak --glob '!utils/ca-soak/logs/**' --glob '!utils/ca-soak/logs_archive/**' > build/task7_live_cache_negative_search.log || true
```

The final search must be empty except for explicit historical supersession text in `BACKLOG.md`/`RUN_HISTORY.md`; inspect and classify every line.

- [ ] **Step 8: Commit Task 7**

```bash
git commit -m 'Remove conditional CAS blob creation state'
```

---

### Task 8: Remove conditional copy and require native S3 staging explicitly {#task-8-remove-conditional-copy-and-require-native-s3-staging-explicitly}

**Files:**

- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h`
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp`
- Modify: `src/IO/S3/copyS3File.h`
- Modify: `src/IO/S3/copyS3File.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp`
- Test: `src/IO/tests/gtest_writebuffer_s3.cpp`
- Test: `src/Disks/tests/gtest_cas_probe.cpp`
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`
- Test: `src/Disks/tests/gtest_ca_wiring.cpp`

- [ ] **Step 1: Add failing mount/selection tests**

Add `CASS3Staging.WritableS3StagingRequiresNativeOnlyCopy`, `CASS3Staging.UnsupportedNativeOnlyCopyDoesNotFallBackToLocal`, `CASS3Staging.GenerationBackendMayUseNativeOnlyCopy`, and `CASWiring.LocalStagingRemainsDefault`. Together they prove that writable `staging_backend=s3` succeeds only when `supportsCopyMode(NativeOnly)` is true, fails closed otherwise, and never falls back to local staging; local staging remains the explicit default; and generation-token GCS is not excluded when native copy is available.

- [ ] **Step 2: Rebuild and establish the mount/selection red result**

```bash
set +e
ninja -C build unit_tests_dbms > build/task8_staging_selection_red_build.log 2>&1
red_build_rc=$?
set -e
test "$red_build_rc" -eq 0
build/src/unit_tests_dbms --gtest_list_tests > build/task8_staging_selection_list.log 2>&1
grep -q 'WritableS3StagingRequiresNativeOnlyCopy' build/task8_staging_selection_list.log
grep -q 'UnsupportedNativeOnlyCopyDoesNotFallBackToLocal' build/task8_staging_selection_list.log
grep -q 'GenerationBackendMayUseNativeOnlyCopy' build/task8_staging_selection_list.log
grep -q 'LocalStagingRemainsDefault' build/task8_staging_selection_list.log
set +e
build/src/unit_tests_dbms --gtest_filter='CASS3Staging.WritableS3StagingRequiresNativeOnlyCopy:CASS3Staging.UnsupportedNativeOnlyCopyDoesNotFallBackToLocal:CASS3Staging.GenerationBackendMayUseNativeOnlyCopy:CASWiring.LocalStagingRemainsDefault' > build/task8_staging_selection_red.log 2>&1
red_test_rc=$?
set -e
test "$red_test_rc" -ne 0
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task8_staging_selection_red.log
```

The tests compile against the `NativeOnly` seam from Task 3 and must fail because the old conditional-copy probe/fallback still governs mount and staging selection.

- [ ] **Step 3: Remove conditional-copy state and probe**

Delete `ConditionalCopyResult`, `IObjectStorage::copyObjectConditional`, the S3 override, `probeConditionalCopy`, `conditional_copy_supported`, and startup probing. Remove conditional destination headers, token extraction, and request-mode parameters added solely for conditional copy from `copyS3File`.

- [ ] **Step 4: Preserve the narrow native-only seam**

Retain `ObjectStorageCopyMode`, `supportsCopyMode`, and `allow_fallback=false` for CAS staged publication. Keep ordinary `copyObject` behavior unchanged. The capability check must derive from actual storage configuration, not endpoint/provider-name heuristics.

- [ ] **Step 5: Simplify staging selection**

When configuration explicitly says `s3`, select S3 staging after the mount capability check. Never silently switch to local staging. Remove GCS-generation special cases. Read-only mounts do not need a publication capability; reject only a configuration that can enter writable staged publication.

- [ ] **Step 6: Run copy, probe, staging, and wiring suites**

```bash
ninja -C build unit_tests_dbms > build/task8_conditional_copy_cleanup_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='*CopyObject*:*CopyS3File*:CASProbe.*:CASS3Staging.*:CASWiring.*' > build/task8_conditional_copy_cleanup_tests.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task8_conditional_copy_cleanup_tests.log
```

- [ ] **Step 7: Commit Task 8**

```bash
git commit -m 'Replace conditional staging copy with native copy'
```

---

### Task 9: Close deterministic AWS/GCS behavior and request-count coverage {#task-9-close-deterministic-aws-gcs-behavior-and-request-count-coverage}

**Files:**

- Modify: `tests/integration/test_cas_gcs/test.py`
- Modify: `tests/integration/test_cas_gcs/gcs_mocks/server.py`
- Modify: `tests/integration/test_cas_s3/test.py`
- Modify: `tests/integration/test_storage_gcp_auth/test.py`

- [ ] **Step 1: Update deterministic GCS expectations**

Permit blob-body multipart in Default mode while keeping mutable conditional writes single-part and NativeConditional. Add request classification so the fixture can distinguish blob body, mutable CAS metadata/control, exact DELETE, ordinary non-CAS traffic, and staged copy.

- [ ] **Step 2: Add request-budget assertions**

For a fresh blob assert exactly one blob `HEAD`, one body PUT/copy, no pre-publication metadata GET, and the existing `Clean` metadata create. For a cold duplicate assert one blob `HEAD`, metadata GET, and no body publication. For condemned staged publication assert staging GET plus retagged body PUT and no conditional-copy request.

- [ ] **Step 3: Pin conditional isolation**

Assert blob PUT/copy remains Default; mutable conditional PUT, native-token `HEAD`, and exact DELETE remain marked and translated. Assert ordinary OAuth and GOOG4 requests preserve existing headers, ETags, cache keys, retry behavior, and copy/delete forms.

- [ ] **Step 4: Run deterministic integration lanes**

```bash
python3 -m ci.praktika run "integration" --test test_cas_gcs > build/task9_cas_gcs_integration.log 2>&1
python3 -m ci.praktika run "integration" --test test_cas_s3 > build/task9_cas_s3_integration.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_gcp_auth > build/task9_gcp_auth_integration.log 2>&1
```

Inspect the test counts and all skipped reasons. A skipped deterministic case is a failed task unless the test is explicitly credential-gated and belongs to Task 10.

- [ ] **Step 5: Commit Task 9**

```bash
git add tests/integration/test_cas_gcs/test.py tests/integration/test_cas_gcs/gcs_mocks/server.py tests/integration/test_cas_s3/test.py tests/integration/test_storage_gcp_auth/test.py
git commit -m 'Test unconditional CAS blob publication across storage dialects'
```

---

### Task 10: Run the mandatory real-storage release gates {#task-10-run-the-mandatory-real-storage-release-gates}

**Files:**

- Modify: `tests/integration/test_gcs_live/test.py`
- Create: `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-live-results.md`

- [ ] **Step 1: Add live GCS blob scenarios**

Use both `gcp_oauth` and `gcs_hmac` where supported. Cover fresh streaming, duplicate adoption, concurrent equivalent publishers, a blob above the former cap, multipart publication, native staged copy, condemned staged retagging, queued old-token delete, and staged-copy ambiguity followed by an absent retry and retagged replacement.

- [ ] **Step 2: Retain ordinary non-CAS characterization**

In the same real-GCS gate, run ordinary non-CAS `HEAD`, GET, LIST, PUT, native copy, single DELETE, batch DELETE, metadata, and multipart. Compare observed ETag/generation and header behavior with the pre-change contract. Mocks are not proof that Google accepts the wire format.

- [ ] **Step 3: Run AWS-compatible and ordinary S3 gates**

Run the CAS S3 integration and `test_storage_s3`. If the local image is unavailable, record that external blocker and require the CI lane; do not treat it as green.

- [ ] **Step 4: Run real GCS with credentials**

```bash
python3 -m ci.praktika run "integration" --test test_gcs_live > build/task10_gcs_live.log 2>&1
python3 -m ci.praktika run "integration" --test test_cas_s3 > build/task10_cas_s3_live.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_s3 > build/task10_storage_s3.log 2>&1
```

No release claim is allowed if real-GCS credentials are absent or any required group skips. Record provider, endpoint class, authentication mode, scenario counts, skips, and results in the live-results document; do not record secrets.
Create the result document with the complete required documentation frontmatter and explicit anchors on every heading.

- [ ] **Step 5: Commit tests and evidence**

```bash
git add tests/integration/test_gcs_live/test.py tests/integration/test_cas_s3 docs/superpowers/cas/2026-08-22-unconditional-blob-publication-live-results.md
git commit -m 'Validate CAS blob publication on real object storage'
```

---

### Task 11: Measure and accept the write-path cost {#task-11-measure-and-accept-the-write-path-cost}

**Files:**

- Modify: `utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py`
- Create: `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md`

- [ ] **Step 1: Update S41 metrics before changing conclusions**

Remove deleted cache/`CASBlobHeadFirst` counters. Report blob `HEAD`, body PUT/copy, metadata GET/create/CAS, per-part and per-GiB request counts, wall time, query duration, and peak resident memory. Distinguish fresh/cold from duplicate/adopt paths.

- [ ] **Step 2: Add a small lone-insert measurement**

Within S41 or a tightly related phase, measure a one-part small insert where the extra serial `HEAD` is visible rather than amortized by wide fan-out. Keep the existing wide 30-column workload for throughput and request-budget evidence.

- [ ] **Step 3: Capture comparable before/after evidence**

Run the same node, endpoint, schema, rows, part count, and profiler settings for the merge-base behavior and target behavior. Record medians over repeated successful runs, variance, request ratios, wall time, and peak memory. Do not compare unlike compose stacks or warm/cold states.

- [ ] **Step 4: Write the acceptance report**

The report must state the measured fresh small-blob latency cost, wide-insert slowdown/speedup, request deltas, duplicate benefit, and whether the result is accepted. The expected logical delta is one blob `HEAD` per fresh blob and no common-path metadata GET; measurements must agree or block completion.
Create the report with the complete required documentation frontmatter and explicit anchors on every heading.

- [ ] **Step 5: Commit Task 11**

```bash
git add utils/ca-soak/scenarios/cards/s41_wide_insert_baseline.py docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md
git commit -m 'Measure unconditional CAS blob publication cost'
```

---

### Task 12: Migrate documentation, backlog, comments, and historical routing {#task-12-migrate-documentation-backlog-comments-and-historical-routing}

**Files:**

- Modify: `docs/en/antalya/cas/architecture/blob-protocol.md`
- Modify: `docs/en/antalya/cas/architecture/backend.md`
- Modify: `docs/en/antalya/cas/architecture/part-lifecycle.md`
- Modify: `docs/en/antalya/cas/architecture/correctness.md`
- Modify: `docs/en/antalya/cas/architecture/design-history.md`
- Modify: `docs/en/antalya/cas/bucket-requirements.md`
- Modify: `docs/en/antalya/cas/configuration.md`
- Modify: `docs/en/antalya/cas/operations/monitoring.md`
- Modify: `docs/en/antalya/cas/roadmap.md`
- Modify: `docs/en/operations/storing-data.md`
- Modify: `docs/superpowers/specs/2026-08-20-cas-gcs-request-isolation-design.md`
- Modify: `docs/superpowers/specs/2026-08-21-cas-object-storage-conditional-operations-proposal.md`
- Modify with an explicit supersession note: `docs/superpowers/plans/2026-08-21-cas-freezeremote-transaction.md`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md`
- Modify: `docs/superpowers/cas/BACKLOG.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/performance.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/operability-and-introspection.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/ref-protocol.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/testing-and-ci.md`
- Audit/modify: `docs/superpowers/cas/BACKLOG/docs-and-cleanup.md`
- Modify: `docs/superpowers/cas/consolidation-2026-08/COVERAGE-MATRIX.md`
- Audit: production/test comments, logs, exceptions, and current model result documents; any stale live-code hit blocks this task and is fixed in a new commit scoped to the owning production task

- [ ] **Step 1: Rewrite user/operator documentation**

Document `HEAD` then unconditional publication, explicit proof, metadata reconciliation, S3 staged-copy first-attempt rule, multipart support, request budget, and the retained mutable conditional/exact-delete requirements. Remove cache settings and rename the conditional cap without an alias. Preserve all explicit heading anchors.

- [ ] **Step 2: Route internal design history**

Add a supersession note to the GCS request-isolation design: typed NativeConditional plumbing remains authoritative, but blob body PUT/copy no longer use it. Narrow the conditional-operations proposal to mutable objects, native-token `HEAD`, and exact deletion. Update the implementation README and coverage matrix rather than rewriting historical review artifacts as if they were current.

- [ ] **Step 3: Resolve backlog entries explicitly**

Close `[gcs-conditional-overwrite-rethink]` with links to the implementation, real-GCS results, and performance report. Reformulate `[emulated-resurrect-should-spill-to-disk]` around the remaining emulated `publishBlob` materialization rather than a deleted `resurrect` API. Record the explicit decision to accept mandatory `HEAD` and its measured cost. Close cache/ProfileEvents items made obsolete. Preserve identifiers and decision history.

- [ ] **Step 4: Audit prose and comments semantically**

Search for “write-once blob”, “conditional create”, `412`, “tokened leaf”, “resurrect”, “single PUT”, “conditional copy”, “HEAD first”, and “dedup cache”. “Resurrection” may remain as lifecycle language, not a backend method or separate correctness branch. Update production comments, test fixture comments, event reasons, logs, and exception text.

- [ ] **Step 5: Run the exact-name negative-search gate**

```bash
rg -n 'putIfAbsentStream|promoteStaged|conditionalCreateControlled|stagingConditionalCreate|copyObjectConditional|deduplication_cache_bytes|deduplication_head_first_min_bytes|gcs_max_token_producing_put_bytes' . > build/task12_removed_names.log || true
rg -n 'write-once blob|conditional create|412|tokened leaf|resurrect|single PUT|conditional copy|HEAD.first|dedup.*cache' docs src tests utils > build/task12_semantic_audit.log || true
```

Inspect the untruncated logs. Every remaining exact-name hit must be marked historical/superseded, be a genuine non-blob concept with an accurate name, or block completion. Do not accept unexplained hits.

- [ ] **Step 6: Commit Task 12**

```bash
git add docs/en/antalya/cas/architecture/blob-protocol.md docs/en/antalya/cas/architecture/backend.md docs/en/antalya/cas/architecture/part-lifecycle.md docs/en/antalya/cas/architecture/correctness.md docs/en/antalya/cas/architecture/design-history.md docs/en/antalya/cas/bucket-requirements.md docs/en/antalya/cas/configuration.md docs/en/antalya/cas/operations/monitoring.md docs/en/antalya/cas/roadmap.md docs/en/operations/storing-data.md docs/superpowers/specs/2026-08-20-cas-gcs-request-isolation-design.md docs/superpowers/specs/2026-08-21-cas-object-storage-conditional-operations-proposal.md docs/superpowers/plans/2026-08-21-cas-freezeremote-transaction.md src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md docs/superpowers/cas/BACKLOG.md docs/superpowers/cas/BACKLOG/formats-and-storage.md docs/superpowers/cas/BACKLOG/performance.md docs/superpowers/cas/BACKLOG/operability-and-introspection.md docs/superpowers/cas/BACKLOG/ref-protocol.md docs/superpowers/cas/BACKLOG/testing-and-ci.md docs/superpowers/cas/BACKLOG/docs-and-cleanup.md docs/superpowers/cas/consolidation-2026-08/COVERAGE-MATRIX.md
git diff --cached --check
git commit -m 'Document unconditional CAS blob publication'
```

Inspect `git diff --cached --name-only` before the commit so this broad documentation stage does not capture unrelated worktree files.

---

### Task 13: Run final formal, build, unit, integration, and audit gates {#task-13-run-final-formal-build-unit-integration-and-audit-gates}

**Files:** Verification only; fix failures in a new commit scoped to their owning task.

- [ ] **Step 1: Re-run the formal gate from scratch**

```bash
docs/superpowers/models/run_blobpublish.sh > build/task13_blobpublish_tla.log 2>&1
docs/superpowers/models/run_condemnmarker.sh > build/task13_condemnmarker_tla.log 2>&1
docs/superpowers/models/run_tlc.sh > build/task13_incarnation_tla.log 2>&1
grep -q '^ALL EXPECTATIONS MET$' build/task13_blobpublish_tla.log
grep -q '^ALL EXPECTATIONS MET$' build/task13_condemnmarker_tla.log
grep -q '^ALL EXPECTATIONS MET$' build/task13_incarnation_tla.log
```

- [ ] **Step 2: Build and run the complete unit gate**

```bash
ninja -C build unit_tests_dbms > build/task13_unit_build.log 2>&1
build/src/unit_tests_dbms --gtest_list_tests > build/task13_cas_list.log 2>&1
grep -Eq 'CASBackend|CASUploadDetached|CASUploadFanout|CASPartWrite|CASS3Staging' build/task13_cas_list.log
build/src/unit_tests_dbms --gtest_filter='CAS*:*CAS*:*SyncAsync*:IOTestAwsS3Client.*:GCSConditionalDialect.*:GOOG4Signer.*:S3ObjectStorageConditionalOpsTest.*' > build/task13_unit_tests.log 2>&1
grep -aEq '^\[==========\] [1-9][0-9]* tests? from [1-9][0-9]* test suites? ran\.' build/task13_unit_tests.log
```

Compare the discovered CAS suite inventory with the pre-change inventory so deleted tests correspond only to deleted APIs/cache, not accidental filter loss.

- [ ] **Step 3: Run deterministic integration gates**

```bash
python3 -m ci.praktika run "integration" --test test_cas_gcs > build/task13_cas_gcs.log 2>&1
python3 -m ci.praktika run "integration" --test test_cas_s3 > build/task13_cas_s3.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_gcp_auth > build/task13_gcp_auth.log 2>&1
python3 -m ci.praktika run "integration" --test test_storage_s3 > build/task13_storage_s3.log 2>&1
```

- [ ] **Step 4: Confirm external release evidence**

Require the real-GCS document to show all credentialed groups passed and the performance document to contain an explicit acceptance. A skipped live gate or unreviewed performance result means implementation may be code-complete but is not release-ready.

- [ ] **Step 5: Repeat structural and documentation audits**

```bash
git diff --check
rg -n 'putIfAbsentStream|promoteStaged|conditionalCreateControlled|stagingConditionalCreate|copyObjectConditional|deduplication_cache_bytes|deduplication_head_first_min_bytes|gcs_max_token_producing_put_bytes' . > build/task13_removed_names.log || true
git status --short
```

Classify every search hit and inspect every changed file. Verify ordinary `copyObject` default behavior and non-CAS GCP tests are still present.

- [ ] **Step 6: Commit only genuine final fixes**

If verification required code/doc changes, add a new non-amended commit named for the actual fix and repeat the affected gate plus Steps 1 and 5. If no changes were required, do not create an empty verification commit.

## Completion criteria {#completion-criteria}

Implementation is complete only when:

1. the focused TLA+ safe model, every exact-invariant sabotage, and every non-vacuity witness pass;
2. affected existing models and current result documents are consistent and green;
3. every blob publication decision starts with `HEAD`, and publication is unconditional only for absent/condemned bodies;
4. `publication_attempted` is shared, monotonic, and consumed before the first publication of any kind;
5. staged verbatim copy is restricted to first-plus-absent; condemned and later attempts retag and stream;
6. dependency proof contains no writer token or pending state;
7. blobs above the former GCS cap use ordinary multipart;
8. mutable conditional operations, native-token `HEAD`, and exact delete retain native-token semantics;
9. conditional blob APIs, conditional-copy API/probe, presence cache, and obsolete settings have no unexplained live reference;
10. explicit S3 staging fails closed without native same-store copy, while ordinary copy fallback is unchanged;
11. fresh-request accounting shows one added blob `HEAD` and no common-path metadata GET;
12. full deterministic unit/integration gates pass, real GCS passes, and non-CAS GCP behavior remains unchanged;
13. the measured latency/request/memory cost is documented and explicitly accepted;
14. user docs, internal docs, backlog, TLA+ docs, comments, logs, exceptions, and tests describe the same protocol.

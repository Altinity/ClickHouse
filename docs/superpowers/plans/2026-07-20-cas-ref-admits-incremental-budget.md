# CAS ref-ledger `admits()` Incremental Budget Accounting — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the O(N)-per-op full re-encode inside `admits()` with two running body-byte totals maintained on `RefTableState`, turning a K-op flush batch against an N-ref table from O(K×N) into O(K), byte-for-byte identical decisions.

**Architecture:** Both budget-relevant encodings (the table snapshot and the hypothetical whole-namespace removal transaction) are pure per-row sums plus O(1) framing. We add two `uint64_t` body-byte counters to `RefTableState`, maintain them at the 5 row-mutation sites in `applyOpInPlace` and the one seeding site in `stateFromSnapshot`, and rewrite `admits()` to read `framing + counter` instead of re-encoding the whole table. Per-row and framing sizes come from small helpers factored out of the two existing encoders, so they are byte-identical to a full encode by construction. A debug-only recompute-and-compare `chassert` plus the existing `AdmitsExactnessPropertyTest` guarantee the incremental path never drifts.

**Tech Stack:** C++ (ClickHouse), GoogleTest (`unit_tests_dbms`), Google Benchmark (`benchmark_cas_ref_protocol`, gated behind `-DENABLE_BENCHMARKS=ON`).

## Global Constraints

- Allman braces (opening brace on its own line); enforced by CI style check.
- Never introduce a wire-format change: the snapshot and removal-txn byte formats are unchanged. No `NativeFormat` spec impact.
- Pre-release CAS: no compat scaffolding for old persisted data.
- Say "exception", not "crash", in comments/messages (logical errors do not crash release builds).
- Do not commit to `master`; work stays on branch `cas-ref-admits-incremental-budget`.
- Spec: `docs/superpowers/specs/2026-07-20-cas-ref-admits-incremental-budget-design.md`.
- Build target for unit tests: `unit_tests_dbms`. Redirect ninja output to a log file in the build dir and have a subagent summarize it. Run tests redirected to `<build_dir>/test_<name>.log`.
- The debug recompute `chassert` is only active where `NDEBUG` is unset — validate it in `build_debug`. Functional correctness (the boolean decisions) can be validated faster in `build`.

---

## File Structure

- `Formats/CasRefSnapshotFormat.h/.cpp` — add snapshot per-row + framing size helpers; extract a shared `writeSnapshotMeta`.
- `Formats/CasRefLogFormat.h/.cpp` — add removal-op + removal-framing size helpers; extract a shared `writeLogMeta`.
- `Pool/CasRefProtocol.h/.cpp` — add the two counters to `RefTableState`; maintain them in `applyOpInPlace`'s helpers and `stateFromSnapshot`; add the two public budget-size accessors; rewrite `admits()`; add the debug recompute chassert; rewrite the header comment.
- `src/Disks/tests/gtest_cas_ref_statemachine.cpp` — new unit tests for the size helpers, the counters, and the budget-size accessors (this file already hosts `admits()` tests and all the builders: `manifestRef`, `addPrecommitOp`, `promoteOp`, `setPayloadOp`, `removeCommittedOp`, `buildRemovalTxnForTest`, `makeTxn`, `kNs`).
- `benchmarks/benchmark_cas_ref_protocol.cpp` — record the "after" `BM_Admits` scaling in its header comment.
- `utils/ca-soak/scenarios/BACKLOG.md` — mark the finding resolved.

Type reference (already declared, do not redefine):
- `RefCommittedRow { String ref_name; ManifestRef manifest_ref; String payload; uint64_t published_at_ms; }` (`Formats/CasRefSnapshotFormat.h`).
- `RefOwnerBinding { RefOwnerKind kind; String ref_name; ManifestRef manifest_ref; }` (`Formats/CasRefWireVocab.h`).
- `enum class RefOwnerKind : uint8_t { Committed = 1, Precommit = 2 };` (`Formats/CasRefWireVocab.h`).
- `RefLifecycle { Live, Removed }` (`Formats/CasRefSnapshotFormat.h`).
- `RefTxnId { uint64_t writer_epoch; uint64_t ref_sequence; }` (`Primitives/CasTypes.h`).
- `RefOp` / `RefLogTxn` (`Formats/CasRefLogFormat.h`).

---

## Task 1: Snapshot per-row and framing size helpers

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_statemachine.cpp`

**Interfaces:**
- Produces:
  - `size_t committedRowEncodedSize(const RefCommittedRow & row);`
  - `size_t precommitRowEncodedSize(const RefOwnerBinding & binding);`
  - `size_t snapshotFramingSize(const String & ns, const RefTxnId & snapshot_id, RefLifecycle lifecycle, const std::optional<RefTxnId> & remove_txn_id, const std::optional<RefTxnId> & sealed_from, uint64_t row_count);`
- Consumes: existing anonymous-namespace `writeCommittedRow`, `writePrecommitRow`, and (after this task) `writeSnapshotMeta` in the same `.cpp`; `writeHeaderLine`/`writeTrailerLine`/`FormatId::RefSnapshot` from `Formats/CasTextFormat.h`.

- [ ] **Step 1: Write the failing tests**

Add to `src/Disks/tests/gtest_cas_ref_statemachine.cpp`, after the existing `AdmitsExactnessPropertyTest` (the file already includes both format headers and defines `manifestRef`, `addPrecommitOp`, `promoteOp`, `kNs`, `makeTxn`, `snapshotOf` via the protocol header):

```cpp
/// ===================================================================================
/// Snapshot size helpers: framing + Σ per-row must equal a full encode, byte for byte.
/// ===================================================================================
TEST(CasRefSnapshotSizeHelpers, FramingPlusRowsEqualsFullEncode)
{
    /// Build a non-trivial Live table: two committed rows (one with a payload) and one precommit.
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(),
         addPrecommitOp("alpha", manifestRef(1, 1, 1)), promoteOp("alpha", manifestRef(1, 1, 1)),
         addPrecommitOp("beta", manifestRef(1, 2, 1)), promoteOp("beta", manifestRef(1, 2, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2},
        {setPayloadOp("alpha", manifestRef(1, 1, 1), String(123, 'p'), 42)}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {addPrecommitOp("gamma", manifestRef(1, 3, 1))}));

    const RefTableSnapshot snap = snapshotOf(state, "");
    const size_t full = encodeRefTableSnapshot(snap).size();

    size_t rebuilt = snapshotFramingSize("", snap.snapshot_id, snap.lifecycle,
                                         snap.remove_txn_id, snap.sealed_from,
                                         snap.committed.size() + snap.precommits.size());
    for (const RefCommittedRow & row : snap.committed)
        rebuilt += committedRowEncodedSize(row);
    for (const RefOwnerBinding & pc : snap.precommits)
        rebuilt += precommitRowEncodedSize(pc);

    EXPECT_EQ(rebuilt, full);
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cd build && ninja unit_tests_dbms > build_task1.log 2>&1` (have a subagent summarize the log).
Expected: FAIL — `snapshotFramingSize` / `committedRowEncodedSize` / `precommitRowEncodedSize` not declared.

- [ ] **Step 3: Extract `writeSnapshotMeta` and add the size helpers**

In `CasRefSnapshotFormat.cpp`, factor the meta block out of `encodeRefTableSnapshot` into a file-local helper so the encoder and the framing helper share one implementation. Add this in the anonymous namespace (alongside `writeCommittedRow`):

```cpp
/// The snapshot's header-object meta line (ns, snapshot_id, lifecycle, and the optional remove/sealed
/// ids). Shared by `encodeRefTableSnapshot` and `snapshotFramingSize` so the two never disagree by a
/// byte. Assumes the caller has already validated the snapshot (or is measuring framing only).
void writeSnapshotMeta(WriteBuffer & out, const RefTableSnapshot & snapshot)
{
    bool first = true;
    writeKey(out, "ns", first);
    writeStringValue(out, snapshot.ns);
    writeIdFields(out, first, "we", "rs", snapshot.snapshot_id);
    writeKey(out, "lc", first);
    writeStringValue(out, lifecycleToWord(snapshot.lifecycle));
    if (snapshot.lifecycle == RefLifecycle::Removed)
        writeIdFields(out, first, "rte", "rts", *snapshot.remove_txn_id);
    if (snapshot.sealed_from)
        writeIdFields(out, first, "sfe", "sfs", *snapshot.sealed_from);
    closeObject(out, first);
    writeChar('\n', out);
}
```

Replace the inline meta block in `encodeRefTableSnapshot` (the `{ bool first = true; ... writeChar('\n', out); }` block) with:

```cpp
    writeSnapshotMeta(out, snapshot);
```

Then add the public helpers at the end of the `namespace DB::Cas` block (before the closing brace):

```cpp
size_t committedRowEncodedSize(const RefCommittedRow & row)
{
    WriteBufferFromOwnString out;
    writeCommittedRow(out, row);
    out.finalize();
    return out.str().size();
}

size_t precommitRowEncodedSize(const RefOwnerBinding & binding)
{
    WriteBufferFromOwnString out;
    writePrecommitRow(out, binding);
    out.finalize();
    return out.str().size();
}

size_t snapshotFramingSize(const String & ns, const RefTxnId & snapshot_id, RefLifecycle lifecycle,
                           const std::optional<RefTxnId> & remove_txn_id,
                           const std::optional<RefTxnId> & sealed_from, uint64_t row_count)
{
    RefTableSnapshot meta_only;
    meta_only.ns = ns;
    meta_only.snapshot_id = snapshot_id;
    meta_only.lifecycle = lifecycle;
    meta_only.remove_txn_id = remove_txn_id;
    meta_only.sealed_from = sealed_from;

    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::RefSnapshot);
    writeSnapshotMeta(out, meta_only);
    writeTrailerLine(out, row_count);
    out.finalize();
    return out.str().size();
}
```

- [ ] **Step 4: Declare the helpers in the header**

In `CasRefSnapshotFormat.h`, after the `encodeRefTableSnapshot` declaration, add (`<optional>` is already transitively available via the struct definitions; include it explicitly if the build complains):

```cpp
/// Encoded byte size of exactly one committed row line, as `encodeRefTableSnapshot` would emit it.
/// Reuses the same writer, so it is byte-identical to that row's contribution to a full encode.
size_t committedRowEncodedSize(const RefCommittedRow & row);

/// Encoded byte size of exactly one precommit row line, as `encodeRefTableSnapshot` would emit it.
size_t precommitRowEncodedSize(const RefOwnerBinding & binding);

/// Encoded byte size of a snapshot's framing (header + meta line + trailer) for the given metadata and
/// row count, excluding all row lines. `snapshotFramingSize(...) + Σ committedRowEncodedSize +
/// Σ precommitRowEncodedSize` equals `encodeRefTableSnapshot(...).size()` exactly.
size_t snapshotFramingSize(const String & ns, const RefTxnId & snapshot_id, RefLifecycle lifecycle,
                           const std::optional<RefTxnId> & remove_txn_id,
                           const std::optional<RefTxnId> & sealed_from, uint64_t row_count);
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > build_task1.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasRefSnapshotSizeHelpers.*' > test_task1_snapshot_sizes.log 2>&1` (subagent summarizes both logs).
Expected: PASS (1 test).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp \
        src/Disks/tests/gtest_cas_ref_statemachine.cpp
git commit -m "cas: snapshot per-row + framing size helpers (byte-exact vs full encode)"
```

---

## Task 2: Removal-op and removal-framing size helpers

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_statemachine.cpp`

**Interfaces:**
- Produces:
  - `size_t removalOpEncodedSize(RefOwnerKind owner_kind, const String & ref_name, const ManifestRef & manifest_ref);`
  - `size_t removalFramingSize(const String & ns, const RefTxnId & txn_id, uint64_t op_count);`
- Consumes: existing anonymous-namespace `writeOp` and (after this task) `writeLogMeta` in the same `.cpp`; `RefOwnerBinding` (`CasRefWireVocab.h`, already included by `CasRefLogFormat.h`).

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_ref_statemachine.cpp`:

```cpp
/// ===================================================================================
/// Removal-txn size helpers: framing + Σ per-owner-op must equal a full removal-txn encode.
/// ===================================================================================
TEST(CasRefLogSizeHelpers, FramingPlusOpsEqualsFullRemovalEncode)
{
    RefTableState state;
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1},
        {birthOp(),
         addPrecommitOp("alpha", manifestRef(1, 1, 1)), promoteOp("alpha", manifestRef(1, 1, 1)),
         addPrecommitOp("beta", manifestRef(1, 2, 1))}));

    /// Ground truth: the whole-namespace removal txn this test file already builds independently.
    const RefLogTxn removal = buildRemovalTxnForTest(state, "", RefTxnId{1, 1});
    const size_t full = encodeRefLogTxn(removal).size();

    size_t rebuilt = removalFramingSize("", RefTxnId{1, 1},
                                        state.committed.size() + state.precommits.size() + 1);
    for (const auto [name, row] : state.committed)
        rebuilt += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
    for (const auto & [name, mref] : state.precommits)
        rebuilt += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);

    EXPECT_EQ(rebuilt, full);
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cd build && ninja unit_tests_dbms > build_task2.log 2>&1` (subagent summarizes).
Expected: FAIL — `removalFramingSize` / `removalOpEncodedSize` not declared.

- [ ] **Step 3: Extract `writeLogMeta` and add the size helpers**

In `CasRefLogFormat.cpp`, factor the meta block out of `encodeRefLogTxn` into a file-local helper. Add in the anonymous namespace (alongside `writeOp`):

```cpp
/// The log transaction's header-object meta line (ns + txn_id). Shared by `encodeRefLogTxn` and
/// `removalFramingSize` so the two never disagree by a byte.
void writeLogMeta(WriteBuffer & out, const String & ns, const RefTxnId & txn_id)
{
    bool first = true;
    writeKey(out, "ns", first);
    writeStringValue(out, ns);
    writeKey(out, "we", first);
    writeU64StringValue(out, txn_id.writer_epoch);
    writeKey(out, "rs", first);
    writeU64StringValue(out, txn_id.ref_sequence);
    closeObject(out, first);
    writeChar('\n', out);
}
```

Replace the inline meta block in `encodeRefLogTxn` (the `/// meta line { bool first = true; ... }` block) with:

```cpp
    writeLogMeta(out, txn.ns, txn.txn_id);
```

Then add the public helpers at the end of the `namespace DB::Cas` block:

```cpp
size_t removalOpEncodedSize(RefOwnerKind owner_kind, const String & ref_name, const ManifestRef & manifest_ref)
{
    /// One exact owner-removal op, exactly as `buildHypotheticalRemovalTxn` emits it: an
    /// owner_transition with only an old binding, no new binding.
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{owner_kind, ref_name, manifest_ref};

    WriteBufferFromOwnString out;
    writeOp(out, op);
    out.finalize();
    return out.str().size();
}

size_t removalFramingSize(const String & ns, const RefTxnId & txn_id, uint64_t op_count)
{
    /// Header + meta + the terminal remove_namespace op + trailer(op_count). `op_count` counts every op
    /// including the remove_namespace op (i.e. committed + precommits + 1).
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::RefLog);
    writeLogMeta(out, ns, txn_id);
    RefOp remove_op;
    remove_op.kind = RefOpKind::RemoveNamespace;
    writeOp(out, remove_op);
    writeTrailerLine(out, op_count);
    out.finalize();
    return out.str().size();
}
```

- [ ] **Step 4: Declare the helpers in the header**

In `CasRefLogFormat.h`, after the `encodeRefLogTxn` declaration, add:

```cpp
/// Encoded byte size of exactly one exact-owner-removal op line, as `buildHypotheticalRemovalTxn` +
/// `encodeRefLogTxn` would emit it (an owner_transition with only an old binding).
size_t removalOpEncodedSize(RefOwnerKind owner_kind, const String & ref_name, const ManifestRef & manifest_ref);

/// Encoded byte size of a removal transaction's framing (header + meta + terminal remove_namespace op +
/// trailer) for `op_count` total ops, excluding the per-owner removal op lines. `removalFramingSize(...)
/// + Σ removalOpEncodedSize` equals `encodeRefLogTxn(buildHypotheticalRemovalTxn(...)).size()` exactly.
size_t removalFramingSize(const String & ns, const RefTxnId & txn_id, uint64_t op_count);
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `cd build && ninja unit_tests_dbms > build_task2.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasRefLogSizeHelpers.*' > test_task2_log_sizes.log 2>&1` (subagent summarizes).
Expected: PASS (1 test).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp \
        src/Disks/tests/gtest_cas_ref_statemachine.cpp
git commit -m "cas: removal-op + removal-framing size helpers (byte-exact vs full encode)"
```

---

## Task 3: `RefTableState` body-byte counters, maintenance, seeding, and debug drift check

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h` (add two fields)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp` (maintain at 5 sites + seeding + debug check)
- Test: `src/Disks/tests/gtest_cas_ref_statemachine.cpp`

**Interfaces:**
- Consumes: `committedRowEncodedSize`, `precommitRowEncodedSize`, `removalOpEncodedSize` (Tasks 1–2).
- Produces: `RefTableState::snapshot_body_bytes` / `RefTableState::removal_body_bytes` (public `uint64_t` fields), maintained as a pure function of `(committed, precommits)`.

- [ ] **Step 1: Write the failing test**

Add to `src/Disks/tests/gtest_cas_ref_statemachine.cpp`. It asserts the counters equal a from-scratch recompute after a sequence of every op kind, including removals and set_payload:

```cpp
/// ===================================================================================
/// Body-byte counters: snapshot_body_bytes / removal_body_bytes are a pure function of the rows.
/// ===================================================================================
namespace
{
uint64_t recomputeSnapshotBody(const RefTableState & s)
{
    uint64_t total = 0;
    for (const auto [name, row] : s.committed)
        total += committedRowEncodedSize(row);
    for (const auto & [name, mref] : s.precommits)
        total += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, name, mref});
    return total;
}
uint64_t recomputeRemovalBody(const RefTableState & s)
{
    uint64_t total = 0;
    for (const auto [name, row] : s.committed)
        total += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
    for (const auto & [name, mref] : s.precommits)
        total += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);
    return total;
}
}

TEST(CasRefStateCounters, CountersTrackRowsThroughEveryOpKind)
{
    RefTableState state;
    EXPECT_EQ(state.snapshot_body_bytes, 0u);
    EXPECT_EQ(state.removal_body_bytes, 0u);

    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 2}, {addPrecommitOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 3}, {promoteOp("a", manifestRef(1, 1, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 4},
        {setPayloadOp("a", manifestRef(1, 1, 1), String(77, 'x'), 5)}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 5}, {addPrecommitOp("b", manifestRef(1, 2, 1))}));
    EXPECT_EQ(state.snapshot_body_bytes, recomputeSnapshotBody(state));
    EXPECT_EQ(state.removal_body_bytes, recomputeRemovalBody(state));

    /// Shrink back down: remove the precommit, then the committed row.
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 6}, {removePrecommitOp("b", manifestRef(1, 2, 1))}));
    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 7}, {removeCommittedOp("a", manifestRef(1, 1, 1))}));
    EXPECT_EQ(state.snapshot_body_bytes, recomputeSnapshotBody(state));
    EXPECT_EQ(state.removal_body_bytes, recomputeRemovalBody(state));
    EXPECT_EQ(state.snapshot_body_bytes, 0u);
    EXPECT_EQ(state.removal_body_bytes, 0u);
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cd build && ninja unit_tests_dbms > build_task3.log 2>&1` (subagent summarizes).
Expected: FAIL — `RefTableState` has no `snapshot_body_bytes` / `removal_body_bytes`.

- [ ] **Step 3: Add the two fields to `RefTableState`**

In `CasRefProtocol.h`, inside `struct RefTableState`, after the `precommits` member, add:

```cpp
    /// Running byte totals of the two admission-budget encodings' *bodies* (row/op lines only, no
    /// header/meta/trailer framing), maintained O(1) per applied op by `applyOpInPlace` and seeded by
    /// `stateFromSnapshot`. A pure function of `(committed, precommits)`: `admits` reads
    /// `framing + total` instead of re-encoding the whole table. See `admits`'s doc for why this is
    /// byte-exact rather than a drift-prone estimate.
    uint64_t snapshot_body_bytes = 0;   /// Σ committedRowEncodedSize + Σ precommitRowEncodedSize
    uint64_t removal_body_bytes  = 0;   /// Σ removalOpEncodedSize(one per committed + one per precommit)
```

- [ ] **Step 4: Maintain the counters at the 5 mutation sites**

In `CasRefProtocol.cpp`, update each row mutation in `applyOwnerTransition` / `applySetPayload`. Apply the counter change immediately adjacent to the row mutation, after the preconditions have passed.

Add precommit (currently `state.precommits.emplace(b.ref_name, b.manifest_ref);`):

```cpp
        state.precommits.emplace(b.ref_name, b.manifest_ref);
        state.snapshot_body_bytes += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
        return;
```

Remove precommit (currently the `if (state.precommits.erase(...) == 0) throw; return;`):

```cpp
        if (state.precommits.erase({b.ref_name, b.manifest_ref}) == 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState: exact precommit binding '{}' to remove is absent", b.ref_name);
        state.snapshot_body_bytes -= precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
        state.removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
        return;
```

Remove committed (currently `state.committed.erase(it); return;`). Capture the row before erasing:

```cpp
        const RefCommittedRow removed = it->second;
        state.committed.erase(it);
        state.snapshot_body_bytes -= committedRowEncodedSize(removed);
        state.removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Committed, removed.ref_name, removed.manifest_ref);
        return;
```

Promote (currently erases the precommit, then `state.committed.emplace(b.ref_name, std::move(row));`). Update both sides — the precommit leaves, the committed row (empty payload) arrives:

```cpp
        if (state.precommits.erase({b.ref_name, b.manifest_ref}) == 0)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState: exact precommit binding '{}' to promote is absent", b.ref_name);
        state.snapshot_body_bytes -= precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
        state.removal_body_bytes  -= removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
        /// ... existing "different manifest already committed" check stays here ...
        if (state.committed.contains(b.ref_name))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "RefTableState: promote '{}' would silently displace a different already-committed "
                "manifest -- remove it with an explicit owner_transition first", b.ref_name);
        RefCommittedRow row;
        row.ref_name = b.ref_name;
        row.manifest_ref = b.manifest_ref;
        state.snapshot_body_bytes += committedRowEncodedSize(row);
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
        state.committed.emplace(b.ref_name, std::move(row));
        return;
```

> Note the ordering: the precommit-erased decrement happens *before* the `state.committed.contains` throw check. That is correct — if the throw fires, the whole op is discarded on a scratch copy by `applyRefLogTxn`'s two-phase apply, so the partially-updated counter never reaches the installed state. Compute `committedRowEncodedSize(row)` from `row` before the `emplace` moves it.

Set payload (`applySetPayload`, currently builds `updated` and `insert_or_assign`s it). The removal side is unchanged (ref_name/manifest_ref are identical); only the snapshot body changes:

```cpp
    RefCommittedRow updated = it->second;
    const uint64_t old_row_bytes = committedRowEncodedSize(it->second);
    updated.payload = op.payload;
    updated.published_at_ms = op.published_at_ms;
    state.snapshot_body_bytes -= old_row_bytes;
    state.snapshot_body_bytes += committedRowEncodedSize(updated);
    /// removal_body_bytes unchanged: set_payload touches neither ref_name nor manifest_ref.
    state.committed.insert_or_assign(op.ref_name, std::move(updated));
```

- [ ] **Step 5: Seed the counters in `stateFromSnapshot`**

In `CasRefProtocol.cpp`, in `stateFromSnapshot`, accumulate both totals in the existing build loops:

```cpp
    for (const RefCommittedRow & row : validated.committed)
    {
        state.committed.emplace(row.ref_name, row);
        state.snapshot_body_bytes += committedRowEncodedSize(row);
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Committed, row.ref_name, row.manifest_ref);
    }
    for (const RefOwnerBinding & b : validated.precommits)
    {
        state.precommits.emplace(b.ref_name, b.manifest_ref);
        state.snapshot_body_bytes += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, b.ref_name, b.manifest_ref});
        state.removal_body_bytes  += removalOpEncodedSize(RefOwnerKind::Precommit, b.ref_name, b.manifest_ref);
    }
```

- [ ] **Step 6: Add the debug drift check in `applyRefLogTxn`**

In `CasRefProtocol.cpp`, add a file-local debug recompute helper in the anonymous namespace:

```cpp
#ifndef NDEBUG
/// Debug-only: recompute both body totals from scratch and assert the incrementally maintained values
/// match. This is what makes the incremental counters *provably* byte-exact rather than a drift-prone
/// estimate -- the concern the old non-incremental admits() cited. O(N); debug builds only.
void debugAssertBodyCounters(const RefTableState & state)
{
    uint64_t snap = 0;
    uint64_t rem = 0;
    for (const auto [name, row] : state.committed)
    {
        snap += committedRowEncodedSize(row);
        rem  += removalOpEncodedSize(RefOwnerKind::Committed, name, row.manifest_ref);
    }
    for (const auto & [name, mref] : state.precommits)
    {
        snap += precommitRowEncodedSize(RefOwnerBinding{RefOwnerKind::Precommit, name, mref});
        rem  += removalOpEncodedSize(RefOwnerKind::Precommit, name, mref);
    }
    chassert(state.snapshot_body_bytes == snap);
    chassert(state.removal_body_bytes == rem);
}
#endif
```

Then call it in `applyRefLogTxn` right after the state is installed (`state = std::move(scratch);`):

```cpp
    scratch.greatest_applied = txn.txn_id;
    state = std::move(scratch);
#ifndef NDEBUG
    debugAssertBodyCounters(state);
#endif
```

Ensure `<Common/Exception.h>` (for `chassert`) is included by `CasRefProtocol.cpp` (it already includes it for `Exception`).

- [ ] **Step 7: Run the counter test (release build for speed)**

Run: `cd build && ninja unit_tests_dbms > build_task3.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasRefStateCounters.*:CasRefStateMachine.*' > test_task3_counters.log 2>&1` (subagent summarizes).
Expected: PASS — the new counter test plus all existing state-machine tests stay green.

- [ ] **Step 8: Run under the debug build to exercise the drift `chassert`**

Run: `cd build_debug && ninja unit_tests_dbms > build_task3_debug.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasRef*' > test_task3_debug.log 2>&1` (subagent summarizes). If `build_debug` is not configured, configure it once with the project's debug preset.
Expected: PASS — `debugAssertBodyCounters` fires on every applied transaction across all ref tests and never aborts.

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp \
        src/Disks/tests/gtest_cas_ref_statemachine.cpp
git commit -m "cas: maintain incremental body-byte counters on RefTableState (debug drift chassert)"
```

---

## Task 4: Rewrite `admits()` to O(1) via budget-size accessors

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h` (declare accessors; rewrite `admits` doc comment)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp` (add accessors; rewrite `admits`)
- Test: `src/Disks/tests/gtest_cas_ref_statemachine.cpp`

**Interfaces:**
- Consumes: `RefTableState::snapshot_body_bytes` / `removal_body_bytes` (Task 3); `snapshotFramingSize` / `removalFramingSize` (Tasks 1–2).
- Produces:
  - `uint64_t encodedSnapshotBudgetSize(const RefTableState & state);` — equals `encodeRefTableSnapshot(snapshotOf(state, "")).size()`.
  - `uint64_t encodedRemovalBudgetSize(const RefTableState & state);` — equals `encodeRefLogTxn(buildHypotheticalRemovalTxn(state, {1,1})).size()`.
  - `admits(...)` unchanged signature, O(1) body.

- [ ] **Step 1: Write the failing test**

Add a property test that the two accessors equal the real encoders across randomized states (a stronger, more direct check than the existing boundary tests). Add to `src/Disks/tests/gtest_cas_ref_statemachine.cpp`:

```cpp
/// ===================================================================================
/// Budget-size accessors equal the real encoders across randomized states.
/// ===================================================================================
TEST(CasRefBudgetSize, AccessorsEqualFullEncodeRandomized)
{
    std::mt19937 rng(1234); // NOLINT(cert-msc32-c,cert-msc51-cpp): deterministic seed for reproducibility.
    for (int trial = 0; trial < 30; ++trial)
    {
        RefTableState state;
        applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, 1}, {birthOp()}));
        uint64_t seq = 2;
        uint64_t build = 1;
        std::vector<std::pair<String, ManifestRef>> committed_names;

        const int steps = 1 + static_cast<int>(rng() % 6);
        for (int i = 0; i < steps; ++i)
        {
            const String name = "r" + std::to_string(rng() % 5);
            const ManifestRef mref = manifestRef(1, build++, 1);
            applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {addPrecommitOp(name, mref)}));
            const bool already = std::any_of(committed_names.begin(), committed_names.end(),
                [&](const auto & c) { return c.first == name; });
            if (!already && rng() % 2 == 0)
            {
                applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++}, {promoteOp(name, mref)}));
                committed_names.emplace_back(name, mref);
                if (rng() % 2 == 0)
                    applyRefLogTxn(state, makeTxn(kNs, RefTxnId{1, seq++},
                        {setPayloadOp(name, mref, String(rng() % 50, 's'), rng())}));
            }
        }

        const size_t true_snapshot = encodeRefTableSnapshot(snapshotOf(state, "")).size();
        const size_t true_removal = encodeRefLogTxn(buildRemovalTxnForTest(state, "", RefTxnId{1, 1})).size();
        EXPECT_EQ(encodedSnapshotBudgetSize(state), true_snapshot);
        EXPECT_EQ(encodedRemovalBudgetSize(state), true_removal);
    }
}
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cd build && ninja unit_tests_dbms > build_task4.log 2>&1` (subagent summarizes).
Expected: FAIL — `encodedSnapshotBudgetSize` / `encodedRemovalBudgetSize` not declared.

- [ ] **Step 3: Add the budget-size accessors and rewrite `admits`**

In `CasRefProtocol.cpp`, replace the body of `admits` and add the two accessors just above it:

```cpp
uint64_t encodedSnapshotBudgetSize(const RefTableState & state)
{
    /// snapshotOf uses snapshot_id = state.greatest_applied, empty ns, sealed_from unset, and the
    /// state's own lifecycle/remove_txn_id -- match that framing exactly, then add the running body sum.
    const uint64_t rows = state.committed.size() + state.precommits.size();
    return snapshotFramingSize("", state.greatest_applied, state.lifecycle,
                               state.remove_txn_id, /*sealed_from*/std::nullopt, rows)
        + state.snapshot_body_bytes;
}

uint64_t encodedRemovalBudgetSize(const RefTableState & state)
{
    /// buildHypotheticalRemovalTxn uses a fixed {1,1} preview id, empty ns, and one removal op per owner
    /// plus a terminal remove_namespace op -- so op_count = committed + precommits + 1.
    static constexpr RefTxnId kPreviewTxnId{1, 1};
    const uint64_t rows = state.committed.size() + state.precommits.size();
    return removalFramingSize("", kPreviewTxnId, rows + 1) + state.removal_body_bytes;
}

bool admits(const RefTableState & state, const RefOp & op, uint64_t snapshot_budget, uint64_t removal_budget)
{
    /// A fixed nonzero placeholder id: this previews `op` in isolation and the scratch state is
    /// discarded immediately after reading its (incrementally maintained) budget sizes.
    static constexpr RefTxnId kPreviewTxnId{1, 1};

    RefTableState scratch = state;
    applyOpInPlace(scratch, op, kPreviewTxnId);   // throws exactly as before if `op` is not a legal transition
#ifndef NDEBUG
    debugAssertBodyCounters(scratch);
#endif

    if (encodedSnapshotBudgetSize(scratch) > snapshot_budget)
        return false;
    return encodedRemovalBudgetSize(scratch) <= removal_budget;
}
```

> `applyOpInPlace` does not advance `greatest_applied` (only `applyRefLogTxn` does), so `scratch.greatest_applied == state.greatest_applied` and the snapshot meta framing matches the pre-existing `snapshotOf(scratch, "")` behavior byte-for-byte, exactly as the old code produced it.

- [ ] **Step 4: Declare the accessors and rewrite the `admits` header doc**

In `CasRefProtocol.h`, add before the `admits` declaration:

```cpp
/// The exact encoded size of `state`'s canonical snapshot (`encodeRefTableSnapshot(snapshotOf(state,
/// "")).size()`), computed in O(1) from the running body counter plus O(1) framing instead of a full
/// re-encode. Used by `admits` and directly property-tested against the real encoder.
uint64_t encodedSnapshotBudgetSize(const RefTableState & state);

/// The exact encoded size of `state`'s hypothetical whole-namespace removal transaction, computed in
/// O(1) from the running body counter plus O(1) framing. Used by `admits`.
uint64_t encodedRemovalBudgetSize(const RefTableState & state);
```

Replace the "Implementation choice: sizes are computed non-incrementally ..." paragraph (`CasRefProtocol.h:263-268`) with:

```cpp
/// Implementation: sizes are computed incrementally. `RefTableState` carries running body-byte totals
/// (`snapshot_body_bytes` / `removal_body_bytes`) maintained O(1) per applied op by `applyOpInPlace`;
/// `admits` applies `op` to a scratch copy and reads `framing + total` via `encodedSnapshotBudgetSize`
/// / `encodedRemovalBudgetSize`, making the whole check O(touched rows) instead of O(table size). This
/// is byte-exact rather than a drift-prone estimate: both budget encodings are pure per-row sums, the
/// per-row contributions come from the same codec primitives the full encoders use, and a debug-only
/// recompute-and-compare `chassert` (`debugAssertBodyCounters`) proves equality on every applied op.
```

- [ ] **Step 5: Run the new test plus every existing `admits`/state test**

Run: `cd build && ninja unit_tests_dbms > build_task4.log 2>&1 && ./src/unit_tests_dbms --gtest_filter='CasRefBudgetSize.*:CasRefStateMachine.*:CasRef*' > test_task4_admits.log 2>&1` (subagent summarizes).
Expected: PASS — new accessor property test green, and all existing `AdmitsExactnessPropertyTest` / `AdmitsRejectsGrowthPast*` boundary tests (the byte-exact guardrail) still green.

- [ ] **Step 6: Run the broader ref/pool/protocol gtest surface**

Run: `cd build && ./src/unit_tests_dbms --gtest_filter='CasRef*:CasProtocol*:CasPromote*:CasTruncate*' > test_task4_broad.log 2>&1` (subagent summarizes).
Expected: PASS — no behavioral regression anywhere that exercises the ref state machine or `admits`.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp \
        src/Disks/tests/gtest_cas_ref_statemachine.cpp
git commit -m "cas: rewrite admits() to O(1) via incremental budget-size accessors"
```

---

## Task 5: Confirm the "after" benchmark and mark the backlog finding resolved

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp` (record the after-numbers in its header comment)
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` (mark resolved)

**Interfaces:** none (measurement + documentation only).

- [ ] **Step 1: Build and run the benchmark**

Run (a build with benchmarks enabled — configure a `build_bench` dir with `-DENABLE_BENCHMARKS=ON` if none exists, RelWithDebInfo):

```bash
cd build_bench && ninja benchmark_cas_ref_protocol > build_task5_bench.log 2>&1 && \
  ./src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol \
  --benchmark_filter='BM_Admits' > test_task5_bench.log 2>&1
```
(subagent summarizes). Expected: `BM_Admits` time/call now roughly flat across N = 100…100,000 (Google Benchmark's complexity fit ≈ O(1) / O(log N) framing, no longer O(N log N)).

- [ ] **Step 2: Record the after-numbers**

In `benchmark_cas_ref_protocol.cpp`'s top comment block, add a short "After incremental admits() (2026-07-20)" note with the measured per-call times for N = 100 / 1,000 / 10,000 / 100,000 from Step 1, alongside the existing before-table context.

- [ ] **Step 3: Mark the backlog finding resolved**

In `utils/ca-soak/scenarios/BACKLOG.md`, edit the heading `## OPTIMIZATION OPPORTUNITY (CPU, algorithmic, MEDIUM-HIGH) — admits() re-encodes the WHOLE ref table once per state-growing op in a flush batch` to `## RESOLVED (CPU, algorithmic) — admits() re-encodes the WHOLE ref table ...` and append a resolution line:

```markdown
- **RESOLVED (2026-07-20):** Implemented Approach A (incremental body-byte counters on
  `RefTableState`, maintained O(1) per op by `applyOpInPlace`, byte-exact vs the full encode,
  guarded by a debug recompute `chassert` and the existing `AdmitsExactnessPropertyTest`).
  `admits()` is now O(touched rows); a K-op flush batch against an N-ref table is O(K) not O(K×N).
  Spec: `docs/superpowers/specs/2026-07-20-cas-ref-admits-incremental-budget-design.md`.
  Plan: `docs/superpowers/plans/2026-07-20-cas-ref-admits-incremental-budget.md`.
  See the `BM_Admits` after-numbers in `benchmark_cas_ref_protocol.cpp`.
```

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp \
        utils/ca-soak/scenarios/BACKLOG.md
git commit -m "cas: record admits() after-benchmark and mark backlog finding resolved"
```

---

## Self-Review

**Spec coverage:**
- §1 data model → Task 3 (two fields).
- §2 size helpers (single source of truth) → Tasks 1 & 2 (extract `writeSnapshotMeta`/`writeLogMeta`; per-row + framing helpers).
- §3 maintenance at 5 sites + seeding → Task 3 Steps 4–5.
- §4 new `admits()` → Task 4.
- §5 anti-drift (debug chassert + fuzz property test) → Task 3 Step 6 (chassert) + Task 4 Step 1 (accessor property test); the pre-existing `AdmitsExactnessPropertyTest` is preserved as the boundary guardrail.
- §6 validation & scope (benchmark, header comment, no wire change, backlog) → Task 4 Step 4 (comment) + Task 5.

**Type consistency:** `snapshot_body_bytes` / `removal_body_bytes`, `committedRowEncodedSize` / `precommitRowEncodedSize` / `removalOpEncodedSize`, `snapshotFramingSize` / `removalFramingSize`, `encodedSnapshotBudgetSize` / `encodedRemovalBudgetSize` are used with identical names and signatures in every task that references them. `RefOwnerBinding{kind, ref_name, manifest_ref}` and `RefOwnerKind::{Committed, Precommit}` match the declarations in `CasRefWireVocab.h`.

**Placeholder scan:** every code step shows complete code; every run step shows the exact command and expected result. No TBD/TODO.

**One subtlety surfaced for the implementer:** in the promote path (Task 3 Step 4) the precommit decrement is written before the `state.committed.contains` throw check; this is safe only because `applyRefLogTxn` applies to a scratch copy and installs it only on whole-transaction success, so a throw discards the partially-updated counter with the rest of the scratch. Do not "optimize" this by reordering the mutation ahead of validation in a way that could leave a live `state` half-updated.

# CAS Cleanup / Simplification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove noise/duplication in the CAS feature (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`) with **zero runtime-behavior and zero on-disk-byte change**.

**Architecture:** A sequence of independent, individually-shippable, behavior-preserving refactors (R1–R14), ordered safest→riskier. New helpers are small and local; call-sites are mechanically rerouted through them. Each task is gated by the existing gtest suite; codec tasks additionally assert byte-for-byte equality; event tasks assert event-field equality. A final 6h chaos soak validates the whole.

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/`), protobuf, `ninja`/`build`, the `utils/ca-soak` harness.

**Spec:** `docs/superpowers/specs/2026-06-23-cas-cleanup-simplification-design.md`.

**Branch:** `cas-vfs-path-mapping` (NOT master). Verify `git branch --show-current` before each commit.

---

## Global rules for every task (the engineer must obey)

- **Behavior-preserving only.** If you cannot prove a change is equivalent, do the smaller change or stop and report. Never "improve" logic, change an exception (except R12's documented code swap), reorder side effects, or alter emitted-event fields.
- **No on-disk byte change.** For any codec touch, add a temporary `EXPECT_EQ(encode_before_bytes, encode_after_bytes)` style assertion proving the bytes are unchanged (use a captured golden from a round-trip), then keep it as a golden test.
- **Build:** `ninja -C build <target>` redirected to `build/build_cleanup_<rN>.log`; analyze the log via `tail`/a sub-subagent, never dump it. Do NOT pass `-j`/`nproc`. Unit binary: `build/src/unit_tests_dbms`.
- **Test:** run the relevant `--gtest_filter='Cas*:Ca*'` (or a tighter filter) and confirm only the known baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow` remains.
- **Style:** Allman braces; `ASan` not `ASAN`; wrap `MergeTree`/symbol names in backticks in comments/commits. Allman applies to new helpers.
- **Commit trailer:** end every commit message with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Use `tmp/` for scratch.

---

## File structure (what each touched/created file is responsible for)

- `src/Common/ProfileEvents.cpp` — drop 4 `CasDbg*` events (R1).
- `src/Disks/ObjectStorages/S3/S3ObjectStorage.{h,cpp}` — drop `casDbgSampleHeadMiss` + call sites (R1).
- `Core/CasEvent.h` — add `toEventKind(ObjectKind)` (R2) and the `EventEmitter` helper (R6).
- `Core/CasGc.cpp` — `cursorKey`/`parseCursorKey` (R3), `persistGenerationProbingUpward` (R7), route events through `EventEmitter` (R6), placement visitor use (R5).
- `Core/CasTreeCodec.{h,cpp}` — `forEachPlacement`/visitor (R5); route through `checkVersion` (R8) + `writeU128`/`readU128` (R9).
- `Core/CasPlacement.h` (NEW) — the placement visitor (R5).
- `Core/CasCodecUtil.h` — `checkVersion` (R8), named `writeU128LE/readU128LE/writeU128BE/readU128BE` (R9), `JsonObjectWriter` (R10).
- `Core/CasEnvelope.cpp`, `Core/CasRootShardCodec.cpp` — route version gates through `checkVersion` (R8); `UNKNOWN_FORMAT_VERSION` (R12); u128 helpers (R9).
- `Core/CasWatermark.cpp`, `Core/CasHeartbeat.cpp`, `Core/CasPoolMeta.cpp`, `Core/CasRootsRegistry.cpp` — encode via `JsonObjectWriter` (R10).
- `MetadataStorageFactory.cpp` + `utils/ca-soak/configs/storage_conf.xml` + any test disk configs — unified config keys (R11).
- `Core/CasBackend.h` + `CasObjectStorageBackend.{h,cpp}` + `CasInMemoryBackend.{h,cpp}` + `CasInstrumentedBackend.{h,cpp}` + `Core/CasStore.cpp` — `WriteResult` + `dropNamespace` reuse (R13).
- `Core/CasSingleWriterSlot.{h,cpp}` (NEW) + `CasWatermark.*` + `CasHeartbeat.*` — keeper merge (R14, gated).
- `src/Disks/tests/*` — new golden/equality tests per task; update `cas_test_helpers.h::expectThrowsCode` for R12.

---

## Task R1: Remove the throwaway `CasDbg*` instrumentation

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (the 4 `CasDbg*` `M(...)` lines, ~`:737-740`)
- Modify: `src/Disks/ObjectStorages/S3/S3ObjectStorage.cpp` (`casDbgSampleHeadMiss` def ~`:53-67`; calls in `exists` ~`:248-252` and `tryGetObjectMetadata` ~`:570-575`); `S3ObjectStorage.h` if a decl exists

- [ ] **Step 1: Find every reference.** `grep -rn "CasDbg\|casDbgSampleHeadMiss" src/` — record all sites (expect: 4 ProfileEvents decls + the sampler fn + 2 call sites + any `extern const Event` lines in `S3ObjectStorage.cpp`).
- [ ] **Step 2: Delete the call sites** in `exists` and `tryGetObjectMetadata` (the `casDbgSampleHeadMiss(...)` calls and any surrounding `if`/sampling guard that exists ONLY to feed them). Leave the real `exists`/`tryGetObjectMetadata` logic byte-for-byte otherwise unchanged.
- [ ] **Step 3: Delete `casDbgSampleHeadMiss`** and its `extern const Event CasDbg*` references in `S3ObjectStorage.cpp`.
- [ ] **Step 4: Delete the 4 `M(CasDbg…)` lines** from `ProfileEvents.cpp`.
- [ ] **Step 5: Build + grep-clean.**
Run: `ninja -C build clickhouse > build/build_cleanup_r1.log 2>&1; tail -3 build/build_cleanup_r1.log` → links clean; `grep -rn "CasDbg\|casDbgSampleHeadMiss" src/` → no matches.
- [ ] **Step 6: Commit.**
```bash
git add src/Common/ProfileEvents.cpp src/Disks/ObjectStorages/S3/S3ObjectStorage.cpp src/Disks/ObjectStorages/S3/S3ObjectStorage.h
git commit -m "CAS cleanup R1: remove throwaway CasDbg* ProfileEvents + S3 head-miss sampler (F1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R2: `toEventKind(ObjectKind)` free function

**Files:**
- Modify: `Core/CasEvent.h` (add the fn near `CasEventObjectKind`)
- Modify: `Core/CasBuild.cpp`, `Core/CasGc.cpp` (replace the ternaries)

- [ ] **Step 1: Add the helper** in `CasEvent.h` (inline, in `namespace DB::Cas`):
```cpp
/// Map an internal ObjectKind to the audit-log CasEventObjectKind. Single source for the
/// kind→event-kind mapping that was previously open-coded as a ternary at each emission site.
inline CasEventObjectKind toEventKind(ObjectKind kind)
{
    switch (kind)
    {
        case ObjectKind::Blob: return CasEventObjectKind::Blob;
        case ObjectKind::Tree: return CasEventObjectKind::Tree;
        case ObjectKind::Pack: return CasEventObjectKind::Pack;
    }
}
```
(Match the EXACT enumerators in `CasEnvelope.h::ObjectKind` and `CasEvent.h::CasEventObjectKind`; if `ObjectKind` has more/fewer members, cover them all — no `default`, so the compiler enforces exhaustiveness. Verify the current mapping in the existing ternaries before writing, to reproduce it precisely.)
- [ ] **Step 2: Find the ternaries.** `grep -rn "CasEventObjectKind::Tree\s*:" Core/CasBuild.cpp Core/CasGc.cpp` and any `kind == ObjectKind::Tree ?` forms.
- [ ] **Step 3: Replace** each `… ? CasEventObjectKind::Tree : CasEventObjectKind::Blob` (and any 3-way) with `toEventKind(<kind expr>)`. Confirm each replaced site produced the identical value for all inputs it could see.
- [ ] **Step 4: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r2.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -8` → baseline red only.
- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit -m "CAS cleanup R2: toEventKind(ObjectKind) free fn; replace duplicated object_kind ternaries (step 2)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R3: `cursorKey(ns, shard)` / `parseCursorKey` helpers

**Files:**
- Modify: `Core/CasGc.cpp` (add the two helpers in the anonymous namespace or as private statics; route the ~5 format sites `:175,232,918,1693` and the parse site `:310-316`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (a golden round-trip test)

- [ ] **Step 1: Read the current sites.** Inspect `CasGc.cpp:175,232,918,1693` (format) and `:310-316` (parse) to capture the EXACT current string grammar (`ns.string() + "/" + std::to_string(shard)` and however it is parsed back).
- [ ] **Step 2: Write a golden test** in `gtest_cas_gc_round.cpp`:
```cpp
TEST(CasGcCursorKey, RoundTripsAndMatchesLegacyFormat)
{
    using namespace DB::Cas;
    const RootNamespace ns{"srv1/tbl"};
    // legacy format was exactly ns.string() + "/" + to_string(shard):
    EXPECT_EQ(cursorKey(ns, 7), "srv1/tbl/7");
    const auto [ns2, shard2] = parseCursorKey("srv1/tbl/7");
    EXPECT_EQ(ns2.string(), "srv1/tbl");
    EXPECT_EQ(shard2, 7u);
}
```
(Adjust the expected string to the EXACT legacy grammar found in Step 1 — the shard is the substring after the LAST `/`; the namespace may itself contain `/`.)
- [ ] **Step 3: Implement** in `CasGc.cpp` (Allman):
```cpp
namespace
{
String cursorKey(const RootNamespace & ns, uint64_t shard)
{
    return ns.string() + "/" + std::to_string(shard);
}

std::pair<RootNamespace, uint64_t> parseCursorKey(const String & key)
{
    const auto slash = key.rfind('/');
    /// reproduce the EXACT inverse of the legacy parse at CasGc.cpp:310-316 (last-slash split)
    return {RootNamespace{key.substr(0, slash)}, std::stoull(key.substr(slash + 1))};
}
}
```
(Make `parseCursorKey` byte-for-byte equivalent to the legacy parse — same split rule, same integer parse, same error behavior. If the legacy parse differs from last-slash, match it exactly.)
- [ ] **Step 4: Route all sites** through the helpers.
- [ ] **Step 5: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r3.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasGc*:Ca*Gc*:CasGcCursorKey.*' 2>&1 | tail -8` → new test PASS; baseline red only.
- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CAS cleanup R3: cursorKey/parseCursorKey helpers; route the ~5 format/parse sites (step 4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R4: Documentation-only reconciliations

**Files:**
- Modify: `Core/CasRootsRegistry.h` (clarify name vs `gc/registry` key), `Core/CasEnvelope.h` (mark test-only knobs). Confirm `Core/CasRootShardCodec.h` JSON doc already fixed (it is — verify).

- [ ] **Step 1: `RootsRegistry` comment.** At the top of the `RootsRegistry` type in `CasRootsRegistry.h`, add a comment: the type is named `RootsRegistry` but persists at the storage key `gc/registry` with format tag `roots`; an operator greps `gc/registry`. Do NOT change the key or tag (format change). State both names so an incident reader is not confused.
- [ ] **Step 2: `EnvelopeHeader` test-only knobs.** At the 2 test-only fields in `CasEnvelope.h` (the ones the analysis flags at `:70-71`), add `/// TEST-ONLY: set only by gtests; default in production` so their production-default invariant is visible in the type.
- [ ] **Step 3: Verify** `CasRootShardCodec.h` top doc-comment already says protobuf (fixed this branch). If any per-function doc still says JSON, fix it to protobuf.
- [ ] **Step 4: Build (headers only → quick) + commit.** No behavior; a compile check suffices.
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r4.log 2>&1; tail -3 build/build_cleanup_r4.log`
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h
git commit -m "CAS cleanup R4: doc reconciliations (RootsRegistry name vs gc/registry key; EnvelopeHeader test-only knobs) (step 16)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R5: `Placement` visitor / `forEachPlacement`

**Files:**
- Create: `Core/CasPlacement.h`
- Modify: the 5–6 switch sites: `CasTreeCodec.cpp` (`encodeTree`/`decodeTree`), `CasClosureWalk.cpp`, `CasBuild.cpp` (`stageTree`/`adoptEvidence`), `CasFsck.cpp`
- Test: `src/Disks/tests/gtest_cas_closure_walk.cpp` or a new `gtest_cas_placement.cpp`

- [ ] **Step 1: Inspect the switches.** Read each of the 5–6 `switch (entry.placement)` (or `placement`) sites and confirm they share the arm set `{Inline, Blob, PackSlice, Subtree}`. Note which sites are genuinely uniform vs which have site-specific bodies (only route the genuinely-parallel ones; do NOT force a visitor onto a switch that does something materially different per site).
- [ ] **Step 2: Design the visitor.** A `Placement` visitor that dispatches to a caller-supplied handler per arm, preserving exhaustiveness. `CasPlacement.h`:
```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
namespace DB::Cas
{
/// Exhaustive dispatch over Placement. Each handler is invoked for exactly the matching arm.
/// Returns whatever the handlers return (must share a type, or be void). Mirrors the hand-written
/// `switch (placement)` used across the codecs/walk so a new placement forces every visitor to
/// handle it (no silent default).
template <typename OnInline, typename OnBlob, typename OnPackSlice, typename OnSubtree>
decltype(auto) visitPlacement(Placement p, OnInline && on_inline, OnBlob && on_blob,
                              OnPackSlice && on_pack, OnSubtree && on_sub)
{
    switch (p)
    {
        case Placement::Inline:    return on_inline();
        case Placement::Blob:      return on_blob();
        case Placement::PackSlice: return on_pack();
        case Placement::Subtree:   return on_sub();
    }
}
}
```
(Confirm the EXACT `Placement` enumerators in `CasTreeCodec.h`; cover them all, no `default`.)
- [ ] **Step 3: Route only the uniform sites.** Convert each genuinely-parallel `switch (placement)` to `visitPlacement(...)` with lambdas whose bodies are byte-identical to the prior arms. Leave non-uniform sites alone (note them in the commit body).
- [ ] **Step 4: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r5.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -8` → baseline red only (codec round-trips prove the arms are unchanged).
- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPlacement.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp
git commit -m "CAS cleanup R5: visitPlacement exhaustive visitor; route the uniform Placement switches (step 10)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R6: `EventEmitter` — collapse the emission boilerplate

**Files:**
- Modify: `Core/CasEvent.h` (add `EventEmitter`)
- Modify: `Core/CasBuild.cpp` (~24 blocks), `Core/CasGc.cpp` (~30 blocks)
- Test: `src/Disks/tests/` — use existing event-assertion tests as the oracle; if none assert fields directly, add ONE test capturing a representative event's full field set before/after via an event-sink probe (the soak's regression-watch + existing `tree_expand`/`CorruptDangle` probes are the live oracle).

- [ ] **Step 1: Study the emission shape.** Read 4–5 representative blocks in each of `CasBuild.cpp` and `CasGc.cpp`. Capture the invariant seed fields (which are constant per emitter — `namespace_`? `round`? `gen`? the `build_id`/`gc_id`?) vs per-call fields (`type`, `object_kind`, `object_hash`, `ref_name`, `at_version`, `outcome`, `reason`, `detail`). The emitter must reproduce EVERY field exactly, including ones currently set from member state.
- [ ] **Step 2: Design `EventEmitter`** in `CasEvent.h`. It wraps the sink + the per-emitter seed and offers `emit(...)`; it must be **zero-cost when no sink** (guard inside). Sketch (adapt field names to the real `CasEvent`):
```cpp
/// Seeds a CasEvent with the emitter's stable identity + round, then fills per-call fields.
/// Replaces the ~54 hand-assembled `if (store->hasEventSink()) { CasEvent _ev; …; emitEvent(_ev); }`
/// blocks. Emits the IDENTICAL event; no-op (no allocation) when the sink is absent.
class EventEmitter
{
public:
    EventEmitter(Store & store, std::string namespace_, UInt128 actor_round_source /*or refs*/);
    void emit(CasEventType type,
              std::optional<ObjectKind> kind,
              std::optional<UInt128> object_hash,
              std::string outcome,
              std::string reason,
              std::vector<std::pair<String,String>> detail = {},
              /* any other per-call fields the real sites set: ref_name, at_version, round, gen */ ...);
private:
    Store & store;            // for hasEventSink()/emitEvent()
    // seed fields …
};
```
**IMPORTANT:** the real `CasEvent` has many fields and several are set per-call (round/gen/at_version vary even within one emitter). Do NOT bake a field into the seed unless it is provably constant at every call site for that emitter. If round/gen vary per call, pass them per `emit`. The safe transformation is: `emit` takes every field that ANY site varies, and only truly-constant identity (the store/sink) is seeded. Err toward more parameters over a wrong seed.
- [ ] **Step 3: Migrate `CasBuild.cpp` first** (smaller, ~24 blocks). Replace each block with one `emitter.emit(...)`. After each handful, diff the emitted fields against the original block to confirm 1:1. Keep `reason` REQUIRED (never empty where it was non-empty).
- [ ] **Step 4: Build + test Build half.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r6.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasBuild*:CaWiring*:Cas*Gc*Leak*' 2>&1 | tail -8` → baseline red only.
- [ ] **Step 5: Migrate `CasGc.cpp`** (~30 blocks `_ev0.._ev26`) the same way; the algorithm should read cleanly afterward.
- [ ] **Step 6: Build + full test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r6.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -10` → baseline red only.
- [ ] **Step 7: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit -m "CAS cleanup R6: EventEmitter collapses the ~54 CasEvent emission blocks in Build+Gc; identical events, zero-cost when no sink (step 3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R7: `persistGenerationProbingUpward` helper

**Files:**
- Modify: `Core/CasGc.cpp` (extract the loop; call from the fold site `~657-698` and the cascade site `~1755-1795`)

- [ ] **Step 1: Diff the two loops.** Read both copies; confirm they are the same algorithm (same `max_generation_probes=1000` brake, same write-once-per-generation persist). Note any difference — if they differ materially, do NOT merge; report instead.
- [ ] **Step 2: Extract** a private method/free fn carrying the exact loop body, parameterized by whatever the two sites differ in (likely just the snap/generation inputs). Keep the brake constant identical.
- [ ] **Step 3: Route both sites** through it.
- [ ] **Step 4: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r7.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasGc*:Ca*Gc*:CasReuse*' 2>&1 | tail -8` → baseline red only.
- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit -m "CAS cleanup R7: extract persistGenerationProbingUpward; one copy for fold + cascade (step 5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R8: `checkVersion(current, seen, what)` helper

**Files:**
- Modify: `Core/CasCodecUtil.h` (add `checkVersion`)
- Modify: `Core/CasEnvelope.cpp` (`:192,330`), `Core/CasRootShardCodec.cpp` (`:183`), `CasCodecUtil.h` JSON gate (`:264`)

- [ ] **Step 1: Read the 4 gates.** Capture each gate's current message text + thrown code (today `NOT_IMPLEMENTED`; R12 will change to `UNKNOWN_FORMAT_VERSION` — do R12 FIRST or fold the code choice into `checkVersion` here and note R12 is satisfied by this helper). Decide: `checkVersion` throws `UNKNOWN_FORMAT_VERSION` (satisfies R12) for "seen > current"; keep `what` in the message so each site's diagnostic stays specific.
```cpp
/// One gate for "this object was written by a newer version than we understand". Fail-closed:
/// a future on-disk version is rejected (not silently mis-read). `what` names the object for the
/// message. Throws UNKNOWN_FORMAT_VERSION (an operator can tell "newer writer" from a feature stub).
inline void checkVersion(uint32_t current, uint32_t seen, std::string_view what)
{
    if (seen > current)
        throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
            "CAS {}: on-disk version {} is newer than this build supports ({})", what, seen, current);
}
```
(Match the message closely enough that any test asserting on substring still matches, OR update that test in this task. Ensure `ErrorCodes::UNKNOWN_FORMAT_VERSION` is declared in `CasCodecUtil.h`'s TU.)
- [ ] **Step 2: Route the 4 gates** through `checkVersion`. Keep the `CORRUPTED_DATA` (malformed) branches exactly as-is — only the future-version branch changes.
- [ ] **Step 3: Update tests** that assert the old `NOT_IMPLEMENTED` future-version code → `UNKNOWN_FORMAT_VERSION` (search `gtest_cas_*` and `cas_test_helpers.h` for version-gate assertions).
- [ ] **Step 4: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r8.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*Codec*:Cas*:Ca*' 2>&1 | tail -10` → version-gate tests PASS with the new code; baseline red only.
- [ ] **Step 5: Commit** (this commit also delivers R12).
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "CAS cleanup R8+R12: checkVersion() gate throwing UNKNOWN_FORMAT_VERSION (was NOT_IMPLEMENTED) at the 4 future-version sites; update test contract (step 9, F5)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R9: Named `writeU128`/`readU128` wire helpers (byte-identical)

**Files:**
- Modify: `Core/CasCodecUtil.h` (add named helpers for the LE-binary and BE-protobuf orders; hex pair already exists in `CasIds.h`)
- Modify: the ad-hoc u128 (de)serialization sites: `CasRootShardCodec.cpp:31` (`u128ToBytes` BE), `CasEnvelope.cpp`/`CasTreeCodec.cpp` (LE binary), and any others found

- [ ] **Step 1: Inventory** every place a `UInt128` is turned to/from bytes. For each, record which byte order it uses (LE binary via `writeBinaryLittleEndian`? BE via the hand-rolled `u128ToBytes`? hex?). These orders are FROZEN on-disk formats.
- [ ] **Step 2: Add named helpers** (do NOT change any byte):
```cpp
/// On-disk UInt128 wire forms. These byte orders are FROZEN (changing them breaks existing objects).
/// Named so a 128-bit (de)serialization can never be mis-paired with the wrong order.
inline void writeU128LE(WriteBuffer & out, const UInt128 & v) { writeBinaryLittleEndian(v, out); }
inline UInt128 readU128LE(ReadBuffer & in) { UInt128 v; readBinaryLittleEndian(v, in); return v; }
// BE form must reproduce the EXACT bytes of the current hand-rolled u128ToBytes/fromBytes:
inline std::array<uint8_t,16> u128ToBytesBE(const UInt128 & v) { /* copy current impl verbatim */ }
inline UInt128 u128FromBytesBE(const uint8_t * p) { /* copy current inverse verbatim */ }
```
(Copy the current implementations VERBATIM — this task only *names* them. The hex pair `u128ToHex/hexToU128` already exists; leave it.)
- [ ] **Step 3: Route the sites** through the named helpers, one order per site, unchanged bytes.
- [ ] **Step 4: Byte-equality oracle.** Ensure the existing codec round-trip gtests cover envelope/tree/root-shard; they already assert decode(encode(x))==x. Add (or confirm) a golden test that encodes a fixed struct and asserts the exact byte vector is unchanged from a captured constant for at least one codec per byte-order.
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r9.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*Codec*:Cas*Envelope*:Cas*Tree*:Cas*RootShard*' 2>&1 | tail -8` → all PASS.
- [ ] **Step 5: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEnvelope.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.cpp src/Disks/tests/gtest_cas_codecs.cpp
git commit -m "CAS cleanup R9: named writeU128/readU128 wire helpers per frozen byte order; route ad-hoc sites (no byte change) (step 9)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R10: `JsonObjectWriter` for the 4 strict-JSON encoders (byte-identical)

**Files:**
- Modify: `Core/CasCodecUtil.h` (add `JsonObjectWriter`)
- Modify: `Core/CasWatermark.cpp`, `Core/CasHeartbeat.cpp`, `Core/CasPoolMeta.cpp`, `Core/CasRootsRegistry.cpp` (encode sides)
- Test: golden byte-equality per encoder

- [ ] **Step 1: Capture goldens FIRST.** Before touching any encoder, add a gtest that encodes a fixed instance of each of `ServerWatermark`/`Heartbeat`/`PoolMeta`/`RootsRegistry` and asserts the exact resulting string equals a captured literal (copy the literal from the current output). This is the oracle that the refactor must not change a byte.
Run it → PASS (proves the goldens match current code).
- [ ] **Step 2: Study the 4 encoders.** Confirm they all emit `{format,version,…fields…}` strict JSON with the same brace/comma/quoting rules. Note field order (must be preserved exactly).
- [ ] **Step 3: Design `JsonObjectWriter`** — a tiny RAII helper that emits a strict-JSON object with the SAME comma/brace/escaping the hand-written encoders produce (verify the escaping rules against the current code; if they use a specific escaper, reuse it):
```cpp
/// Emits a strict-JSON object to a WriteBuffer with the exact brace/comma/quoting the CAS metadata
/// encoders use. Field order is the caller's responsibility (call key() in the legacy order).
class JsonObjectWriter
{
public:
    explicit JsonObjectWriter(WriteBuffer & out);   // writes '{'
    void field(std::string_view key, std::string_view str_value);   // "key":"value"
    void field(std::string_view key, uint64_t num_value);           // "key":123
    // … the value kinds the 4 encoders actually use …
    void finalize();                                                 // writes '}'
private:
    WriteBuffer & out; bool first = true;   // inserts ',' between fields
};
```
(Match the EXACT escaping/number formatting the current encoders use — if they use `writeJSONString` or a manual escaper, route through the same one so bytes are identical.)
- [ ] **Step 4: Convert the 4 encoders** to use `JsonObjectWriter`, preserving field order. After each, run its golden test → must still PASS (byte-identical).
- [ ] **Step 5: Build + full codec test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r10.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -10` → goldens + round-trips PASS; baseline red only.
- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasCodecUtil.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasPoolMeta.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootsRegistry.cpp src/Disks/tests/gtest_cas_codecs.cpp
git commit -m "CAS cleanup R10: JsonObjectWriter for the 4 strict-JSON metadata encoders; byte-identical (golden-asserted) (step 9, F9)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R11: Unify the config keys (atomic: code + all configs + tests)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorageFactory.cpp` (`:228`, `:242-249`)
- Modify: `utils/ca-soak/configs/storage_conf.xml` and ANY other config setting these keys (grep the whole repo)
- Modify: any gtest/integration config referencing the old keys

- [ ] **Step 1: Grep every reference.** `grep -rn "cas_scratch_path\|content_addressed_gc_enabled\|content_addressed_gc_interval_sec\|content_addressed_root_shards\|content_addressed_dedup_cache_bytes\|content_addressed_dedup_head_first_min_bytes\|content_addressed_gc_snap_generations_to_keep" --include=*.cpp --include=*.xml --include=*.yml --include=*.yaml --include=*.md .` — list ALL.
- [ ] **Step 2: Decide the bare-key names** (matching the disk-node norm): `scratch_path`, `gc_enabled`, `gc_interval_sec`, `root_shards`, `dedup_cache_bytes`, `dedup_head_first_min_bytes`, `gc_snap_generations_to_keep`. Keep the SAME defaults and SAME semantics in `MetadataStorageFactory.cpp`.
- [ ] **Step 3: Rename in the factory** (the `config.getX(prefix + ".<oldkey>", default)` reads). Same defaults.
- [ ] **Step 4: Update EVERY config** found in Step 1 (esp. `utils/ca-soak/configs/storage_conf.xml`) to the new key names. A missed reference = the disk silently falls back to the default → behavior change; this is why the grep must be exhaustive.
- [ ] **Step 5: Build + test + a config-bringup smoke.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r11.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -8` → baseline red only. Then confirm `grep -rn "content_addressed_\|cas_scratch_path" --include=*.cpp --include=*.xml .` returns no stale config keys (code may still use `content_addressed` as the disk *type* string — that is NOT a config key and stays).
- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorageFactory.cpp utils/ca-soak/configs/storage_conf.xml
git commit -m "CAS cleanup R11: unify disk config keys to bare snake_case under the disk node (F4)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R12: (folded into R8) `UNKNOWN_FORMAT_VERSION`

Delivered by Task R8 (the `checkVersion` helper throws `UNKNOWN_FORMAT_VERSION`). No separate task; verify the R8 commit covers all 3 future-version sites + the test contract. If any future-version `NOT_IMPLEMENTED` remains after R8, fix it here:
- [ ] **Step 1:** `grep -rn "NOT_IMPLEMENTED" Core/CasEnvelope.cpp Core/CasRootShardCodec.cpp` → only genuine *unsupported-operation* throws remain (none for future-version). If a future-version one remains, route it through `checkVersion` and commit.

---

## Task R13: `WriteResult{outcome,token}` + reuse `casRemoveObject` in `dropNamespace`

**Files:**
- Modify: `Core/CasBackend.h` (the 3 write methods' signature: drop `Token * out_token`, return a `WriteResult`)
- Modify: `Core/CasObjectStorageBackend.{h,cpp}`, `Core/CasInMemoryBackend.{h,cpp}`, `Core/CasInstrumentedBackend.{h,cpp}` (the 3 impls)
- Modify: every caller of the 3 write methods (`Core/CasStore.cpp`, `Core/CasBuild.cpp`, `Core/CasGc.cpp`, keepers, probe) — read the token from the returned struct
- Modify: `Core/CasStore.cpp::dropNamespace` (`:733-746`) to reuse `casRemoveObject`

- [ ] **Step 1: Define `WriteResult`** in `CasBackend.h`:
```cpp
/// Result of a backend write: the put/cas outcome plus the resulting object token (the value
/// previously returned via an out-parameter). Lets callers read `.token` without an `if(out_token)`.
struct WriteResult
{
    PutOutcome outcome;     // or the existing per-method outcome enum; keep the SAME type each method returned
    Token token;
};
```
(If the 3 methods return different outcome enums (`PutOutcome`/`CasOutcome`), make `WriteResult` a template or one per method — keep each method's outcome type EXACTLY as today.)
- [ ] **Step 2: Change the interface + 3 impls** to return `WriteResult` instead of writing `*out_token`. The token VALUE produced must be identical to today.
- [ ] **Step 3: Update every call site** to use `res.outcome`/`res.token`. Where a caller passed `nullptr` for `out_token` (didn't want the token), just ignore `res.token`.
- [ ] **Step 4: `dropNamespace` reuse.** Replace the inlined head+conditional-delete+bounded-body in `CasStore.cpp:733-746` with a call to the existing `casRemoveObject` helper (confirm the helper's semantics match the inlined code EXACTLY — same retry bound, same conditional-delete, same outcomes; if it differs, do NOT substitute).
- [ ] **Step 5: Build + full test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r13.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -10` → baseline red only.
- [ ] **Step 6: Commit.**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInstrumentedBackend.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp
git commit -m "CAS cleanup R13: WriteResult{outcome,token} retires the out_token out-param across the backend seam; dropNamespace reuses casRemoveObject (step 15)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task R14: (GATED) merge `WatermarkKeeper` + `HeartbeatKeeper` → `SingleWriterSlotKeeper`

> **Gate:** attempt this LAST. If the spec-compliance or code-quality review finds the two policies
> cannot factor without blurring the single-writer/liveness safety contract, **DEFER R14** (revert
> its commit) and ship R1–R13. The 6h soak is the final validator.

**Files:**
- Create: `Core/CasSingleWriterSlot.{h,cpp}`
- Modify: `Core/CasWatermark.{h,cpp}`, `Core/CasHeartbeat.{h,cpp}` (become thin policy wrappers / typedefs over the base)
- Test: existing `gtest_cas_*` watermark/heartbeat tests are the oracle; add none unless a gap is found.

- [ ] **Step 1: Side-by-side diff.** Put `CasWatermark.cpp:198-253` next to `CasHeartbeat.cpp:144-202`. Enumerate EVERY difference: body-encoder (watermark `ServerWatermark{epoch,min_active}` vs heartbeat `Heartbeat{created_at_ms}`); terminal op (`farewell` sets `min_active=UINT64_MAX` vs heartbeat `discard`→`deleteExact`); foreign-touch fail-close wording; the `min_active_fn` hook (watermark only); `farewell()` (watermark only, **no production caller**).
- [ ] **Step 2: Design the base** `SingleWriterSlotKeeper` owning the common machinery (`start`/`renewOnce`/`startBackground`/`stopBackground`/`backgroundLoop`/`lastRenewTime` + the `seq`/`last_token`/`dead`/thread/sync members + the head→putIfAbsent/putOverwrite anchor dance + foreign-touch fail-close). The deltas become explicit policy: a body-encoder hook and a terminal-op hook (and the optional `min_active_fn`). Keep `farewell()` as a watermark-policy method (preserve it; it is uninvoked but part of the contract).
- [ ] **Step 3: Reimplement** `WatermarkKeeper` and `HeartbeatKeeper` as thin subclasses/instantiations supplying their policy. Every observable op (the exact JSON body bytes, the anchor sequence, the renew cadence, the fail-close conditions/messages) must be IDENTICAL to today.
- [ ] **Step 4: Build + test.**
Run: `ninja -C build unit_tests_dbms > build/build_cleanup_r14.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -10` → baseline red only. If any watermark/heartbeat/GC-lease test changes behavior, STOP — the merge blurred the contract; revert and defer.
- [ ] **Step 5: Commit (only if green + reviews pass).**
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasWatermark.* src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.*
git commit -m "CAS cleanup R14: SingleWriterSlotKeeper base merges Watermark+Heartbeat keepers; deltas as explicit policy; identical on-storage behavior (step 8, C3)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final gate (after R1–R13, and R14 if it landed)

- [ ] **Full unit sweep.** `ninja -C build unit_tests_dbms > build/build_cleanup_final.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' 2>&1 | tail -20` → ALL green except the known baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow`.
- [ ] **Rebuild the server binary.** `ninja -C build clickhouse > build/build_cleanup_server.log 2>&1; tail -3` → clean.
- [ ] **6h chaos soak with 30-min reports.** Fresh-restart per `reference_ca_soak_fresh_restart` (archive+clean syslog-owned `ch1`/`ch2` via a root container; `down -v`); then `SEED=20260623 DURATION=6h WORKERS=6 METRICS=soak_cleanup_6h.db MAX_POOL_GB=40 bash utils/ca-soak/scripts/run_24h.sh`. Arm a 30-min status Monitor (containers, latest tick, MAX `unreach`/`dangling`, `Code 499`/`<Fatal>`, `workload_failures`, stage). **Watch disk free space** (the prior run filled the host) — abort + report if `df /` approaches full.
- [ ] **Success criteria:** `dangling=0` throughout; `unreachable` drains toward ~0; replicas converge; regression-watch all 0; soak prints `PHASE3 OK`.

## Notes for the implementer
- Branch `cas-vfs-path-mapping`; never master; new commits only (no rebase/amend).
- This is a REFACTOR: read the live code at each site; reproduce behavior exactly; prove with the oracle.
- If any task cannot be made provably behavior-preserving, STOP and report rather than guessing.
- R12 is folded into R8. R14 is gated (defer if review flags). The remaining order is R1→R13 then R14.

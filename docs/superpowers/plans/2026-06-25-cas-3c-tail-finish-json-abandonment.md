# CAS Finish JSON Abandonment — Implementation Plan (Plan 3c-tail)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Checkbox steps.

**Goal:** Convert the last three JSON objects — **`CasHeartbeat`, `CasRootsRegistry`, `CasGcOutcomes`** — to protobuf (same framing-header pattern as 3a/3c), replace `CasGcSnap`'s use of the monotone `checkVersion` with an inline version check, and then **delete the entire JSON codec family** from `CasCodecUtil.h` plus `checkVersion` and the dead `tolerateUnknownKeys`. After this CA has exactly two encodings: canonical binary (blob/tree) and protobuf (mutable). (3c converted pool-meta/watermark/gc-state/retired-set; the deletion was blocked because these three + gc-snap still used the family.)

**Architecture:** Identical to the proven 3a/3c pattern — per object: a protobuf message in `cas_root_shard.proto`, `encodeX` = `writeFramingHeader(out, MAGIC, currentWriterVersion(FormatId::X))` + serialized body, `decodeX` = `readFramingHeader(in, MAGIC, "x")` + `ParseFromArray`, keeping every JSON-era invariant as a post-parse check with the same error code; UInt128 → raw 16-byte BE via `u128ToBytesBE`/`u128FromBytesBE`; sets/maps → sorted `repeated` (NO proto `map<>`). gc-snap stays binary but its one `checkVersion(GC_SNAP_VERSION, version, ...)` call becomes an inline `if (version > GC_SNAP_VERSION) throw Exception(UNKNOWN_FORMAT_VERSION, ...)`. Then the JSON helpers are removed.

**FormatIds / magics:** `CasFormat::FormatId` already has the needed enumerators for the converted-in-3c set; ADD enumerators for any of these three that lack one (Heartbeat, RootsRegistry, GcOutcomes) — append to the `FormatId` enum (new numbers, do not renumber). Magics: heartbeat=`CAHB`, roots-registry=`CARR`, gc-outcomes=`CAGO`.

**Branch:** `cas-vfs-path-mapping` (no master, no amend/rebase). Allman, `-Werror`. Pre-release — no migration.

**Build & test:** `cd build && cmake . && ninja unit_tests_dbms > cas_3ct_build.log 2>&1` (no `-j`); full sweep `--gtest_filter='Cas*:Ca*'`; only tolerated red = baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**Scope guards:** the three codecs + gc-snap's version check + the proto + the `CasCodecUtil`/`CasFormat` deletions + tests. Does NOT touch the tree/envelope/manifest/part-writer (done), nor convert gc-snap's BODY to protobuf (B176 deferred — only its `checkVersion` call changes). Out of scope: B92, Part IV, B164b, 3d (consolidation).

---

### Task 1: `CasHeartbeat` → protobuf
**Files:** `cas_root_shard.proto`, `CasHeartbeat.cpp` (+ its gtest). Read `CasHeartbeat.h`/`.cpp` to enumerate fields; add `HeartbeatProto`; magic `CAHB`; `FormatId::Heartbeat` (add to the enum if missing). Keep every invariant the JSON decode enforced as post-parse checks. TDD round-trip + future-min_reader fail-closed test. Commit `CA: heartbeat JSON -> protobuf (framing header)`.

### Task 2: `CasRootsRegistry` → protobuf
**Files:** `cas_root_shard.proto`, `CasRootsRegistry.cpp` (+ gtest). Read the codec; the registry is a pool-global set/list (B129) — model the collection as sorted `repeated` (deterministic), NOT proto `map<>`. `RootsRegistryProto`, magic `CARR`, `FormatId::RootsRegistry`. Keep invariants. Commit `CA: roots-registry JSON -> protobuf (framing header)`.

### Task 3: `CasGcOutcomes` → protobuf
**Files:** `cas_root_shard.proto`, `CasGcOutcomes.cpp` (+ gtest). The outcome log is a sequence — model as `repeated` entries in insertion/sorted order matching the JSON. `GcOutcomeLogProto`, magic `CAGO`, `FormatId::GcOutcomes`. Note: `CasGcOutcomes.cpp` also has `objectKindFromString` for an enum field — switch that to a proto enum or the existing `objectKindToProto`/`objectKindFromProto` helper used in 3c (reuse, don't duplicate). Keep invariants. Commit `CA: gc-outcomes JSON -> protobuf (framing header)`.

### Task 4: gc-snap — drop the shared `checkVersion`
**Files:** `CasGcSnap.cpp`. Replace the `checkVersion(GC_SNAP_VERSION, version, "gc/snap")` call with an inline guard preserving identical behavior:
```cpp
if (version > GC_SNAP_VERSION)
    throw Exception(ErrorCodes::UNKNOWN_FORMAT_VERSION,
        "CAS gc/snap: on-disk version {} is newer than this build supports (max {})", version, GC_SNAP_VERSION);
```
(gc-snap stays binary; this only removes its dependency on the soon-deleted shared helper. Ensure `UNKNOWN_FORMAT_VERSION` is in its ErrorCodes block.) Build; gc-snap tests still pass. Commit `CA: gc-snap uses an inline version check (drop shared checkVersion dep)`.

### Task 5: Delete the JSON codec family + `checkVersion` + `tolerateUnknownKeys`
**Files:** `CasCodecUtil.h`, `CasFormat.{h,cpp}`, `gtest_cas_format.cpp`, plus any remaining JSON-golden tests.
- [ ] **Step 1 (gate):** `grep -rn "JsonObjectWriter\|parseJsonDocument\|requireObject\|requireString\|requireU64\|requireHash\|requireStringMap\|requireArray\|requireObjectAt\|requireKey\|checkNoUnknownKeys\|decodeJsonGuarded\|writeJsonString\|writeJsonKey\|checkVersion\|tolerateUnknownKeys\|objectKindFromString" src/Disks/ | grep -v tests` — MUST be empty before deleting. If anything remains, convert it first (do not delete with live callers).
- [ ] **Step 2:** Delete those symbols from `CasCodecUtil.h` (keep the binary helpers: `writeU128LE`/`readU128LE`/`u128ToBytesBE`/`u128FromBytesBE`/`readFixedBytes`/`decodeGuarded`). Drop now-unused `<Poco/JSON/*>` and `<Poco/Dynamic/*>` includes. Update the file's top doc comment (binary helpers only; "mutable objects are protobuf — see CasFormat + cas_root_shard.proto").
- [ ] **Step 3:** Delete `tolerateUnknownKeys` (`CasFormat.h`/`.cpp`) and its test.
- [ ] **Step 4:** Delete the remaining JSON-golden tests (heartbeat/roots-registry/gc-outcomes goldens, the `CasJsonGolden` suite) — their formats no longer exist. Keep the protobuf round-trip tests.
- [ ] **Step 5:** Build clean; full sweep green (baseline only); re-run the Step-1 grep → empty. Commit `CA: delete the JSON codec family + monotone checkVersion + tolerateUnknownKeys (two encodings)`.

### Task 6: Full sweep (no commit) — confirm only baseline red + the grep gate is clean.

---

## Self-Review (inline)
**Spec coverage:** completes "abandon JSON / two encodings" (Part I + the JSON-abandon note + Part V CasCodecUtil deletion). All seven mutable JSON objects now protobuf (4 in 3c + 3 here); gc-snap stays binary with its own version guard; JSON family + checkVersion + tolerateUnknownKeys deleted. ✓
**Placeholder scan:** per-object procedure is the proven 3a/3c pattern (read codec → message → framing → keep invariants → test); the deletion task has an explicit grep gate + symbol list. Field enumeration per object is "read the codec" (worked in 3c). ✓
**Type consistency:** framing helpers + `FormatId` (add 3 enumerators); magics 4 bytes; UInt128 via BE helpers; sorted repeated (no map<>); reuse `objectKindToProto`/`objectKindFromProto` from 3c. ✓
**Risk:** gc-outcomes' `objectKindFromString` must map onto the existing proto enum helper (don't reintroduce string parsing). Keep every post-parse invariant. Reviews + soak backstop.

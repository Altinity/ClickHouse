---
description: 'Implementation plan for CAS codecs v3 phase 5: converting the CARN block-framed binary GC run format to sorted NDJSON (record stream) with a whole-file seal-checksum verified before use, a line-based k-way merger, and the deletion of the seek/footer/inDegree machinery and the part-manifest-cleanup run. The highest-risk phase (byte-adoption determinism + the GC data plane).'
sidebar_label: 'CAS codecs v3 phase 5 plan'
sidebar_position: 65
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase5-runs
title: 'CAS Codecs V3 — Phase 5: Runs (Record Stream) Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 5: Runs (Record Stream) Text Cutover Implementation Plan {#cas-codecs-v3-phase5}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (explicit):** this plan is written against **the phase-2 draft integrated** (the control-plane cutover; `docs/superpowers/plans/2026-07-15-cas-codecs-v3-phase2-control-plane.md`) as the assumed-landed foundation — it relies on the phase-1/phase-2 `Core/Formats/` surface (`CasTextFormat`, the pinned `jsonWriteSettings`, `CasWireVocab`, the `FormatId::RunFile` registry entry + `cas_run` `TRAITS` row: `RecordStream`, `Strict`, `PinnedRaw`, line cap 4 KiB), and it edits the phase-2 `Core/Formats/CasFoldSealFormat` (NOT the pre-phase-2 `CasGenerationSeal`). The **code draft of this phase must be written against the integrated phase-2 mainline, not a patch stack** — this is why the lead gates the draft until after phase-2 integration + soak.

**Goal:** convert the GC data-plane run objects (`cas_run`) from the `CARN` dense block-framed binary format to **sorted NDJSON**: header line `{"type":"cas_run","v":N,"kind":"<kind>"}`, one JSON record per line in today's binary sort order, and a `{"n":<count>}` trailer. Integrity moves from per-block `CRC32C` to the **whole-file seal-checksum** (`RunRef.checksum`, CityHash128, carried by the referencing fold seal), accumulated during the full sequential read and **verified before the read is acted on**. `RunFileReader::seek`, `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`, the `CARN` block/footer machinery, and the **part-manifest-cleanup run** (+ the fold seal's `part_manifest_cleanup` field + `partManifestCleanupKey`) are DELETED. The k-way merger is rewritten line-based. `PinnedRaw` + `Strict`: the run is byte-deterministic for `putDeterministicArtifact` adoption. This is the **highest-risk phase** of the migration.

**Architecture:** `cas_run` is the `RecordStream` family — unbounded-cardinality sorted records, written once per GC generation/attempt/shard, always read whole sequentially (the spec's two in-repo findings: ranged `seek` has no production caller, and no consumer point-looks-up into a run). The codec moves to `Core/Formats/CasRecordStreamFormat` (renamed from `CasRunFile`); the record shape is kind-aware (each `RunKind` renders its key+payload as named JSON fields matching today's sort order over fixed-width lowercase hex). Accepted cost: hex NDJSON is ≈2× the retired binary (spec §record-streams); measured in soak, fallback = re-binarize `cas_run` alone.

Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` §migration-order step 5; reference: `docs/superpowers/cas/codecs_proposal_v3.md` §record-streams + §determinism.

**Tech Stack:** C++ (`dbms`), the phase-1 `CasTextFormat` helpers + `JsonObjectReader`, `ReadHelpers`/`WriteHelpers`, gtest (`unit_tests_dbms`).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere.
- **Layering (physical):** `Core/Formats/CasRecordStreamFormat.h` may include only `Formats/` headers + the identifier vocabulary (`CasBlobRef`, `CasBlobDigest`, `CasToken`, `CasRefIds`, `CasWireVocab`) + IO primitives — NEVER `CasBackend.h`/`CasStore.h`. **Exception to note:** today's `RunFileReader` streaming ctor takes a `Backend &` (it reads a run object off the backend). The v3 `RecordStream` reader must keep a streaming mode, so it needs a minimal read interface. Resolve this in Task 1 (either a tiny `ReadBuffer`-over-backend adapter passed in by `Core/`, or keep the streaming open in `Core/` and hand the codec a `ReadBuffer`) so the Formats codec stays backend-free — flag the chosen approach as an interface decision.
- **Determinism (HARD — this phase's headline risk):** `cas_run` is `PinnedRaw` + `Strict` and goes through `putDeterministicArtifact` (a retrying/failing-over leader re-encodes and the backend rejects any byte drift as `CORRUPTED_DATA`). The writer MUST be byte-reproducible: records emitted in the exact binary sort order (tuple over fixed-width lowercase hex = today's key order), fixed field order per record, strict keys, no compression, no floats. The determinism test (same record set, different append/input order → byte-equal output) is a hard gate on both the writer (Task 2) and the merger (Task 4).
- **Seal-checksum integrity (HARD):** the read unit is the WHOLE file; the guard is `RunRef.checksum` (CityHash128 over the stored bytes) carried by the fold seal. Every consumer accumulates the hash while streaming and verifies it against the seal **before acting on what it read** (a deletion decision). The trailer `n` additionally catches truncation at a line boundary (NDJSON's blind spot).
- **Pre-release, hard cutover, no dual-read:** the `CARN` codec is deleted; no "sniff CARN vs NDJSON".
- **Error taxonomy:** malformed / truncated / trailer-count mismatch / seal-checksum mismatch / unknown `kind` / out-of-order key / duplicate key → `CORRUPTED_DATA`; future `v` / unknown `!`-key → `UNKNOWN_FORMAT_VERSION`.
- **Consumer-sweep discipline (mandated — the T9/phase-7 blast-radius lesson):** EVERY deletion (seek, `inDegreeInGeneration`, `seekPrefix`, block/footer machinery, the part-manifest-cleanup run, the fold-seal field, `partManifestCleanupKey`) has an explicit `grep`-sweep step in its task that enumerates and fixes/removes every consumer BEFORE the deletion. No deletion lands without its sweep printing zero residual references.
- **Build/commit discipline** (as prior phases): `flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p5t<N>.log 2>&1; echo "NINJA_EXIT=$?"`, foreground only, subagent-analyze, commit per task, explicit-path `git add`, trailer:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## The v3 record-stream shape {#record-shape}

```text
{"type":"cas_run","v":3,"kind":"source_edge"}      header line (identity + version + kind gate; v = G_BUILD, as every phase)
{"b":"01<digest-hex>","s":"<32hex>","m":"edge"}     one JSON record per line, SORTED (binary key order)
{"b":"01<digest-hex>","s":"<32hex>","m":"zero"}
...
{"n":184267}                                        trailer: record count (line-truncation guard)
```

- **Header line** carries `type`, `v`, and `kind` (the `RunKind` word). `openSourceEdgeRun`-style typed opens validate all three before any record is interpreted; an unknown `kind` fails closed.
- **Records** are one JSON object per line, keys in today's sort order. The record fields are **kind-specific** (each `RunKind` renders its binary key + payload as named JSON fields whose tuple order over fixed-width lowercase hex equals the current binary key order — the property the k-way merge and the fold's sorted-scan both depend on). Marker values are **words** (`edge` / `zero` / `condemned`), via `CasWireVocab`-style word maps local to the run codec.
- **Trailer** `{"n":N}` — `N` = record count. Guards truncation at a line boundary. No footer index, no per-block CRC, no `seek`.
- **Integrity:** the whole-file CityHash128 (`RunRef.checksum`) is accumulated during the full read and verified against the seal before the fold acts; the trailer `n` cross-checks line-truncation.

### Per-`RunKind` record fields (grounded in the survey) {#kind-fields}

Only ONE record-stream kind is live in the `cas_run` object family: **`source_edge`**. The survey confirmed the rest:

| `RunKind` | Live? | Disposition in phase 5 |
|---|---|---|
| `SourceEdge` (3) | YES — the GC in-degree data plane | the only `cas_run` kind; full NDJSON mapping below |
| `BlobDelta` (2) | **DEAD** — no producer/consumer | drop from the new kind vocabulary (do NOT carry it) |
| `TargetShardDelta` (5) | **DEAD** — no producer/consumer | drop from the new kind vocabulary |
| `ManifestEntries` (4) | live in TWO places, NEITHER is a `cas_run` object | see boundary note |

**`ManifestEntries` boundary (do NOT touch in phase 5):** it has two producers with different `key_schema` — (a) the part-manifest **cleanup bundle** (`key_schema` 1, written at `CasGc.cpp:1610` under `partManifestCleanupKey`) which is DELETED in **Task 7**; and (b) the **embedded run inside a `CAPT` part manifest** (`key_schema` 0, `CasManifestCodec.cpp:123` writer / `:165` reader) which is **PHASE 6's surface** — phase 5 must NOT touch the embedded-manifest stream or the `ManifestEntries` enumerator. State this boundary in Tasks 1 and 5.

**`source_edge` record mapping.** Source-edge row tags (`CasBlobInDegree.h:24-26`): `kEdgeActive=0x01`, `kZeroMarker=0x00`, `kCondemned=0x02`. The key is produced by `SourceEdgeKeyCodec` as `(algo, digest, source_id)` bytes; the sort key is that tuple in BYTE order.

| JSON key | From | Rendering | Sort role |
|---|---|---|---|
| `b` | blob ref (algo + digest) | algo-prefixed lowercase hex: `<algo-byte 2-hex><digest-hex-at-width>` | primary sort component |
| `s` | source id (`UInt128`) | 32-char lowercase hex | secondary sort component |
| `m` | row tag | word: `edge` (`kEdgeActive`) / `zero` (`kZeroMarker`) / `condemned` (`kCondemned`) | not a sort component (payload) |

**HARD sort-order requirement (survey §8, `gtest_cas_gc_source_edge` ordering tests):** the NDJSON record key MUST reproduce the current binary `(algo, digest, source_id)` BYTE order under fixed-width lowercase-hex tuple comparison — i.e. sorting records by `(b, s)` string-lexicographically MUST equal today's binary key order (lowercase hex preserves unsigned byte order: digits sort below `a`–`f`). This is what the fold's two-cursor merge and the k-way merge depend on; it is a golden-test-pinned invariant, not a convention.

## Tasks {#tasks}

Per the DAG (`…-phase2…` §dag-phase5): the run codec is **[INDEPENDENT]** (draftable against the phase-2 wire vocabulary in parallel with phase-2 integration); the coupled deletions are **[PHASE-2-DEPENDENT]**. Each deletion carries an explicit consumer-sweep step whose grep MUST print zero residual references before it lands. **These tasks are specifications** (deferred draft): the JIT executor writes the C++ against the integrated phase-2 tree; here we fix scope, interfaces, the exact consumer file:line lists, invariants, and the RED tests. Code sketches appear only where load-bearing (the record encode, the checksum contract, the determinism/corruption tests).

### Task 1 [INDEPENDENT] — `Formats/CasRecordStreamFormat` skeleton + header/trailer + typed opens {#task1}

Move `Core/CasRunFile.{h,cpp}` → `Core/Formats/CasRecordStreamFormat.{h,cpp}`. Produce: the `RunKind` enum PRUNED to the live set (drop `BlobDelta`/`TargetShardDelta`; keep `SourceEdge`; keep `ManifestEntries` ONLY as the phase-6-owned embedded-manifest kind — comment that phase 5 does not encode/decode it as a `cas_run`); the `RunKind` word map (`source_edge`); the header-line writer (`{"type":"cas_run","v":N,"kind":"<word>"}`) and reader/gate; the `{"n"}` trailer; and the `openSourceEdgeRun` typed opens (validate `type`/`v`/`kind` before any record). **Interface decision (Global Constraints — resolve here):** the streaming reader needs to read a run object off a `Backend`, but `Formats/` may not include `CasBackend.h`. Resolve by having `Core/` open the stream and hand the codec a `ReadBuffer &` (the codec stays backend-free); `openSourceEdgeRun(Backend&, key)` stays in `Core/CasBlobInDegree` as a thin adapter that opens the stream and constructs the `Formats/` reader over it. Document the chosen shape. Battery row: register `FormatId::RunFile` with a stub source-edge encode (real records land in Task 2). Consumes: phase-1 `CasTextFormat`.

### Task 2 [INDEPENDENT] — NDJSON writer + `source_edge` record encoder + determinism {#task2}

The sorted-record writer: `append(record)` in non-decreasing `(b,s)` order (the writer **asserts** order and throws `CORRUPTED_DATA`/`LOGICAL_ERROR` on a regression — this replaces the old `prev_key` monotonicity check). The `source_edge` record encoder renders `b` (algo-prefixed digest hex via `CasWireVocab`/`codecFor(algo).toHex`), `s` (`UInt128` → 32 hex), `m` (marker word) — see §kind-fields. The `{"n":<count>}` trailer. `PinnedRaw` (no compression). **Determinism test (HARD gate):** build the same source-edge record set, feed it in two different pre-sorted encounters (the writer requires sorted input, so the test constructs the identical sorted sequence twice) → assert `encode(a) == encode(b)` byte-for-byte; AND a golden NDJSON text file pinned. **Sort-order golden (§kind-fields HARD requirement):** a fixed set of `(algo, digest, source_id)` tuples spanning the algo/digest/source boundaries, asserted that string-sorting the emitted `(b,s)` equals the binary `(algo,digest,source_id)` order — reproduces the `gtest_cas_gc_source_edge` ordering invariant. Consumes: Task 1, `CasWireVocab`.

### Task 3 [INDEPENDENT] — NDJSON reader + `{"n"}` verify + NET-NEW whole-file seal-checksum {#task3}

**Survey finding — this is NEW functionality, not a port:** `RunRef.checksum` (CityHash128 over run bytes) is COMPUTED today at exactly two seal sites (`CasBlobInDegree.cpp:561`, `CasGc.cpp:1622`) but **verified on NO read path** — current on-read integrity is only the per-block `CRC32C` internal to `CasRunFile` (which this phase deletes). So the spec's "whole-file seal-checksum verified before use" must be BUILT here and WIRED into every reader.

Produce: the sequential NDJSON reader (borrowed `std::string_view` + streaming `ReadBuffer &`), reading records in stored order; it **accumulates the CityHash128 over the raw stored bytes as it streams** and exposes `verifiedChecksum()` / a `verifyAgainst(UInt128 expected)` that throws `CORRUPTED_DATA` if the accumulated whole-file hash != the seal's `RunRef.checksum` — the caller calls it **after draining the run and BEFORE acting on the records** (the deletion decision). The trailer `n` is verified against the record count (line-truncation guard). RED tests: (a) flip one byte anywhere in a sealed run → `verifyAgainst` throws `CORRUPTED_DATA` before any record is consumed for a decision; (b) drop the trailer / wrong `n` → `CORRUPTED_DATA`; (c) truncate at a line boundary → `CORRUPTED_DATA`. **This task also DELETES `RunFileReader::seek`** — sweep first:

```bash
grep -rn '\.seek(\|->seek(' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/ src/Disks/tests/ | grep -viE 'CasObjectStorageBackend|ReadBuffer'
# EXPECT: only CasBlobInDegree.cpp:600 (inside inDegreeInGeneration, which Task 5 deletes) + the 5 seek unit
# tests in gtest_cas_run_file.cpp (which die here). CasObjectStorageBackend.cpp:456/492 are ReadBuffer::seek — UNRELATED, keep.
```

Consumes: Tasks 1/2.

### Task 4 [INDEPENDENT] — the merge rewrite (two-cursor fold loop + test-only `RunMerger`) {#task4}

**Survey finding — there is NO production `RunMerger`:** the fold's merge is the hand-rolled **two-cursor loop in `foldDeltasIntoGeneration`** (`CasBlobInDegree.cpp:330`), driven by `PriorEdgeCursor` (`CasBlobInDegree.cpp:50`) reading the prior generation's source-edge runs while the new generation's edges are folded in. `RunMerger` (`CasRunFile.cpp` + `gtest_cas_run_file.cpp`) is **test-only**. So this task targets BOTH: (a) re-point `PriorEdgeCursor` + the two-cursor loop at the `CasRecordStreamFormat` reader (line-based, O(one record) resident), and (b) rewrite the test-only `RunMerger` line-based (or delete it if the two-cursor loop is the only real merge and the merger tests migrate to fold-path tests — decide at draft, documenting which). **Determinism test (HARD gate):** feed the same record multiset across several sorted inputs in different per-input orderings and assert the merged output is byte-equal (the merge must be a pure function of the multiset + key order). **Duplicate-key test:** a key present in multiple inputs (and duplicated within one) yields every payload in a deterministic order. Consumes: Task 3.

### Task 5 [INDEPENDENT] — delete the seek/footer/inDegree/seekPrefix machinery (each with its sweep) {#task5}

The seek chain is **fully dead** (survey): `RunFileReader::seek` ← `inDegreeInGeneration` (its ONE caller) ← nothing (its doc comment claiming `previewDeletes` uses it is FALSE — `previewDeletes` uses `zeroInDegree` + a raw `kCondemned` scan; **fix that stale comment in passing**); `SourceEdgeKeyCodec::seekPrefix` dies with it. Per-deletion sweeps (each must print zero residual before deleting):

- **`inDegreeInGeneration`** (def `CasBlobInDegree.cpp:590`, decl `CasBlobInDegree.h:214`): `grep -rn 'inDegreeInGeneration' src/` → expect only the def/decl + comments in `CasGcShardPlan.h:79-81` (update those) + any unit test. Delete the function + its tests.
- **`SourceEdgeKeyCodec::seekPrefix`** (def `CasBlobInDegree.cpp:307`, decl `CasBlobInDegree.h:53`): `grep -rn 'seekPrefix' src/` → expect only `CasBlobInDegree.cpp:600` (inside the deleted `inDegreeInGeneration`) + its def/decl. Delete.
- **CARN block/footer machinery** (internal to the old `CasRunFile.cpp`): the footer index, per-block `CRC32C`, `kRunTargetBlockSize`/`kRunHardCapBlockSize`, `RunHeader.magic`/`block_size`, `kRunFormatVersion`/`kRunCodecNone`. `grep -rn 'kRunTargetBlockSize\|kRunHardCapBlockSize\|RunHeader\|kRunCodecNone' src/` → confirm all internal to the old file / migrated; delete the binary framing entirely (it is fully replaced by the NDJSON path Tasks 1-4). **Boundary:** do NOT delete the `ManifestEntries` embedded-run path in `CasManifestCodec.cpp` — that is phase 6; it may temporarily still use a private copy of the framing, or phase 6 converts it. Flag this coupling explicitly (phase 5 leaves `CasManifestCodec`'s embedded stream working; if the shared framing code is deleted, `CasManifestCodec` needs its own until phase 6 — decide at draft and document).

Consumes: Tasks 1-4.

### Task 6 [INDEPENDENT] — migrate the source-edge producers + read consumers + wire the seal-check {#task6}

Rewire every live source-edge run site to `CasRecordStreamFormat` + the Task-3 verify-before-use contract. Exact sites (survey):

- **Producer:** `CasBlobInDegree.cpp:364` (the new-generation source-edge run writer inside the seal path) → NDJSON writer; the checksum is computed over the produced bytes at `CasBlobInDegree.cpp:561` (keep the compute; it now checksums the NDJSON bytes).
- **Read consumers (each: open via `CasRecordStreamFormat`, stream records, then `verifyAgainst(seal RunRef.checksum)` BEFORE acting):**
  - fold two-cursor merge — `PriorEdgeCursor` at `CasBlobInDegree.cpp:140`, driven from `foldDeltasIntoGeneration` (`:330`). **This is the checksum's most important consumer — the fold decides deletions.**
  - `zeroInDegree` scan — `CasBlobInDegree.cpp:574`.
  - `previewDeletes` `kCondemned` scan — `CasGc.cpp:2281`.
  - `fsck` source-edge read — `CasFsck.cpp:456`.
- Each consumer gets a RED test proving the seal-checksum mismatch aborts THAT entry point before a delete/decision (per the lead's "RED test per consumer entry point").

Consumes: Tasks 1-5.

### Task 7 [PHASE-2-DEPENDENT] — remove the part-manifest-cleanup run + fold-seal field + key {#task7}

**Survey finding — the part-manifest-cleanup run has NO reader:** its deletes execute inline from the in-memory `folded.mf_cleanup` map (`CasGc.cpp:635-637`); the run object + its seal `RunRef`s are **record-only** (rendered by `CasInspect` + one gtest). So removing it breaks NO functional consumer — it is dead weight. Deletions, each with its sweep:

- **The part-manifest-cleanup run writer** — `CasGc.cpp:1610` (writer) + `:1616` (`partManifestCleanupKey`). Delete the write; confirm nothing reads the object (`grep -rn 'partManifestCleanupKey' src/` → expect only the builder `CasLayout.h:362` + the writer site).
- **`CasLayout::partManifestCleanupKey`** (`CasLayout.h:362`) — delete after the writer is gone.
- **The fold seal's `part_manifest_cleanup` field** — on the **integrated phase-2 tree this is `Core/Formats/CasFoldSealFormat`** (NOT `CasGenerationSeal`, which phase 2 deleted). Remove the field + its `btr`/`pmc`-style record emission (phase 2 emits it as a `pmc` record — remove that record kind from the fold-seal codec) + the `condemned_summary`-adjacent population. Sweep the readers/writers: on mainline these are `CasGc.cpp:1331` (populates `result.fold_seal.part_manifest_cleanup`), `CasInspect.cpp:360-380` (renders `part_manifest_cleanup`) — remove both; the phase-2 codec's `pmc` encode/decode arm is removed in the phase-2 `CasFoldSealFormat`. `grep -rn 'part_manifest_cleanup' src/` → zero after.
- Update the one `gtest` that asserts the cleanup run + the fold-seal `pmc` record (survey §8).

**Determinism note:** removing the `pmc` record from the fold seal changes its bytes — this is fine (a fresh format generation of the deterministic fold seal; no persisted data pre-release), but the fold-seal golden + determinism tests (phase-2's `CasFoldSeal.EncodingIsByteDeterministic` + `TextIsByteDeterministic`) must be updated in the same commit.

Consumes: integrated phase-2 `CasFoldSealFormat`, Task 6.

### Task 8 — golden text + full CAS slice + the soak-cost hook {#task8}

Golden NDJSON text file for `source_edge` (the §kind-fields shape, sort-order-pinned); the full `unit_tests_dbms --gtest_filter='Cas*'` green; and REGISTER the 2×-byte-cost soak measurement (exit criteria) — a hook that, at the phase-5 completion soak, logs the per-full-fold run-object byte totals for the before/after comparison. No fallback code is written (it triggers only on soak evidence).

## Phase-5 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_run` is sorted NDJSON (header + records + `{"n"}` trailer); `CARN` block/footer/CRC/seek gone; `CasRunFile` → `Formats/CasRecordStreamFormat`.
- **Determinism proven:** same sorted record set → byte-equal run (writer test + merger test); adoption via `putDeterministicArtifact` still works (a GC resume/failover e2e passes).
- **Seal-checksum proven:** whole-file CityHash128 verified before use; a flipped byte → `CORRUPTED_DATA` before any deletion decision (unit + a GC-fold-path test); trailer `n` catches line truncation.
- `RunFileReader::seek`, `inDegreeInGeneration`, `SourceEdgeKeyCodec::seekPrefix`, the block/footer machinery, the part-manifest-cleanup run, the fold-seal `part_manifest_cleanup` field, and `partManifestCleanupKey` are DELETED — each with a consumer-sweep showing zero residual references.
- **2×-byte-cost measurement (named soak step):** at the phase-5 completion soak, capture the run-object byte totals (blob-target + source-edge runs per full fold) and compare against the pre-cutover baseline; record the ratio. If the soak shows the ≈2× cost is a real bottleneck, the **localized fallback is to re-binarize `cas_run` ALONE** (one family, one `kind`-versioned binary arm behind the same typed-open API — no other object affected), per the spec §record-streams. Do NOT pre-build the fallback; it triggers only on soak evidence.
- Phase 6 (part manifest) can then delete `RunKind::ManifestEntries` + the embedded-stream path (its own JIT plan).

## Phases: this is JIT — draft gate {#draft-gate}

Per the lead: PLAN ONLY. The **code draft** is NOT written until phase-2 is integrated + soaked + stateless-gated on mainline, so Tasks 1-6 (independent) draft against the integrated phase-2 wire vocabulary and Task 7 (phase-2-dependent) edits the integrated `CasFoldSealFormat` directly — never a patch stack. The lead clears the draft explicitly.

---
description: M5 Step 4 — versioned, little-endian, exactly-parsed on-disk formats for the content-addressed manifest, ref payload, ref sidecar, and _pool_meta (B19, B28).
sidebar_label: 'CAS MergeTree M5.4 plan'
sidebar_position: 7
slug: /superpowers/plans/cas-mergetree-m5s4
title: 'Content-Addressed MergeTree — M5 Step 4 Plan (versioned LE formats, B19/B28)'
doc_type: 'guide'
---

# CAS MergeTree — M5 Step 4: versioned little-endian formats (B19, B28) {#m5s4}

> **For agentic workers:** REQUIRED SUB-SKILL: `superpowers:subagent-driven-development`. Steps use `- [ ]`.

**Goal:** make every persisted content-addressed format **explicitly little-endian, versioned, and exactly parsed**, with a reader that **fails closed on an unknown version**. "POCs become production data" — these objects are content-addressed (hashed), so a host-endian or fuzzy format is both a cross-arch determinism bug and a forward-compat trap. Today: `PartManifest` (de)serialization uses host-endian `memcpy` of integers (B19); the ref payload is parsed as "first hex run" (B28); `RefSidecar`/`_pool_meta` got ad-hoc magics in Steps 2–3. This step standardizes all four.

**Architecture:** one small shared codec helper (LE fixed-width + varint + length-prefixed strings, built on `IO/WriteHelpers.h`/`ReadHelpers.h` `writeBinaryLittleEndian`/varint) used by all four formats. Each format = `MAGIC(4) + version(1) + body`, where the reader rejects an unknown magic or a version it does not understand (`CORRUPTED_DATA`/`NOT_IMPLEMENTED`). The **ref payload** becomes a real struct (version + `part_id` + reserved room for B1's `ReplicatedMergeTreePartHeader` and the per-ref mutable fields), parsed by ONE function `partIdFromRefPayload` shared by the read path AND the GC (closes B28 — they can no longer diverge).

**Tech stack:** `IO/WriteBufferFromString`+`writeBinaryLittleEndian`/`writeVarUInt`/`writeStringBinary` and the `Read*` equivalents; the consolidated `ContentAddressed/` units (`PartManifest`, `PoolPaths`, `ContentAddressedTransaction`, `ContentAddressedMetadataStorage`, `Identifiers.h`).

## Build & test {#build}
`cmake --build build --target clickhouse unit_tests_dbms`; `--gtest_filter='ContentAddressed*'` (47) + `'*PlainRewritable*:*DiskObjectStorage*'` (66). Stateless via the `clickhouse-praktika-tests` skill. Allman; `DB::Exception`; no `<...>` in `///`.

## File structure {#files}
- New `Codec.h` (or fold into `PartManifest.h`) — LE/varint/length-prefixed read+write helpers + a `FormatHeader{magic, version}` read/write with fail-closed checks.
- `PartManifest.cpp` — re-implement serialize/deserialize on the codec (LE, varint, version byte); keep the logical content identical (so existing `part_id`s computed over `(name, checksum)` are unaffected — `part_id` is over checksums, not over the manifest bytes, so this does NOT change `part_id`; confirm).
- `PoolPaths`/ref payload — a versioned ref struct + the single `partIdFromRefPayload`; remove the "first hex run" parse; the GC and read path call the same function.
- `RefSidecar` + `_pool_meta` — move their ad-hoc magics onto the shared header/codec (versioned, LE).

## Tasks {#tasks}

### Task 1 — the shared codec + format header (TDD)
- [ ] Test: LE round-trip of u32/u64/varint/string across values incl. high bytes; `FormatHeader` rejects wrong magic and an unknown (future) version with a clear error; bytes are byte-identical regardless of host endianness (assert exact bytes for a known value).
- [ ] Implement `Codec`/`FormatHeader`.
- [ ] Commit `CAS M5.4: little-endian codec + versioned format header`.

### Task 2 — PartManifest on the codec (TDD; determinism)
- [ ] Test: a fixed manifest serializes to a FIXED, pinned byte string (golden), round-trips, rejects bad magic/version/truncation/overlong-length; **`part_id` for a fixed file set is unchanged** by this re-serialization (golden `part_id`).
- [ ] Re-implement `PartManifest::serialize/deserialize` on the codec; keep `MAGIC`+version.
- [ ] Commit `CAS M5.4: little-endian versioned PartManifest format + golden test`.

### Task 3 — ref payload struct + single parser (B28) (TDD)
- [ ] Test: the ref payload is written as `MAGIC+version+part_id(+reserved)`; `partIdFromRefPayload` reads it exactly; an unknown version fails closed; **the read path (`readRefPartId`) and the GC (`listLivePartIds`) call the SAME parser** (assert via a shared function / a test that feeds a versioned payload to both). Old "first hex run" behavior is gone.
- [ ] Implement: `ContentAddressedTransaction::commit` writes the versioned ref payload; `partIdFromRefPayload` is the one parser; wire both call sites.
- [ ] Commit `CAS M5.4: versioned ref payload + single shared parser (B28)`.

### Task 4 — RefSidecar + _pool_meta onto the shared header
- [ ] Re-base `RefSidecar` and `_pool_meta` (de)serialization on the shared codec/header (LE, versioned, fail-closed). Keep their existing semantics + tests green; add unknown-version-rejection tests.
- [ ] Commit `CAS M5.4: RefSidecar + _pool_meta on the shared versioned codec`.

## Verify — HARD GATE
- Build clean; `--gtest_filter='ContentAddressed*'` (≥47 + golden/version tests) + regression 66 green.
- Stateless `04278 04279 04280 04281 04282` still `[ OK ]`/`Failed: 0` (the new format is written + read by the same build, so round-trips; `part_id` unchanged means existing pools/tests are unaffected within a run).
- A cross-arch note: since CI also runs arm, the LE format means a manifest written on one arch is readable on the other; the golden byte tests pin it.

## Self-review {#self-review}
- **Coverage:** B19 (LE manifest), B28 (single exact ref parser) — both closed; sidecar/`_pool_meta` unified.
- **No behavior change:** `part_id` is computed over `(name, checksum)`, NOT over manifest bytes, so re-serialization does not change identities or dedup; the golden `part_id` test pins it.
- **Forward-compat:** every format now version-gated + fail-closed, so later additive fields (B1 header in the ref, B5 projections in the manifest) bump the version without breaking readers.

## Deferrals likely to surface {#deferrals}
- If `part_id` ever needs to be over canonical manifest bytes (it isn't today), revisit. Note in backlog.
- Compression of large manifests (B10 one-GET) is out of scope.

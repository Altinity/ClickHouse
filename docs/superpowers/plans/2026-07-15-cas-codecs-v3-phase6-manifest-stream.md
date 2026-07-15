---
description: 'Implementation plan for CAS codecs v3 phase 6: converting the part manifest (cas_part_manifest) from the hybrid binary form (CAPT header + an embedded CARN ManifestEntries run) to the v3 PayloadHybrid text shape (JSON descriptor + sorted entry record lines + a head -v-banner raw payload zone for inline entry bytes), migrating the manifest key from .proto to the Always/.zst suffix, and DELETING Core/CasRunFile.{h,cpp} + gtest_cas_run_file.cpp entirely — phase 6 is the last user of the CARN run codec.'
sidebar_label: 'CAS codecs v3 phase 6 plan'
sidebar_position: 66
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase6-manifest-stream
title: 'CAS Codecs V3 — Phase 6: Part Manifest (PayloadHybrid) Text Cutover + CasRunFile Deletion'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 6: Part Manifest Text Cutover Implementation Plan {#cas-codecs-v3-phase6}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (verified against HEAD `13823e3154b`, 2026-07-15):** phases 2, 3, 5, and 7 are landed. The `Core/Formats/` foundation is in place (`CasTextFormat`, `CasFormat` with `FormatId::PartManifest = 12` + its `TRAITS` row `{cas_part_manifest, PayloadHybrid, Tolerant, Always, object_cap 256 MiB, line_cap 64 KiB}`, `CasWireVocab` with `writeBlobRefFields`/`blobHashAlgoFromWord`, `CasBlobEnvelopeFormat` from phase 7). The manifest is STILL the legacy binary `CasManifestCodec` (a CAPT header + an embedded CARN `RunKind::ManifestEntries` run); `Core/Formats/CasPartManifestFormat.*` does not exist yet. Phase 5 pruned `CasRunFile` to its last kind (`ManifestEntries`) and its last user (`CasManifestCodec`); phase 6 removes that user and DELETES `CasRunFile` entirely (the boundary the phase-5 FLAG-1 established, recorded in the corrected phase-2 DAG at `6c227fafa92`).

**Goal:** convert `cas_part_manifest` to the v3 **PayloadHybrid** text shape and delete the CARN run codec. Concretely:

- **`Core/CasManifestCodec.{h,cpp}` → `Core/Formats/CasPartManifestFormat.{h,cpp}`** — the descriptor (`ManifestRef ref`, `root_namespace_id`, `payload_digest`) becomes a header line + a meta line; the entries become sorted JSON record lines (one per `ManifestEntry`, `Blob` or `Inline`) + a `{"n":count}` trailer; the inline entry BYTES move into a **`head -v`-banner raw payload zone** after the trailer (JSON strings cannot hold arbitrary binary). The embedded CARN stream + `RunKind::ManifestEntries` + `key_schema` are gone.
- **`CasLayout::manifestKey` / `manifestOrdinalFileName` / `parseManifestKey`** migrate from the hardcoded `.proto` suffix to the `Always` `.zst` `storedSuffix` — and the object is ACTUALLY compressed now (today it is an uncompressed `.proto` body despite the `Always` `TRAITS` row: the reconcile is phase-6 work).
- **`Core/CasRunFile.{h,cpp}` and `src/Disks/tests/gtest_cas_run_file.cpp` are DELETED** along with the stale `#include`s of `CasRunFile.h` (`CasFsck.cpp`, `CasBlobInDegree.h` comment, `gtest_cas_blob_indegree.cpp`).

The `encodePartManifest`/`decodePartManifest` and the helper signatures (`computePayloadDigest`, `refMatchesBody`, `manifestNamespaceMatches`, `findEntry`, `entryRange`) are preserved verbatim so all call sites compile at the type level; the persist sites gain a `sealObject`/`openObject` wrap (the Always/`.zst` policy, exactly as `cas_gc_outcomes`/refsnaplog).

Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` §migration-order step 6, §corrected-object-inventory (the `cas_part_manifest` row: "Payload hybrid (JSON descriptor + banner payload zone)"), §container-design (raw payload zone, "no smuggling" verified padding zones); reference: `docs/superpowers/cas/codecs_proposal_v3.md`.

**Tech Stack:** C++ (`dbms`), phase-1 `CasTextFormat`, phase-2 `CasWireVocab`, `CasManifestId`/`CasBlobRef`, `ReadHelpers`/`WriteHelpers`, gtest (`unit_tests_dbms`).

## Cruxes the lead asked (answered from spec + code, with FLAGs) {#cruxes}

**CRUX A — embedded stream: stays line-structured INSIDE the manifest body, becomes NO separate object.** The spec (§migration step 6) folds the entries into the manifest body as sorted JSON record lines and "delete[s] the embedded stream path and `RunKind::ManifestEntries`". So `cas_part_manifest` stays ONE object; the CARN sub-stream is replaced by inline NDJSON entry lines + a trailer, all within the single manifest body. No separate object, no `CasRecordStreamFormat` reuse (that codec is the standalone source-edge `cas_run`, a different object with a `b/s/m` record).

**CRUX A2 — the "banner payload zone" (FLAG A, the defining complexity).** The spec calls `cas_part_manifest` `PayloadHybrid` = "JSON descriptor + banner payload zone", but the survey found the current binary manifest has NO banner/payload zone — inline entry bytes (`ManifestEntry.inline_bytes`, `EntryPlacement::Inline`) live inside each entry's binary payload. In text, arbitrary inline bytes CANNOT be a JSON string value (JSON strings are UTF-8; raw binary isn't, and NUL/quotes bloat under escaping). Resolution (the PayloadHybrid shape): the body is `header line + meta line + entry record lines + {"n"} trailer + a RAW PAYLOAD ZONE`. A `Blob` entry record carries `ha`/`h`/`sz` (via `writeBlobRefFields` + blob_size); an `Inline` entry record carries `il` (the inline byte length) and NO bytes. The inline BYTES go into the payload zone after the trailer, one segment per inline entry in path order, each as a **`head -v`-style banner line** (`==> <path> il=<n> <==\n`, deterministic, human-legible, `less`/`head -v`-friendly) + exactly `n` raw bytes + `\n`. The reader, knowing each `il` from the records, reads banner→exact-n-bytes→`\n` per inline entry and reconstructs `inline_bytes`; "no smuggling" = after the last segment the object is at EOF (zero unaccounted bytes). The whole object is `Always`/`.zst`, so the payload zone is inside the zstd frame; the object is read WHOLE (openObject), so the zone is materialized (bounded by `object_cap = 256 MiB` + `kMaxManifestEntries`/`kMaxManifestEncodedBytes`). **FLAG A:** phase 7's `CasBlobEnvelopeFormat` is a SINGLE fixed-offset payload (256-byte header + one raw body) — it provides NO reusable multi-segment banner primitive, so the manifest payload zone is manifest-specific (built on `CasTextFormat` line/read primitives). **FLAG A2 (production frequency):** confirm at draft whether `Inline` entries still occur in production (the all-tree-part-files work made per-part files ordinary tree/`Blob` entries) — the payload zone is still REQUIRED for correctness + test coverage regardless, but if inline is vestigial it is rarely exercised, which raises the bar on the inline round-trip / no-smuggling tests.

**CRUX B — streaming: NONE; read WHOLE.** Every production manifest read is `backend.get(key)` then `decodePartManifest(bytes)` (`CasStore::readManifestShared` `CasStore.cpp:1053/1059`, GC fold `CasGc.cpp:691/696`, fsck `CasFsck.cpp:302`, exchange receiver, inspect). The part-folder cache (`CachedPartFolderAccess`) caches the DECODED `PartFolderView` (a fully in-memory ascending `entries` vector); `findEntry`/`entryRange` are in-memory binary searches — NO consumer point-seeks into the on-disk entry stream. So entries stay a line-structured stream read whole; there is no random-access / `getStream` / `seek` requirement (confirming the phase-5 FLAG-1 note at HEAD). This is WHY `CasRunFile`'s footer index / block machinery has nothing to preserve.

**CRUX C — framing to replace + determinism (FLAG C).** The CARN framing being deleted: the 13-byte `RunHeader` (magic `CARN`), per-block `CRC32C`, the footer index (`kRunTargetBlockSize`, `loadFooter`, `installBlockFrame`), the 3-request streaming open. The text form replaces per-block `CRC32C` with NOTHING at the codec level — integrity is delegated (spec §integrity): the `Always` **zstd frame** (checksum on) for the whole object, `payload_digest` for the payload zone, the `{"n"}` trailer for entry-line truncation, and `fsck`. **Determinism:** the manifest does NOT go through `putDeterministicArtifact` and is NEVER byte-compare-trusted (survey item 5 + the hash-equality doctrine): it is keyed by IDENTITY (`writer_epoch`, `build_sequence`, `manifest_ordinal`), persisted via `putIfAbsentControlled` (a *conflict* check — a divergent object at the key is a `ManifestId` collision → `CORRUPTED_DATA`, not a byte-adoption), and the exchange receiver re-decodes, re-stages a FRESH local `ManifestId`, and re-proves every blob by re-hashing (bytes never adopted). So the text codec is `Tolerant`, NOT `Strict`, NOT `PinnedRaw`. It stays deterministic BY CONSTRUCTION (entries sorted by canonical path, `payload_digest` a pure function of contents) so `gtest_cas_manifest_codec.cpp::ByteDeterminism` keeps passing — but that is a nicety, not an adoption gate (and zstd would defeat a byte-equality gate anyway).

**CRUX D — reuse vs manifest-specific (FLAG D).** Reuse: `CasTextFormat` (header/meta/record lines, `readLine`, `JsonObjectReader`, trailer), `CasWireVocab::writeBlobRefFields`/`blobHashAlgoFromWord` for `Blob` entries. Do NOT reuse `CasRecordStreamFormat` (source-edge-specific standalone-object codec) NOR the blob-envelope fixed-offset header (single payload). The entry record shape + the multi-segment banner payload zone + `payload_digest` are manifest-specific. `ManifestRef` field rendering: phase 3 put `writeManifestRefFields` in `Core/Formats/CasRefWireVocab.h` and flagged it for promotion to `CasWireVocab` when phase 6 shares it — **phase 6 IS that consumer.** Resolution: if `writeManifestRefFields`/`manifestRefFromFields` in `CasRefWireVocab` fit the manifest `ref` rendering exactly, PROMOTE them to `CasWireVocab` (Task 1) and have both the ref codecs and the manifest codec use them; otherwise the manifest renders `ref` locally (three flat keys `we`/`bs`/`mo`). Decide at draft.

**CRUX E — key `.proto`→`.zst` + actual compression (FLAG E).** `manifestKey` (`CasLayout.h:251`) builds `…/cas/manifests/<ns>/<epoch-hex>-<buildseq-hex>/<NNNNNN>.proto` via `manifestOrdinalFileName` (`CasManifestId.h:80`, hardcoded `.proto`), and `parseManifestKey` requires `.proto`. But the `TRAITS` row is `Always` and `storedSuffix(FormatId::PartManifest)` returns `.zst` (asserted by `gtest_cas_text_format.cpp`). Today the object is an uncompressed `.proto` binary — the `Always` row is aspirational. Phase 6 reconciles: `manifestOrdinalFileName` (or `manifestKey`) uses `storedSuffix(FormatId::PartManifest)` (`.zst`), `parseManifestKey` strips `.zst`, and the persist path actually seals (compresses). Sweep every `manifestOrdinalFileName` / `parseManifestKey` caller. **FLAG E:** `manifestOrdinalFileName` is a shared helper (check its callers — if only `manifestKey` uses it, change the suffix there; if others render the ordinal for a non-key purpose, parameterize the suffix).

**CRUX F — the encode/decode contract (return TEXT; persist wraps), settled by an external budget caller.** Like phase 3, `CasBuild.cpp:757` measures `encodePartManifest(body).size() > kMaxManifestEncodedBytes` (256 MiB) — a TEXT-size budget. So `encodePartManifest` MUST return canonical TEXT (uncompressed), and the PERSIST sites wrap: PUT `sealObject(FormatId::PartManifest, encodePartManifest(body))` (`CasBuild::stageManifest` `:781`), GET `decodePartManifest(openObject(FormatId::PartManifest, got->bytes))` (`CasStore.cpp:1059`, `CasGc.cpp:696`, `CasFsck.cpp:302`, `CasInspect.cpp:452`). The EXCHANGE path (`ContentAddressedMetadataStorage.cpp` sender `:1238` `encodePartManifest` → receiver `:1267` `decodePartManifest`) is text-to-text (the receiver re-stages fresh, which seals via `CasBuild`) — leave it unwrapped (raw text over the wire). `kMaxManifestEncodedBytes` is now interpreted over the TEXT (re-derive as a comment; JSON inflates the binary — confirm 256 MiB still comfortably bounds the largest real manifest, or the object_cap/line_cap trait needs a look).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere.
- **Layering (physical):** `Core/Formats/CasPartManifestFormat.h` includes only other `Formats/` headers (`CasFormat.h`, `CasTextFormat.h`, `CasWireVocab.h`), the identifier vocabulary (`CasManifestId.h`, `CasBlobRef.h`, `CasIds.h`), `CasCodecUtil.h` for `checkCanonicalRefName`/`checkManifestRef` (dependency-clean identifier layer, as the ref codecs use it), `base/`, `src/IO/`, `src/Common/`. NEVER `CasBackend.h`/`CasStore.h`/`CasLayout.h`/`CasBuild.h`/`CasRunFile.h`. The codec is pure mapping + invariants + the payload zone.
- **All legacy invariants carry over unchanged, at BOTH encode and decode:** entries strictly ascending by canonical `ref_name`/path with duplicate rejection; every path a canonical clean relative path (`checkCanonicalRefName`); `Blob` entries a valid `BlobRef` (`checkManifestRef` for the manifest `ref`; the entry `BlobRef` validated by algo width); the `EntryPlacement` shape (Blob ⇒ `ref`+`blob_size`, no inline; Inline ⇒ `inline_bytes`, no ref); `refMatchesBody`/`manifestNamespaceMatches` unchanged; the entry count / encoded-byte caps.
- **`payload_digest` (FLAG G — define coverage):** today `computePayloadDigest` is CityHash128 over the canonical body with `payload_digest` zeroed (integrity/debug only, never a key/dedup). In the text form it must remain a pure function of contents and stay deterministic; define it as CityHash128 over the canonical TEXT body (descriptor + entry lines + trailer + payload zone) computed with the `pd` meta value rendered as 32 zero-hex during the hash (matching the current "zeroed" scheme), then written into the meta line. Decode recomputes-and-compares (or carries it), preserving `PayloadDigestStableAndContentSensitive`.
- **`Tolerant` keys, `Always`/`.zst`:** `JsonObjectReader` runs `KeyStrictness::Tolerant`; the caller seals/opens; the zstd declared size is checked against `object_cap` before allocation.
- **`v` = `G_BUILD = 3`**, no breaking generation, no `changePoints` append; drop the binary `writer_version` field (redundant with the header `v`, as the phase-7 envelope dropped it). The header gate is `expectHeaderLine`.
- **Pinned JSON write settings** inherited from phase 1 (`escape_forward_slashes = false`) — paths are `/`-dense.
- **Pre-release, hard cutover, no dual-read.**
- **Build/commit discipline** (as prior phases): substitute the real build dir, foreground, no `-j`/`nproc`, per-task log + `NINJA_EXIT=`, subagent-analyze. Commit per task, explicit-path `git add`, branch `cas-gc-rebuild`, trailer:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## Interfaces consumed {#consumed-interfaces}

- `CasTextFormat.h`: `writeKey`, `writeStringValue`, `writeU64StringValue`, `writeHex128Value`, `closeObject`, `writeHeaderLine`, `writeTrailerLine`, `readLine`, `expectHeaderLine`, `JsonObjectReader`, `sealObject`/`openObject`. (The payload zone uses raw `WriteBuffer::write` + `ReadBuffer` exact reads, not `readLine`, because inline bytes contain arbitrary bytes incl. `\n`.)
- `CasFormat.h`: `FormatId::PartManifest`, `traitsFor`, `storedSuffix`, `checkCompatibility`, `G_BUILD`.
- `CasWireVocab.h`: `writeBlobRefFields`, `blobHashAlgoFromWord`; and (Task 1 decision) possibly the promoted `writeManifestRefFields`/`manifestRefFromFields`.
- `CasCodecUtil.h`: `checkCanonicalRefName`, `checkManifestRef`.
- `CasManifestId.h`: `ManifestRef`, `kMaxManifestOrdinal`, `manifestOrdinalFileName`. `CasBlobRef.h`: `BlobRef`, `codecFor`, `blobHashLenFor`, `blobHashAlgoName`.

## Per-object text shape {#text-shape}

```text
{"type":"cas_part_manifest","v":3}
{"we":"5","bs":"15","mo":1,"ns":"00/aa@cas@","pd":"<32hex>"}                 descriptor: ManifestRef + root_ns + payload_digest
{"p":"a/b.bin","pm":"blob","ha":"ch128","h":"<digest hex>","sz":4096}         a Blob entry
{"p":"c/small.txt","pm":"inline","il":12}                                     an Inline entry (bytes in the zone)
{"n":2}                                                                       trailer: entry count
==> c/small.txt il=12 <==                                                     payload zone: one banner+bytes+'\n' per inline entry, path order
hello world!
```

- **descriptor meta line:** `we`/`bs` (ManifestRef writer_epoch/build_sequence, unbounded → decimal strings), `mo` (manifest_ordinal, bounded → number), `ns` (root_namespace_id string), `pd` (payload_digest, 32-hex).
- **entry record:** `p` (path), `pm` (placement word `blob`/`inline`). `Blob` ⇒ `ha`/`h` (via `writeBlobRefFields`) + `sz` (blob_size, unbounded → decimal string). `Inline` ⇒ `il` (inline length, number). Records ascending by `p`, no duplicates.
- **payload zone** (after the trailer): for each Inline entry in path order — a banner line `==> <path> il=<n> <==\n`, then exactly `n` raw bytes, then `\n`. A manifest with no inline entries has an EMPTY payload zone (object ends at the trailer). No-smuggling: EOF exactly after the last segment's `\n`.

## Tasks {#tasks}

Hard cutover, phase-2/3/5 rigor. Task 1 = the codec skeleton + shape decisions; Task 2 = encode/decode + payload zone + invariants; Task 3 = key migration + persist wraps + delete the binary codec + rewire; Task 4 = DELETE `CasRunFile` + tests + docs.

### Task 1 — `Formats/CasPartManifestFormat` skeleton + shape + vocab decision {#task1}

**Files:** Create `Core/Formats/CasPartManifestFormat.{h,cpp}` (move `EntryPlacement`, `ManifestEntry`, `PartManifest` + the helper decls); Test `src/Disks/tests/gtest_cas_part_manifest_format.cpp` (new). Possibly modify `Core/Formats/CasWireVocab.{h,cpp}` + `Core/Formats/CasRefWireVocab.{h,cpp}` if `writeManifestRefFields` is promoted (CRUX D).

**Steps:** (1) RED test — a header + descriptor round-trip (a `PartManifest` with one Blob + one Inline entry), asserting the §text-shape; wrong-type / `v+1` header gates; the placement word maps. (2) Compile-fail. (3) Implement: the structs move verbatim; drop `writer_version` from the wire; the header/meta writers; the entry-record writer skeleton (Blob via `writeBlobRefFields`, Inline via `il`); `placementToWord`/`fromWord`; resolve CRUX D (promote `writeManifestRefFields` to `CasWireVocab` and re-point `CasRefWireVocab`, OR render `we`/`bs`/`mo` locally — document the choice). (4) Green. (5) Commit `cas: formats v3 phase 6 — CasPartManifestFormat skeleton (descriptor + entry record shape)` + trailer.

### Task 2 — encode/decode entries + the banner payload zone + payload_digest + determinism {#task2}

**Files:** Modify `Core/Formats/CasPartManifestFormat.{h,cpp}`; extend the test.

**Steps:**
- [ ] **RED tests:** full round-trip incl. inline bytes with embedded `\n`/NUL/quotes (proving the payload zone, not JSON escaping); `ByteDeterminism` re-pointed; `MixedAlgoEntriesRoundTrip` (16- and 32-byte digests in one manifest); `DuplicatePathRejectedOnEncode` + `DecodeRejectsOutOfOrderEntries` + `DecodeRejectsNonAdjacentDuplicatePath` (re-expressed over the text records — NOT the CARN forge helpers); `UnknownEntryAlgoFailsClosed`; `EmptyEntriesRoundTrips`; `PayloadDigestStableAndContentSensitive`; a no-smuggling test (extra trailing byte after the last payload segment → `CORRUPTED_DATA`); an `il`-mismatch test (a payload segment banner `il` disagreeing with the record → `CORRUPTED_DATA`); truncation-at-line-boundary sweep → `CORRUPTED_DATA`.
- [ ] **Implement `encodePartManifest`** (returns TEXT): header line; descriptor meta (`we`/`bs`/`mo`/`ns`/`pd`, `pd` computed by `computePayloadDigest` with the field zeroed); entries sorted by canonical path (sort inside the encoder — never trust caller order), one record line each; `{"n"}` trailer; then the payload zone (banner+bytes+`\n` per Inline entry, path order). Self-check the entry count / encoded-text byte budget. `computePayloadDigest` re-defined over the canonical text (FLAG G).
- [ ] **Implement `decodePartManifest`** (takes TEXT): `expectHeaderLine`; meta line → descriptor; `readLine` loop of entry records until `{"n"}` (validating ascending path + no dup + placement shape + algo width via `blobHashAlgoFromWord`/`codecFor`); then read the payload zone by exact length per Inline entry (banner line, cross-check path+`il`, read `il` bytes, read `\n`); no-smuggling EOF check; recompute + verify `payload_digest`; re-apply every invariant. `Tolerant`.
- [ ] **Verify** `unit_tests_dbms --gtest_filter='CasPartManifest*'` green.
- [ ] **Commit** `cas: formats v3 phase 6 — part manifest encode/decode + banner payload zone + payload_digest` + trailer.

### Task 3 — key .proto→.zst, persist wraps, delete the binary codec, rewire {#task3}

**Files:** `Core/CasLayout.h` (`manifestKey`/`parseManifestKey`), `Core/CasManifestId.h` (`manifestOrdinalFileName`), `Core/CasBuild.cpp` (PUT wrap + budget comment), `Core/CasStore.cpp`/`CasGc.cpp`/`CasFsck.cpp`/`CasInspect.cpp` (GET wraps), `ContentAddressedMetadataStorage.cpp` (exchange — leave text-to-text, verify no wrap needed), delete `Core/CasManifestCodec.{h,cpp}`, include-rewrite all includers → `Formats/CasPartManifestFormat.h`.

**Steps:**
- [ ] **Key migration (CRUX E):** `manifestOrdinalFileName` (or `manifestKey`) emits `storedSuffix(FormatId::PartManifest)` (`.zst`) instead of `.proto`; `parseManifestKey` strips `.zst`. Sweep every `manifestOrdinalFileName`/`parseManifestKey` caller (grep) — fix each. Grep `'\.proto'` under `ContentAddressed/` → zero in the manifest key path after (comments aside).
- [ ] **Persist wraps (CRUX F):** PUT `sealObject(FormatId::PartManifest, encodePartManifest(body))` at `CasBuild::stageManifest`; GET `decodePartManifest(openObject(FormatId::PartManifest, got->bytes))` at every backend read (`CasStore.cpp:1059`, `CasGc.cpp:696`, `CasFsck.cpp:302`, `CasInspect.cpp:452`). Leave the exchange sender/receiver text-to-text (unwrapped). Add the `Formats/CasTextFormat.h` include where a wrap is added. Re-derive `kMaxManifestEncodedBytes` as a text-size budget (comment).
- [ ] **Delete `Core/CasManifestCodec.{h,cpp}`**, include-rewrite (`grep -rl 'ContentAddressed/Core/CasManifestCodec\.h' src/ | xargs sed -i 's|…/CasManifestCodec.h|…/Formats/CasPartManifestFormat.h|g'`).
- [ ] **Verify** `unit_tests_dbms --gtest_filter='Cas*'` green; the build/read (`gtest_cas_build`, `gtest_cas_store`), GC-fold (`gtest_cas_gc_fold`), fsck, part-folder-cache, and exchange behavioral suites green (they drive the codec through the persist wraps).
- [ ] **Commit** `cas: formats v3 phase 6 — part manifest .proto→.zst key + persist wraps; delete binary CasManifestCodec` + trailer.

### Task 4 — DELETE CasRunFile entirely + tests + docs {#task4}

**Files:** Delete `Core/CasRunFile.{h,cpp}` + `src/Disks/tests/gtest_cas_run_file.cpp`; remove the stale `#include <…/CasRunFile.h>` from `CasFsck.cpp` and `gtest_cas_blob_indegree.cpp` + the `CasBlobInDegree.h` comment reference; migrate `gtest_cas_manifest_codec.cpp`; `Core/Formats/README.md`; `Core/Formats/CasFormat.cpp` (dead magic).

**Steps:**
- [ ] **Sweep-before-delete:** `grep -rn 'CasRunFile\|RunFileReader\|RunFileWriter\|RunHeader\|RunKind\|kRunTargetBlockSize\|CARN' src/` → after the manifest cutover, the ONLY hits must be `CasRunFile.{h,cpp}` itself + `gtest_cas_run_file.cpp` + the stale includes/comments. Confirm zero live users, then delete `CasRunFile.{h,cpp}` + `gtest_cas_run_file.cpp` + the stale includes.
- [ ] **Migrate `gtest_cas_manifest_codec.cpp`:** DELETE the CARN-forge helpers (`singleBlockPayloadRange`/`patchBlockCrc`/`forgeSwappedRecordOrder`, `find("CARN")`, `kRunHeaderLen`) and re-express `DecodeRejectsOutOfOrderEntries`/`DecodeRejectsNonAdjacentDuplicatePath` as text-record corruptions (hand-write an out-of-order / duplicate-path entry-line text and assert `CORRUPTED_DATA`); move the surviving tests into `gtest_cas_part_manifest_format.cpp` (Tasks 1–2) or keep the behavioral ones here re-pointed at the text codec. Add a `FormatId::PartManifest` battery row.
- [ ] **Docs:** `README.md` — flip the manifest bucket-map row to `CasPartManifestFormat`, drop its `*`, mark phase 6 DONE, and note `CasRunFile`/CARN is gone (the migration is complete for the run family). `CasFormat.cpp` — the CARN literal lived in `CasRunFile.h` (now deleted); check `magicFor(FormatId::PartManifest)` (`CAPT`) and `magicFor(FormatId::RunFile)` (`CARN`) for dead references now that neither object is binary, and remove the dead `magicFor` arms / update the comment (FLAG: confirm nothing still calls `magicFor` for these two ids).
- [ ] **Verify** full `unit_tests_dbms --gtest_filter='Cas*'` green.
- [ ] **Commit** `cas: formats v3 phase 6 — delete CasRunFile + gtest_cas_run_file; migrate manifest tests; docs` + trailer.

## Phase-6 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_part_manifest` is the PayloadHybrid text shape (header + descriptor meta + sorted entry records + `{"n"}` trailer + banner payload zone), read WHOLE; the CAPT header + embedded CARN stream + `RunKind::ManifestEntries` are gone.
- **`Core/CasRunFile.{h,cpp}` and `gtest_cas_run_file.cpp` are DELETED**, with a sweep showing zero residual `CasRunFile`/`RunKind`/`CARN` references — the codecs-v3 run family migration is complete.
- Key migrated: `manifestKey` carries `.zst` (not `.proto`); `parseManifestKey` strips `.zst`; the object is actually zstd-compressed; grep shows no `.proto` in the manifest key path.
- **Inline payload zone proven:** an inline entry with embedded `\n`/NUL/quote bytes round-trips byte-faithfully through the banner zone; a trailing unaccounted byte or an `il` mismatch → `CORRUPTED_DATA` (no smuggling).
- **Determinism-by-construction preserved** (`ByteDeterminism`, `PayloadDigestStableAndContentSensitive`); documented as NOT a `putDeterministicArtifact` gate (manifest keyed by identity, never byte-adopted; exchange re-hashes).
- Persist sites wrap `sealObject`/`openObject`; the text-size budget (`kMaxManifestEncodedBytes`) is re-derived; signatures verbatim (call sites compile at the type level).

## Phases: this is JIT — draft gate {#draft-gate}

PLAN ONLY; the lead clears the code draft. Phase 6's predecessors (2, 3, 5, 7) are landed, so the draft is written against post-integration HEAD. Sequencing note: phase 6 shares files with landed phases — `CasLayout.h` (phase 3 touched ref keys; phase 6 touches `manifestKey`/`parseManifestKey` — disjoint regions), `CasGc.cpp`/`CasFsck.cpp`/`CasInspect.cpp` (phases 3/5 touched other regions; phase 6 touches the manifest read/render sites). Draft against HEAD as-is.

## Deferred / open {#deferred}

- **FLAG A2:** confirm inline-entry production frequency at draft (the payload zone is required regardless).
- **FLAG D:** promote `writeManifestRefFields` to `CasWireVocab` iff it fits the manifest `ref` rendering exactly.
- **FLAG G:** the exact `payload_digest` coverage over the text body (recommend: full canonical body with `pd` zeroed during the hash).
- The JSON-inflated `object_cap`/`kMaxManifestEncodedBytes` are re-derived at draft against the largest realistic manifest.

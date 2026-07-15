---
description: 'Implementation plan for CAS codecs v3 phase 3: converting the two refsnaplog binary formats — the ref transaction log (cas_ref_log) and the per-namespace ref table snapshot (cas_ref_snap, which carries the rev.6 recovery seal) — to the phase-1 text file shape (header + line-structured JSON records + trailer, Always/.zst), preserving the key/body binding check, the rev.6 sealed_from semantics, and deterministic-by-construction re-encode; plus the ref key .proto/.zst suffix migration.'
sidebar_label: 'CAS codecs v3 phase 3 plan'
sidebar_position: 63
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase3-refsnaplog
title: 'CAS Codecs V3 — Phase 3: Refsnaplog (Ref Log + Ref Snapshot) Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 3: Refsnaplog Text Cutover Implementation Plan {#cas-codecs-v3-phase3}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (verified against HEAD, 2026-07-15):** phase 2 is integrated AND soaked (soak #48 green); the `Core/Formats/` foundation is landed (`CasTextFormat`, `CasFormat` registry with `FormatId::RefLog = 19` / `RefSnapshot = 20` + their `TRAITS` rows, `CasWireVocab`, the shared battery). **Phase 5 (runs) is a draft IN FLIGHT on subsystem-adjacent files** (`CasLayout.h`, `CasGc.cpp`, `CasFsck.cpp`, `CasInspect.cpp`) — see the sequencing note in §draft-gate: this is a PLAN only; the phase-3 code draft is written against post-phase-5-integration mainline so the two do not race on the shared files.

**Goal:** convert the two **refsnaplog** objects from their custom binary encodings to the phase-1 text file shape, one object per commit, each independently green:

- **`cas_ref_log`** — the immutable ref transaction log object at `_log/<txn_id>` (one object = one committed transaction, a batch of `RefOp`s). Codec `Core/CasRefLogCodec.{h,cpp}` (`kRefLogTxnFormatVersion = 1`) → `Core/Formats/CasRefLogFormat.{h,cpp}`.
- **`cas_ref_snap`** — the complete per-namespace ref table snapshot at `_snap/<snapshot_id>`, which ALSO carries the **rev.6 recovery seal** (a snapshot with `sealed_from` set at a synthetic `snapshot_id = {my_epoch-1, UINT64_MAX}`). Codec `Core/CasRefSnapshotCodec.{h,cpp}` (`kRefTableSnapshotFormatVersion = 2`) → `Core/Formats/CasRefSnapshotFormat.{h,cpp}`.

Both are the `Control` family (`Tolerant`, `Always`/`.zst`, `object_cap = 64 MiB`, `line_cap = 64 KiB` per the landed `TRAITS`). The `encodeX`/`decodeX` signatures — including the `expected_ns`/`expected_txn_id` key/body-binding parameters — are preserved verbatim so every call site compiles unchanged; only the wire bytes and the codec file change. Both keys migrate to the `Always` `.zst` suffix (and `cas_ref_snap` drops its `.proto` suffix); `parseRefObjectKey` is updated to match.

Spec: `docs/superpowers/specs/2026-07-15-cas-codecs-v3-design.md` §migration-order step 3, §corrected-object-inventory (the `cas_ref_log` / `cas_ref_snap` rows), §container-design (Control family + the carried key↔body binding invariant); reference: `docs/superpowers/cas/codecs_proposal_v3.md` §control-plane.

**Tech Stack:** C++ (`dbms`), the phase-1 `CasTextFormat` helpers, phase-2 `CasWireVocab`, `CasRefIds` (`renderRefTxnId`/`parseRefTxnId`), `ReadHelpers`/`WriteHelpers`, gtest (`unit_tests_dbms`).

## Cruxes the lead asked (answered from spec + code, with FLAGs) {#cruxes}

**CRUX A — the streaming question, and a CORRECTION to the task framing (FLAG A).** The task framed refsnaplog snapshots as "the T2/T0 STREAMING surface (streaming reader, seek/getStream/ranged get)." **The code refutes this.** The survey found NO streaming/ranged/seek reader for either ref object: every reader does `backend.get(key)` then `decodeRef*(got->bytes, …)` — the snapshot and the log are **materialized whole** (`CasRefIntake.cpp:179`, `CasStore.cpp:1272`/`2047`, `CasFsck.cpp:198`/`:216`, `CasInspect.cpp:466`/`:468`). Streaming (`getStream`/`seek`/`RunFileReader`) is **GC-run-only** — that is phase 5's `cas_run`, a different object. **So refsnaplog is NOT a streaming surface and there is no seal-ref resolution to preserve here** (that is the fold seal's `RunRef` resolution, phase 5). Consequence for the layout: `cas_ref_log`/`cas_ref_snap` follow the **line-structured Control** pattern already landed in phase 2 for `cas_gc_outcomes` and `cas_fold_seal` — header line + a meta line + one JSON record per line + a `{"n":count}` trailer, **read whole** (a `readLine` loop over the decompressed body), `Always`/`.zst` for the size (`object_cap = 64 MiB` bounds the decompressed materialization; the zstd arm checks the declared size against the cap before allocation). No `seek`, no `getStream`, no offset index — nothing to reconcile against T2/T0.

**CRUX B — are ref log entries inlined or batched (FLAG none).** A `cas_ref_log` OBJECT is already exactly **one transaction** (`RefLogTxn` = `ns` + `txn_id` + a vector of `RefOp`, written once per ref commit via `ref_request_controller->putIfAbsentControlled`, `CasStore.cpp:2269`). The spec does not inline or further batch: one log object per commit is the existing granularity and stays. The transaction's `RefOp` vector becomes the record lines. (A normal txn is capped at `ref_txn_max_ops = 1000`; a `RemoveNamespace`-bearing txn is "removal-class" and shares the 64 MiB byte budget.)

**CRUX C — byte-determinism / retry-compare (FLAG C).** **No refsnaplog artifact goes through `putDeterministicArtifact`** — that path is `cas_run` + `cas_fold_seal` only (both GC formats). Every ref log/snapshot/seal PUT uses `ref_request_controller->putIfAbsentControlled(key, bytes, fence_ok)` (single-owner-key idempotent CAS), and GC's removed-snapshot republish uses `backend.putIfAbsent`. So the ref text codecs are **`Control` / `Tolerant` / `Always` — NOT `Strict`, NOT `PinnedRaw`, and there is no adoption byte-compare gate.** BUT the legacy codecs are deterministic **by construction** (sorted rows, no timestamps/iteration-order) and a test pins it (`gtest_cas_ref_codecs.cpp:679 ByteIdenticalReencode`). The text codecs MUST preserve that determinism-by-construction (emit committed rows sorted by canonical `ref_name`, precommits sorted by `(ref_name, manifest_ref)`, ops in stored order — sort inside the encoder, never trust caller order) so `ByteIdenticalReencode` keeps passing — but this is a correctness *nicety*, not a `putDeterministicArtifact` equality gate, and zstd (Always) would defeat a byte-equality gate anyway.

**CRUX D — rev.6 lease-exclusivity / recovery seal (FLAG D, the highest-risk preservation).** The recovery seal is **NOT a separate object and NOT a special log entry** — it is a `RefTableSnapshot` published at a synthetic epoch-closing id with `sealed_from` set (`CasStore.cpp:1307-1385`): `seal.snapshot_id = {my_epoch-1, UINT64_MAX}` (`:1332`), `seal.sealed_from = rt.state.greatest_applied` (`:1345`). The `sealed_from` (`std::optional<RefTxnId>`) rides the ordinary snapshot codec (on-wire `u8 has_sealed_from` + the pair, `CasRefSnapshotCodec.cpp:207-209`/`:255-258`), and `sealed_from` detection is a pure read of the decoded field (`CasRefIntake.cpp:211`, gated by `unclean_epoch_boundary_seen_at`, `CasStore.h:1170`). **This phase RE-ENCODES the bytes; it MUST NOT touch the seal semantics.** The text `cas_ref_snap` codec must round-trip, verbatim:
  - `sealed_from` — optional `RefTxnId`; absent vs present must be distinguishable (a dedicated key present/absent, not a sentinel).
  - the synthetic `snapshot_id = {my_epoch-1, UINT64_MAX}` — the **`ref_sequence = UINT64_MAX`** must survive round-trip → serialize every `RefTxnId` field as a **decimal string** (genuinely full-range `u64`), never a JSON number (this mirrors the phase-2 fold-seal `UINT64_MAX` sentinel handling).
  - the `lifecycle`/`remove_txn_id` coupling (Live ⇒ no `remove_txn_id`; Removed ⇒ `remove_txn_id` set + `committed`/`precommits` empty) and the `snapshot_id >= *sealed_from` invariant (`CasRefSnapshotCodec.cpp:179-187`) — carried into the text decoder unchanged.
  **Coverage-gap FLAG:** the survey found NO dedicated `sealed_from` round-trip test (`sealed_from` appears only in comments in `gtest_cas_ref_codecs.cpp`). Phase 3 must ADD one (a Live snapshot with `sealed_from` set + a synthetic `{e-1, UINT64_MAX}` snapshot_id round-trip) — this is net-new coverage the re-encode makes essential.

**CRUX E — key-suffix migration (FLAG E).** Both objects are `Always` → both keys take the `.zst` `storedSuffix`, and `cas_ref_snap` additionally DROPS its current `.proto` suffix (`refSnapshotKey` returns `…/_snap/<render>.proto` today, `CasLayout.h:128`). So: `refLogKey` → append `storedSuffix(FormatId::RefLog)` (`.zst`); `refSnapshotKey` → replace `.proto` with `storedSuffix(FormatId::RefSnapshot)` (`.zst`). `parseRefObjectKey` (`CasLayout.h:148`) must **stop stripping `.proto` and start stripping `.zst`** for both `_log` and `_snap` keys — this is the LIST-discovery inverse (`CasRefIntake.cpp:155-161`) and the source of the `expected_ns`/`expected_txn_id` the key/body binding checks against, so it must land atomically with the key builders.

**CRUX F — byte budgets re-derived for JSON inflation (FLAG F).** The legacy caps are `ref_txn_max_bytes = 1 MiB`, `ref_removal_max_bytes = 64 MiB` (= `ref_snapshot_max_bytes`), `ref_txn_max_ops = 1000` (`CasRefLogCodec.h:81-83`). The landed `TRAITS` give both objects `object_cap = 64 MiB`, `line_cap = 64 KiB` — with a comment that they are "provisional until phase 3 re-derives the byte budgets for JSON." JSON inflates the binary ~2–3× (hex ids, key names, quotes). Phase 3 must re-derive and RECONCILE the two sources of truth: (i) confirm one op-record / one committed-row LINE fits `line_cap = 64 KiB` (a single op with a `ManifestRef` + a canonical `ref_name` ≤ a few hundred bytes — comfortable); (ii) confirm the whole decompressed object fits `object_cap = 64 MiB` at the inflated size, or raise it. The codec-header constants (`ref_txn_max_bytes` etc.) stay the ENCODE-side budget the encoder self-checks against (now measured over the JSON bytes); the `TRAITS` caps are the DECODE-side fail-closed allocation guard. Task 4 states the derived numbers next to the `TRAITS` row and removes the "provisional" comment.

## Global Constraints {#global-constraints}

- **Allman braces** everywhere.
- **Layering (physical, phase-1 rule):** files in `Core/Formats/` include only other `Formats/` headers, the identifier vocabulary (`CasRefIds.h`, `CasManifestId.h`, `CasIds.h`, `CasToken.h`), `base/`, `src/IO/`, `src/Common/`, `<zstd.h>`. NEVER `CasBackend.h`/`CasStore.h`/`CasLayout.h`/`CasRefIntake.h`. Protocol logic that needs a backend (`ref_request_controller`, recovery, the seal-write in `CasStore`) STAYS in `Core/` and includes the new `Formats/` header. The `Formats/` codecs are pure mapping + invariants over `CasTextFormat`.
- **Key↔body binding PRESERVED (HARD):** `decodeRefLogTxn(data, expected_ns, expected_txn_id)` and `decodeRefTableSnapshot(data, expected_ns, expected_snapshot_id)` keep their EXACT signatures and their cross-check (`CasRefLogCodec.cpp:223-228` / `CasRefSnapshotCodec.cpp:272-277`) — the decoded body `ns`/`txn_id` must equal the key-derived expected values or `CORRUPTED_DATA`. The one intentional skip (wedge-preview raw decode, `CasStore.cpp:1836`) stays a raw decode.
- **All the legacy invariants carry over unchanged** (the codecs enforce them at BOTH encode and decode): canonical clean `ref_name` (`checkCanonicalRefName`), non-zero `ManifestRef` fields, non-zero `RefTxnId` fields, `committed` strictly ascending by `ref_name`, `precommits` strictly ascending by `(ref_name, manifest_ref)`, every `precommits` entry `kind == Precommit`, the lifecycle/`remove_txn_id` coupling, and `snapshot_id >= *sealed_from`. Re-express each over the JSON form; do not drop any.
- **`Tolerant` keys, `Always`/`.zst`:** the `JsonObjectReader` runs `KeyStrictness::Tolerant` (additive-friendly; an unknown plain key is skipped, an unknown `!`-key is `UNKNOWN_FORMAT_VERSION`). `sealObject`/`openObject` handle the single zstd frame; the declared decompressed size is checked against `object_cap` before allocation.
- **`v` stamping stays at `G_BUILD = 3`.** No breaking generation is introduced (this is a re-encode of already-generation-3 objects); no `G_BUILD` bump, no `changePoints` append. The header gate is `expectHeaderLine` (future `v` → `UNKNOWN_FORMAT_VERSION`). (The legacy `kRefLogTxnFormatVersion=1` / `kRefTableSnapshotFormatVersion=2` disappear with the binary codecs — `v` in the text header is the only version field.)
- **Pinned JSON write settings** inherited from phase-1 (`escape_forward_slashes = false`) — ref names and namespaces are `/`-dense (`00/aa@cas@`, `t-1/all_1_2_0`), so the readable, byte-stable form is load-bearing for the `ByteIdenticalReencode` determinism-by-construction.
- **Pre-release, hard cutover, no dual-read:** each object flips in ONE commit; no "try proto then text" fallback.
- **Build/commit discipline** (as prior phases): substitute the real build dir (`ls -d build*`; examples use `build_debug`), foreground only, no `-j`/`nproc`, redirect to a per-task log, read back `NINJA_EXIT=`, subagent-analyze:

  ```
  flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p3t<N>.log 2>&1; echo "NINJA_EXIT=$?"
  ```

  Commit after every task; never rebase/amend; branch `cas-gc-rebuild`; explicit-path `git add` (never `git add -A` on this shared worktree); `git log -1 --stat` names only your files. Trailer on every commit:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## Interfaces consumed from phases 1–2 {#consumed-interfaces}

- `Core/Formats/CasTextFormat.h`: `writeKey`, `writeStringValue`, `writeU64StringValue`, `writeBoolValue`, `writeHex128Value`, `closeObject`, `writeHeaderLine`, `writeTrailerLine`, `readLine`, `expectHeaderLine`, `JsonObjectReader` (`nextKey`/`readString`/`readU64String`/`readU64Number`/`readBool`/`skipUnknown`), `sealObject`/`openObject`.
- `Core/Formats/CasFormat.h`: `FormatId::RefLog`/`RefSnapshot`, `traitsFor`, `storedSuffix`, `checkCompatibility`, `G_BUILD`.
- `Core/Formats/CasWireVocab.h`: reuse where applicable (the ref formats carry no `Token`/`BlobRef`, so `writeTokenFields`/`writeBlobRefFields` are NOT used; the shared ManifestRef + RefOwnerBinding rendering is NEW — see the placement decision).
- `Core/CasRefIds.h`: `RefTxnId` (`{writer_epoch, ref_sequence}`), `renderRefTxnId`/`parseRefTxnId` (canonical `16hex-16hex`, used by the KEY not the body). `Core/CasManifestId.h`: `ManifestRef` (`{writer_epoch, build_sequence, manifest_ordinal}`). `Core/CasIds.h`: `RootNamespace` (strong `String`).

## Object-to-codec-file map + placement decision {#object-file-map}

| Object | New `Formats/` file | Structs moved from | Protocol logic that STAYS in `Core/` |
|---|---|---|---|
| shared ref wire sub-types | `CasRefWireVocab.{h,cpp}` (NEW) | `RefOwnerKind`, `RefOwnerBinding`, `RefOpKind`, `ManifestRef` field rendering | — |
| `cas_ref_log` | `CasRefLogFormat.{h,cpp}` | `Core/CasRefLogCodec.h` (`RefOp`, `RefLogTxn`; budgets) | none (the codec is pure) — `CasStore`/`CasRefIntake` include the new header |
| `cas_ref_snap` | `CasRefSnapshotFormat.{h,cpp}` | `Core/CasRefSnapshotCodec.h` (`RefLifecycle`, `RefCommittedRow`, `RefTableSnapshot`) | recovery seal write (`CasStore::ensureRefTableRecovered`), snapshot selection (`CasRefIntake`) — stay, include the new header |

**Placement decision (FLAG G — resolve in Task 1):** `RefOwnerBinding` (+ `RefOwnerKind`) is SHARED — the log uses it inside `OwnerTransition` ops, the snapshot uses it for `precommits`. Its wire struct must move to `Formats/`, and it must be includable by BOTH ref codecs without one depending on the other. Resolution: a small `Core/Formats/CasRefWireVocab.{h,cpp}` holding `RefOwnerKind` + `RefOwnerBinding` + the enum word maps (`RefOpKind`, `RefOwnerKind`, `RefLifecycle`) + `ManifestRef` field writers/readers (`me`/`mb`/`mo`). `ManifestRef` rendering is ALSO phase-6's (part manifest); flag it for promotion to `CasWireVocab` when phase 6 lands if identical — for now it lives in the ref vocab. (Alternative rejected: leaving `RefOwnerBinding` in `CasRefLogFormat.h` and having the snapshot codec include the log codec — that couples two peer formats and violates the one-struct-set-per-codec convention.)

**Field-to-JSON-key policy (per the naming policy: keys 2–5 chars, full-word enum values, hashes/ids lowercase hex, unbounded u64 = decimal strings, bounded ints = numbers):**

- **`RefTxnId`** everywhere (`txn_id`, `snapshot_id`, `remove_txn_id`, `sealed_from`) → two sibling decimal-STRING keys (unbounded, and `ref_sequence` reaches `UINT64_MAX` for a seal): `we`/`rs` for the primary id on the meta line; `rte`/`rts` for `remove_txn_id`; `sfe`/`sfs` for `sealed_from` (presence of the pair = present). Never JSON numbers (the seal's `UINT64_MAX`).
- **`ManifestRef`** → three flat keys `me` (writer_epoch, string), `mb` (build_sequence, string), `mo` (manifest_ordinal, number — structurally bounded small). In an `OwnerTransition` op the old/new bindings prefix them: `o…`/`n…` (see the record table).
- **Enums as full words:** `RefOpKind` → `namespace_birth`/`owner_transition`/`set_payload`/`remove_namespace`; `RefLifecycle` → `live`/`removed`; `RefOwnerKind` → `committed`/`precommit`.
- **`ref_name`** → `rn` (string, canonical-checked). **`payload`** → `pl` (string; production always empty but kept as a wire carrier). **`published_at_ms`** → `ts` (number, ms timestamp < 2^53).
- **FLAG H (flat vs nested for bindings):** an `OwnerTransition` op carries up to two `RefOwnerBinding`s (`old_binding`/`new_binding`), each with a kind + ref_name + ManifestRef. Phase 2 forbade nested JSON objects (the flat `JsonObjectReader` has no array/nesting support). **Resolution: flatten with an `o`/`n` prefix** — old binding → `obk`(kind word)/`orn`(ref_name)/`ome`/`omb`/`omo`; new binding → `nbk`/`nrn`/`nme`/`nmb`/`nmo`. Presence of the `obk`/`nbk` key signals the optional binding present. No `JsonObjectReader` change. (Alternative rejected: extending the reader for one level of nesting — larger blast radius, and the flat prefix is `jq`-legible enough.)

## Per-object text shape {#text-shape}

**`cas_ref_log`** (one object = one transaction; op record lines):

```text
{"type":"cas_ref_log","v":3}
{"ns":"00/aa@cas@","we":"5","rs":"12"}                                   meta: ns + txn_id
{"op":"namespace_birth"}
{"op":"owner_transition","nbk":"committed","nrn":"a/b.bin","nme":"5","nmb":"15","nmo":1}
{"op":"owner_transition","obk":"committed","orn":"a/b.bin","ome":"4","omb":"9","omo":1,"nbk":"committed","nrn":"a/b.bin","nme":"5","nmb":"15","nmo":1}
{"op":"set_payload","rn":"a/b.bin","me":"5","mb":"15","mo":1,"pl":"","ts":1752537600000}
{"n":4}
```

**`cas_ref_snap`** (full table; committed + precommit record lines; meta carries lifecycle/seal):

```text
{"type":"cas_ref_snap","v":3}
{"ns":"00/aa@cas@","we":"5","rs":"12","lc":"live"}                        Live, no remove/seal
{"k":"c","rn":"a/b.bin","me":"5","mb":"15","mo":1,"pl":"","ts":1752537600000}
{"k":"p","rn":"c/d.bin","me":"6","mb":"20","mo":1}                        precommit (kind must be Precommit)
{"n":2}
```

Meta variants: a **Removed** table → `"lc":"removed","rte":"<epoch>","rts":"<seq>"` and NO record lines (`committed`/`precommits` empty, `{"n":0}`). A **recovery seal** → a Live meta with `"sfe":"<epoch>","sfs":"<seq>"` present and the synthetic `"we":"<my_epoch-1>","rs":"18446744073709551615"` (`UINT64_MAX`).

## Tasks {#tasks}

Hard cutover per object (pre-release, no dual-read), phase-2 rigor. Task 1 is the shared vocab; Tasks 2–3 are the two objects (independent of each other once Task 1 lands); Task 4 is the key migration + finish. Each object task deletes its legacy binary codec, rewires includers, and migrates its tests in the same commit.

### Task 1 — shared ref wire vocab (`Formats/CasRefWireVocab`) {#task1}

**Files:** Create `Core/Formats/CasRefWireVocab.{h,cpp}`; Test `src/Disks/tests/gtest_cas_ref_wire_vocab.cpp` (new).

**Interfaces produced (frozen; Tasks 2/3 draft against):**
```cpp
namespace DB::Cas
{
enum class RefOwnerKind : uint8_t { Committed = 1, Precommit = 2 };   // moved verbatim
struct RefOwnerBinding { RefOwnerKind kind = RefOwnerKind::Committed; String ref_name; ManifestRef manifest_ref; bool operator==(const RefOwnerBinding &) const = default; };

std::string_view refOwnerKindToWord(RefOwnerKind);
RefOwnerKind     refOwnerKindFromWord(std::string_view, std::string_view what);
// (RefOpKind / RefLifecycle word maps live with their owning codec — they are not shared — OR here if
//  the executor prefers one vocab; decide at draft and keep it consistent.)

/// Append `,"<p>me":"..","<p>mb":"..","<p>mo":N` for a ManifestRef under an optional key prefix `p`
/// ("" for a bare row, "o"/"n" for old/new bindings). Reader helper validates non-zero fields.
void writeManifestRefFields(WriteBuffer & out, bool & first, std::string_view prefix, const ManifestRef & r);
}
```

**Steps:** (1) RED test — enum-word round-trip (fail-closed on unknown word), `writeManifestRefFields` + read-back round-trip incl. the `o`/`n` prefix, a non-canonical/zero `ManifestRef` field → `CORRUPTED_DATA`. (2) Compile-fail. (3) Implement (move `RefOwnerKind`/`RefOwnerBinding` verbatim from `CasRefLogCodec.h`; word maps; the flat ManifestRef writers/readers). (4) Green. (5) Commit `cas: formats v3 phase 3 — shared ref wire vocab (RefOwnerBinding + ManifestRef fields)` + trailer.

### Task 2 — `cas_ref_log` text cutover {#task2}

**Files:** Create `Core/Formats/CasRefLogFormat.{h,cpp}` (move `RefOp`, `RefLogTxn`, the budget constants); Delete `Core/CasRefLogCodec.{h,cpp}`; include-rewrite every includer (`grep -rl 'ContentAddressed/Core/CasRefLogCodec\.h' src/ | xargs sed -i 's|…/CasRefLogCodec.h|…/Formats/CasRefLogFormat.h|g'`); Modify `Core/CasLayout.h` (`refLogKey` appends `storedSuffix(FormatId::RefLog)`); Test — migrate `gtest_cas_ref_codecs.cpp`'s log suite into `gtest_cas_ref_log_format.cpp` + a battery row.

**Interfaces produced:** `struct RefOp`, `struct RefLogTxn`, `String encodeRefLogTxn(const RefLogTxn &)`, `RefLogTxn decodeRefLogTxn(std::string_view, const String & expected_ns, const RefTxnId & expected_txn_id)` — signatures identical to today.

**Steps:**
- [ ] **RED test** — battery row (`FormatId::RefLog`) with a golden text; a multi-op txn round-trip (namespace_birth + owner_transition add/replace/removal + set_payload); the key/body binding rejections (`DecodeRejectsBodyNamespaceMismatch`/`DecodeRejectsBodyTxnIdMismatch`, re-pointed); a non-canonical `ref_name` → `CORRUPTED_DATA`; a zero `txn_id` field → `CORRUPTED_DATA`; over-`ref_txn_max_ops`/over-budget → `CORRUPTED_DATA`; the `ByteIdenticalReencode` determinism assertion re-pointed at the text codec.
- [ ] **Implement** — `encodeRefLogTxn`: header line; meta line `{"ns","we","rs"}`; one record line per `RefOp` (discriminator `op` = kind word; fields per §text-shape via `CasRefWireVocab`); `{"n":ops.size()}` trailer. Emit ops in stored order (deterministic). Self-check the encoded byte budget (`ref_txn_max_bytes`, or `ref_removal_max_bytes` for a `RemoveNamespace`-bearing txn) — now measured over JSON bytes (CRUX F). `decodeRefLogTxn`: `expectHeaderLine`; meta line → `ns`/`txn_id`; a `readLine` loop dispatching `op`/`n`; the trailer count check; the key/body binding cross-check; re-apply every legacy invariant. `Tolerant` reader. Delete `Core/CasRefLogCodec.{h,cpp}`, rewire includers.
- [ ] **Verify** `unit_tests_dbms --gtest_filter='CasFormatBattery.RefLog:CasRefLog*:CasRefCodec*'` green; the ref state-machine / writer / intake behavioral suites (`gtest_cas_ref_statemachine`, `gtest_cas_ref_writer`, `gtest_cas_ref_intake`) still green.
- [ ] **Commit** `cas: formats v3 phase 3 — cas_ref_log text cutover (.zst key suffix)` + trailer.

### Task 3 — `cas_ref_snap` text cutover (carries the rev.6 seal) {#task3}

**Files:** Create `Core/Formats/CasRefSnapshotFormat.{h,cpp}` (move `RefLifecycle`, `RefCommittedRow`, `RefTableSnapshot`, `ref_snapshot_max_bytes`); Delete `Core/CasRefSnapshotCodec.{h,cpp}`; include-rewrite includers; Modify `Core/CasLayout.h` (`refSnapshotKey`: drop `.proto`, append `storedSuffix(FormatId::RefSnapshot)`; `parseRefObjectKey`: strip `.zst` not `.proto` — see Task 4 note); Test — migrate `gtest_cas_ref_codecs.cpp`'s snapshot suite into `gtest_cas_ref_snapshot_format.cpp` + a battery row + the NET-NEW `sealed_from` test.

**Interfaces produced:** `enum class RefLifecycle`, `struct RefCommittedRow`, `struct RefTableSnapshot`, `String encodeRefTableSnapshot(const RefTableSnapshot &)`, `RefTableSnapshot decodeRefTableSnapshot(std::string_view, const String & expected_ns, const RefTxnId & expected_snapshot_id)` — signatures identical to today.

**Steps:**
- [ ] **RED test** — battery row (`FormatId::RefSnapshot`) golden; `RoundTripLive`/`RoundTripLiveEmpty`/`RoundTripRemoved` re-pointed; the key/body binding rejections (`DecodeRejectsNamespaceMismatch`/`DecodeRejectsSnapshotIdMismatch`); the lifecycle/`remove_txn_id` coupling rejections; non-ascending `committed`/`precommits` → `CORRUPTED_DATA`; a `precommits` entry with `kind != Precommit` → `CORRUPTED_DATA`; `ByteIdenticalReencode` re-pointed. **NET-NEW (CRUX D coverage gap):** `SealedFromRoundTrips` — a Live snapshot with `sealed_from` set and `snapshot_id = {e-1, UINT64_MAX}`, asserting both survive round-trip AND `rs`/`rts`/`sfs` serialize as decimal STRINGS (grep the encoded text for `"rs":"18446744073709551615"`); and `RejectsSnapshotIdBelowSealedFrom` (`snapshot_id < *sealed_from` → `CORRUPTED_DATA`).
- [ ] **Implement** — `encodeRefTableSnapshot`: header; meta line `{"ns","we","rs","lc"[,"rte","rts"][,"sfe","sfs"]}`; committed rows (`{"k":"c",...}`, sorted by `ref_name`); precommit rows (`{"k":"p",...}`, sorted by `(ref_name, manifest_ref)`, `kind` forced/checked `Precommit`); `{"n":committed+precommits}` trailer. `decodeRefTableSnapshot`: header; meta (lifecycle word, optional `remove_txn_id`/`sealed_from`); record loop by `k`; trailer count; key/body binding; ALL legacy invariants incl. `snapshot_id >= *sealed_from` and the lifecycle coupling. `Tolerant`. Delete `Core/CasRefSnapshotCodec.{h,cpp}`, rewire includers.
- [ ] **Verify** `unit_tests_dbms --gtest_filter='CasFormatBattery.RefSnapshot:CasRefSnapshot*:CasRefSnapshotCodec*'` green; the seal/lease-exclusivity behavioral suites green: `gtest_cas_store.cpp` (`CasRemountTmat.*` incl. the `CasRefRecoverySealPublished`-counter assertions at `:1621-1627`, `CasStoreRemount.OldEpochBuildFailsClosedAfterRemount`), `gtest_cas_ref_intake.cpp`, `gtest_cas_ref_gc.cpp` (removed-namespace publish). **This is the rev.6 safety net — it must pass unchanged through the re-encode.**
- [ ] **Commit** `cas: formats v3 phase 3 — cas_ref_snap text cutover (.zst key, drops .proto; rev.6 sealed_from preserved)` + trailer.

### Task 4 — key/parse migration, budget reconciliation, CasInspect, README, finish {#task4}

**Files:** `Core/CasLayout.h` (`parseRefObjectKey`), `Core/CasInspect.cpp` (ref log/snap render → text), `Core/Formats/CasFormat.cpp` (`TRAITS` caps comment), `Core/Formats/README.md` (bucket-map rows), and the LIST-discovery cross-check.

**Steps:**
- [ ] **`parseRefObjectKey`** (`CasLayout.h:148`) — stop stripping `.proto`; strip `.zst` for both `_log` and `_snap` keys (both are `Always` now). Confirm it still returns `nullopt` (never throws) on a foreign/malformed key. Sweep the LIST-discovery caller (`CasRefIntake.cpp:155-161`) and every `parseRefObjectKey` user (fsck, gc, orphan-sweep) — they inherit the fix; grep `'\.proto'` under `ContentAddressed/` → zero refs after (except historical comments). **Ordering note:** `refLogKey` (`.zst`) landed in Task 2 and `refSnapshotKey` (`.zst`, drop `.proto`) in Task 3; the `parseRefObjectKey` change must be consistent across both — if Task 2 lands first, `parseRefObjectKey` must already tolerate a `.zst` `_log` key at that point (do the parser's `.zst` handling in Task 2 and the `.proto` removal in Task 3, OR do the whole parser change here in Task 4 and have Tasks 2/3 note the dependency). Recommended: land the full `parseRefObjectKey` rewrite in Task 4 and gate Tasks 2/3's integration behind it (they are pre-release; a half-migrated key space never persists).
- [ ] **`CasInspect.cpp`** (`:466`/`:468`) — the ref log/snap inspect render currently calls the binary `decodeRef*` + `renderRef*`; re-point at the text codecs (decode is unchanged signature; the "render" can become a decompress-and-print of the canonical text, per the spec's `CasInspect` direction — a thin `openObject` + emit). Keep the output informative.
- [ ] **Budget reconciliation (CRUX F)** — measure/derive the JSON-inflated sizes; state the numbers in a comment next to the `RefLog`/`RefSnapshot` `TRAITS` rows (`CasFormat.cpp:114-115`) and REMOVE the "provisional until phase 3" comment (`CasFormat.cpp:105`). Confirm `line_cap = 64 KiB` covers the largest single op/committed-row line and `object_cap = 64 MiB` covers the largest inflated removal-class txn / full snapshot (raise if the derivation shows otherwise; a raise is a `TRAITS`-only edit).
- [ ] **`README.md`** — flip the two bucket-map rows (`cas/refs/<ns>/…_log` → `CasRefLogFormat`, `…_snap` → `CasRefSnapshotFormat`), drop their `*`, and move refsnaplog into the DONE set of the phase-status sentence.
- [ ] **Verify + Commit** — full `unit_tests_dbms --gtest_filter='Cas*'` green. `cas: formats v3 phase 3 — ref key .proto→.zst migration + parseRefObjectKey + CasInspect + README` + trailer.

## Phase-3 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_ref_log` and `cas_ref_snap` are the phase-1 text shape (header + meta line + record lines + `{"n"}` trailer, `Always`/`.zst`), read WHOLE (no streaming/seek — CRUX A); the binary `CasRefLogCodec`/`CasRefSnapshotCodec` are deleted; `kRefLogTxnFormatVersion`/`kRefTableSnapshotFormatVersion` are gone.
- **Key/body binding preserved:** the `expected_ns`/`expected_txn_id` cross-check fires on a body-under-wrong-key (both codecs); the decode signatures are unchanged so all call sites compile.
- **rev.6 seal preserved (CRUX D):** `sealed_from` (optional) and the synthetic `snapshot_id = {my_epoch-1, UINT64_MAX}` round-trip byte-faithfully (every `RefTxnId` field a decimal string); the `snapshot_id >= *sealed_from` and lifecycle/`remove_txn_id` invariants hold; the `gtest_cas_store.cpp` `CasRemountTmat.*` seal/lease-exclusivity suite passes unchanged; the NET-NEW `SealedFromRoundTrips` test is green.
- **Determinism-by-construction preserved (CRUX C):** `ByteIdenticalReencode` passes for both codecs (sorted rows, stored op order) — and it is documented that this is NOT a `putDeterministicArtifact` gate (refsnaplog uses `putIfAbsentControlled`).
- **Key migration (CRUX E):** both keys carry `.zst`; `cas_ref_snap` no longer carries `.proto`; `parseRefObjectKey` strips `.zst`; LIST-discovery + fsck/gc/orphan-sweep still classify keys correctly; grep shows no `.proto` in the ref key path.
- **Budgets re-derived (CRUX F):** the JSON-inflated sizes are stated next to the `TRAITS` caps and the "provisional" comment is gone.

## Phases: this is JIT — draft gate + sequencing {#draft-gate}

PLAN ONLY. The **code draft** is NOT written until the lead clears it. **Sequencing (lead-flagged): phase 5 (runs) is a draft in flight and touches shared files** — `CasLayout.h` (phase 5 removed `partManifestCleanupKey`; phase 3 edits `refLogKey`/`refSnapshotKey`/`parseRefObjectKey`), `CasGc.cpp` (phase 5 heavy; phase 3 reads `cas_ref_log` at `CasGc.cpp:1005` and publishes a removed snapshot at `:1585`), `CasFsck.cpp` (both), `CasInspect.cpp` (phase 5 removed the pmc render; phase 3 changes the ref render). To avoid a patch-stack race, **the phase-3 code draft is written against post-phase-5-integration mainline** (the edits are then to disjoint regions of the shared files and merge cleanly). Tasks 1–3 are otherwise independent of phase 5. The lead clears the draft explicitly.

## Deferred / open {#deferred}

- **FLAG G** (RefOwnerBinding placement) and **FLAG H** (flat-prefix vs nested bindings) are resolved above but re-confirmed at draft.
- **ManifestRef → CasWireVocab promotion:** phase 6 (part manifest) renders `ManifestRef` too; if identical, promote `writeManifestRefFields` from `CasRefWireVocab` to `CasWireVocab` when phase 6 lands (not now — YAGNI).
- Exact JSON-inflated cap numbers (CRUX F) are measured at draft against representative removal-class txns / large snapshots.

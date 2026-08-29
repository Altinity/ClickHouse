---
description: 'Phase-2 implementation plan for the CAS semantic wire-keys design: the atomic cut — flip every key, tag, and the two listed value representations to the semantic vocabulary and reset the generation history'
sidebar_label: 'CAS wire keys phase 2'
sidebar_position: 21
slug: /superpowers/plans/cas-wire-keys-phase2-cut
title: 'CAS wire keys — phase 2 (atomic cut)'
doc_type: 'guide'
---

# CAS Wire Keys Phase 2: Atomic Cut Implementation Plan {#cas-wire-keys-phase2-cut}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flip every CAS persisted-format key, record tag, and the two listed value representations to the semantic vocabulary, resetting the generation history to `{1, 1}` — the constants change in one place each; the codecs were bound to them in phase 1.

**Architecture:** Phase 1 (landed, `29b74be717e..14729b5956b`) put every writer and reader of all 17 registered formats onto `WireKey` constants, `EnumWireTable` vocabularies, and shared bundles/collectors, byte-frozen on the OLD spellings. This plan edits the constant VALUES and the goldens that pin them, format by format, plus the four spec-listed structural changes that ride the cut: the generation reset, the `algos_used` JSON array, the fold-seal `class` words with a typed `CoverageClass`, and the `TokenFields` requiredness unification. Each task leaves the full `CAS*` gate green; the branch is unreleased, so "atomic" means "no release between tasks", not "one commit".

**Tech Stack:** C++ (ClickHouse tree), gtest unit gate, `magic_enum` (`.cpp`/tests only).

**Spec:** docs/superpowers/specs/2026-08-28-cas-semantic-wire-keys-design.md (revision 14). The spec is the binding authority; every old→new table in this plan is copied from it verbatim. On any conflict, the spec wins.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`, this checkout. Run `git branch --show-current` before AND after every work session; if not on `cas-gc-rebuild`, STOP (do not create branches). No rebase, no amend, NEVER push.
- Gate: `ninja -C build unit_tests_dbms > build/build_wirekeys_p2_task<N>.log 2>&1` then `build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_cas_p2_task<N>.log 2>&1` — PASS required at the END of every task. Every build/test log is analyzed by a subagent that returns a concise summary (repository rule); the controller never greps a log tail and calls it analyzed. Baseline entering Task 1: 2212 tests. Known flake `CASRefWriterChunkedFlush.SnapshotPublisherLatchedAcrossChunks`: rerun once if it is the only failure. Each task's commit message lists every updated expectation (old→new) or names the task's report file (`.superpowers/sdd/2026-08-29-cas-wire-keys-phase2-cut/task-<N>-report.md`), which does.
- **Worklist rule (inventory-first).** Test sources spell JSON both raw (`"tt":`) and ESCAPED (`\"tt\":`), and some build raw JSON that never fails a gate (e.g. `gtest_cas_json_writer.cpp`). Therefore: before flipping any key or tag, build the update worklist by grepping BOTH forms of every spelling the task flips across `src/Disks/tests/` AND the codec comments under `ContentAddressed/` — gate-failure lists are drift verification only, never the inventory.
- **Docs ride each commit (spec rule).** Every format task updates, in the SAME commit as its key flip: the codec's own header/impl comments that spell old keys, and that format's row/example in `Formats/README.md`. Task 19 is the final docs AUDIT plus the one format-wide piece that belongs to no single format task (the README naming-rules section); per-format docs never wait for it.
- Goldens spell their bytes LITERALLY and must never be constructed from the production key constants or enum tables. The battery's `currentFormatHeader(...)` header-composition helper is existing framing practice and stays.
- Mixed-vocabulary trees BETWEEN tasks are expected and fine: constants are the single truth; each task moves its constants and every golden that pins them together.
- One canonical spelling per containing format; keys with `!` carry the prefix inside the literal (`"!prev_epoch"`).
- No `EXPECT_THROW` on `LOGICAL_ERROR` (death-split rule; new negatives here are `CORRUPTED_DATA` or `UNKNOWN_FORMAT_VERSION`). Allman braces. Comments state constraints, never provenance.
- The permanently forbidden known-field guards stay: `pl` rejected on ref-log and ref-snapshot rows, `rte`/`rts` rejected on the ref-snapshot meta line — literals, not constants, together with their tests.
- `CasInspect` renders through the shared tables (phase 1), so its words follow each table flip automatically — but its pinned assertions are updated in the SAME task that flips the vocabulary they pin.
- Wire namespaces + codec tables stay inside each `.cpp`'s anonymous namespace; public delegates in headers. `magic_enum` stays out of production headers. New codec files (if any) follow the same placement rule.
- Keys that do NOT change and must survive every sweep: framing `type`/`v`/`n` and the run-header `kind`; mount-lease `pid`, `seq`, `write_attempt_id`; catalog `ns`; envelope `tag`, `op`, `ref`; fold-seal `key`, `shard`, `life`.

---

### Task 1: Generation reset {#task-1}

**Files:**
- Modify: `Formats/CasFormat.h` (line 62: `constexpr uint32_t G_BUILD = 10;` and the legacy constants at lines ~69-98)
- Modify: `Formats/CasFormat.cpp` (change-point arrays)
- Modify: `Formats/CasPoolMetaFormat.cpp` (lines ~113-124: `decodePoolMeta`'s backward gate reads `kMountWriteAttemptIdGeneration` — a PRODUCTION consumer of a deleted constant)
- Modify: `src/Disks/tests/gtest_cas_format.cpp` (lines ~31-101: change-point-history and generation-9 assertions), `gtest_cas_ns_file_incarnation.cpp` (lines ~233-278: dynamic generation-5 fixtures), plus every fixture the Step 3 sweep finds
- Modify: `Formats/README.md` (generation-history table)

**Interfaces:**
- Produces: `G_BUILD == 1`; `changePoints(FormatId)` returns the existing shared `BASELINE` (`{{1, 1}}`) for all 17 formats and `Roster`; the legacy named constants are GONE; `decodePoolMeta`'s backward gate is expressed against the baseline floor.

- [ ] **Step 1: Flip the version core.** `G_BUILD` 10 → 1. In `CasFormat.cpp`, delete the per-format change-point arrays (`REF_STREAM`, `REF_CKPT`, `REF_CATALOG`, `GC_MAINTENANCE_STATE`, `POOL_META`, `MOUNT_LEASE`) so every format returns `BASELINE`. Sweep EVERY consumer of the legacy constants first with the NAME-SHAPE grep `rg -n 'k[A-Z][A-Za-z]*Generation' src/Disks/` (the authority — the full set at `CasFormat.h` ~69-98 is `kContiguousRefStreamsGeneration`, `kNamespaceLifeKeyedGeneration`, `kOpaqueNamespaceLifeLayoutGeneration`, `kUnifiedRefLifeFoldGeneration`, `kPoolGcShardsGeneration`, `kCommittedRefFrontierGeneration`, `kMountWriteAttemptIdGeneration`; a hand list of three would miss four) — the known production hit is `decodePoolMeta`'s backward gate (`header.v < kMountWriteAttemptIdGeneration`), which is re-expressed against the reset floor so a persisted `v:0` rejects with `UNKNOWN_FORMAT_VERSION` and `v:1` passes; then delete the constants and every remaining reference, including history-narrating comments.
- [ ] **Step 2: Rewrite the history tests.** Delete only the tests specific to historical generations (the pre-contiguous, generation-five, generation-six, and generation-nine fixtures — `gtest_cas_format.cpp:31-101` and `gtest_cas_ns_file_incarnation.cpp:233-278` are the known homes; the Step 3 sweep is the completeness check). Replace old change-point assertions with ONE test asserting all 17 formats plus `Roster` report the `{1, 1}` baseline. RULE (spec): deleting historical fixtures must NOT delete the last test of either pool gate. Then verify all four post-reset gate tests exist (add any missing, next to the pool-meta codec tests):
  - forward gate: a header stamped `v:2` → `UNKNOWN_FORMAT_VERSION` before the body;
  - pool forward gate separately: pool meta `v:1` with `min_reader_generation: 2` → `UNKNOWN_FORMAT_VERSION`;
  - dormant backward gate: a header stamped `v:0` → `UNKNOWN_FORMAT_VERSION`;
  - a freshly created pool mints `min_reader_generation: 1`.
- [ ] **Step 3: Re-stamp fixtures (raw AND escaped, FIXED-STRING searches).** Regex escaping is the trap here (a wrongly-doubled backslash silently matches nothing), so use `rg -F`: for each N in 2..10 run `rg -nF '"v":N' src/Disks/tests/` and `rg -nF '\"v\":N' src/Disks/tests/` with the literal N substituted (known escaped homes: `gtest_cas_blob_meta_format.cpp:65-71`, `gtest_cas_gc_state_format.cpp:92,134,166`, `gtest_cas_server_root_format.cpp:141`, `gtest_cas_gc_outcomes_format.cpp:121-137`, `gtest_cas_text_format.cpp:218,260,266`). Classify every hit: (a) historical refusal → deleted in Step 2; (b) the `v:2` forward-gate fixture → stays; (c) a body-under-test fixture → restamp to `"v":1` and update its "any version <= G_BUILD" comment. Finish with a zero-hit check for versions 3–10 in both fixed-string forms.
- [ ] **Step 4: README.** Replace the per-generation history table with a single note recording the reset.
- [ ] **Step 5: Build + full gate green.** Record the exact test-count delta in the report. **Commit:** `cas: reset the format generation history to the {1,1} baseline`.

---

### Task 2: Shared repeated-value vocabulary {#task-2}

**Files:**
- Modify: `Formats/CasWireVocab.{h,cpp}` (SharedWire, the three `ManifestRefWireKeys` bundles, `BindingWireKeys`; the comment blocks at `CasWireVocab.h` ~53-68, 82-102, 135-150 spell old keys — same commit)
- Modify (worklist per the inventory rule; known homes): `src/Disks/tests/gtest_cas_ref_log_format.cpp`, `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_ref_snapshot_format.cpp`, `gtest_cas_part_manifest_format.cpp`, `gtest_cas_record_stream_format.cpp`, `gtest_cas_gc_outcomes_format.cpp`, `gtest_cas_encoding_pins.cpp`, `gtest_cas_wire_vocab.cpp`, `gtest_cas_json_writer.cpp` (raw JSON that does NOT fail the gate — inventory catches it)
- Modify: `Formats/README.md` (the shared-vocabulary description)

**Interfaces:**
- Produces: the flipped bundles every later task's goldens assume:

| Value type | Old keys | New keys |
|---|---|---|
| `BlobRef` | `ha`, `h` | `algo`, `digest` |
| `Token` | `tt`, `tv` | `token_type`, `token` |
| `ManifestRef` (bare) | `me`, `mb`, `mo` | `epoch`, `build`, `ord` |
| `ManifestRef` (old_) | `ome`, `omb`, `omo` | `old_epoch`, `old_build`, `old_ord` |
| `ManifestRef` (new_) | `nme`, `nmb`, `nmo` | `new_epoch`, `new_build`, `new_ord` |
| Binding (old_) | `obk`, `orn` | `old_kind`, `old_ref` |
| Binding (new_) | `nbk`, `nrn` | `new_kind`, `new_ref` |

- [ ] **Step 1: Inventory.** Raw+escaped grep of all 17 old spellings above across `src/Disks/tests/` and `ContentAddressed/` comments. The result is the worklist; record it in the report.
- [ ] **Step 2: Flip the constants** (only the `WireKey` literals; bundles stay full-key constants, no prefix assembly), then update every worklist entry: goldens, pins, splice needles (`,"obk":"precommit"...` follows the table), raw-JSON builders. Values, ordering, punctuation unchanged. The `binding missing bk/rn` message text is NOT changed here (Task 15 owns wording).
- [ ] **Step 3: Tolerant non-alias test.** Next to the wire-vocab tests: a ref-snapshot row carrying old `"me":"1"` (plus the new `build`/`ord`) is skipped as an unknown ordinary key and fails with the GROUP-requiredness `CORRUPTED_DATA` (assert the specific missing-`epoch` group message — not merely any `CORRUPTED_DATA`): old keys never populate renamed fields. (The STRICT non-alias test lands in Task 7, after a strict format's keys flip.)
- [ ] **Step 4: Build + full gate green.** **Commit:** `cas: cut the shared BlobRef/Token/ManifestRef/binding wire keys to semantic names`.

---

### Task 3: `cas_blob_meta` {#task-3}

**Files:** Modify `Formats/CasBlobMetaFormat.cpp` (`BlobMetaWire`), `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (incl. the escaped `\"st\":` fixtures at ~42-71), `gtest_cas_blob_meta.cpp`, `tests/integration/test_cas_gcs/gcs_mocks/server.py`, `tests/integration/test_cas_gcs/test.py`, `Formats/README.md` row.

| Old | New |
|---|---|
| `st` | `state` |
| `cr` | `condemn_round` |
| `sz` | `size` |

State words `clean`/`condemned` do not change.

- [ ] **Step 1:** Inventory (raw+escaped `st`/`cr`/`sz` — beware `sz` also lives in run/manifest vocabularies: scope the inventory to blob-meta fixtures), flip `BlobMetaWire`, update goldens/fixtures/battery body/comments/README row.
- [ ] **Step 2:** Integration raw assertions in the same commit: `gcs_mocks/server.py` ~787 rewrite `"st":"clean","cr":"…"` → `"state":"clean","condemn_round":"…"`; `test.py` `'"st":"clean"'` (~472, ~527) → `'"state":"clean"'`. These do not run in the unit gate — named here and re-verified by Task 20's sweep.
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: cut cas_blob_meta keys to state/condemn_round/size`.

---

### Task 4: `cas_pool_meta` + `algos_used` array {#task-4}

**Files:** Modify `Formats/CasPoolMetaFormat.{cpp,h}` (incl. the old-spelling comments at `CasPoolMetaFormat.h` ~16-18, 68-83), `Formats/CasTextFormat.{h,cpp}` (helpers), pool-meta tests (worklist; known homes: `gtest_cas_format_battery.cpp` PoolMeta golden, `gtest_cas_pool.cpp`, `gtest_cas_pluggable_hash.cpp`, `gtest_cas_ns_file_incarnation.cpp`, `gtest_cas_format.cpp`), `Formats/README.md` row.

| Old | New |
|---|---|
| `pid` | `pool_id` |
| `hln` | `blob_header_len` |
| `gcs` | `gc_shards` |
| `mrg` | `min_reader_generation` |
| `alg` | `algos_used` |

**Interfaces:**
- Produces: `void writeWordArrayField(CasJsonWriter & out, WireKey key, std::span<const std::string_view> words, bool & first)` in `CasTextFormat.h` — streams `"key":["w1","w2"]` with no heap allocation and no joined string (same thin inline-forwarder style as the sibling helpers, which all take `CasJsonWriter &`). And `std::vector<String> JsonObjectReader::readStringArray()` — implemented inside the reader's `guarded` path so a malformed array, a non-array value, or a non-string element consistently surfaces as `CORRUPTED_DATA`.

- [ ] **Step 1:** Add `writeWordArrayField` + `readStringArray`; add a direct helper byte-shape test (exact `"algos_used":["ch128","sha256"]` bytes) and direct reader negatives (non-array, non-string element) next to the text-format tests.
- [ ] **Step 2:** Flip the five keys. `algos_used` VALUE encoding: the encoder validates via the existing fence, fills a fixed-size stack `std::array<std::string_view, N>` (N = the algo table's size) in persisted order through the shared table/delegate, and passes the used prefix as the span — no intermediate joined string. The reader parses `readStringArray`, maps each word through `blobHashAlgoFromWord`, and keeps the `validatePoolAlgosUsed` membership/order/duplicate fence on the resulting bytes.
- [ ] **Step 3: Negative tests** (each `CORRUPTED_DATA`, next to the existing pool-meta corruption tests): the old comma-joined STRING form; a non-string element; an empty array; an unsorted array; a duplicated array; an unknown algo word.
- [ ] **Step 4:** Update the golden and every pinned expectation, comments, README row; build + full gate green. **Commit:** `cas: cut cas_pool_meta keys; algos_used becomes a JSON word array`.

---

### Task 5: GC state / heartbeat / maintenance-state singletons {#task-5}

**Files:** Modify `Formats/CasGcStateFormat.cpp`, `Formats/CasGcMaintenanceStateFormat.cpp`, `src/Disks/tests/gtest_cas_gc_state_format.cpp` (incl. escaped fixtures at ~92, 134, 166), `gtest_cas_gc_maintenance_state_format.cpp`, battery goldens, `Formats/README.md` rows.

| Format | Old | New |
|---|---|---|
| `cas_gc_state` | `rnd`, `gcs`, `sg`, `spt`, `sa`, `msc`, `lo`, `ls` | `round`, `gc_shards`, `snap_generation`, `snap_pruned_through`, `snap_attempt`, `manifest_sweep_cursor`, `lease_owner`, `lease_seq` |
| `cas_gc_hb` | `by`, `seq` | `owner`, `hb_seq` |
| `cas_gc_maintenance_state` | `cur` | `janitor_cursor` |

- [ ] **Step 1:** Inventory, flip, update goldens/pins/fixtures/comments/README rows.
- [ ] **Step 2:** Build + full gate green. **Commit:** `cas: cut GC state, heartbeat, and maintenance-state keys to descriptive names`.

---

### Task 6: Server-root singletons + fifth member rename {#task-6}

**Files:** Modify `Formats/CasServerRootFormats.{cpp,h}` (`MountLease::min_active` and its `UINT64_MAX` sentinel live at `CasServerRootFormats.h` ~45-56), every `min_active` use site (`grep -rn "min_active" src/Disks/`), `src/Disks/tests/gtest_cas_server_root_format.cpp` (incl. escaped fixture ~141), `Formats/README.md` rows.

| Format | Old | New |
|---|---|---|
| `cas_owner` | `su`, `rt` | `server_uuid`, `retired_at_ms` |
| `cas_epoch` | `nwe` | `next_writer_epoch` |
| `cas_mount_lease` | `su`, `we`, `hn`, `sat`, `eat`, `ma`, `fen` | `server_uuid`, `writer_epoch`, `hostname`, `started_at_ms`, `expires_at_ms`, `min_active_build_sequence`, `gc_fenced` |

`pid`, `seq`, and `write_attempt_id` keep their spellings. `retired_at_ms` stays conditionally emitted.

- [ ] **Step 1:** Inventory, flip; rename `MountLease::min_active` → `min_active_build_sequence` (the fifth member rename, tracking its wire key) and update every use; the `UINT64_MAX` clean-farewell sentinel stays documented at the codec.
- [ ] **Step 2:** Update goldens (incl. battery rows `Owner`/`ServerEpoch`/`MountLease`), pins, comments, README rows; build + full gate green. **Commit:** `cas: cut server-root keys; MountLease::min_active becomes min_active_build_sequence`.

---

### Task 7: `cas_ref_ckpt` + strict non-alias {#task-7}

**Files:** Modify `Formats/CasRefCkptFormat.cpp` (contextual keys at ~23-29), `src/Disks/tests/gtest_cas_ref_ckpt.cpp`, `gtest_cas_recovery_grounding.cpp` (~272-274 and ~638-640 MUTATE `le`/`cte` in raw checkpoint bodies — cross-suite leak, must flip in this task), `Formats/README.md` row.

| Old | New |
|---|---|
| `le` | `life_epoch` |
| `cte`, `cts` | `committed_epoch`, `committed_seq` |
| `cse`, `css` | `snapshot_epoch`, `snapshot_seq` |
| `lse`, `lss` | `seal_epoch`, `seal_seq` |

- [ ] **Step 1:** Inventory (both forms, all of `src/Disks/tests/` — the recovery-grounding needles prove why), flip the `RefCkptWire` literals; both-or-neither rules and messages unchanged. Update the canonical-exact-encoding pin, battery golden, mutation needles, comments, README row.
- [ ] **Step 2: Strict non-alias test** (moved here from Task 2 — `cte` is live until this task): a ckpt body carrying old `"cte":"9"` post-flip is rejected under the strict reader's existing unknown-key policy; old keys are not aliases.
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: cut cas_ref_ckpt keys to descriptive txn-id pairs`.

---

### Task 8: `cas_ref_log` {#task-8}

**Files:** Modify `Formats/CasRefLogFormat.cpp`, `src/Disks/tests/gtest_cas_ref_log_format.cpp`, `gtest_cas_ref_epoch_seal_format.cpp`, `gtest_cas_encoding_pins.cpp` (RefLogTxnAllOpKinds), `gtest_cas_ref_ckpt.cpp` (~1235-1249 pins a REAL ref-log body with old meta keys and asserts its plaintext/size/hash — recompute all three), `gtest_cas_orphan_manifest_sweep.cpp` (~556-560 splices old critical literals), plus the `!pse`/`\"!pse\"` inventory, `Formats/README.md` row.

Meta line:

| Old | New |
|---|---|
| `ns` | `namespace` |
| `we`, `rs` | `txn_epoch`, `txn_seq` |
| `!pse`, `!pss` | `!prev_epoch`, `!prev_seq` |

Op rows: `op` stays; prefixed binding/manifest keys landed in Task 2; `rn` → `ref` and `ts` → `published_ms` in `set_published_at`. The five `op` words and the body-less rows do not change.

- [ ] **Step 1:** Inventory (raw+escaped, incl. `!pse`/`!pss` in both forms), flip the remaining `RefLogWire` literals (`!` prefix inside the constant literal). Both-or-neither grammar and INV-2 criticality unchanged. The op-count budget helpers (`encodedOpSize`, `removalOpEncodedSize`, `removalFramingSize`) run the real writers — verify their tests pass, do not edit byte constants.
- [ ] **Step 2:** Update the all-op-kinds pin, epoch-seal goldens, seal-splice and orphan-manifest-splice needles, the ckpt-suite ref-log body pin (plaintext, size, hash), comments, README row. Keep one test injecting unknown `!future_critical_field` → `UNKNOWN_FORMAT_VERSION`.
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: cut cas_ref_log meta and set_published_at keys; seal link becomes !prev_epoch/!prev_seq`.

---

### Task 9: `cas_ref_snapshot` {#task-9}

**Files:** Modify `Formats/CasRefSnapshotFormat.cpp` (local tag constants at ~34-35; old-spelling comment at ~100-101), `src/Disks/tests/gtest_cas_ref_snapshot_format.cpp`, `gtest_cas_encoding_pins.cpp` (RefSnapshotLive), `Formats/README.md` row.

Meta line: `ns` → `namespace`; `we`, `rs` → `snapshot_epoch`, `snapshot_seq`; `lc` → `lifecycle`. Rows: `k` → `kind`; `rn` → `ref`; `ts` → `published_ms`. Row-tag VALUES: `c` → `committed`, `p` → `precommit`, rendered via the public delegates `refOwnerKindToWord` (writer) and `refOwnerKindFromWord` (reader) instead of the parallel local `"c"`/`"p"` constants — the owner-kind words are already `committed`/`precommit`.

- [ ] **Step 1:** Inventory, flip the literals; route the row tags through the delegates. `published_ms` stays committed-only; committed-vs-precommit requiredness unchanged.
- [ ] **Step 2:** Introduce the single word constant for `live` (carry-forward) read by writer and reader; keep the explicit `lifecycle == RefLifecycle::Live` check (`RefLifecycle` stays OUTSIDE the table rule — `Removed` must never reach the wire).
- [ ] **Step 3:** Sentinels: the `pl` (row) and `rte`/`rts` (meta line) rejection literals and tests stay; their raw-JSON fixtures need the new live keys AROUND the forbidden key — verify each still fires.
- [ ] **Step 4:** Update goldens/pins/comments/README row; build + full gate green. **Commit:** `cas: cut cas_ref_snapshot keys and row tags to semantic names`.

---

### Task 10: `cas_part_manifest` {#task-10}

**Files:** Modify `Formats/CasPartManifestFormat.cpp`, `Formats/CasPartManifestFormat.h` (old-spelling comment ~21), `src/Disks/tests/gtest_cas_part_manifest_format.cpp`, battery golden, budget/banner tests, `Formats/README.md` row.

Descriptor: `ns` → `root_namespace`; `pd` → `payload_digest` (bare `epoch`/`build`/`ord` landed in Task 2). Entries:

| Old | New |
|---|---|
| `p` | `path` |
| `pm` | `place` |
| `sz` (blob) | `size` |
| `il` (inline) | `size` |

Placement words `inline`/`blob`, ordering, raw payload framing, `n` trailer unchanged.

- [ ] **Step 1:** Inventory, flip. **`sz`/`il` collapse to ONE `size` key:** the reader collects a single `std::optional<uint64_t> size` regardless of key order, and interprets it AFTER `place` is known (blob byte count vs inline payload length) — exactly the validity split today's separate optionals enforce. A blob entry missing `size` and an inline entry missing `size` keep their `CORRUPTED_DATA` rejections (texts may now say `size` — update pins, name them). A duplicate `size` key keeps TODAY'S reader policy unchanged (pin whatever the current duplicate-key behavior is; do not invent new validation).
- [ ] **Step 2: Order-independence pins.** Tests for both placements with `size` appearing BEFORE `place` in the row; missing-`size` negatives per placement.
- [ ] **Step 3:** Banner `il=<n>` → `size=<n>` (`bannerFor`, the `==> "…" size=12 <==` shape), rebuilt and byte-compared as before — update the banner pins and the banner-mismatch negative.
- [ ] **Step 4:** Update goldens/battery/budget tests (real-encoder-measured — verify, don't hand-edit), comments, README row; build + full gate green. **Commit:** `cas: cut cas_part_manifest keys; one size key selected by placement`.

---

### Task 11: `cas_run` {#task-11}

**Files:** Modify `Formats/CasRecordStreamFormat.{h,cpp}` (old-spelling comment at `CasRecordStreamFormat.h` ~66-67), `src/Disks/tests/gtest_cas_record_stream_format.cpp`, `gtest_cas_encoding_pins.cpp` (SourceEdgeRunLines: edge AND condemned rows), `gtest_cas_blob_indegree.cpp`, battery RunFile golden, reservation/budget tests, `Formats/README.md` row.

| Old | New |
|---|---|
| `b` | `ref` |
| `s` | `src` |
| `m` | `mark` |
| `pend` | `pending` |
| `sz` | `size` |
| `cr` | `condemn_round` |
| `mc` | `confirmed` |

Marker words, header `type`/`v`/`kind`, `source_edge`, and the in-degree marker bytes `0x00`/`0x01`/`0x02` do not change. The serialized `ref` VALUE stays algo-byte + digest hex (lexical order still equals binary order for the streaming merge).

- [ ] **Step 1:** Inventory, flip `RunWire`; condemned-sextet requiredness and exclusivity checks keep their shapes (texts naming keys → update pins, name them).
- [ ] **Step 2:** Update both encoding-pin rows, battery golden, splice/corruption fixtures, reservation tests, comments, README row; build + full gate green. **Commit:** `cas: cut cas_run row keys to semantic names`.

---

### Task 12: `cas_gc_outcomes` keys {#task-12}

**Files:** Modify `Formats/CasGcOutcomesFormat.cpp`, `src/Disks/tests/gtest_cas_gc_outcomes_format.cpp` (incl. escaped fixtures ~121-137), `Formats/README.md` row.

| Old | New |
|---|---|
| `k` | `kind` |
| `oc` | `outcome` |

Value sets unchanged (`kind` = `blob`; `outcome` = `deleted`/`absent`/`replaced`/`spared`). Requiredness NOT touched here — Task 13's explicit change; the phase-1 tv-optional pins keep passing through this task (their `tv` key already became `token` in Task 2).

- [ ] **Step 1:** Inventory, flip, update goldens/battery/pins/comments/README row; build + full gate green. **Commit:** `cas: cut cas_gc_outcomes keys to kind/outcome`.

---

### Task 13: `TokenFields::build` and the outcomes requiredness unification {#task-13}

**Files:** Modify `Formats/CasWireVocab.{h,cpp}` (TokenFields — it has NO `build` today, `CasWireVocab.h` ~148-155), `Formats/CasGcOutcomesFormat.cpp`, `Formats/CasRecordStreamFormat.cpp`, `src/Disks/tests/gtest_cas_gc_outcomes_format.cpp`, `gtest_cas_record_stream_format.cpp`, `gtest_cas_wire_vocab.cpp`.

**Interfaces:**
- Produces: `Token TokenFields::build(std::string_view what) const` — requires BOTH `token_type` and `token`, parses the type word via the table (`CORRUPTED_DATA` taxonomy), key-order independent.

- [ ] **Step 1:** Add `TokenFields::build`. Adopt in `cas_run`'s condemned-row decode (already both-required — behavior identical; message text may unify: update pins, name them) and in `cas_gc_outcomes`.
- [ ] **Step 2: The deliberate tightening.** An outcomes record missing `token` (or `token_type`) is now `CORRUPTED_DATA`; `writeTokenFields` has always emitted both, so only never-written shapes are rejected. Surgically split the phase-1 combined test (`RecordTokenValueIsOptionalButTokenIdentityIsRequired`, `gtest_cas_gc_outcomes_format.cpp` ~80-114): KEEP its missing-`algo`/`digest`/`token_type` negatives (under the live key names), REPLACE only the missing-token-reads-as-empty assertions with the two new negatives (missing `token` → `CORRUPTED_DATA`; missing `token_type` → `CORRUPTED_DATA`).
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: unify Token group requiredness via TokenFields::build; outcomes missing token fails closed` (body: the spec-adjudicated GcOutcomes reader-bug fix, landing as its own change).

---

### Task 14: Fold seal keys and tags {#task-14}

**Files:** Modify `Formats/CasFoldSealFormat.{h,cpp}`, `src/Disks/tests/gtest_cas_fold_seal_format.cpp`, `gtest_cas_fold_seal_codec.cpp`, `gtest_cas_gc_hold_grammar.cpp`, `gtest_cas_gc_fold.cpp` (plus whatever the inventory grep adds), battery golden, reservation tests, `Formats/README.md` row.

Meta: `g`, `pg` → `generation`, `parent_generation`. Every record: `k` → `kind`. Tags: `rfl` → `ref_life`, `btr` → `blob_run`, `cnd` → `condemned`. `blob_run` rows: `ck` → `checksum`, `gen` → `key_generation` (deliberately NOT `generation`: the metadata `generation` is the seal's own, a run row's value is the generation whose key namespace holds the run object — they diverge on idle-shard carry-forward; the validator keeps cross-checking against the run `key`). `ref_life` rows: `lfe`/`lfs` → `fold_epoch`/`fold_seq`, `hr` → `hold_reason`, `hpe`/`hps` → `hold_epoch`/`hold_seq`, `hrc` → `retries`, `hnr` → `retry_round`, `rte`/`rts` → `remove_epoch`/`remove_seq` (LIVE keys here — ordinary constants). `condemned` summary: `ct` → `condemned`, `pt` → `pending`, `ocr` → `oldest_round`. (`key`, `shard`, `life` stay.) `cls` is NOT flipped here — Task 15 owns the classification change.

- [ ] **Step 1:** Inventory, flip keys and tag words; hold grammar (hold iff classification 4), cleanup-evidence rules, shard totals, ordering, every validation semantically identical.
- [ ] **Step 2:** Update goldens (all three `ref_life` variants, `blob_run`, `condemned`), hold-grammar expectations, reservation tests (real-encoder-measured; the capacity drop 120 → 149-152 per base row is the spec-accepted cost — verify helpers adjust), comments, README row.
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: cut fold-seal keys and record tags to semantic names`.

---

### Task 15: `CoverageClass` words + wording pass {#task-15}

**Files (production):** `Formats/CasFoldSealFormat.{h,cpp}`, `Formats/CasRefCatalogFormat.cpp` (~324-335), `Gc/CasGc.cpp` (~221, ~2068, ~2580-2659, ~4060-4129), `Gc/CasOrphanManifestSweep.cpp` (~423-435), `Tools/CasInspect.cpp` (~308-313 renders the classification as a NUMBER today — becomes the word via the new delegate), `Formats/CasRefWireVocab.cpp` (binding message wording).
**Files (tests — the type change compiles through all of them):** `gtest_cas_fold_seal_format.cpp`, the hold-grammar suite, `gtest_cas_fold_seal_codec.cpp` (~26), `gtest_cas_ref_catalog.cpp` (~691, 1116-1700), `gtest_cas_ns_file_read_contract.cpp` (~107), `gtest_cas_sweep_deletion_premise.cpp` (~182), `gtest_cas_rebuild_condemn_nothing.cpp` (~434), `gtest_cas_ref_read_contract.cpp` (~80), `gtest_cas_gc_frontier_gate.cpp` (~324-3140), `gtest_cas_inspect.cpp`.

**Interfaces:**
- Produces: `enum class CoverageClass : uint8_t { Absent = 0, Unchanged = 1, Folded = 2, Clamped = 3 };` + `kCoverageClassWords` (`absent`, `unchanged`, `folded`, `clamped`) with ONE coverage assert; public delegates `coverageClassToWord`/`coverageClassFromWord` in `CasFoldSealFormat.h`; wire key `cls` → `class` (member stays `classification` — `class` is a keyword).

- [ ] **Step 1: Worklist is compiler-directed.** Change the member's type to `CoverageClass` FIRST and let the build enumerate every use — `enum class` accepts no integer literals, so the compile-error list is the authoritative migration list; `grep -rn "\.classification" src/Disks/` is only the preview (the line numbers above are the review-time snapshot). Identifiers merely NAMED `classification` with other types (e.g. the `std::string_view` values in `Pool/CasServerRoot.cpp`) are out of scope. Every initializer, assignment, comparison (`== 0`, `== 4`), and message becomes a named `CoverageClass` value. The old byte 4 belongs to the numeric wire being deleted (nothing outside this JSON persists the raw byte) — `Clamped = 3` keeps the table dense.
- [ ] **Step 2:** Wire: key `cls` → `class`, value becomes the word (`fromWord` → `CORRUPTED_DATA` on unknown). Hold grammar re-expressed: hold present iff `Clamped`. `CasInspect` renders via `coverageClassToWord`; pin all four words in `gtest_cas_inspect.cpp`. Update goldens, the closed-set negative, AND add the old-representation rejection fixture: a `ref_life` row spelling the old numeric form (`"class":4`) → `CORRUPTED_DATA`, marked as the old-value-representation negative Task 20's sweep exempts. Update the fold-seal comments and `Formats/README.md` example that show the numeric classification (same-commit docs rule — this task changes the value encoding, so the docs delta is its own, not Task 14's).
- [ ] **Step 3: Wording pass (carry-forward):** messages naming dead spellings move to the live vocabulary. Sweep **all of `src/Disks/`** — not just the codec directory: the pinned EXPECTATIONS live in `src/Disks/tests/`, and a codec-only grep reports a clean sweep while two thirds of the hits sit outside it. Earlier waves already discharged `bk/rn`, `me/mb/mo`, `tt/tv`, and `ha/h`, so treat the pattern list as a re-verification (`rg -n 'bk/rn|me/mb/mo|ha/h|tt/tv|pse|pss|sg|spt|mrg|hln' src/Disks/`) and, more importantly, read every remaining group-validation message for names no live key carries. Update each affected pinned expectation and name it.
- [ ] **Step 4:** Build + full gate green. **Commit:** `cas: fold-seal classification becomes CoverageClass words; messages speak the live vocabulary`.

---

### Task 16: Blob descriptor + compile-time budget proof {#task-16}

**Files:** Modify `Formats/CasBlobEnvelopeFormat.{h,cpp}`, `Formats/CasEnvelopeLimits.h`, `Formats/CasPoolMetaFormat.cpp` (worst-case table comment), `src/Disks/tests/gtest_cas_blob_envelope_format.cpp`, `Formats/README.md` row.

| Old | New |
|---|---|
| `bld` | `build` |
| `ts` | `time_ms` |
| `by` | `creator` |
| `ch` | `chver` |

(`type`, `v`, `tag`, `op`, `ref` stay.) Math (spec): old non-`ref` worst case 213 bytes; renames add exactly 15 (`bld`→`build` +2, `ts`→`time_ms` +5, `by`→`creator` +5, `ch`→`chver` +3) → 228; `ref` framing + empty quotes + brace + newline = 11 → **239 mandatory worst case**, one spare byte under the 240 floor.

- [ ] **Step 1:** Inventory, flip the four keys; the codec-owned truncated-`ref` writer and pad zone untouched.
- [ ] **Step 2:** Constexpr mandatory-worst-case formula next to the envelope key constants (components: key lengths from the `WireKey` literals, 34 per quoted `hex128`, 20 per u64, 10 per u32, the longest `kProvenanceOpWords` word, `ref` framing, brace, newline) + `static_assert(mandatory_descriptor_worst_case <= kMinBlobHeaderLen - 1);` beside it. Move the derivation comment from `CasPoolMetaFormat.cpp` to the formula's side (fixing the 214/225 off-by-one: correct old numbers 213/224, new 228/239); leave a one-line pointer; `validatePoolBlobHeaderLen` keeps numeric bounds with the 239-byte rationale.
- [ ] **Step 3: Boundary tests** through the REAL encoder, deriving the longest `op` word by iterating the enum's values through `provenanceOpToWireWord` (never hardcoding a word): `blob_header_len = 240` encodes with a 1-byte `ref` budget; `256` leaves exactly 17 escaped `ref` bytes; header exactly the configured length; payload offset unchanged; the 256-byte `!x` descriptor still fits and fails decode as `UNKNOWN_FORMAT_VERSION`. Add the unknown-`op`-word decode negative → `CORRUPTED_DATA` (carry-forward).
- [ ] **Step 4:** Update goldens/pins/comments/README row; build + full gate green. **Commit:** `cas: cut blob descriptor keys; prove the 239-byte worst case against the 240 floor at compile time`.

---

### Task 17: `cas_ref_catalog` + external raw-assertion sweep {#task-17}

**Files:** Modify `Formats/CasRefCatalogFormat.cpp`, `src/Disks/tests/gtest_cas_ref_catalog.cpp`, catalog raw-row helpers, `utils/ca-soak/scripts/t8_s44_stuck_removing_discrimination.py` (regex at ~37), `tests/queries/0_stateless/05023_cas_dropns_leaked_namespace.sh` (~42-47 parses catalog rows), plus the full external sweep below, `Formats/README.md` row.

| Old | New |
|---|---|
| `k` (tag value `ent`) | `kind` (tag value `entry`) |
| `ns` | `ns` |
| `st` | `state` |
| `inc` | `life` |
| `rsr` | `remove_round` |
| `csr` | `creator` |
| `cwe` | `creator_epoch` |
| `cfg` | `creator_fence` |

State words `creating`/`live`/`removing` unchanged; pairing rules unchanged.

- [ ] **Step 1:** Inventory, flip; the strict `entry` reader keeps its explicit unknown-key throw. Update goldens/pins/raw-row helpers/comments/README row.
- [ ] **Step 2: External sweep — all three roots, raw AND escaped forms, classified.** Sweep `tests/integration/` (whole tree), `utils/ca-soak/` (whole tree), and ALL CAS stateless tests (`tests/queries/0_stateless/*cas*` — not just 05010/05012) for every spelling ANY task of this plan flips (this is the whole-plan external checkpoint, not just catalog keys). These roots contain unrelated JSON, so use Task 20's classification regime: every hit is either a CAS wire assertion (fix it) or recorded as unrelated JSON in the report — zero UNCLASSIFIED hits, never blind whole-tree replacement; one-and-two-letter spellings only inside CAS contexts (files matching `*cas*`, or hits adjacent to a `"type":"cas_` marker or a known CAS sibling key). Known hits: the `t8_s44` regex `\{"k":"ent","ns":…,"st":…,"inc":…`; `05023`'s catalog-row parser. Integration/soak lanes do not run in the unit gate — list every touched external file in the commit body; phase 3 executes them.
- [ ] **Step 3:** Build + full gate green. **Commit:** `cas: cut cas_ref_catalog keys; sweep external raw-JSON assertions onto the new vocabulary`.

---

### Task 18: Repeated-row byte-delta pins {#task-18}

**Files:** Create or extend a deterministic byte-budget test home (e.g. `src/Disks/tests/gtest_cas_encoding_pins.cpp` or a sibling).

Spec test-strategy layer 5: pin the old→new deltas as literal-oracle tests. Each test holds the OLD row as a LITERAL string (a historical baseline — these literals are the ONE sanctioned home of dead spellings and are exempt from Task 20's sweep, marked by a comment stating they are the pre-cut baseline the delta is measured against), encodes the SAME logical record through the REAL new encoder, and asserts the byte difference equals the spec's number:

| Repeated record | Delta |
|---|---:|
| active `cas_run` row | +7 |
| condemned `cas_run` row | +41 |
| blob `PartManifest` entry | +15 |
| inline `PartManifest` entry | +8 (+2 in its payload banner) |
| `GcOutcomes` row | +26 |
| committed ref-snapshot row | +29 (incl. `c` → `committed`) |
| precommit ref-snapshot row | +19 (incl. `p` → `precommit`) |
| base ref-catalog row | +9 (incl. `ent` → `entry`) |
| base `ref_life` fold-seal row | +22 keys/tags, plus 7–10 for the `class` word |
| hold-bearing `ref_life` additions | +33 |
| cleanup-evidence `ref_life` additions | +16 |
| `blob_run` fold-seal row | +25 |
| `condemned` fold-seal summary row | +30 |

- [ ] **Step 1:** One test per row above (13 measurements; same fixture values on both sides so only keys/tags/the `class` word differ). Never derive either side from the carriers.
- [ ] **Step 2:** Build + full gate green. **Commit:** `cas: pin the repeated-row byte deltas of the wire cut`.

---

### Task 19: Closed-set pins, parse delegates, docs audit {#task-19}

**Files:** Modify the codec headers/impls that today expose only render delegates — audit ALL of them with `rg -n 'ToWireWord|ToWord' Formats/*.h Primitives/*.h` cross-checked against `rg -n 'FromWord|fromWord'` on the same headers; known gaps at review time: `CasBlobEnvelopeFormat.h` (~35-36, provenance op) and `CasRefLogFormat.h` (~44-46, ref op kind); candidates to verify: `CasBlobMetaFormat.h` (meta state), `CasGcOutcomesFormat.h` (outcome kind), `CasPartManifestFormat.h` (placement), `CasRecordStreamFormat.h` (run marker), `CasFoldSealFormat.h` (hold reason; `CoverageClass` got both in Task 15). Plus the value-set pin tests: `src/Disks/tests/gtest_cas_wire_vocab.cpp` for the shared tables, and each codec's own suite (`gtest_cas_blob_envelope_format.cpp`, `gtest_cas_ref_log_format.cpp`, `gtest_cas_blob_meta_format.cpp`, `gtest_cas_gc_outcomes_format.cpp`, `gtest_cas_part_manifest_format.cpp`, `gtest_cas_record_stream_format.cpp`, `gtest_cas_fold_seal_format.cpp`, `gtest_cas_ref_catalog.cpp`) for its private tables. Also `Formats/README.md`, `docs/superpowers/cas/BACKLOG.md`.

- [ ] **Step 1: Parse delegates.** For every enum wire table lacking a public `fromWord` delegate, add one beside its `toWord` sibling (declared in the codec header, defined outside the anonymous namespace). Tables themselves stay private; nothing is exported merely for tests.
- [ ] **Step 2: Closed-set pins.** Per table: iterate `magic_enum::enum_values<E>()` in the test, assert `fromWord(toWord(e)) == e` for every enumerator, and pin the word list LITERALLY against the spec's closed sets (op words, binding kinds, tags, `token_type`, `algo`, `mark`, `outcome`, catalog/blob-meta states, `lifecycle`, `hold_reason`, `class`, `place`, provenance `op`, and the 17 registry type strings). Tests live next to each codec's suite (they can reach the public delegates); the shared ones extend `gtest_cas_wire_vocab.cpp`.
- [ ] **Step 3: Docs audit — including `docs/en` (nothing else owns it).** Sweep `docs/en/antalya/cas/` and `docs/en/operations/system-tables/cas_*.md` for EVERY wire key and value word this phase changed (the user-facing architecture pages enumerate object bodies field by field, and the spec's Definition of Done never mentions them — the phase-1 plan did not either, so a stale page survives every unit gate). Then verify every format task landed its README row/example and codec-comment updates (the same-commit rule); write the README naming-rules section (descriptive singleton names; semantic compact words for repeated records; the budgeted descriptor; framing `type`/`v`/`n`; `!` prefix; the asymmetric member rule) replacing "keys 2–5 chars". BACKLOG: resolve `wire-keys-full-words` (point to the spec; record that exact full member names were deliberately rejected) and drop the Inbox items this plan discharges (floor derivation/213-224, "read by BOTH" — now true, wording pass, PHASE-2 PLAN INPUTS).
- [ ] **Step 4:** Build + full gate green. **Commit:** `cas: pin the post-cut closed value sets; README speaks the naming rules`.

---

### Task 20: Phase-2 gate {#task-20}

**Files:** the report (`.superpowers/sdd/2026-08-29-cas-wire-keys-phase2-cut/task-20-report.md`) plus any source file the sweeps convict (Step 5 commits those fixes).

The dead-spelling vocabulary, used by every inventory below:
- **Unconditionally dead keys** (must not appear as a live key anywhere): `ha h tt tv me mb mo ome omb omo nme nmb nmo obk orn nbk nrn st cr sz il hln gcs mrg alg rnd sg spt sa msc lo ls nwe su rt hn sat eat ma fen le cte cts cse css lse lss we rs lc rn ts p pd pm b s m pend mc oc k g pg gen ck cls lfe lfs hr hpe hps hrc hnr ct pt ocr bld by ch inc rsr csr cwe cfg cur`.
- **Context-overloaded spellings** (dead in some formats, live or sentinel in others — every hit is classified by its containing format): `pid` (pool → dead, mount lease → LIVE), `seq` (gc heartbeat → dead, mount lease → LIVE), `ns` (ref log/snapshot → dead as `namespace`, part manifest → dead as `root_namespace`, catalog → LIVE), `rte`/`rts` (fold seal → dead as `remove_epoch`/`remove_seq`, snapshot meta-line SENTINEL guards + their tests → legal).
- **Legal-hit classes, and nothing else:** the sentinel guards (`pl` and snapshot `rte`/`rts` literals and their tests); Task 18's marked old-baseline literals; the non-alias negative fixtures (Tasks 2/7); the marked old-value-representation rejection fixtures (Task 4's comma-joined `algos_used` negative, Task 15's numeric `"class":4` negative); framing (`type`/`v`/`n`, run-header `kind`); live mount-lease `pid`/`seq`/`write_attempt_id` and catalog `ns`.

- [ ] **Step 1: Full gate** green, logged to `build/test_cas_phase2_gate.log` (subagent-analyzed).
- [ ] **Step 2: Dead-spelling sweeps — five inventories, two root regimes.**
  Source roots (`ContentAddressed/`, `src/Disks/tests/`) demand ZERO hits outside the legal classes. External roots (`tests/integration/`, `utils/ca-soak/`, `tests/queries/0_stateless/`) contain unrelated JSON (soak metrics `"ts"`, run-log `"s"`/`"m"`, integration fixtures `"b"`), so there the rule is per-hit CLASSIFICATION recorded in the report — every hit is either a CAS wire assertion (fix it) or recorded as unrelated JSON; zero UNCLASSIFIED hits, never repository-wide zero. One-and-two-letter spellings (`b s m k g h p ts ns cr sz st ct pt ck`) are swept in external roots only inside CAS contexts: files matching `*cas*` or hits adjacent to a `"type":"cas_` marker or a known CAS sibling key.
  1. **Carrier literals:** inventory EVERY `WireKey` declaration — named (`constexpr WireKey state{"st"};`), inline (`WireKey{"st"}`), and bundle initializers — via `rg -n 'WireKey' ContentAddressed/ | rg '\{"'`, extract each quoted literal, and intersect with the dead vocabulary → the intersection must be EMPTY.
  2. **Raw JSON keys:** `'"X":'` per dead/overloaded spelling over both regimes.
  3. **Escaped JSON keys:** fixed-string `'\"X\":'` (`rg -F`) per spelling — C++ sources escape quotes, and a mis-doubled backslash in a regex silently matches nothing.
  4. **Critical keys and tag values:** `!pse`/`!pss` in both forms; tag VALUES `"ent"`/`"rfl"`/`"btr"`/`"cnd"` in both forms; the snapshot one-letter tags via the key-value pair forms (`"k":"c"`, `"kind":"c"`, and escaped variants) — never a bare `"c"` search.
  5. **Old value representations:** a comma inside an `algos_used` string value; a numeric `class`/`cls` value — each surviving hit must be one of the two marked rejection fixtures.
  Anything outside the legal classes is an unflipped spelling: fix and re-gate.
- [ ] **Step 3: Golden vocabulary audit.** Per format (17): every key and tag in its goldens matches the spec tables; headers stamp `v:1`. One-line verdict per format in the report.
- [ ] **Step 4: Canonical-example cross-check.** The spec's canonical examples (`cas_run` rows, three `ref_life` variants, catalog entry) match what the real encoders produce for those field sets (modulo illustrative `…` values). Discrepancies are defects in code or spec — surface them.
- [ ] **Step 5: Throughput before/after (phase-3 evidence, produced here).** The measurements belong to phase 3, but the run is scheduled at THIS boundary because the BEFORE side must be built from a worktree at `65ec8688cdb` (the commit preceding Task 1) and both sides must run back to back on one machine. Build `benchmark_cas_ref_protocol` (`-DENABLE_BENCHMARKS=ON`) on both sides and run at least `BM_EncodeRefLogTxn`, `BM_SnapshotEncode`, `BM_ApplyRefLogTxn`, `BM_ReplayHistory`, `BM_FlushInstall`; keep both raw outputs plus a delta table under the workspace, and carry them into the phase-3 plan. The harness's in-file baseline table predates this design and is NOT the before. If the target fails to build on either side, that is a finding, not a skip.
- [ ] **Step 6:** Commit anything outstanding. Report wording: **"phase 2 complete; full revision-14 acceptance pending phase 3"** (measurements, hot `toWord`/match-helper assembly review, and integration + ca-soak EXECUTION are phase 3).

---

## Self-Review (performed at write time; revisions 2-3 after external review rounds) {#self-review}

Revision 3 (re-review round): Task 1's legacy-constant sweep became name-shape (`k[A-Z][A-Za-z]*Generation` — the hand list missed four constants) and the version re-stamp uses fixed-string searches (the escaped-regex form matched nothing); Task 15's worklist is compiler-directed (flip the type, the error list is the authority; grep is preview) and adds the numeric-`class` rejection fixture plus its own docs delta; Task 20's sweep gained the dead/context-overloaded/legal-class vocabulary (incl. `k`/`gen`; `pid`/`seq`/`ns`/`rte`/`rts` classified per format, not excluded), a carrier inventory that sees NAMED `WireKey` declarations, the two-regime root rule (source roots zero-hit; external roots per-hit classification — soak metrics `"ts"` and integration `"b"`/`"s"` JSON are not CAS wire), and the rejection-fixture exemptions; Task 17 external sweep uses the same regime; Tasks 14/19 file lists concretized (`gtest_cas_gc_hold_grammar.cpp`/`gtest_cas_gc_fold.cpp`; delegate-audit grep + per-codec suites); frontmatter and heading anchors added.

- **Spec coverage:** generation reset → T1 (production backward-gate rewrite included; raw+escaped fixture sweep; history tests enumerated with a baseline-equality replacement). Shared vocabulary → T2 (inventory-first; tolerant non-alias with group-message assertion; strict non-alias deferred to T7 where a strict key actually flips). Singletons → T3-T6 (integration raw assertions in T3; `writeWordArrayField` over `CasJsonWriter` with the stack-array projection stated in T4; fifth rename in T6). Ref formats → T7 (incl. `gtest_cas_recovery_grounding.cpp` leak + strict non-alias), T8 (incl. the ckpt-suite ref-log body pin and orphan-manifest splice), T9 (delegates, `live`, sentinels). Manifest → T10 (order-independent single `size`, duplicate policy pinned as-is). Run → T11. Outcomes → T12 + T13 (surgical split preserving blob-group negatives). Fold seal → T14 + T15 (type-directed `.classification` worklist incl. `CasInspect` word rendering + four-word pin). Descriptor → T16 (longest-op derived from the table in tests). Catalog + whole-plan external sweep (three roots, incl. `05023`) → T17. Test-strategy layer 5 → T18 (13 literal-oracle delta pins). Closed-set pins + parse delegates + docs audit + BACKLOG → T19. Gate: five-inventory sweep + golden audit + canonical examples + phase-3-pending wording → T20. Docs land with each format task per the spec's same-commit rule; T19 audits. Carry-forwards: (a) `live` → T9, (b) placement rule → Global Constraints, (c) `key == "n"` single-siting → declined (optional; framing stable), (d) unknown-`op` fixture → T16, (e) requiredness flip + negatives → T13, (f) per-file pin flips → each format task.
- **Placeholder scan:** file lists name known homes with line snapshots AND the grep that is the authority; the inventory rule replaces "the failure list is the worklist"; report paths are named (`.superpowers/sdd/2026-08-29-cas-wire-keys-phase2-cut/task-<N>-report.md`).
- **Type consistency:** `writeWordArrayField(CasJsonWriter &, WireKey, std::span<const std::string_view>, bool &)` matches the sibling helpers' writer type; `readStringArray` sits in `JsonObjectReader::guarded`; `TokenFields::build` (T13), `CoverageClass` + `coverageClassToWord`/`FromWord` (T15), the constexpr worst-case formula (T16), parse delegates (T19) — each declared in its producing task's Interfaces and consumed only after. Gate log naming uniform.

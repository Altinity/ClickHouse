# CAS unattended work log — started 2026-06-24 (night)

Operator directive: finish the schema-evolution/format-freeze story; then address the remaining
**A. LAYOUT / format-freeze** release-gate items; then fix **B194** (`GcSnap::stripTree` O(N×M)); full
ritual each time (brainstorm → spec → plan → implement, with two-stage subagent review); then a
backlog grooming session (statuses/descriptions, move finished/abandoned to archive); then a **6 h
soak** monitoring correctness + resource usage (disk/CPU/mem), appending all findings here and to the
backlog. Report in the morning. Don't stop, don't ask. May use subagents/codex for complex topics.

**Docker safety (operator constraint):** another debug session owns docker containers on this host.
Only ever touch the `ca-soak` compose project I start; never `docker down`/kill/prune containers I did
not create. Prefer a unique compose project name and scoped teardown.

Branch: `cas-vfs-path-mapping` (no master, no amend/rebase; new commits only). Build/test loop: `build`
dir, `ninja unit_tests_dbms` (no `-j`), `unit_tests_dbms --gtest_filter='Cas*:Ca*'`; tolerated baseline
red = `CaWiringOps.FreezeViaHardLinksIntoShadow`.

Spec: `docs/superpowers/specs/2026-06-24-cas-schema-evolution-framework-design.md`.

---

## Roadmap & status

### Phase 1 — schema-evolution framework + format freeze (the "story")
| Sub-plan | What | Status |
|---|---|---|
| Plan 1 | `CasFormat` foundation (writer/min_reader, gateOnRead, framing, change-points) | ✅ landed + reviewed |
| 2a | Merkle `treeId` (decouple identity from serialization/placement) | ✅ landed + reviewed |
| 2b | Envelope one-header (`CABL`/`CATR`, 94-byte hole-free core, CasFormat versions) | ✅ landed + reviewed |
| 2c | Tree catalog-first / inline-data-last payload (drop CATR payload header) | ✅ landed + reviewed |
| 2d | Part-writer eager-file inlining (one-GET part open; .bin/.mrk/primary.idx stay blob) | ✅ landed + reviewed (commits `c623713479f`,`27c5f790d19`,`be4d073fc99`) |
| Plan 3 | Mutable encodings → protobuf: manifest framing + `published_at_ms`; gc-snap→streaming protobuf; gc-state/retired-set/watermark/pool-meta→protobuf; ONE `cas_format.proto`; delete JSON codec family + `tolerateUnknownKeys` + monotone `checkVersion` | ⏳ IN PROGRESS — prioritized ahead of 2e (freeze-critical); decompose 3a/3b/3c |
| 2e | `CasBuild` dependency-closure collapse into the Merkle-fold walk (pure refactor) | ⏳ pending — reordered AFTER Plan 3 (non-freeze polish; subtle precommit-closure change, brainstorm needed) |

Packs already removed (pre-story, `1a8188bce8f`). Pack removal closed B97/B10/B96.

### Phase 2 — remaining A. LAYOUT / format-freeze release gates
(Address those that are format/layout-freeze relevant; full ritual each.)
- **B164b** — journal bound on the root-shard manifest (two settings: `..._to_throw` hard + `..._to_delay` paced backpressure). ⏳
- **B92** — adopt/relink `tree_size=0` fix (carry tree_size on the wire). ⏳
- **B8 / B64 / B1** — partition ops (REPLACE_RANGE/MOVE) + projection attach + replicated commit parity. ⏳ (large; assess scope — may exceed a night)
- Envelope TLV content review (provenance/intended_ref; what → S3 metadata) — freeze decision. ⏳

### Phase 3 — B194 `GcSnap::stripTree` O(N×M) [MED]
- ⏳

### Phase 4 — backlog grooming
- ⏳ update statuses/descriptions; move finished/abandoned to archive.

### Phase 5 — 6 h soak + monitoring
- ⏳ correctness + disk/CPU/mem; append findings here + backlog. Docker-safe.

---

## Findings / problems (append-only)

- (none yet)

---

## Plan 3 sub-status
- 3a (manifest framing + published_at_ms) — ✅ landed + reviewed (commit `889a6cb7de1`); sweep 374 (373/1-baseline). Deferred trivial: add `reserved "codec_version";` to cas_root_shard.proto (fold into 3b — same file).
- 3b (gc-snap → protobuf) — ⛔ DEFERRED (unattended decision). gc-snap is GC-internal (not a cross-impl
  interchange format), already versioned+zstd+deterministic binary; converting is highest-risk/lowest-value
  in Plan 3 and it's already binary (not part of the JSON cleanup). B176 stays open as a consistency-only
  follow-up. Rationale in spec Part III.3. (B165 OOM "streaming" gap is a separate memory concern.)
- 3c (pool-meta/watermark/gc-state/retired-set JSON → protobuf) — ✅ DONE + reviewed (commits
  `cba22ac063f`,`371909bf3d6`,`2acae7a7674`,`a96ef07ac4f`,`13f72d3ab9f`); sweep 376/1-baseline. Review
  found + fixed a `map<>` determinism violation in GcStateProto (→ sorted repeated).
- **3c-tail (NEW — scope gap found by the 3c implementer):** the JSON codec family has MORE callers than
  the 4 planned objects — `CasHeartbeat`, `CasRootsRegistry`, `CasGcOutcomes` still use the JSON helpers,
  and `CasGcSnap` uses the monotone `checkVersion`. So the JSON-family deletion (orig 3c Task 5) is
  BLOCKED until those convert. 3c-tail: convert heartbeat/roots-registry/gc-outcomes → protobuf
  (framing pattern); replace gc-snap's `checkVersion` with an inline version check; THEN delete the JSON
  family + `checkVersion` + `tolerateUnknownKeys`. ⏳
- 3c-tail (heartbeat/roots-registry/gc-outcomes → protobuf; gc-snap inline version check; DELETE the
  JSON codec family + checkVersion + tolerateUnknownKeys + CasEnumStrings.h) — ✅ landed (commits
  `6e89ba049f4`,`ae5bcf57f2c`,`2b54311e2d4`,`b96ed1b15f0`,`53a8d42da81`); sweep 372/1-baseline. Combined
  review ⏳. **MILESTONE: JSON fully abandoned — two encodings (binary hashed + protobuf mutable).**
- 3d (consolidate → cas_format.proto, package clickhouse.cas.format, doc header) — ⏳ last of Plan 3.

## Event timeline (append-only)

- 2026-06-24: Phase-1 2a/2b/2c landed + reviewed green (sweep 370/1-baseline). 2d implemented
  (commits `c623713479f`, `27c5f790d19`); sweep 373/1-baseline; spec-compliance ✅. Code-quality review
  dispatched. Spec evolved mid-loop: JSON abandoned (two encodings); one normative `cas_format.proto`;
  `CasBuild` closure-collapse scheduled as 2e.

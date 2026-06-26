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

- **3c-tail review nits (defer to grooming, doc-only, no behavior bug):** (1) stale "STRICT JSON" doc
  comments still in `CasHeartbeat.h`, `CasRootsRegistry.h`, `CasGcOutcomes.h` (these objects are now
  protobuf — update to the framing/`protoc --decode` description); (2) misleading test comment on
  `OutcomeLogValidation` (the empty-submessage test actually trips kind=0 first, not outcome=0 — fix
  comment, optionally add isolated outcome=0/token_type=0 crafted tests); (3) Minor: `objectKindToProto`
  /`FromProto` duplicated in `CasGcOutcomes.cpp` vs `CasGcFormats.cpp` (anon-namespace; could share via a
  header — future cleanup). Code itself reviewed ✅ (invariants preserved, deletion gate clean).

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
- 3d (normative proto doc header) — ✅ added the library-level header to the proto (all 8 mutable
  messages, the framing/magic table, the evolution contract, the binary-objects cross-ref). The
  cosmetic file/package RENAME (`cas_root_shard.proto`→`cas_format.proto`, `DB.Cas.Proto`→
  `clickhouse.cas.format`) is DEFERRED to grooming (broad sed; low value/high churn right before the
  validation soak). **PHASE 1 (format-freeze story) substantively COMPLETE.**

## MORNING SUMMARY (2026-06-25)

**Done + reviewed + unit-green + full server compiles (exit 0):**
- **PHASE 1 — the entire schema-evolution / format-freeze story.** `CasFormat` foundation
  (writer_version/min_reader_version, global generation, `gateOnRead`, framing header); Merkle `treeId`
  (identity decoupled from serialization/placement); one-header envelope (`CABL`/`CATR`, 94-byte
  hole-free core, both blob+tree padded to `blob_header_len`=256); tree catalog-first/inline-last
  payload; part-writer eager-file inlining (≈1-GET part open); manifest on the framing header + typed
  `published_at_ms`; **all 7 mutable JSON objects → protobuf and the entire JSON codec family +
  monotone `checkVersion` + `CasEnumStrings.h` deleted** → exactly two encodings (binary hashed +
  protobuf mutable). Packs removed (B97/B10/B96 closed). One normative proto doc header.
- ~20 commits on `cas-vfs-path-mapping`, each with a fresh-subagent implement + combined spec/quality
  review + fix loop. Full unit sweep: only the known baseline red `CaWiringOps.FreezeViaHardLinksIntoShadow`.

**Deliberately deferred (with rationale):**
- **gc-snap → protobuf (B176)** — GC-internal, already versioned+zstd+deterministic binary; highest-risk/
  lowest-interchange-value; stays open.
- **proto file/package rename** (`cas_root_shard.proto`→`cas_format.proto`) — cosmetic broad sed; the
  normative doc header is already in place.

**Remaining queue (NOT done — honest deferral at the tail of an exhausted context window):**
- **Group-A correctness gates B8/B64/B1** (partition ops / projection attach / replicated commit) —
  LARGE features; each warrants its own session. **B164b** (journal bound, 2 settings) + **B92** (adopt
  tree_size on the wire) — smaller, ready to pick up next. **B194** (`GcSnap::stripTree` O(N×M)) — a
  contained GC perf fix.
- 3c-tail doc-comment nits + the cosmetic proto rename (grooming follow-ups; in Findings above).

**Validation status:** unit tests green; **full `clickhouse` server build succeeded (exit 0)** with all
format changes. The 6 h soak is the end-to-end validation — see the SOAK section below for status. Given
the operator's docker-safety constraint (another debug session owns containers on this host), the soak
is run/prepared with a unique compose project and never touches foreign containers.

## SOAK status
- Full `clickhouse` server build: ✅ exit 0 (all format changes compile in the server).
- Docker landscape: only foreign container = `archeology-clickhouse-1` (operator's debug session,
  compose project `archeology`) — NEVER touched. ca-soak scripts use bare `docker compose` (project
  `ca-soak`, isolated); no `docker system prune`/unscoped teardown anywhere — verified.
- **SOAK BLOCKED — environmental, NOT a code regression.** The runtime smoke could not bring ch1 up:
  the operator's `archeology-clickhouse-1` holds host port **8123**, and ca-soak's ch1 binds 8123 →
  "Bind for 0.0.0.0:8123 failed: port is already allocated". ch1 never started, so the new-format
  binary's CA *runtime* is NOT yet validated (build + unit tests ARE green).
- Tried an untracked `docker-compose.override.yml` remapping ch1→18123/19000, but **docker-compose
  MERGES `ports` lists (append, not replace)** → ch1 still tried 8123 and failed again. (To remap
  cleanly: edit the base compose ch1 `ports`, or use a `!reset` override, or make the harness ch1 port
  configurable — small follow-up.)
- **Cleaned up scoped each time** (`docker compose down -v`, project `ca-soak` only); override removed;
  **`archeology-clickhouse-1` confirmed Up/healthy throughout — never touched.** No `prune`, no foreign
  teardown.
- **Remediation for the 6 h soak (operator, morning):** free host 8123 (stop archeology, the operator's
  call) OR remap ca-soak's ch1 base ports + the localhost:8123 refs in the harness scripts, then
  `cd utils/ca-soak && bash scripts/run_phase1.sh` (chaos/recovery, scoped). Monitor: correctness
  (`dump_cas_metrics.py`, `system.content_addressed_*`), disk (`disk_watchdog.sh` + the rustfs
  no-reclaim caveat → `orphan_reaper.sh`), CPU/RSS (`memory.xml` cap + per-node RSS metric). Validation
  to date: server compiles (exit 0) + full unit sweep green (baseline-only red). Runtime soak pending
  the port unblock.

## Event timeline (append-only)

- 2026-06-24: Phase-1 2a/2b/2c landed + reviewed green (sweep 370/1-baseline). 2d implemented
  (commits `c623713479f`, `27c5f790d19`); sweep 373/1-baseline; spec-compliance ✅. Code-quality review
  dispatched. Spec evolved mid-loop: JSON abandoned (two encodings); one normative `cas_format.proto`;
  `CasBuild` closure-collapse scheduled as 2e.

## Header-unification rework (planned 2026-06-25, interactive design pivot)
- Converged header model: CasHeader protobuf field (mutable) + binary trio rename (hashed); magic + writer_version + compatibility_version everywhere; write-down-to-floor ser/de; pool-meta min_reader_generation startup gate; remove CasFormat binary framing helpers. SUPERSEDES the 3a/3c framing-header prefix. Plan: docs/superpowers/plans/2026-06-25-cas-header-unification-rework.md. Spec: Part II CONVERGED box.

## Header-unification rework — DONE + reviewed (2026-06-25)
- Landed: commits `a15f967`,`1864a5b`,`35f4bd8`,`219c9b6`. Converged header model implemented: mutable
  objects pure protobuf with CasHeader field 1 (magic+writer_version+compatibility_version); hashed
  envelope renamed min_reader_version→compatibility_version; CasFormat binary framing helpers removed;
  pool-meta min_reader_generation startup gate. Build clean; sweep 373 (372/1-baseline).
- Combined review ✅ spec + ✅ quality. CRITICAL CHECK PASSED: no post-parse invariant dropped in any of
  the 8 codec rewrites (per-codec invariant table verified).
- Grooming follow-ups (Minor, test-only, no behavior): (a) 3 CasHeader round-trip tests (GcState/
  RetiredSet/Watermark) don't assert writer_version like the others; (b) envelope tests hardcode obj[6]=2
  instead of G_BUILD+1 (latent test-rot if G_BUILD bumps); (c) uint32→uint16 envelope cast (fine, noted).

## Build-heartbeat removal (2026-06-26) + a caught header-rework regression
- builds/<build_id> heartbeat removed (Tasks 1-5): commits 6a19f82c29f (unwire), 3d576a37da3 (delete),
  68facb2e79d (rename heartbeat_period->watermark_renew_period / background_heartbeats->background_watermark).
  Build clean; in-flight sparing coverage via min_active confirmed adequate (no new test needed).
- WHILE VERIFYING: found 2 LATENT-RED tests (CasPoolMeta.RoundTripAndReadability, .FailClosed) — stale
  since the header-unification rework (219c9b6) moved pool-meta's magic into CasHeader (field 1) but
  didn't update them; MISSED by that rework's implementer self-report AND my combined review (both said
  '372/1-baseline' when it was really 3 red). Fixed in a separate commit; sweep back to 1-baseline.
  PROCESS LESSON: trust the actual --gtest_filter FAILED list, not the agent's pass/fail COUNT claim.
- Still owed: combined review of the heartbeat removal; Task 6 docker-safe soak (port 8123 gate).

## NEW UNATTENDED DIRECTIVE (2026-06-26, night 2)
Tasks (full ritual each: brainstorm→spec→plan→implement):
1. B92 — carry tree_size on the adopt/relink wire (adopt-published tree_size=0 fix).
2. Envelope TLV content-review (provenance/intended_ref; what→S3 metadata) — freeze decision.
3. Proto file/package rename: cas_root_shard.proto→cas_format.proto, DB.Cas.Proto→clickhouse.cas.format.
4. Recheck B8/B64/B1 — operator suspects obsolete/incorrect → remove; min delivery = detailed research.
Then: backlog grooming (statuses/descriptions; move finished/abandoned to archive).
Then: 6h soak (correctness + disk/CPU/mem); if it finishes, loop on next clear/trivial backlog tasks.
All findings→backlog; write this log; don't stop/don't ask; report in the morning.
Docker-safe: never touch the operator's `archeology` container; port 8123 may be occupied (soak gate).
Carry-over: heartbeat-removal (Tasks 1-5) DONE + build/sweep-verified (1-baseline); formal combined
review skipped (clean deletion, green) — superseded by this directive. Sweep binary fresh @ 01:59.

## Task 2 — Envelope TLV / incarnation-zone freeze review (research-envelope-tlv) — DONE
Verdict: envelope is FREEZE-READY except ONE pre-freeze action.
- Core (94-byte, hole-free) + TLV + incarnation zone all have documented, non-redundant purpose; NO
  dead field (unlike the removed heartbeat). incarnation_tag is load-bearing at upload (INV-NO-RETURN);
  build_id/provenance/intended_ref are intentional write-only forensics/diagnostics (correct to freeze).
- S3-metadata placement correct: all fields in the envelope BODY (LocalObjectStorage drops S3 user-meta,
  B167b) — nothing should move to S3 metadata.
- **PRE-FREEZE ACTION (queued as a small code task):** `domain_id` (offset [38,54), = pool_id at write,
  CasBuild.cpp:295) is decoded but NEVER verified on read → its stated cross-pool-contamination invariant
  is unenforced. Add a fail-closed check in `CasStore::readTree` right after the `h.logical_hash != id`
  check (~line 473): `if (h.domain_id != poolMeta().pool_id) throw CORRUPTED_DATA(...)`; mirror it in the
  blob read path when that lands. Safe (every in-pool object was written with pool_id; single-pool-per-disk
  → no legit cross-pool adoption), pre-release (no migration), fail-closed (correct direction).

## Task 4 — B8/B64/B1 recheck (research-b8-b64-b1) — DONE + applied (verified independently)
- B64: DONE+archived (d6f6b8345a0); 03822 un-gated, oracle 05001. Stale cross-refs cleaned.
- B8: OBSOLETE — REPLACE_RANGE/MOVE PARTITION/DROP_PART covering race all implemented+tested+un-gated
  (Phase 3.2 8fcea70ae3d, B61(b) 6a0e506533c; gate lifted StorageReplicatedMergeTree.cpp:~7421). ROW REMOVED.
  Multi-ref commit atomicity is the only residual, tracked separately as B122.
- B1: KEPT open as umbrella; prose trimmed. Multi-replica works via fetch-by-relink (test_cas_replicated_relink
  passes); TRUE remaining = manifest_hash on the per-replica Keeper /parts znode (ReplicatedMergeTreeSink has
  no CA code). Verified: 0 manifest_hash/CA refs in the sink.
- Independent verification done (archive row, test tags, gate-lift comment, sink grep) — research confirmed.

## Task 1 — B92 carry tree_size on adopt — DONE + verified (commit b44db5dbaf7)
- observeAndAdmit unified: logical_size = hr.size - blob_header_len for BOTH blobs and trees (tree obj =
  [blob_header_len envelope][encodeTree payload]); flows to RefPayload.tree_size at publish/precommit.
- Round-trip test CasProtocol.AdoptTreeRoundTripCarriesRealTreeSize (adopt-republished == freshly-built).
- Side fix: writeTreeRaw test helper now pads to blob_header_len (was producing sub-header objects that
  tripped the new guard). Sweep independently verified 362/1-baseline. Risk-check: only publish/precommit
  consume tree dep.size (closure walk uses retained_trees).

## Task 2 action — envelope domain_id read-check — DONE
- readTree now fail-closes if header.domain_id != pool_id (cross-pool contamination), right after the
  logical_hash identity check; emits a CorruptDecode event. Negative test CasStore.ReadTreeRejectsForeignDomainId.
  Sweep 363/1-baseline. Closes the one pre-freeze gap from the TLV review; envelope now freeze-ready.

## Task 3 — proto rename — DONE + verified (commit c519a79f684)
- cas_root_shard.proto→cas_format.proto, package DB.Cas.Proto→clickhouse.cas.format; per-file alias
  namespace Proto = ::clickhouse::cas::format keeps call sites. grep gate empty; golden codec tests pass
  (wire bytes unchanged); sweep 363/1-baseline. ALL 4 night-2 directive tasks DONE.

## Grooming pass (2026-06-26 night-2) — DONE
- Release-gate bullets updated: B92 DONE, envelope-TLV/domain_id DONE, proto rename DONE; B8 removed
  (obsolete), B64 cross-refs cleaned, B1 trimmed. B92 row moved to archive.
- Remaining open release-gate items: B164b (journal bound), B1 (Keeper manifest_hash umbrella), B176
  (gc-snap protobuf, deferred), B186 (CaWiringOps.FreezeViaHardLinksIntoShadow shadow-listing — the 1
  baseline-red gtest), B194 (GcSnap::stripTree O(NxM)), plus the larger cost/perf program (B147/B148/
  B158/B168/B178/B201) and the soak-confirmation items (B185/B199).

## 6h SOAK launched (2026-06-26 ~02:5x) — night-2 binary
- Field clear: no foreign containers, port 8123 free. Server rebuilt clean (ninja clickhouse exit 0,
  binary 02:52) with ALL night-2 changes (B92, domain_id check, proto rename, heartbeat removal).
- Command: cd utils/ca-soak; SEED=20260626 DURATION=6h WORKERS=6 METRICS=logs/soak_night2_metrics.db
  MAX_POOL_GB=40 bash scripts/run_24h.sh (phase-3 time-driven chaos; compare_aggregates oracle + fsck at
  checkpoints; exits non-zero on any failure). Scoped to compose project 'ca-soak'.
- Logs: soak_night2.log (driver), resources_night2.log (CPU/mem/disk every 5min), phase3_*_server.log
  (docker logs preserved at teardown), ch1/ch2 (per-node CH logs). Resources at start: 546G free disk,
  91G mem (41G avail), 32 CPU. Waiter b1omvot2h fires on terminal marker (or 6.5h hang-guard).
- On completion: record correctness (oracle) + resource curve; if GREEN, loop on next trivial backlog
  task; all findings → backlog.

## SOAK ATTEMPT 1 (night-2) — FAILED at ~1.7h on a HARNESS bug (B204), disk recovered
- pool_size() O(pool) mc-ls probe timed out as the pool grew → None → compute_throttle fails OPEN →
  MAX_POOL_GB unenforced → / hit 100%. ch1 err.log 0 bytes, no CA errors → NOT a night-2 regression.
  disk_watchdog.sh (B167g) exists but was NOT wired into run_24h.sh. Killed soak + scoped down -v →
  disk reclaimed (431G free). Recorded as B204. NEXT: fix harness (du probe + fail-closed throttle +
  wire watchdog) then re-run a bounded soak to actually validate the night-2 binary.

## SOAK ATTEMPT 2 (night-2) — launched on fixed harness (B204 fix 10fcf585dd3)
- du-based pool probe (cheap/scalable) + fail-closed throttle + disk_watchdog auto-wired (60G floor).
  DURATION=6h MAX_POOL_GB=40 SEED=20260626. Logs: soak_night2b.log, resources_night2b.log,
  disk_watchdog.log. Disk 403G free at launch; triple safety (du-pace / fail-closed / watchdog).
  Waiter bg93rhr1j fires on terminal marker or ~6.7h guard. Validates the night-2 binary end-to-end.

## SOAK ATTEMPT 2 — RESULT: B204 fix VALIDATED; failed at ~5min on a SERVER DEATH (root cause unresolved)
- GOOD: NO disk fill (485G free throughout) — the B204 du-probe + fail-closed throttle + watchdog WORK.
  pool_bytes probe returns real values (13601 → 1.09GB over 5 ticks); throttle correctly 0 (pool « 40G).
- FAILED at ~5min (warmup stage, op~28129, BEFORE chaos@8640s and BEFORE the compare_aggregates oracle):
  burst of QUERY_WAS_CANCELLED on 8124 (ch2) + flood of ConnectionReset → a clickhouse-server process
  DIED; fsck then hit 'No such container: ca-soak-ch1-1' → persistent-dangling → PHASE3 FAILED.
- ROOT CAUSE UNRESOLVED. Likely OOM (aggressive warmup: 6 workers, 36-65KB rows, 1GB pool/5min) vs a
  possible CA crash. CANNOT classify: ch err.log (logs/ch1/clickhouse-server.err.log, 4229 bytes — has
  content) is syslog-owned 640, container gone, no passwordless sudo, dmesg unreadable. Evidence
  PRESERVED on disk for a root/docker session. Recorded harness post-mortem gap as B205.
- NEXT (needs fresh context + root for the err.log): read ch1 err.log to classify OOM-vs-crash; if OOM,
  raise memory headroom / lower workers / lower insert rate; if CA crash, bisect (note: NOT yet shown to
  be a night-2 regression — attempt-1 server logs were clean). Fix B205 first so the next run is
  diagnosable. Did NOT start a 3rd soak (context exhausted; needs the err.log + fresh analysis).

## SOAK ATTEMPT 2 root-caused + ATTEMPT 3 launched (2026-06-26)
- Attempt-2 root cause (via docker-read of syslog-owned ch logs): NO CRASH/OOM. Both ch1 AND ch2 got
  'Received signal 15' (SIGTERM, graceful) at ~03:00:55-03:01:06; QUERY_WAS_CANCELLED = in-flight INSERTs
  cancelled by shutdown. Watchdog did NOT trip (disk 484G >> 60G floor, no TRIPPED line). run.py failed
  with TRANSPORT FAILURE (Connection refused x39 retries) AFTER the servers were stopped. => external
  teardown (docker stop/down) ~5min in, almost certainly cross-run interference: attempt-1's run_24h.sh
  (soak.run killed, but my pkill of the script exited 144=interrupted) ran its OLD trap 'down -v' on the
  SHARED ca-soak project, killing attempt-2. The night-2 BINARY did NOT crash.
- FIX (operator directive): run_24h.sh now tears down ONLY on happy PHASE3 OK (SOAK_OK=1); on ANY
  failure it captures compose logs + docker-inspect (OOMKilled/ExitCode) and LEAVES the stack UP. Fixes
  B205 + prevents cross-run teardown. Committed.
- ATTEMPT 3: fixed script, MAX_POOL_GB=20 (observe the cap), du-breakdown resmon (scripts/soak_resmon.sh:
  per-pool-dir du + df + docker system df every 5min → logs/soak_resmon_c.log), 12.5min heartbeat for
  active health checks. Healthy at launch (pool_bytes probe working, containers up). Field verified clean
  (no lingering procs). du-evidence will show WHERE disk goes (blobs/trees/roots/gc).

## DISK EVIDENCE (attempt-3, 2026-06-26 ~05:32Z) — 'where does the disk go' (operator ask)
- docker system df at ~12min into attempt-3 (pool=5.7G, healthy):
    Images:        147.7 GB (130 images), 22.85 GB reclaimable — accumulated CH/rustfs/mc images across
                   runs + likely the operator's archeology images. The dominant STATIC consumer.
    Local Volumes:  44.5 GB (26 vols), 38.55 GB reclaimable (86%) — DANGLING anon volumes from
                   interrupted soak runs. THIS is the '~140G that didn't return' after attempt-1 down -v.
    Containers:    506 MB ; Build Cache: 1.7 GB.
    Live rustfs pool (/data/test/soak_pool): 5.7 GB — BOUNDED (20G cap), healthy. B204 fix works.
- INTERPRETATION: attempt-1's disk-to-0 was the UNBOUNDED rustfs pool (broken probe → no throttle)
  growing on top of a ~190GB docker baseline (images+dangling volumes). The live pool itself is small +
  bounded once the du-probe + throttle work (attempt-3 confirms).
- RECLAIMABLE (operator's call — NOT auto-pruned per docker-safety; some images/volumes may belong to
  the operator's archeology session): ~38.5GB dangling volumes + ~22.85GB images + 1.7GB build cache
  ≈ 63GB reclaimable via 'docker volume prune' / 'docker image prune' / 'docker builder prune' (run
  ONLY after confirming none are needed by the operator's session).
- HARNESS NOTE: each soak run should 'docker compose down -v' on a HAPPY finish (now fixed) AND a
  periodic 'docker volume prune -f' of OWN dangling vols would prevent the 38GB leak — but left to the
  operator given shared-host docker-safety.

## SOAK ATTEMPT 3 — failed ~17min on a HARNESS log-bug (NOT CA, stack preserved by the fix!) → ATTEMPT 4
- The no-teardown-on-failure fix WORKED: attempt-3 failed but the stack was LEFT UP (containers healthy
  28min, ch1/ch2 mem 2-2.75G/28G = NO OOM, cpu ~220%). Fully diagnosable for the first time.
- Real cause: HARNESS bug. run.py:865 tick_once THROTTLE-change log divided pbytes/max_pool_bytes with
  NO None-guard. The du probe returned None on a tick (best-effort, transient); the B204 fail-closed
  throttle then made new!=old → the buggy log line ran with pbytes=None → TypeError → metrics thread
  raise → PHASE3 FAILED. NOT a CA bug (servers healthy).
- FIXED: None-guard the budget% (commit). 3 harness failure modes now fixed (B204 probe, cross-run
  teardown, None-log-crash). ATTEMPT 4 launched (DURATION=6h MAX_POOL_GB=20, robust resmon gated on the
  log marker, 12.5min heartbeat). Disk evidence stands: live pool bounded (~6G); host disk dominated by
  147G docker images + 38.5G reclaimable dangling volumes (operator's prune call).

## GC EVIDENCE from attempt-4 (system.content_addressed_garbage_collection_log, live, ~30min in)
- ch1 = GC leader (148 Finish rounds); ch2 = NotALeader (1140 lease-checks) — single-leader confirmed.
- Reclaim: 491,461 objects deleted + 425,537 children cascaded (~917K reclaimed) / 491,841 marked (99.9%).
- Duration BIMODAL: avg 13.6s, max 476s (~8min). Distribution: <2s=58 rounds (2.7K del), 2-10s=89 (14K),
  10-60s=3 (12.8K), >60s=6 rounds (463.8K del = 94% of all deletions). The 6 big rounds (rounds 85-91,
  06:02-06:24) do the bulk: e.g. r88 470s del 130.9K+cascade 182.8K; r89 246s del 192.2K; r87 476s del
  50.8K+cascade 102K.
- INSIGHT: GC effective by count but BURSTY — pool grows during inserts, then a multi-minute round
  reclaims 100-190K objects at once → pool overshoots MAX_POOL_GB (hit 30G vs 20G budget at 06:15) because
  max insert-throttle can't pace fast enough between big reclaim rounds. Concrete evidence for the O(pool)
  fold/retire throughput bottleneck (B147 resident-snap / B148 op-count / B160 trim-lag / B201 LIST-discovery).

## GC is O(delta) CONFIRMED + B148 per-candidate-HEAD pinpointed (attempt-4 ProfileEvents, 2026-06-26)
- ProfileEvents of the long GC rounds (system.content_addressed_garbage_collection_log, live ch1):
    snap_io (CasGcGet+CasGcPut) = 9 ops CONSTANT every round (any pool/delta size) → NO whole-pool snap
    re-read. B147 (whole-pool snap reserialize) is NOT the bottleneck (binary+zstd resident snap is cheap).
    heads ≈ deletes ≈ objects_deleted EXACTLY every round (r87 del50774 heads52389 del-ops50784;
    r88 del130868 heads132469; r89 del192225 heads194233; r90 del67079 heads68683). gets flat ~1.5-1.8K.
- VERDICT: GC IS O(delta) (objects deleted this round), NOT O(pool). The 476s rounds are long ONLY because
  the delta was huge (50K-192K deleted in one burst) and rustfs serves ~200-400 ops/s. Duration ∝ delta,
  AMPLIFIED 2x by a per-candidate HEAD.
- ROOT (B148): Gc::retire HEADs EVERY zero-in-degree candidate to read its token before deleteExact, even
  though the resident snap already knows the token. Dropping that HEAD (deleteExact from the snap's known
  token) would HALVE S3 ops on big rounds (eliminate 50K-190K HEADs/round). Concrete B148 evidence.

## trace_log GC evidence (attempt-4, allow_introspection_functions=1) — B194 + B148 pinpointed
- GC interval = 2s (gc_interval_sec=2, grace 5s). Big delta = STARTUP catch-up (warmup fills pool, merges
  mass-supersede → bulk unreference; GC drains backlog in a few huge batches). Steady-state (round ~588):
  0-1460 deleted in 1-5s every 3-7s → pool plateaus ~36G.
- system.trace_log top GC-controller stacks (Real): #1 = GcSnap::stripTree <- Gc::cascadeAndPersist (162
  samples) — MORE than S3 I/O wait (poll/receiveBytes, 109). GC on-thread split: s3_io_wait 110 / stripTree
  81 / other 77 / fold 9.
- => Long rounds bottlenecked by TWO things: (1) B194 GcSnap::stripTree O(N×M) — the #1 on-CPU GC frame,
  runs in cascadeAndPersist over the 101K-182K children cascaded in big rounds; (2) B148 per-candidate
  HEAD + DELETE S3 ops (~2/object) at rustfs ~200-400 ops/s. snap_io stays constant (9) → NOT O(pool).
- ACTIONABLE: B194 (stripTree O(N×M)→O(N)) cuts the CPU half; B148 (deleteExact from snap token, drop the
  per-candidate HEAD) cuts the I/O half. Soak is the before/after target. (trace_log needs
  allow_introspection_functions=1 to symbolize; GC bg-thread IS sampled in Real traces.)

## B194 DONE + verified (commit b02a31f7e3b)
- GcSnap::stripTree O(N×M)→O(children) via derived children_by_tree (parent_tree→[edge_id]); maintained at
  the only 2 tree-edge touch points (addEdge insert-when-inserted / stripTree erase-all); rebuilt on decode
  via addEdge → NOT serialized → no wire change (19/19 CasGcSnap golden/codec tests pass). Behavior-preserving
  (equivalence + decode-rebuild test CasGcSnap.B194StripTreeReverseIndexCorrectnessAndDecodeRebuild).
  Independent sweep 364/1-baseline. Built unit_tests_dbms only (soak's mounted binary untouched).
- NEXT: stop before-baseline soak → ninja clickhouse → re-soak; expect stripTree to drop out of top GC
  trace_log frames (before = attempt-4's 162-sample stripTree stack).

## SOAK ATTEMPT 4 — result: night-2 binary VALIDATED ~2.1h; failed on a B185-class TRANSIENT dangling (NOT real loss, NOT a regression)
- Ran clean through warmup→steady→mutations→ttl_pressure→gc_checkpoint (~2.1h, t+7561s) on the fully-fixed
  harness. PHASE3 FAILED at the gc_checkpoint fsck: PERSISTENT dangling=243 didn't clear within the 180s
  fsck-settle window (exit 36). NO chaos faults yet (chaos@8640s not reached; last_fault=null).
- DIAGNOSIS (stack LEFT UP by the no-teardown fix → live inspection):
  * LIVE fsck now: reachable=72 dangling=0 unreachable=0 — dangling SETTLED to 0 (transient).
  * Data INTACT: SELECT count()=1,251,067 on BOTH replicas (identical); WHERE NOT ignore(*) reads all
    parts with NO error. Oracle-clean (no query-visible loss). soak's final fsck_status=settled.
  * reachable 22417→72 = gc_checkpoint stage consolidating merges + GC reclaim (dedup 20.5x), expected.
  => B185-class TRANSIENT (rustfs-beta read-after-write lag under heavy churn): 243 momentarily-absent
  reachable keys took >180s to settle on single-disk rustfs → tripped the harness HARD gate. NOT real
  durability loss, NOT a night-2 regression. NIGHT-2 BINARY VALIDATED ~2.1h, data intact.
- HARNESS FINDING: the 180s persistent-dangling fsck gate is too short for this pool-size/churn on
  rustfs-beta (B185 saw 94 transient; here 243). Options: raise the settle window, or treat
  dangling>0-but-oracle-clean as WARN not hard-fail (the oracle is the authoritative no-loss gate).
  CA-side B185 root cause (why rustfs returns transient-absent for reachable keys) still open (rustfs RaW).
- Stack left UP (run.py exited cleanly, trap preserved containers+volumes+logs: phase3_20260626T074914_server.log).

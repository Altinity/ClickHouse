# Unattended session worklog — 2026-07-08

Branch: `cas-gc-rebuild` (DO NOT switch). Mode: unattended, no questions.

## Task list
1. **s3_cache disk support** — `<type>cache</type>` over CA disk fails at startup (`NOT_IMPLEMENTED`). Fix write-path wiring. E2E test (scenario) showing cache effect on repeated queries via metrics. Flow: reproduce → brainstorming → plans → subagent-driven TDD. Possibly integration tests.
2. **CAS metadata part-folder cache** — implement per spec `docs/superpowers/specs/2026-07-08-cas-part-folder-cache-design.md` + plans phase1-5. Subagent-driven TDD.
3. **Stabilization**
   - a) Work BACKLOG (`utils/ca-soak/scenarios/BACKLOG.md`): do everything obvious, add docker-compose infra for scenarios needing it, run all scenarios, monitor resources/disk. Small bugs→fix now; complex→backlog. Triage design items by YAGNI/importance.
   - b) Add soak failure type "replica freeze" (SIGSTOP then SIGCONT after X). Run soak 4h.
   - c) Run stateless tests on S3 CA disk via praktika. Triage failures.

Watchdog cron: job 60470431 (hourly at :23).

## Environment baseline
- Disk: 1.8T, 1.2T used, 560G free (68%) at start.
- Builds: `build/` (45G, RelWithDebInfo, binary 2026-07-08 19:52), `build_asan/` (48G).

---

## Progress log

### 2026-07-08 — session start
- Set watchdog cron (hourly :23), created worklog.
- Task 1 reproduction: exception is known (memory `project-ca-cache-disk-unwired`): cache disk over CA →
  `checkAccess`→`writeFileImpl`→`generateObjectKeyForPath`→CA txn `notYet("M-W")`→NOT_IMPLEMENTED.
  Root: `MetadataStorageFromCacheObjectStorage` forwards neither `isContentAddressed` nor the CA txn surface.
- Design (Approach B): reuse CA metadata storage directly under cache disk; wrap only object storage.
  Spec `docs/superpowers/specs/2026-07-08-cas-file-cache-disk-support-design.md`, plan
  `docs/superpowers/plans/2026-07-08-cas-file-cache-disk-support.md`. Fresh-model consult CONFIRMED core
  safety (idempotent startup, cache routing, generic virtual dispatch); spec corrected.
- Harness smoke: docker + praktika + pytest all work (existing test reached pytest execution).
- **User directive: use RustFS (not MinIO) for CA integration tests — MinIO doesn't enforce
  conditional-PUT semantics CAS needs.** Added `with_rustfs` to the integration harness
  (`tests/integration/helpers/cluster.py` — 7 wiring spots mirrored from `with_minio`) +
  `tests/integration/compose/docker_compose_rustfs.yml` (image `rustfs/rustfs:1.0.0-beta.8`, port 11121,
  creds clickhouse/clickhouse, bucket `test`). Python syntax OK.
- **Execution-model note:** the `with_rustfs` plumbing + tiny C++ fix are tightly coupled + context-heavy,
  so doing Task 1 INLINE (controller) with a review-subagent gate rather than strict subagent-per-task.
  Task 2 of session (part-folder-cache phases 1-5) will use full subagent-driven-development.
- Created `tests/integration/test_cas_file_cache/` (RustFS-backed): startup+roundtrip test + cache-metrics
  test. RED verified against buggy binary (exact NOT_IMPLEMENTED for disk_ca_s3_cache at checkAccess;
  RustFS infra confirmed working — CA disk mounts fine, only cache wrapper failed).
- Applied fix (DiskObjectStorageCache.cpp wrapWithCache CA-bypass + wrapper isContentAddressed forward);
  built clickhouse clean; re-ran → **GREEN: 2 passed** (roundtrip + cache-hit metrics cold≫warm).
- **Task 1 COMPLETE.** Commits: af43bfc5dcb (with_rustfs infra), 40cf056cacb (integration test),
  3ed0e5f5030 (fix). Docs: ROADMAP row → DONE; tmp/test_stand_ca_storage.xml comment updated; memory
  `project-ca-cache-disk-unwired` → resolved. Review gate: no Critical.

### 2026-07-09 — Task 2 (part-folder metadata cache) COMPLETE
- All 5 phases implemented subagent-driven on cas-gc-rebuild (~19 commits a80a32553f5..76d46ad96a1).
  Cas* gtests 535/535. Per-phase reviews (P1/P2/P4 dedicated, P3 inline) + FINAL whole-branch review (opus)
  = READY TO MERGE, no surviving Critical/Important.
- Highlights: index-free PartFolderView over shared decode; readManifestShared (no per-op copy); strict
  decoder ordering; CachedPartFolderAccess facade owns committed reads+writes; validate-on-hit retention
  (ON by default, cas_part_folder_cache_bytes=0 disables); single-flight; write-through erase; byte-bounded
  manifest decode cache. ACCEPTANCE MET: ≤1 manifest GET + 0 HEAD per load window on validated hits
  (confirmed: on=1GET+1HEAD/5reads, off=1GET+5HEADs).
- Baseline-confirmed PRE-EXISTING (not this work), backlogged: 3 CaWiring* GC/shadow tests (fail identically
  at e6fa3bf16f6); 2 CA stateless env failures (04286 EISDIR-on-LOCAL, 05009 log-enabled). 3 Phase-4
  observability minors backlogged (dead evictions counter etc.). GATING GAP noted: CaWiring* never in gate.
- NOT pushed (CLAUDE.md: push only when asked). NEXT: Task 3 stabilization (3a backlog sweep + scenario
  infra, 3b soak+replica-freeze 4h, 3c stateless CA-s3 triage).

### 2026-07-09 — Task 3c: CA-s3 stateless → zero, and the GC-race fix cycle
- Watchdog: disk 72% (497G free), no hung tasks/builds/soak. 61G apport coredumps still need user sudo.
- **Baseline attribution (no hand-waving, per user):** ran the 38 CA-s3 FAILs on the NORMAL non-CA job.
  31 fail there too (local env: clickhouse-local persistence, no mysql, s2-geo precision, ref drift, loaded
  box) → NOT CA-caused. Only 7 fail ONLY under CA-s3. Committed to BACKLOG (ea0dd0619ee).
- **The 7:** 3× promote-vs-GC-condemn ABORTED (01156/01710/02346), 2× timeout (03582/03800), 1× TTL diff
  (00933), 1× write-path memory (03829). The 3 ABORTED are the meaningful class = real production
  robustness gap.
- **Brainstorming cycle — promote resurrect-on-condemn (tokened blob).** User directive: "at commit we
  must have the data in hand; recovery invisible to client." Root-caused: promote's fail-closed blob
  revalidation (CasBuild.cpp:886-899) aborts on a prematurely-condemned blob (precommit→blob edge not yet
  GC-folded); a copy-forward pre-pass already resurrects the TOKENLESS case but skips TOKENED (putBlob'd)
  deps. Fix = retain the writer's re-readable BlobSource + bounded resurrect-then-recheck from source
  (uploadFromSource, INV-1, no GET) inside the closure AFTER the owner-liveness check.
- **Fresh-model consult (opus, adversarial): SOUND-WITH-CHANGES.** Adopted its 3 corrections: (1) resurrect
  AFTER owner check, not in pre-pass (else orphan debris on abort path); (2) BOUNDED loop, not single-shot
  (single re-upload doesn't close the race); (3) retain sources in a PARALLEL map (DepEntry gets reassigned/
  clobbered), incl dedup-adopts. Confirmed INV-1 clean, temp-file lifetime safe (cleanup at commit-end after
  promote), memory a non-issue. Fold-barrier = ideal follow-up (larger writer↔GC coupling), out of scope.
- Spec: docs/superpowers/specs/2026-07-09-cas-promote-resurrect-tokened-blob-design.md. NEXT: writing-plans
  → subagent-driven impl, TLA+ gate first.

- **TLA+ gate GREEN (Task 1):** `CaIncarnationCore_reval_stage2.cfg` (EnableResurrect=TRUE, EnableReval=TRUE)
  → "Model checking completed. No error has been found" (2.54M states, exit=0); INV_NO_DANGLE, INV_NO_LOSS,
  INV_NO_RETURN, INV_JOURNAL_COVERAGE all hold. Mapping: C++ promote resurrect-on-condemn ≙ the model's
  `WResurrect` (condemned incarnation → overwrite in place, fresh token, old token into deadTok) composed
  with the `EnableReval` publish gate (children must be ~CondemnedAtView). The fix brings the promote
  IMPLEMENTATION into line with this already-verified path (impl was stricter: aborted where model resurrects).
  No model change. Plan: docs/superpowers/plans/2026-07-09-cas-promote-resurrect-tokened-blob.md.

- **Impl COMPLETE (Tasks 2–4), reviewed APPROVE-WITH-MINOR.** Commits 559a6879368 (retain BlobSource),
  7176e39e29b (bounded resurrect-then-recheck loop in Build::promote, after the owner-liveness check),
  3091db32a50 (2 new gtests + 3 pre-existing CasProtocol safety tests rewritten to resurrect). RED proof
  captured (Test A ABORTED pre-fix), GREEN after. Regression Ca*/Cas* 650/652 (2 = pre-existing flaky
  CaWiring*, not grown). Whole-branch review (opus, adversarial): no Critical/Important — INV-1 preserved
  (uploadFromSource never GETs), owner-check-first ⇒ no orphan on abort, loop terminates ≤2 iters (fixed
  retire-view snapshot), tokenless no-source backstop still aborts (EvidenceHit test). 2 Minor doc fixes
  applied (3b06153d770). SCOPE note: the 3 rewritten CasProtocol tests encoded the OLD no-source
  fail-closed contract for a leaf THIS build putBlob'd — now source-backed ⇒ resurrect; verified each is
  owner-live (promote succeeds only past the owner check), so not a safety weakening; the genuine
  reclaimed-precommit abort is covered by the new PromoteAbandonedPrecommit... test.
- Building full `clickhouse` binary to validate the 3 GC-race stateless tests (01156/01710/02346) on the
  CA-s3/RustFS lane end-to-end. Unpushed commits accumulating on cas-gc-rebuild (push on user's word).

- **END-TO-END VALIDATION.** Rebuilt full `clickhouse` (incremental, ninja exit=0). Ran the 3 GC-race
  tests on the CA-s3/RustFS lane (`Stateless tests (arm_binary, content_addressed s3 storage, parallel)`
  → starts RustFS + installs CA-s3-default policy): **01156_pcg_deserialization OK, 01710_projection_detach_part
  OK, 02346_exclude_materialize_skip_indexes_on_insert OK** (all pass). Combined with the deterministic
  seeded-condemn unit test (RED→GREEN) + green TLA+ gate, the fix is validated. Full CA-s3 lane launched in
  background as the under-load regression check (confirm the 3 stay green + no NEW promote-condemned ABORTED
  anywhere); monitoring. Remaining non-race CA-s3 lane items (unchanged by this fix): 2 timeouts
  (03582/03800), 00933 TTL timing, 03829 memory, + ~31 local-env failures (not CA).

- **FULL CA-s3 LANE RESULT (under load): Passed 10357 / Failed 55 / Skipped 104.** The 3 tokened-INSERT
  GC-race tests (01156/01710/02346) are GONE from failures — fix validated under concurrent GC churn. The
  ONLY promote-condemned ABORTED in the whole lane is 03283_optimize_on_insert_level (blob b09909, one
  occurrence) = the tokenless DETACH/freeze follow-up (backlogged, separate cycle, NOT a regression). The
  other 54 failures are the known local-env/pre-existing set (mysql, clickhouse-local, s2-geo, dynamic-json,
  alias-marker) + known CA non-race items (03829 memory, 05008/05009 CA scenarios). Tokened-INSERT
  resurrect fix = DONE. Memory: [[project_promote_resurrect_condemn]].

### 2026-07-09 — 03283 tokenless copy-forward condemn-race cycle
- Root cause: promote's copy-forward pre-pass reads the PRE-refresh retire view; the in-closure
  revalidation checks the POST-refresh (fence_round) view — a condemnation revealed only by the in-closure
  refresh is missed for tokenless adoptEvidence leaves → fail-closed ABORTED (03283 DETACH/freeze).
- Fresh-model consult (opus, adversarial): Option A — in-closure copy-forward backstop after the refresh +
  owner-liveness check; KEEP the pre-pass as the outside-the-lock fast path (view_gate serialization, not
  the void PUT-in-closure objection); ONE shared predicate isCopyForwardableTokenless; unknown-leaf(no dep)
  + absent-tokenless stay fail-closed. INV-1/no-dangle window = the SAME one the tokened fix already accepts
  (past owner-check + fold barrier). Spec+plan committed.
- **TLA+ gate GREEN:** CaIncarnationCore_stage6_evstale (EnableEvStale+EnableReval) → "No error has been
  found" (153M states; INV_NO_DANGLE/LOSS/RETURN hold) = re-observe-stale-evidence-then-publish is verified.
  Negative control sab_noevreobserve → INV_NO_LOSS VIOLATED (exit 12) = admitting stale evidence without
  re-observe provably dangles → the re-observe (our copy-forward backstop) is load-bearing. Mapping:
  in-closure copy-forward against the refreshed view ≙ the model's WEvObserve re-observe.

- **03283 cycle COMPLETE + reviewed** (APPROVE-WITH-MINOR, minors applied d0eadef700b). Commits:
  1932e174ee0 (helper), 2b43cb913d1 (in-closure copy-forward backstop), 3ab997c2bf9 (gtests + 2 flipped
  safety tests). Review confirmed: the wider envelope = existing pre-pass behavior (not new); backstop is
  STRICTER (post-owner-check, no orphan); INV-1 = same sanctioned copyForwardFromCondemned exception;
  fail-closed absent/no-dep/tokened-source-lost preserved; NewbornPrecommit self-floor still load-bearing.
- **END-TO-END:** rebuilt clickhouse (ninja exit 0). 03283_optimize_on_insert_level passes in ISOLATION on
  CA-s3 lane (Failed 0/Passed 1). Full CA-s3 lane #2 launched under load (the original reproducer) to
  confirm 03283 stays green + the 3 tokened tests stay green + ZERO promote-condemned aborts. Monitoring.
- BOTH condemn-race cycles landed this session: tokened INSERT (01156/01710/02346) + tokenless DETACH/freeze
  (03283). Together close the full promote condemn-race class (putBlob source-backed + adoptEvidence
  tokenless), each: fresh-model consult → TLA+ gate → subagent impl → independent review. See
  [[project_promote_resurrect_condemn]].

- **FULL CA-s3 LANE #2 (post tokenless fix): Passed 10359 / Failed 53 / Skipped 104. ZERO promote-condemned
  aborts in the entire run.** All 4 condemn-race targets OK under load: 01156, 01710, 02346_on_insert,
  **03283** (the tokenless DETACH/freeze reproducer — now green). The 53 fails are the known env set
  (clickhouse-local family 01146/02956/03456/03536x2/03793/04039 + 04286/05008/05009 CA-env, show_privileges
  ref drift, remote_ipv6, s2, kafka-shutdown, avro, alias_marker, test_optimize_using_constraints) — no
  promote/condemn involvement; 03829/00933/03582/03800 did not reproduce this run (flaky timing/memory class).
  **The promote condemn-race class is CLOSED: tokened INSERT resurrect + tokenless copy-forward backstop,
  both TLA+-gated, reviewed, and validated under the full concurrent lane.**
- Protocol review with user recorded in BACKLOG (PROMOTE-REVALIDATION-MINIMIZATION-2026-07-09): the
  ack-floor model licenses skipping per-leaf promote HEADs when the installed round is unchanged since dep
  observation (DepEntry::observed_view_round) — complementary to (not replacing) the resurrect fixes: skip
  covers the round-unchanged happy path, the fixes are the recovery when the round moved mid-commit.

### 2026-07-09/10 — Tier-2 writer↔GC simplification cycle (user-driven brainstorm)
- User probe chain invalidated successively deeper layers: (1) re-checks needed? → (2) round-change
  irrelevant? → (3) pinned-round ack enough? → (4) **early precommit already enough** — the decisive one.
  Verified: publishStaging order (body → precommitAdd → putBlob → promote) makes the closure edge durable
  BEFORE any observation; fold-activation + d>0→spared + two-phase d-recheck ⇒ closure-named hashes
  undeletable ⇒ promote freshness machinery is redundant defense-in-depth. Named EDGE-BEFORE-OBSERVE.
- Scope decision (user): **Tier 2 now** (writer side whole: delete tokened promote revalidation,
  retained_sources, copy-forward pre-pass, view_gate drain, writer fence_round refresh [TLA+-conditional],
  dead observed_view_round; NO paranoid mode), **Tier 3 → backlog**.
- User probe "does the writer still need the condemned list?" found my K1 rationale UNDERSTATED — it is
  safety-critical: pre-precommit-graduated entry can be adopted present-but-doomed (pass seals before our
  precommitAdd → fold misses edge → deleteExact after our HEAD) — only condemned-check + exact-token
  displacement close it; the ack floor is the list's DELIVERY GUARANTEE (min_ack > condemn_round ⇒ every
  live writer's view covers every graduated entry). Spec strengthened; Tier-3 "floor likely redundant"
  CORRECTED to a package deal (floor removal = replace dedup-adoption safety wholesale).
- Spec committed: docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md
  (7d90f7b4812 + f427d8b1fd5 + 890a8c15439). Variants A/B superseded in BACKLOG; Tier-3 recorded.
- Spec at user-review gate. Meanwhile dispatched adversarial fresh-model consult (tier2-consult, opus,
  background): verify the theorem's seal/fold/settle/delete ordering claims, the K1 race, D1 across all
  write paths (multi-part/exchange/republish/hardlink/abandon), D4/D5, the ordering chassert. Plan
  (writing-plans) after consult returns clean + user approves spec.

- **TLA+ GATE A GREEN (Phase A authorized).** New focused model docs/superpowers/models/CaEdgeBeforeObserve.tla
  (writer order + GC condemn→graduate(floor)→same-pass-decided-delete with per-pass d-recheck + displacement
  revoking decided deletes). Matrix exactly as spec'd: `reduced` (NO tokened revalidation, NO drain, NO
  fence-refresh) → "No error has been found"; all four sabotages violate INV_NO_DANGLE: sab_late_edge
  (adopt before durable closure), sab_no_adopt_check (K1 blind adopt — the pre-graduated interleaving),
  sab_no_k3_head (tokenless absent leaf), sab_no_k3_adopt_check (finding C promote-adopt shape).
  **D5 CONFIRMED** (Task 8 GO): the reduced model has no refresh action at all and is green;
  SabotageNewbornNoFloor targets the KEPT shard-side born-floor stamp (birth_floor_provider), not the
  writer refresh; consult's verified-negative concurs. Phase-B meta-consult still in flight (Phase B only).

- **Phase-B meta-consult verdict: SOUND-WITH-CHANGES, 1 Critical pair — FOLDED INTO SPEC.** C1: my
  birth-completion contended on meta ABSENCE (If-None-Match), not the condemned etag → not serialized
  against GC's meta-first delete → adopting the orphan body in GC's transient (meta deleted, body pending)
  window = dangle + INV-META-BODY break; FIX = birth-completion resurrects from the writer's OWN source
  (putOverwrite displaces, GC's pending body delete misses). C2: GC body delete keys on the CONDEMN-TIME
  token (head_blob capture), never a fresh HEAD (would delete a displaced live body). I1: supersede
  re-shape specified (peek_meta analog of peek_head; ledger stores meta condemn-etag; untouched-entry
  induction → Gate B property). I2 rebuild meta capture; I3 idempotent redelete. no-floor argument verified
  SOUND (minimal guarantee stated + code-provided); backends key-agnostic (no gap); ca-inspect .meta
  dispatch bug pre-identified; M2/M4/M5 folded; Gate B += sabotages (f) birth-completion-by-adoption and
  (g) fresh-HEAD body delete.

- **Phase A Tasks 7-8 landed (controller-inline after pa78-impl hung 2h/zero artifacts — stood down).**
  D4 (0d86389b93b): view_gate member + both lock sites removed; syncer installs under RetireView's own
  mutex; the drain test INVERTED (SyncInstallsWithoutWaitingForInFlightMutation — the new non-blocking
  contract). D5 (55877eb623c): writer-side fence_round refresh removed; 4 tests migrated — the newborn
  shard-incarnation test now asserts the Phase-A contract (condemned token bound UNCHANGED; floor holds
  graduation since our ack < condemn round; durable edge spares at next fold; dangling=0), and the 3
  K3 tests install the view explicitly (preserving visible-condemnation gate coverage). Full sweep after:
  only the 2 known CaWiring* flakes. Task 9 validation started (clickhouse rebuild → 4 stateless → lane;
  ASan units for the chassert gate after).

- **PHASE-A VALIDATION (Task 9) — all gates passed except the soak (launching):**
  4 condemn-race stateless tests: Failed 0/Passed 4. ASan gate (M3): CLEAN — 130/137 suites green under
  ASan+abort-on-logical-error, ZERO chassert(precommitted) trips; found+fixed 2 real pre-existing bugs
  (event-sink stack-use-after-scope, 10 test sites, c46de859cbb; production sink immune); negative-
  LOGICAL_ERROR test class backlogged (CA-ASAN-SUITE-2026-07-09). Release sweep clean (2 known flakes).
  **Full CA-s3 lane: Passed 10357 / Failed 55 / Skipped 104, ZERO promote-condemned aborts, all 4 targets
  OK, failure list = the known env families only (no new CA class).** Remaining Phase-A exit criterion:
  the soak with the fsck gate.

- **TLA+ GATE B GREEN (soak-parallel work).** New model docs/superpowers/models/CaMetaDescriptor.tla:
  the meta as the per-hash lifecycle register {gen(etag), inc, condemned}; body {tok}; INV-META-BODY;
  writer fresh/adopt/2-step-resurrect(crash window)/birth-completion; GC condemn(capture etag+condemn-token)
  → graduate → meta-first exact-delete → body delete at CONDEMN-TIME token; claim-first sweep; a
  CrashedBirth debris source (needed to arm the sweep race — first draft's sab_e was unreachable-green,
  caught and fixed). Matrix: reduced GREEN (both invariants over all interleavings incl. the C1 window and
  the resurrect crash window); ALL 7 sabotages RED breaking the expected invariant — (f) birth-adopt,
  (g) fresh-head delete, (a) meta-first create, (b) body-first delete, (c) blind adopt (INV_NO_DANGLE),
  (e) claimless sweep, (d) post-lost-CAS body delete. CAVEAT recorded: a 1-hash/1-writer atomicity
  sandbox (~41 distinct states) — fold/pacing timing remains Gate A's domain; multi-writer meta races
  (two resurrectors, adopt-vs-resurrect) to be added when writing the Phase-B plan.

- **PHASE-A EXIT SOAK: PHASE3 OK.** 4h chaos (seed 991): 16 green checkpoints (1 GC + chaos-recovery +
  cliff + final converge), oracle agreement on every one; 25 fault windows fired, 17 restarts, 41
  transport-retried ops, **1 (one) ABORTED-retried INSERT over the whole run** (pre-fix binaries showed
  dozens — the condemn-race class is visibly closed); 0 hard failures (no MISMATCH/Traceback). Known
  degrades recurred as designed: fsck>180s on every busy-pool checkpoint (B146/B154 — the in-run
  dangling==0 gate was unavailable; pool ballooned to ~112GB under chaos-lagged GC), TTL-band count-range.
  **Authoritative no-timeout fsck launched on the quiescent pool (clickhouse disks -C fsck-only.xml
  --disk ca_ro) — the formal Phase-A dangling==0 gate; result pending.**

- **AUTHORITATIVE FSCK: dangling=0 — PHASE A COMPLETE.** Quiescent-pool scan (no timeout):
  reachable=15 dangling=0 unreachable=764118 (pending_gc=389543 awaiting_gc=160190 unaccounted=48451)
  physical=93.7GB. The unreachable mass is the post-cliff GC backlog draining through the ack-floor
  pipeline + chaos-restart debris (17 restarts) — reclaimable classes, not loss. ALL Phase-A gates
  passed: TLA+ Gate A, unit+review per task, 4 stateless targets, ASan/chassert gate, full CA-s3 lane
  (0 promote-aborts), PHASE3 OK soak (1 ABORTED-retry in 4h chaos), dangling=0. Soak cluster left up
  (GC draining the backlog). NEXT: Phase-B plan (Gate B model already green; extend with multi-writer
  races per the recorded caveat).

- **P1 FINDING (watchdog, post-soak): GC-WEDGE-REMOVAL-FOLD-2026-07-10.** The exit-soak stand revealed a
  pool-wide GC liveness wedge: 63 owner-removal events (one early-DROPped table, ~40 shards) with bodies
  missing at removal-fold ⇒ fold clamps EVERY pass from t+540s ⇒ zero collection for the entire 4h run
  (56.8k clamp events; pool 112GB; explains the in-run fsck timeouts). Integrity held (the clamp is
  correct fail-closed) — but liveness is permanently wedged. NOT Phase-A (fold untouched). Backlogged as
  P1 (above Phase B) with live-repro stand preserved + forensics file. Root-cause candidates: orphan
  manifest sweep racing the drop / dropNamespace ordering / chaos-kill half-drop.

### 2026-07-10 — GC-wedge root cause (live-stand forensics)
- Nailed end-to-end: condemn-race ABORTED mid-63-part-commit → B122 rollback (dropRef x63) → same-txn
  retry RE-PUBLISHED the SAME ManifestIds → fold folded [removal, re-precommit(+1), promote] same-pass →
  mf_cleanup kept the removal-half entries (no erase-on-re-own) → R6 deleted 63 LIVE bodies (transient
  dangle 20:24:51→20:33) → final table DROP's removals clamp forever on missing bodies → pool-wide GC
  stop for the entire run. Evidence chain: content_addressed_log per-edge events (root_add/root_remove
  with manifest_ref_instance), shard-20 journal via ca-inspect (single removal event v1487 above the
  stuck cursor 1486), R6 reason strings, the soak's "ABORTED-retried INSERT attempts: 1". The B170 event
  log + ca-inspect (both built this session cycle-family) made this diagnosable from SQL + one object
  decode — exactly their purpose. Fix design going to consult: (W) writer fresh-ids/idempotent re-drive,
  (F) mf_cleanup symmetric maintenance, (R) snap-source-edge removal recovery (unwedge).

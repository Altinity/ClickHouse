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

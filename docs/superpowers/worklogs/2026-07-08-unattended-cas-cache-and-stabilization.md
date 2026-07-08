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
  `project-ca-cache-disk-unwired` → resolved. NEXT: review-subagent gate, then session-Task 2
  (part-folder-cache phases 1-5, subagent-driven, on cas-gc-rebuild — NOT a new branch per user).

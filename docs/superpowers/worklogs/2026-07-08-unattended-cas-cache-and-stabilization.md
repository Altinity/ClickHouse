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
- Starting code exploration for exact wiring.

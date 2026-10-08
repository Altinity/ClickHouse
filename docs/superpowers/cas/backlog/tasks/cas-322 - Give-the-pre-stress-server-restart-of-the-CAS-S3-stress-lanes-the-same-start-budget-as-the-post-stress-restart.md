---
id: CAS-322
title: >-
  Give the pre-stress server restart of the CAS-S3 stress lanes the same start
  budget as the post-stress restart
status: To Do
assignee: []
created_date: '2026-09-30 07:43'
updated_date: '2026-09-30 07:54'
labels:
  - 'area:ci'
  - 'complexity:trivial'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:review'
  - 'touches:upstream-code'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2396'
  - tests/docker_scripts/stress_runner.sh
  - 'https://github.com/Altinity/ClickHouse/pull/2440'
  - 'https://github.com/Altinity/ClickHouse/issues/2439'
priority: high
type: bug
ordinal: 408000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
The CAS-S3 stress lanes fail with `Cannot start clickhouse-server` whenever `stress_runner.sh` draws `async_load_databases=false` (a 50% coin flip a few lines above the call).
The restart before `stress.py` (`tests/docker_scripts/stress_runner.sh:293`, plain `start_server`) waits 6 tries of 10 s; the restart after it (`:314`) already uses `start_server 10` with the comment "under sanitizers with async_load_databases=false the server may need minutes to load all tables".
With synchronous loading the server loads 46 tables (76-108 parts on `cas_s3`, thousands of S3 requests) under ASan/debug plus ThreadFuzzer; the server log ends at 92-95% of loading when the script gives up. No `<Fatal>`, `fatal.log` empty.
Evidence (PR #2396, three heads, plus one sibling): every failing run has the sync flag; six sibling runs without it pass with 4-7 readiness retries, PR #2398 with it fails the same way.
Logs: `lane-g/tmp/pr2396/{stress,h2,h3}/` (job.log, server.log per run), sibling job logs `lane-g/tmp/pr2396/h3/sib_*_asan_cas_stress_job.log`.
Fix shape: `start_server 10` at `:293` (or skip the sync-load draw when `USE_CAS_S3_STORAGE_FOR_MERGE_TREE=1`). The function is upstream code (`tests/docker_scripts/stress_tests.lib`); the call site change is one line.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The pre-stress restart uses the larger budget (or the sync-load draw is off for CAS-S3), and a CAS-S3 stress run with `async_load_databases=false` reaches `stress.py`
- [ ] #2 Stress test (amd_asan_ubsan, cas s3 storage) and Stress test (amd_debug, cas s3 storage) are green on a PR that draws synchronous loading
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

## Implementation Notes

<!-- SECTION:NOTES:BEGIN -->
2026-09-30: the slow start is the CAS-95 LIST storm (79% of 23,739 S3 requests in 40 s of loading are LISTs of one _files/ prefix per namespace, ~176 per part; numbers in CAS-95 notes). PR #2440 (CAS-95.1) removes the per-checksum LIST; after it lands, re-measure this restart before deciding whether the larger start budget is still needed. The budget fix stays cheap and correct either way (the post-stress restart already has it).

Request breakdown of the failing pre-stress start (2026-09-30). Source: `lane-g/tmp/pr2396/stress/server.log`, from the last `Application: starting up` (line 509663, 12:22:48) to the end of the log (12:23:43, 55 s); counted `AWSClient: Make S3 request to` lines.
- 16,779 S3 requests in total.
- 12,448 (74%) are LISTs of `ns/state/<ns>/_files/`: one LIST per part file at part load (`MergeTreeDataPartChecksum::checkSize` -> `existsDirectory` -> `listNamespaceFiles`). Same mechanism as CAS-95.1 and the restart storm of https://github.com/Altinity/ClickHouse/issues/2439.
- 2,075 (12%) are GETs of `ns/stream/<ns>/_log/` (ref-log reads at mount); 803 are manifest GETs.
The larger start budget treats the symptom. CAS-95.1 (https://github.com/Altinity/ClickHouse/pull/2440, open) removes the `_files/` LISTs; whether the synchronous load then fits the default 60 s under ASan is not measured. The `_log` GETs are a separate cost that PR #2440 does not touch.
Same cost on the yardstick (`utils/cas-yardstick/reference/2026-09-30-647dbf9dfe9`, card `attach_200`, cold): CAS 1005 LIST + 358 GET in 123 ms, plain S3 0 LIST + 1801 GET in 50 ms.
<!-- SECTION:NOTES:END -->

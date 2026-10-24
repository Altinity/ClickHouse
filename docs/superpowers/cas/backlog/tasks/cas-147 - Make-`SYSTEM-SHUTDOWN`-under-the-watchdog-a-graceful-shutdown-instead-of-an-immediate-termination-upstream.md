---
id: CAS-147
title: >-
  Make `SYSTEM SHUTDOWN` under the watchdog a graceful shutdown instead of an
  immediate termination (upstream)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 12:37'
labels:
  - 'area:upstream'
  - 'area:mounts'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
milestone: m-5
dependencies: []
references:
  - src/Interpreters/InterpreterSystemQuery.cpp
  - src/Daemon/BaseDaemon.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f17
priority: high
type: upstream
ordinal: 189000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`SYSTEM SHUTDOWN` sends `kill(0, SIGTERM)` to the whole process group (`src/Interpreters/InterpreterSystemQuery.cpp:419`).
In a container that group holds the watchdog, which forwards every signal except `SIGINT` to the server
(`src/Daemon/BaseDaemon.cpp:673-679`). So the server gets `SIGTERM` twice within a millisecond, and a second
delivery means immediate termination.
clickhouse-operator 0.27.2 restarts a host on every config change with `SYSTEM SHUTDOWN`, so every such restart
on otel.demo was unclean. All four restarts in the week were unclean: no ref-lane drain (`_ckpt` write cancelled),
a 36.5 s wait on the stale-looking mount lease, a recovery seal, and GC round 1383 lost after 2 h 38 min (audit F17, F28).
This affects any ClickHouse under the watchdog, not only CAS. Fix: signal only the server's own pid, or have the
watchdog not forward a signal whose `si_pid` is the child. Check upstream master first.

Provenance: BACKLOG/gc.md#otel-demo-s3-budget-audit-2026-09-25 (F17, one of the nine findings not threaded elsewhere). Verified 2026-09-26 against d4be7f7045a and 0dbbd797792: `kill(0, SIGTERM)` and the forwarding branch unchanged.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 `SYSTEM SHUTDOWN` on a server started under the watchdog logs one termination signal and a clean shutdown
- [ ] #2 After that shutdown a CAS disk's next mount does not report a predecessor whose death was not proven clean
- [ ] #3 The change is a separate upstream pull request with its own test
- [ ] #4 The CAS operations docs note that a pod delete is graceful where `SYSTEM SHUTDOWN` was not, until the fix is in the deployed version
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
First recorded: 2026-09-26 (83949645ce7, by 'audit F17')
<!-- SECTION:NOTES:END -->

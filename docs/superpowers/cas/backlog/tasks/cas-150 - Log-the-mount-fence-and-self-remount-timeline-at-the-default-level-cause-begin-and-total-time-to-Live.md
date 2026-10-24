---
id: CAS-150
title: >-
  Log the mount fence and self-remount timeline at the default level: cause,
  begin, and total time to Live
status: To Do
assignee: []
created_date: '2026-07-28'
updated_date: '2026-09-26 12:55'
labels:
  - 'area:mounts'
  - 'area:observability'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'origin:soak'
milestone: m-7
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2421'
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
  - docs/superpowers/cas/2026-09-04-gcs-soak-15min.md
priority: high
type: enhancement
ordinal: 193000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
An operator cannot reconstruct a lease-loss episode from the server log. What exists: the renewal outcome
(`deliverMountRenewObservability`, `CA/Pool/CasServerRoot.cpp:336`, Warning "fenced after N attempts") and one line per remount
attempt with its step (`CA/Pool/CasPool.cpp:1283-1297`). Missing: a line when the local fence arms (with the cause: renewal
budget, GC fence-out, foreign interference), a remount-begin line, and a remount-complete line with the total fence-to-Live
duration. Physical renewal retries are Debug only (`CasServerRoot.cpp:350-356`).
Evidence: a 7-minute msan CA-s3 fence window (run `07f8398acddff2c`) held zero lease lines; on the canary the audit found no
renewal retries at the default level (F28), and issue #2421 dropped the lease there. The real-GCS soak (2026-09-04) lost the
lease to a DNS outage that the remount log did not name as DNS.
The totals are the measurement the blast-radius design question waits for.

Provenance: BACKLOG/mounts-and-lifecycle.md#mount-fence [fence-window observability], plus the DNS-naming ask of #remount-backoff-no-jitter and the measurement step of [fence-window blast radius]; verified 2026-09-26 against b1c34d03479 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A lease-loss episode logs, at Information or above, fence armed with its cause, remount begun, and remount complete with the total fence-to-Live time
- [ ] #2 A remount attempt that fails on name resolution says so in its failure line
- [ ] #3 A test forces a fence and a remount and asserts the three lines appear in order
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
First recorded: 2026-07-28 (1605b049a33, by 'fence-window observability')

Correction: issue #2421 was closed on 2026-09-26 as not reproducible. Its one reproduced Code 210 was the soak harness (RustFS at Docker's `nofile` 1024 returned `os error 24` on the renewal), not the canary. The msan fence window and audit F28 evidence stand.
<!-- SECTION:NOTES:END -->

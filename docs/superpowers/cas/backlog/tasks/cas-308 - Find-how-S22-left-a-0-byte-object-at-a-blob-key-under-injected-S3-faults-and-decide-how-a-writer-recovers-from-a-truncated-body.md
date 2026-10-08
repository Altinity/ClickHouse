---
id: CAS-308
title: >-
  Find how S22 left a 0-byte object at a blob key under injected S3 faults, and
  decide how a writer recovers from a truncated body
status: To Do
assignee: []
created_date: '2026-09-26 14:38'
labels:
  - 'area:write-path'
  - 'area:soak'
  - 'complexity:medium'
  - 'risk:medium'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'needs:decision'
  - 'origin:soak'
  - 'touches:protocol'
milestone: m-8
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp
  - utils/ca-soak/scenarios/cards/s19_s22_clone_fetch.py
  - utils/ca-soak/docker-compose-s3faultproxy.yml
  - utils/ca-soak/scenarios/RUN_HISTORY.md
priority: high
type: bug
ordinal: 387000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
S22 (injected `503`/`429`/slow faults through the S3 fault proxy, run `20260707T064805_S22_seed20260707`) failed an ordinary INSERT with `CORRUPTED_DATA`: the object at `blobs/f2/f2123bb7…` had size 0, below the blob envelope length. The next run on the same binary passed and no later record explains who wrote the 0-byte object.
The refusal is still in place on both branches (`CA/Pool/CasPartWriteTxn.cpp:408-414`, `ensureBlobPresent`). It fails closed, but nothing repairs the body: every later write of the same content fails the same way.
The body has no edge, so GC never revisits it (CAS-33), and the bad object stays until someone deletes it by hand.
Candidate origins: the writer's own PUT cut short by the proxy's slow mode, a RustFS commit of a truncated upload, or a staging path that publishes an empty file.
Any repair that re-PUTs over the bad body changes the blob publication protocol, so it needs a decision first (decision-1: HEAD-before-PUT is not to be optimised away).

Provenance: utils/ca-soak/scenarios/BACKLOG.md#S22-20260707T064805-1. Related: CAS-124 (fsck size check), CAS-33 (edge-less bodies never revisited). Verified 2026-09-26 against 66087be0ffb (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The 0-byte object is reproduced under S22's fault modes, or a targeted repro rules out each candidate origin with evidence recorded in `RUN_HISTORY.md`
- [ ] #2 If CAS wrote it, the write path is fixed so no published blob can be shorter than its envelope, with a gtest that injects the fault
- [ ] #3 A recorded decision says what a writer does on a truncated body at its key (refuse and report, or re-publish), and the chosen behaviour has a test
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

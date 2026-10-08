---
id: CAS-285
title: >-
  Size the manifest decode cache for the node's part count instead of a fixed
  128 MiB
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-28 16:46'
labels:
  - 'area:read-path'
  - 'complexity:small'
  - 'risk:low'
  - 'touches:settings'
  - 'confidence:solid'
  - 'needs:measurement'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'theme:parity'
milestone: m-2
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedSettings.cpp
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasManifestReader.cpp
  - src/Common/CurrentMetrics.cpp
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f25
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md#f9
  - docs/en/antalya/cas/configuration.md
priority: high
type: enhancement
ordinal: 357000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
On the canary `CASManifestDecodeCacheBytes` sat at its 128 MiB cap all week with 1.9-3.3k entries on a node with ~1,700 parts
of 7-17 KB manifests, plus the other replica's manifests read by GC (audit F25). Part-folder view rebuilds on merges (F9, 2.9 per merge)
come partly from this eviction.
Default: `manifest_decode_cache_bytes = 128ULL << 20` (`CA/ContentAddressedSettings.cpp:80`, `CA/Pool/CasPool.h:77`); the entry cap
16384 is hard-coded (`CA/Pool/CasManifestReader.cpp:40`).
The cache has no hit or miss counters, only the bytes and entries metrics (`src/Common/CurrentMetrics.cpp:236-237`), so its effect
cannot be read today.
Options: raise the default, or size it from the local part count with a floor and a ceiling.
Neighbours: CAS-22 (part-folder view weight), CAS-176.4 (seed the view after commit).

Provenance: umbrella-roadmap.md section 2 bullet 'Manifest decode cache'; filed 2026-09-26 (user decision). Verified 2026-09-26 against c16a2589f56 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The manifest decode cache counts hits, misses and evictions in `ProfileEvents`
- [ ] #2 On a node with the canary's part count the cache no longer sits at its cap, and the part-folder view misses per merge fall, measured before and after
- [ ] #3 The new default or sizing rule is documented in `configuration.md` with the memory it costs per 1,000 parts
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
First recorded: 2026-09-26 (filed during the Backlog.md migration; no earlier trace in docs/superpowers history)

Identifier trace: the earliest docs mention of `CasManifestReader` is 2026-07-15 (416c982b5d6); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->

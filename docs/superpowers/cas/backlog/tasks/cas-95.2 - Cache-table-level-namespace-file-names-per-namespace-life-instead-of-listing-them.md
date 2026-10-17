---
id: CAS-95.2
title: >-
  DOUBTFUL: cache table-level namespace file names instead of listing them (cure
  worse than the disease)
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-27 06:24'
labels:
  - 'area:read-path'
  - 'complexity:high'
  - 'risk:high'
  - 'confidence:contested'
  - 'needs:decision'
  - 'origin:otel-demo-audit'
  - 'origin:canary'
  - 'origin:issue'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2439'
documentation:
  - docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md
parent_task_id: CAS-95
priority: low
type: enhancement
ordinal: 132000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`listDirectory` on a table dir (`R/ContentAddressedMetadataStorage.cpp:1801`) merges part names from the in-memory ref table with table-level files (`format_version.txt`, `mutation_*.txt`, `deduplication_logs/...`) that it LISTs via `listNamespaceFiles` (`:1853`, `:1900`).
`MergeTreeData::clearOldTemporaryDirectories` calls it once per table per minute: the constant 155 LISTs per 10 minutes on a 26-table stand (audit F16).
These files have no index; `CasPlainObjects::putNamespaceFile`/`removeNamespaceFile` (`R/Pool/CasPlainObjects.cpp:44,72`) are the only writers, and the namespace life includes `server_root_id`, so only this node writes them.
Fix: one LIST on first use after start, then write-through updates from those two writers.

Provenance: audit #f16 via BACKLOG/performance.md#scale-findings [startup O(refs)]. Verified 2026-09-26 against dd0ed2f263a and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A second `listDirectory` of the same table dir issues no S3 LIST
- [ ] #2 A file added or removed through `putNamespaceFile`/`removeNamespaceFile` is reflected in the next listing without a LIST
- [ ] #3 Steady-state `CASRootList` on an idle node with tables is zero per minute
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
First recorded: 2026-09-26 (27654231df2, by 'scale-findings [startup O')

Issue #2439 proposal 3, not part of this fix: record table-level file names (or the files) in the ref table next to the part refs, so a cold start needs no LIST either. It is an on-S3 format change (decision-4: new format version with a compatibility path), listed by the issue as a design question only.

2026-09-27: closed as not worth its mechanism after eight codex rounds on spec 2026-09-26-cas-directory-probes-no-list-design.md (see its section 0) and two rounds on the design study 2026-09-26-cas-table-files-as-refs-design.md. Root cause: _files/ PUT and DELETE carry no seal and the backend gives no bound on when an accepted request is applied, so any resident copy of the names can name a deleted object after an unclean crash until the next mount. Reopen only together with sealed table-level files (format generation 2).

2026-09-27 VERDICT (user): doubtful, the cure is worse than the disease. Saving at stake: 155 LIST per 10 min on otel.demo (one per table per minute from clearOldTemporaryDirectories, 0.26 req/s); the restart storm of #2439 is CAS-95.1 and is unaffected.

Main challenges found over eight codex rounds (spec rev.1-rev.9) and a two-round design study:
1. A resident copy of the _files/ names must be kept exact against writers that go outside the ref lane (plain PUT/DELETE with HEAD-before-PUT). Lazy population races a concurrent write of the same life (rev.2 version counter: PUT and DELETE settle out of durable order, runtime ABA at install; rev.3-4 per-runtime mutex: does not survive the remount that detaches the runtime; rev.5-7 striped mutexes held across I/O: sound but heavy, plus admission re-check inside the stripe and ref-table budget accounting).
2. Population at mount (rev.8-9) removes the overlap but needs a closed gate against a deferred write-buffer finalize that admits under the new generation during remount population (a shared_mutex: ops shared, population exclusive), synchronous lease renewal during a long population, a catalog ambiguity check, and exact erasure on dropNamespace.
3. Stragglers of a dead predecessor: _files/ writes carry no seal (unlike ref-log writes, which lose to EpochSeal). The lease protocol bounds them in time (attempt envelope reserved against the lease deadline; unclean slot reclaimed after a full TTL of observed lapse), leaving only the assumption that the object store does not apply a request after the client gave up; the user accepts that assumption, the reviewer did not. A stale name for mutation_<n>.txt makes the table fail to load until the next mount, whereas today's per-probe LIST heals itself.
4. Durable directory entries in the ref log (design study rev.2): a format generation, LIST-vs-log authority contradictions (remove-before-delete plus resync resurrects files; explicit empty directories dropped by resync), still the same-name serialization, and partition_exports / tmp/ paths misparsed as parts (CAS-318 class).
5. Table-level files as refs plus dedup claims (design study rev.1): content migration not idempotent with existing primitives, one durable write per insert on deduplicating tables, MergeTree interface extraction larger than stated.

Where to look (all on cas-gc-rebuild):
- Spec history: docs/superpowers/specs/2026-09-26-cas-directory-probes-no-list-design.md, rev.1 2068160f80e ... rev.9 e2772b9ddad, closed 8d0bc675c1f, lease correction 998c89c0975; rev.10 keeps only 95.1.
- Codex reports r1-r8: docs/superpowers/reports/2026-09-26-cas-directory-probes-no-list-codex-reviews/
- Design study (files as refs; directory entries in the ref log): docs/superpowers/specs/2026-09-26-cas-table-files-as-refs-design.md, 2f0eccc7975, d4f9ea346ec, closed ec61a74790e; reports docs/superpowers/reports/2026-09-26-cas-table-files-as-refs-codex-reviews/
- Probe of non-MergeTree engines on CAS (Log family works via _files/, persistent Join/Set broken): docs/superpowers/reports/2026-09-27-cas-non-mergetree-engines-probe.md, CAS-318.
Reopen only with a decision on challenge 3 and, preferably, sealed table-level files (format generation 2).

2026-09-27 CANDIDATE (user): invalidate-on-write cache instead of write-through. Per life: names of the last LIST + valid flag + counter, in Pool under a plain mutex (no I/O under it). listDirectory/existsDirectory(TableSubdir) answer from memory while valid; otherwise LIST, install, set valid. putNamespaceFile/removeNamespaceFile bump the counter at START and at SETTLE (success or exception) and clear the flag; a LIST installs only if the counter is unchanged between its start and the install (a LIST overlapping any part of a write is dropped, not installed). Hits need fence admission (rev.7 rule); table cleared on lease loss; entry erased on dropNamespace; rebirth is a new key; predecessor stragglers covered by the lease protocol (accepted assumption). Removes everything rounds 2-8 fought over (callback order, runtime ABA, mount population, drain, shared_mutex, catalog cut, birth hook). Cost: one LIST after every _files/ write of that table, so deduplicating tables (a segment per insert) keep today's per-minute LIST; the saving is on tables without table-level writes between scans. Not yet reviewed by codex; the spec is narrowed to 95.1 (rev.10).
<!-- SECTION:NOTES:END -->

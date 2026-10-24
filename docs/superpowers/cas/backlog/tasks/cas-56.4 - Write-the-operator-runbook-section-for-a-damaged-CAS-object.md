---
id: CAS-56.4
title: Write the operator runbook section for a damaged CAS object
status: To Do
assignee: []
created_date: '2026-09-26'
updated_date: '2026-09-26 14:28'
labels:
  - 'area:docs'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:solid'
  - 'origin:soak'
milestone: m-7
dependencies:
  - CAS-56.1
  - CAS-56.3
  - CAS-32
references:
  - docs/en/antalya/cas/operations/troubleshooting.md
  - docs/en/antalya/cas/operations/debugging.md
parent_task_id: CAS-56
priority: medium
type: docs
ordinal: 70000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`docs/en/antalya/cas/operations/` has no section for a damaged object; `troubleshooting.md:22` covers only a dangling read.
The section must cover: how the condition announces itself (suppressed rounds naming the namespace, the fsck row, `CASRefNeedsRecovery`);
why there is no urgency (GC has frozen everything irreversible); the asymmetry (an absent `_ckpt` triggers cold recovery, a corrupt one
does not, so the last-resort move is to DELETE a damaged derived object, never hand-edit it); the repair sequence; what not to do
(`DROP POOL MEMBER` is for dead members; never hand-delete blob bodies or ref-log records; never restore bytes from an unofficial copy);
and when the answer is backup/restore. `_pool_meta` is restore-only: `pool_id` is a random u128 minted at creation, not derivable.

Provenance: BACKLOG/operability-and-introspection.md#damaged-object-repair item 4 and #pool-meta-bootstrap-blocks-dr-tools (c); verified 2026-09-26 against 59494ebf366 (cas-gc-rebuild) and 0dbbd797792 (altinity/antalya-26.6). The repair steps land once fsck-repair-derived-objects is done.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 The runbook section exists under `docs/en/antalya/cas/operations/` with an explicit anchor and covers every point above
- [ ] #2 Each command in it was run against a damaged pool in a test or soak scenario
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

Identifier trace: the earliest docs mention of `_pool_meta` is 2026-06-02 (cafc256906e); the finding itself has no record before this migration, so the creation date stays 2026-09-26.
<!-- SECTION:NOTES:END -->

---
id: DRAFT-3
title: Slot-bound blob hash `ch128ctx` against cross-slot dedup collisions
status: Draft
assignee: []
created_date: '2026-07-14'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:formats'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:plausible'
  - 'needs:decision'
  - 'needs:spec'
  - 'origin:review'
dependencies: []
references:
  - docs/superpowers/cas/2031-triage.md#cas-008
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
Middle tier between `cityHash128` and `sha256`: `cityHash128(content) || xxh3_64(part_name, file_name) || size` (256 bits; variable-width `BlobDigest` supports it). Closes the realistic adversarial vector: attacker-crafted content deduped into a victim's future blob, at ~zero cost.
Kept: relink/carry-forward (reference-based), retry idempotency, same-name replica writes, snapshot-upload to TTL-move prepayment (same slot). Lost: only cross-slot content coincidence, an explicit non-goal.
Main touch: the staged-blob hasher needs `(part_name, file_name)` before `ensureBlobPresent`. Not in code on either branch; 2031-triage CAS-008 rates today's selectable hash as by-design.
A new hash algorithm is a format change: decision-4 requires a new format version and a compatibility path.

Provenance: BACKLOG/performance.md#read-write [ch128ctx] (origin 10-backups.md multi-disk, 2026-07-14). Verified 2026-09-26 against aefe80eba98 and 0dbbd797792.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 Owner decides whether the threat model warrants the tier
- [ ] #2 A spec names the format version bump and how existing pools and `algos_used` accept the new algorithm
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
First recorded: 2026-07-14 (f54ef43ff5a, by 'ch128ctx')
<!-- SECTION:NOTES:END -->

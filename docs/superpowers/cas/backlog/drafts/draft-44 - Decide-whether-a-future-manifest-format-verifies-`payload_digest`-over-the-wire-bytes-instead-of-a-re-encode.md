---
id: DRAFT-44
title: >-
  Decide whether a future manifest format verifies `payload_digest` over the
  wire bytes instead of a re-encode
status: Draft
assignee: []
created_date: '2026-08-21'
updated_date: '2026-09-26 12:44'
labels:
  - 'area:formats'
  - 'complexity:medium'
  - 'risk:high'
  - 'touches:on-s3-format'
  - 'confidence:solid'
  - 'needs:decision'
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp
documentation:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md
priority: low
type: design
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`decodePartManifest` verifies `payload_digest` by re-encoding the decoded model (`computePayloadDigest`,
`CA/Formats/CasPartManifestFormat.cpp:296-301`). Two consequences: `cas_part_manifest` is registered `Tolerant`
(`CA/Formats/CasFormat.cpp:127`) but an unknown key cannot survive a digest of what the struct re-emits, so additive changes are
impossible for this format; and the re-encode cost 27-63% of decode time plus two transient copies (manifest cap 256 MiB).
Not reachable today: every object carries `G_BUILD` and there is no write-down policy.
Either fix (digest the wire bytes, or declare the format `Strict`) changes what the persisted field means, so it ships only as a
new format version with a compatibility path (decision-4). Decide together with the format-version rollout (CAS-117).

Provenance: BACKLOG/formats-and-storage.md#manifest-digest-by-reencode (2031-triage CAS-041); verified 2026-09-26 against 8b87aa15d21 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6).
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A recorded decision: wire-byte digest in the next manifest version, `Strict`, or keep as is
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
First recorded: 2026-08-21 (4268978c8f7, by 'manifest-digest-by-reencode')
<!-- SECTION:NOTES:END -->

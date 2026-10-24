---
id: CAS-306
title: >-
  Refuse `cas_staging_backend = s3` on an SSE-C disk at mount, or send
  copy-source SSE-C headers
status: To Do
assignee: []
created_date: '2026-09-26 12:54'
labels:
  - 'area:backend'
  - 'area:write-path'
  - 'complexity:small'
  - 'risk:low'
  - 'confidence:plausible'
  - 'needs:repro'
  - 'origin:2031-triage'
milestone: m-6
dependencies: []
references:
  - >-
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp
  - src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp
  - src/IO/S3/copyS3File.cpp
  - 'https://github.com/Altinity/ClickHouse/issues/2031'
documentation:
  - docs/en/antalya/cas/bucket-requirements.md
  - docs/en/antalya/cas/architecture/backend.md
priority: low
type: bug
ordinal: 385000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
With `cas_staging_backend = s3`, a staged blob is published by a native same-store copy (`ObjectStorageCopyMode::NativeOnly`, `CA/Backend/CasObjectStorageBackend.cpp:930`).
An object written with an SSE-C key can only be the source of a copy when the request carries the `x-amz-copy-source-server-side-encryption-customer-*` headers. No such header is sent anywhere in `src/` on either branch.
The writable-mount gate checks only `supportsCopyMode(NativeOnly)` (`CA/ContentAddressedMetadataStorage.cpp:859-869`), and `S3ObjectStorage::supportsCopyMode` looks only at `allow_native_copy` (`src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:777-782`).
So a disk with `server_side_encryption_customer_key_base64` and S3 staging mounts, and then every staged publish should fail loudly at copy time. The failure is fail-closed with no corruption.
The 2026-08-21 ledger assumed a mount probe that falls back to local staging; the mount now explicitly never substitutes local staging, so that premise is gone.
Cheapest fix (CAS-only): refuse the mount when S3 staging is combined with an SSE-C key, and name the combination in `bucket-requirements.md`. The alternative, copy-source headers in `copyS3File`, is generic S3 code and needs the upstream-consult step.

Provenance: docs/superpowers/cas/2031-triage.md#cas-090 (2031-triage CAS-090; the ledger's 'fail-closed probe falls back to local staging' is superseded by the NativeOnly mount gate). Verified 2026-09-26 against cae9288ee65 (cas-gc-rebuild) and 8d62c314ec1 (altinity/antalya-26.6). Not reproduced against an SSE-C bucket, hence needs:repro.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 A writable mount with `cas_staging_backend = s3` and an SSE-C key either refuses with a message naming both settings, or publishes staged blobs successfully
- [ ] #2 A test against an S3 service that enforces SSE-C (MinIO or RustFS with KMS/SSE-C enabled) covers the chosen behaviour
- [ ] #3 `docs/en/antalya/cas/bucket-requirements.md` states how SSE-C interacts with S3 staging
<!-- AC:END -->

## Definition of Done
<!-- DOD:BEGIN -->
- [ ] #1 CAS* gtest gate green (utils/cas-gate); LOGICAL_ERROR expectations are death tests
- [ ] #2 ASan lane green for touched suites
- [ ] #3 Docs updated where user-visible (docs/en/antalya/cas) and the spec if the on-S3 format is touched (frozen since 26.6.4: new version + compatibility path)
- [ ] #4 No fallback paths added; failures propagate
<!-- DOD:END -->

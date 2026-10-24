---
id: CAS-177
title: >-
  Stop a CAS write failure inside `noexcept` MergeTree-transaction callbacks
  from terminating the server (land PR #2396)
status: In Progress
assignee:
  - '@filimonov'
created_date: '2026-09-16'
updated_date: '2026-09-30 20:09'
labels:
  - 'area:write-path'
  - 'area:upstream'
  - 'complexity:medium'
  - 'risk:medium'
  - 'touches:upstream-code'
  - 'confidence:solid'
  - 'origin:issue'
milestone: m-8
dependencies: []
references:
  - 'https://github.com/Altinity/ClickHouse/pull/2396'
  - 'https://github.com/Altinity/ClickHouse/issues/2344'
  - src/Interpreters/MergeTreeTransaction.cpp
  - R/ContentAddressedTransaction.cpp
documentation:
  - >-
    docs/superpowers/specs/2026-09-16-transaction-metadata-store-best-effort-design.md
priority: critical
type: bug
ordinal: 234000
---

## Description

<!-- SECTION:DESCRIPTION:BEGIN -->
`MergeTreeTransaction::afterCommit` and `rollback` are `noexcept` and write `txn_version.txt` into committed parts; on a CAS disk that is a transaction over a live ref with six throwing steps.
Observed: PR #2300 CI, ASan cas-s3, "Terminate called" on `COMMIT` in `01169_old_alter_partition_isolation_stress` after a routine "append lane not Ready" refusal.
Point fix on cas-gc-rebuild only: `ContentAddressedTransaction::abandonBuildBestEffort` (`fb035142262`, tests `CASCommitRollback.AbandonRefused*DoesNotFailCommit`).
antalya-26.6 lacks it: its `publishStaging` still calls a throwing `st.build->abandon()` after the repoint (`R/ContentAddressedTransaction.cpp:413` at `0dbbd797792`).
Class fix: PR #2396 (open, base antalya-26.6) retries the six metadata writes in one helper for up to 60 s and rethrows on exhaustion or shutdown. Closes #2344.
Rejected: catching at each CAS throw point (hides a lost durable write) and retrying under the lease (turns the abort into a minutes-long `COMMIT`).
The CAS-side surface reduction (one repoint instead of a scratch build) lives in `CAS-15`.

Provenance: BACKLOG/ref-protocol.md#cas-txn-commit-inside-noexcept-aftercommit. Verified 2026-09-26 against b1c34d03479 (point fix fb035142262 is an ancestor) and 0dbbd797792 (point fix absent); PR #2396 state OPEN via gh.
<!-- SECTION:DESCRIPTION:END -->

## Acceptance Criteria
<!-- AC:BEGIN -->
- [ ] #1 PR #2396 is merged, or its replacement is, with the failpoint test `05053_transaction_metadata_store_retry` green
- [ ] #2 antalya-26.6 no longer calls a throwing `abandon` after a durable repoint in `publishStaging` (point fix ported or made unnecessary by the class fix, stated in the PR)
- [ ] #3 A forced transient CAS refusal during `COMMIT` on a CAS disk leaves the server running and the transaction visible
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
2026-09-30 upstream forward-port opened: https://github.com/ClickHouse/ClickHouse/pull/123134 (branch filimonov/fix/transaction-metadata-write-retry = 246746d8732 over upstream/master cce42c6bde0; one commit, no fork wording). Upstream differences: rollback swallows errors since #117822 (RollbackResult::Failed), so the termination is only in afterCommit there; test renamed 05315; red-first proven by rebuilding without the retry (server terminates via afterCommit). Upstream history: #99494 (catch-all, rejected: lost CSN write blocks log cleanup), #84084 / #92141 (the Append trigger), #115501 open.
<!-- SECTION:NOTES:END -->

---
description: 'Audit: CA transient throw sites vs upstream retryable classifiers — plane separation, blast-radius comparison of fix directions, recommendation.'
sidebar_label: 'CA transient classifier audit'
sidebar_position: 75
slug: /superpowers/reports/2026-07-29-ca-transient-classifier-audit
title: 'CA transients vs upstream retryable classifiers'
doc_type: 'reference'
---

# CA transients vs upstream retryable classifiers {#ca-transient-classifier-audit}

Trigger: the lease-blip/part-check collapse RCA (row 12d of the Stage A gate) found
`ReplicatedMergeTreePartCheckThread` consuming the CA disk's transient `INVALID_STATE`
("mount lease not held") as "part looks broken" because `isRetryableException`
(`checkDataPart.cpp`) does not list it. The user asked two questions: are there more
cases like this, and would it be simpler for CA to throw an error code upstream already
treats as retryable (`NETWORK_ERROR` with clarifying text) instead of editing upstream
classifier lists. Method: three-way audit (this agent + two relayed sub-audits over
`Formats/` and `Gc/`+`CasRefLedger`+`CasRefProtocol`), reconciled counts, all greps
untruncated.

## Headline numbers {#headline}

- `Formats/`: **217 throw sites, zero transient** — pure wire codecs, nothing consults live state.
- Whole CA tree: **~58 transient throw sites**. **50 already land in an upstream-retryable
  class** (32 `NETWORK_ERROR` via the `throwCasWriteRetryLater` /
  `makeCasWriteRetryLaterExceptionPtr` family, 18 `ABORTED`). **5 use `INVALID_STATE`**,
  **3** `ABORTED`-in-GC.
- Plane separation (the decisive analysis): of ~58 transient sites, **exactly 2 can produce a
  destructive misclassification** — both on the READ plane, both in
  `ContentAddressedMetadataStorage.cpp`: `:1111` (`TransientNotLive` — the collapse site) and
  `:1163` (probe `Indeterminate`). The Write plane cannot destroy (a failed commit leaves no
  committed part to declare broken; the one Write-plane consumer is zero-copy-guarded and CA is
  not zero-copy). The Mount plane is contained by type (`MountFencedException` caught at
  `CasPool.cpp:687`). The GC plane is contained by `catch (...)` in `CasGcScheduler`.

## Blast radius per fix direction {#blast-radius}

| Direction | Sites fixed | Unintended reclassifications | Upstream files touched |
|---|---|---|---|
| (i) add `INVALID_STATE` to `isRetryableException` | 2 | **18** (8 terminal CA `INVALID_STATE` sites + 8 non-CA: KeeperMap, NATS, TemporaryDataOnDisk + `CasPool.cpp:444`) | 1 |
| (ii) re-code the transient sites to `NETWORK_ERROR` | 2 required + 1 uniformity | **0** | **0** |
| (iii) dedicated new ErrorCode | same 3 | 0 | 2 lines / 2 shared files (violates the rebase-conflict policy, `CasServerRoot.h:359-361`) |

## Recommendation {#recommendation}

**Direction (ii), narrowed to 3 throw sites.** Add `throwCasTransientUnavailable` beside
`throwCasWriteRetryLater` (`Backend/CasRequestControl.cpp:190-201`), minting `NETWORK_ERROR`
with the lease-gap message; route `ContentAddressedMetadataStorage.cpp:1111`, `:1163` (the
only two sites that can destroy data) plus `CasMountRuntime.cpp:106` (Write-plane uniformity —
its 32 sibling sites already use `NETWORK_ERROR`). Zero upstream edits; finishes a migration
the CA layer has already performed 50 times. Cost, stated honestly: `NETWORK_ERROR` is coarser
than the truth — CA lease gaps and socket errors share a `system.errors` row (HTTP status,
`Exception::isErrorCodeImportant`, connection failover and the S3/HTTP retry layers are all
unaffected). Fail-close: affected consumers move from "detach the part" to "rethrow and retry
later" (`ReplicatedMergeTreePartCheckThread.cpp:653-658` keeps the part queued); terminal CA
errors keep their codes and behaviour.

Carry-alongs for the fix: `utils/ca-soak/soak/cluster.py:215`;
`gtest_cas_operation_gate.cpp:182-217` (shared-code contract asserting both `TransientNotLive`
and `IdentityLost` throw 668 — must split); `gtest_cas_fsck.cpp:669`;
`gtest_cas_gc_stop_start.cpp:472`;
`docs/superpowers/specs/2026-07-22-cas-disk-lease-loss-throw-and-stop-verbs-design.md:56`.
Zero `tests/` references.

## Adjudicated flags (separate filings, zero cross-product rows) {#flags}

1. `CasGc.cpp:718` — an undecodable WRITE-ONCE outcome log coded `ABORTED` (retry): the bytes
   never change, so this buys an unbounded GC-round retry. Real CA-internal defect; should be
   `CORRUPTED_DATA`. Contrast `:808` where `ABORTED` is correct (gc/state genuinely moved).
2. `CasBlobInDegree.cpp:289` vs `pruneSupersededGenerations` (`CasGc.cpp:3217`, called `:802`):
   a deposed leader mid-read of a superseded generation could see benign absence reported as
   `CORRUPTED_DATA`. GC-plane, contained; plausible residual window worth confirming impossible.
3. `CasRefLedger.cpp:1014` — NOT a defect; the "Terminal" comment scopes to the inner brake
   latch, and the `:1010` (proven hole ⇒ `CORRUPTED_DATA`) vs `:1014` (churn ⇒ retry-later)
   split is principled.
4. `INVALID_STATE` deposition family (`CasRefLedger.cpp:2041,2312,2892`) — LEAVE ALONE:
   Write-plane, no destructive consumer; queue backoff engages correctly today because
   `INVALID_STATE` is absent from `ReplicatedMergeMutateTaskBase.cpp:65-78`'s exemption set;
   re-coding to `ABORTED` would actively regress (the exact bug `CasRequestControl.h:233-239`
   documents).
5. `checkFenceOrThrow` (`CasMountRuntime.cpp:98-110`) cannot distinguish a lease blip from a
   FORGET decommission — real ambiguity, but NOT the collapse's root (`checkOpAdmitted` DOES
   distinguish: `TransientNotLive` vs `throwIfLifecycleTerminal`). Kept in the fix set for
   plane uniformity only.
6. `CasServerRoot.cpp:261` (100-attempt CAS loop coded `CORRUPTED_DATA`; siblings use
   `ABORTED`) and `:241` (`ContainerAbsent`/`AccessDenied`/`Indeterminate` bundled as
   corruption) — ErrorCode-consistency mistags, Mount-plane, contained.

## The second Read-plane hole {#format-version-hole}

`UNKNOWN_FORMAT_VERSION` version-skew (`Formats/CasFormat.cpp:79`,
`CasPoolMetaFormat.cpp:111,164`, `CasTextFormat.cpp:250`) is on the READ plane and absent from
`isRetryableException`: during a rolling upgrade an older build reading a newer manifest would
detach parts as broken. Benign under the pre-release / no-persisted-data posture; must be
tracked before that posture changes.

## Inventory additions {#inventory}

`isTransientRecoveryError` (`CasRefLedger.cpp:89-97`) — CA-internal classifier
(`NETWORK_ERROR, S3_ERROR, POCO_EXCEPTION, SOCKET_TIMEOUT, CANNOT_READ_FROM_SOCKET,
TIMEOUT_EXCEEDED`), guards the ref-recovery re-drive at `:1085`; denylist-shaped with an
unsafe default, same structural hazard as `isRetryableException`, but not reachable from the
lifecycle-gate throw (recovery reads bypass `ref_request_controller` per `:84-87`).

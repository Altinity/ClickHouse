---
description: 'TLC evidence for split-phase conditional CAS mount renewal retries, exact sabotages, and non-vacuity witnesses'
sidebar_label: 'CAS mount renewal retry TLC results'
sidebar_position: 5
slug: /superpowers/models/CaMountRenewRetryCore-results
title: 'CAS mount renewal retry TLC results'
doc_type: 'reference'
---

# `CaMountRenewRetryCore` results {#camountrenewretrycore-results}

## Verdict {#verdict}

The complete focused gate passed. The asserted runner printed `ALL EXPECTATIONS MET` and
`EXECUTED ROWS: 17/17`: all ten single-defect configurations violated their exact mapped
invariant, the honest configuration exhausted its reachable state graph without an error, and all
six negated-reachability witnesses fired while every stable safety invariant remained enabled.

## Checker and finite scope {#checker-and-finite-scope}

- TLC: `2026.07.18.145032`, revision `30cc360`.
- Pinned `tla2tools.jar` SHA-256:
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Workers: `1`, selected by the runner default because `TLC_WORKERS` was unset.
- Attempt IDs: `attempt1` and `attempt2`; current attempt: `attempt1`.
- Holders: `self` and `foreign`.
- Tokens: predecessor, renewal, same-pair twin, successor, and foreign tokens.
- Time: `0..4`; initial confirmed deadline: `3`; request timeout: `1`; lease duration: `4`;
  cadence period: `2`.
- Retry bound: one retry; outstanding-copy bound: two.
- TLC row logs and private metadata directories were stored below the repository `tmp` directory.
- The runner passes `-noGenerateSpecTE`, so TLC does not create source-adjacent trace modules.

The model stores durable holder/body/token/deadline state separately from local confirmation and
authority. One logical request captures a symbolic `write_attempt_id`, body, expected predecessor
token, deadline, and cadence anchor. Its physical copies are represented only by an outstanding
count, so honest retry cannot manufacture a different tuple. A conditional landing consumes the
predecessor token. The multiplicity sabotage is enabled only after the count reaches two and then
lets both copies replace that same predecessor, which makes the reduction's atomicity dependency
explicit.

## Stable safety invariants {#stable-safety-invariants}

The honest and witness configurations check these exact stable names:

- `ExactAttemptOnly`
- `ForeignOrSuccessorNeverAdopted`
- `ConfirmedDeadlineNeverExtendedByResponse`
- `NoRequestAfterSafeDeadline`
- `TerminalNeverRearmsAuthority`
- `OneLogicalBodyPerExpectedToken`
- `AcknowledgedRenewalIsDurable`
- `LateDeliveryCannotOverwriteSuccessor`
- `PendingSurvivesLocalTerminal`
- `OneIncarnationPerPredecessor`
- `CadenceAnchoredAtAttemptStart`

`PendingSurvivesLocalTerminal` is a transition-history guard: `LocalTerminalize` may not decrement
an already-positive outstanding count. It does not assert that every path to local `Unresolved`
must have a pending physical copy.

## Recorded complete run {#recorded-complete-run}

Command output: `tmp/run_mountrenewretry.log`. TLC row-log run ID:
`CaMountRenewRetryCore-3-1787523035326396663`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Depth | Seconds |
|---|---|---|---:|---:|---:|---:|
| `CaMountRenewRetryCore_sab_ignore_attempt_id.cfg` | violation | `ExactAttemptOnly` | 994 | 514 | 6 | 0 |
| `CaMountRenewRetryCore_sab_refresh_deadline_from_response.cfg` | violation | `ConfirmedDeadlineNeverExtendedByResponse` | 175 | 122 | 5 | 0 |
| `CaMountRenewRetryCore_sab_retry_with_new_body.cfg` | violation | `OneLogicalBodyPerExpectedToken` | 915 | 477 | 6 | 0 |
| `CaMountRenewRetryCore_sab_accept_after_terminal.cfg` | violation | `TerminalNeverRearmsAuthority` | 1,118 | 560 | 6 | 0 |
| `CaMountRenewRetryCore_sab_accept_successor.cfg` | violation | `ForeignOrSuccessorNeverAdopted` | 361 | 210 | 5 | 0 |
| `CaMountRenewRetryCore_sab_drop_pending_on_terminal.cfg` | violation | `PendingSurvivesLocalTerminal` | 57 | 50 | 4 | 1 |
| `CaMountRenewRetryCore_sab_late_rearm.cfg` | violation | `TerminalNeverRearmsAuthority` | 304 | 193 | 5 | 0 |
| `CaMountRenewRetryCore_sab_response_relative_cadence.cfg` | violation | `CadenceAnchoredAtAttemptStart` | 774 | 424 | 6 | 0 |
| `CaMountRenewRetryCore_sab_send_after_deadline.cfg` | violation | `NoRequestAfterSafeDeadline` | 93 | 64 | 5 | 1 |
| `CaMountRenewRetryCore_sab_double_conditional_landing.cfg` | violation | `OneIncarnationPerPredecessor` | 2,813 | 1,222 | 7 | 0 |
| `CaMountRenewRetryCore_safe.cfg` | green | `green` | 139,763 | 34,921 | 21 | 1 |
| `CaMountRenewRetryCore_witness_direct_retry.cfg` | witness | `WitnessDirectRetry` | 6,773 | 2,555 | 8 | 0 |
| `CaMountRenewRetryCore_witness_read_adoption.cfg` | witness | `WitnessReadAdoption` | 1,060 | 526 | 6 | 1 |
| `CaMountRenewRetryCore_witness_exhaustion_fences.cfg` | witness | `WitnessExhaustionFences` | 1,278 | 602 | 7 | 0 |
| `CaMountRenewRetryCore_witness_late_before_reclaim.cfg` | witness | `WitnessLateBeforeReclaim` | 3,301 | 1,357 | 7 | 0 |
| `CaMountRenewRetryCore_witness_late_after_successor.cfg` | witness | `WitnessLateAfterSuccessor` | 1,162 | 570 | 6 | 1 |
| `CaMountRenewRetryCore_witness_catchup.cfg` | witness | `WitnessImmediateCatchup` | 2,209 | 1,007 | 7 | 0 |

The honest graph completed with zero states left on the queue. No row timed out, deadlocked, failed
to parse, reached an unexpected invariant, or relied on an unlisted configuration.

## Witness audit {#witness-audit}

- `WitnessDirectRetry` follows `SendRenewal` → non-landed `ResponseLoss` → `ExactResolve` of the
  predecessor → `EnterRetryWait` → `RetrySend` → `LandCopy` → `AcknowledgeRenewal`.
- `WitnessReadAdoption` lands the original copy, loses its response, performs `ExactResolve`, and
  accepts only the body with the exact current attempt ID and bytes.
- `WitnessExhaustionFences` advances until the absolute budget cannot fit another request, reaches
  local `Unresolved`, and leaves local authority false.
- `WitnessImmediateCatchup` delays success until the attempt-start cadence is already due, then
  schedules that anchored beat at or before the current time.

Every witness configuration lists all eleven stable safety invariants plus its one negated
reachability property.

## Late-delivery witness narration {#late-delivery-witness-narration}

`WitnessLateBeforeReclaim` takes this seven-state path:
`SendRenewal` → `Cancel` → `LocalTerminalize` → `LateDeliveryLands` → `GCFence` → `Reclaim`.
`LocalTerminalize` first records local `Unresolved`, preserves one outstanding copy, and fences
authority. The copy then conditionally consumes the predecessor and durably extends the old-epoch
body while authority remains false. Recovery observes that body conservatively through `GCFence`
before `Reclaim`; the delivery is not treated as a new local lease.

`WitnessLateAfterSuccessor` takes this six-state path:
`SendRenewal` → `Cancel` → `LocalTerminalize` → `SuccessorClaim` → `LateDeliveryRefused`.
Local `Unresolved` again precedes delivery and preserves one pending copy. The successor changes the
durable token first. `LateDeliveryRefused` then consumes the physical delivery outcome, records an
explicit old-copy refusal, leaves the successor body/token unchanged, and keeps old local authority
false. The outstanding message is not silently deleted by local terminalization.

## Reproduction commands {#reproduction-commands}

Run from the repository root:

```bash
docs/superpowers/models/run_mountrenewretry.sh > tmp/run_mountrenewretry.log 2>&1
sha256sum tmp/tla2tools.jar
```

The first command exited `0`. Its final lines were:

```text
EXECUTED ROWS: 17/17
ALL EXPECTATIONS MET
```

## Abstraction boundary {#abstraction-boundary}

No formal refinement to atomic `CaCasMountCore.Renew` is claimed. The focused model owns landed but
locally unconfirmed states that the atomic action deliberately abstracts away. When the focused
operation reaches local `Committed`, its durable body/token and authority satisfy the intended
atomic postconditions; terminal focused outcomes leave old authority fenced at the existing remount
boundary. This correspondence is an audit aid, not a machine-checked refinement.

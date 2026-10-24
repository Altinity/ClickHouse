---
description: 'TLC evidence for CAS mount renewal without a lease deadline: retries, two renewals, exact sabotages, and non-vacuity witnesses'
sidebar_label: 'CAS mount renewal retry TLC results'
sidebar_position: 5
slug: /superpowers/models/CaMountRenewRetryCore-results
title: 'CAS mount renewal retry TLC results'
doc_type: 'reference'
---

# `CaMountRenewRetryCore` results {#camountrenewretrycore-results}

## Verdict {#verdict}

The battery passed: `EXECUTED ROWS: 19/19` and `ALL EXPECTATIONS MET`.

- Eleven single-defect configurations each violate their mapped invariant.
- The honest configuration exhausts its state graph with no error.
- Seven negated-reachability witnesses fire with all twelve safety invariants enabled.

## What the model covers {#what-the-model-covers}

The renewal is not bounded by the lease. It sends and retries past the confirmed deadline and
ends only on success, a definitive answer, or a cancel. There is no exhaustion transition:
`MaxRetries` and `MaxOutstanding` bound the state space and nothing else.

- **Authority is the holder's own timer.** `LocalAuthority` is `~fenceLost /\ clock <
  confirmedDeadline`. Only `LocalTerminalize` sets `fenceLost`. Observer actions never touch
  local state.
- **Deadline from the start.** A commit sets `confirmedDeadline` to `attemptStartedAt +
  LeaseDuration`. `firstSendAt` records each renewal's first send. Only `SendRenewal` writes it.
- **Two renewals.** After a commit, `ScheduleNextBeat` and `SendRenewal` start the second renewal
  with a new attempt ID, expected token and start. `SendRenewal` needs no authority. Copies of the
  first renewal still in flight become `staleOutstanding`. `StaleCopyDelivery` lands one only if
  the first renewal's expected token is still current.
- **Observer entitlement.** Every durable write sets `tokenStableUntil` to `clock +
  LeaseDuration`. A takeover by `GCFence`, `SuccessorClaim` or `ForeignHolderWrite` is entitled
  once `clock >= tokenStableUntil`. `SuccessorClaim` is also entitled once the old runtime is
  locally terminal. Observers still run at any time. Unentitled takeovers stay in the graph as
  adversarial perturbations.

`sab_send_after_deadline`, `NoRequestAfterSafeDeadline` and `WitnessExhaustionFences` were
removed. Sending after the deadline is the intended behaviour, and exhaustion no longer exists.

## Checker and finite scope {#checker-and-finite-scope}

- TLC: `2026.07.18.145032`, revision `30cc360`, one worker.
- Pinned `tla2tools.jar` SHA-256:
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`.
- Attempt IDs: `attempt1` (first renewal), `attempt2` (second), `attempt3` (twin, successor,
  foreign).
- Tokens: predecessor, two renewal tokens, same-pair twin, successor, foreign.
- Time `0..5`. Initial confirmed deadline `3`. Lease duration `4`. Cadence period `2`.
- One retry per renewal. At most two outstanding copies.
- `MaxTime = 5` admits a first success at clock `5` from a start at `0`: `start + LeaseDuration`
  is strictly past.

## Safety invariants {#safety-invariants}

The honest and witness configurations check these twelve:

- `ExactAttemptOnly`: an adopted attempt ID is the committed renewal's own.
- `ForeignOrSuccessorNeverAdopted`: twins, fenced, successor and foreign bodies are never
  adopted.
- `ConfirmedDeadlineNeverExtendedByResponse`: a commit sets the deadline from the start it used,
  never from the response time.
- `TerminalNeverRearmsAuthority`: after a local terminal, authority stays false.
- `OneLogicalBodyPerExpectedToken`: every copy of a renewal keeps its attempt, body and expected
  token. The first renewal expects the predecessor token, the second expects the first renewal's
  token.
- `AcknowledgedRenewalIsDurable`: a direct acknowledgement needs the exact body to be durable.
- `LateDeliveryCannotOverwriteSuccessor`: a post-terminal copy never replaces a successor.
- `PendingSurvivesLocalTerminal`: `LocalTerminalize` does not drop outstanding copies. This is a
  transition-history guard. It does not claim every unresolved result has pending I/O.
- `OneIncarnationPerPredecessor`: at most one copy consumes each renewal's expected token. Stale
  copies count against the first renewal.
- `CadenceAnchoredAtAttemptStart`: the next beat is the renewal's first send plus the period.
- `AuthorityEndsAtConfirmedDeadline`: the confirmed deadline is the initial one before any
  commit, then `firstSendAt` of the committed renewal plus the lease. The conjunct "authority
  implies `clock < confirmedDeadline`" holds by the definition of `LocalAuthority`.
- `NoAuthorityAfterEntitledTakeover`: once an entitled takeover happened, the holder has no
  authority.

## Recorded complete run {#recorded-complete-run}

TLC row-log run ID: `CaMountRenewRetryCore-2694715-1790889808360628074`.

| Configuration | Expected | Exact observed result | Generated | Distinct | Depth |
|---|---|---|---:|---:|---:|
| `sab_ignore_attempt_id` | violation | `ExactAttemptOnly` | 1,160 | 691 | 6 |
| `sab_refresh_deadline_from_response` | violation | `ConfirmedDeadlineNeverExtendedByResponse` | 187 | 143 | 5 |
| `sab_response_deadline_outlives_observer` | violation | `NoAuthorityAfterEntitledTakeover` | 26,627 | 10,559 | 9 |
| `sab_anchor_deadline_at_retry` | violation | `AuthorityEndsAtConfirmedDeadline` | 23,893 | 9,876 | 9 |
| `sab_retry_with_new_body` | violation | `OneLogicalBodyPerExpectedToken` | 1,074 | 642 | 6 |
| `sab_accept_after_terminal` | violation | `TerminalNeverRearmsAuthority` | 1,330 | 766 | 6 |
| `sab_accept_successor` | violation | `ForeignOrSuccessorNeverAdopted` | 401 | 260 | 5 |
| `sab_drop_pending_on_terminal` | violation | `PendingSurvivesLocalTerminal` | 59 | 53 | 4 |
| `sab_late_rearm` | violation | `TerminalNeverRearmsAuthority` | 324 | 230 | 5 |
| `sab_response_relative_cadence` | violation | `CadenceAnchoredAtAttemptStart` | 901 | 562 | 6 |
| `sab_double_conditional_landing` | violation | `OneIncarnationPerPredecessor` | 3,821 | 1,942 | 7 |
| `safe` | green | `green` | 11,942,930 | 2,775,783 | 33 |
| `witness_direct_retry` | witness | `WitnessDirectRetry` | 10,986 | 4,911 | 8 |
| `witness_read_adoption` | witness | `WitnessReadAdoption` | 1,272 | 726 | 6 |
| `witness_late_before_reclaim` | witness | `WitnessLateBeforeReclaim` | 4,626 | 2,240 | 7 |
| `witness_late_after_successor` | witness | `WitnessLateAfterSuccessor` | 1,390 | 790 | 6 |
| `witness_catchup` | witness | `WitnessImmediateCatchup` | 2,971 | 1,573 | 7 |
| `witness_retry_past_deadline` | witness | `WitnessRetryPastDeadline` | 13,661 | 5,887 | 9 |
| `witness_authority_restored` | witness | `WitnessAuthorityRestored` | 234,410 | 78,639 | 13 |

Configuration files are `CaMountRenewRetryCore_<name>.cfg`. The honest graph completed with zero
states left on the queue.

## Sabotages on the deadline {#sabotages-on-the-deadline}

- `sab_anchor_deadline_at_retry` moves `attemptStartedAt` to the retry's send. The commit then
  agrees with its own anchor, so `ConfirmedDeadlineNeverExtendedByResponse` holds. Only
  `AuthorityEndsAtConfirmedDeadline` catches it, by comparing with `firstSendAt`. Trace:
  `SendRenewal` at 0 → `ResponseLoss` → `ExactResolve` (predecessor) → `EnterRetryWait` →
  `RetrySend` at 1 → `LandCopy` → `AcknowledgeRenewal` with deadline 5 instead of 4.
- `sab_response_deadline_outlives_observer` uses the response-time defect against the observer.
  Trace: `SendRenewal` and `LandCopy` at 0 → `AcknowledgeRenewal` at 4 with deadline 8 →
  entitled `GCFence` at 4 while the holder still has authority.

## Witness audit {#witness-audit}

- `WitnessDirectRetry`: `SendRenewal` → `ResponseLoss` (not landed) → `ExactResolve`
  (predecessor) → `EnterRetryWait` → `RetrySend` → `LandCopy` → `AcknowledgeRenewal`.
- `WitnessReadAdoption`: `SendRenewal` → `LandCopy` → `ResponseLoss` → `ExactResolve` →
  `AcceptResolved` of the exact attempt ID and bytes.
- `WitnessImmediateCatchup`: success at clock 2 from a start at 0, then `ScheduleNextBeat`
  schedules a beat that is already due.
- `WitnessRetryPastDeadline`: the first renewal starts at 3, the confirmed deadline. Its read
  finds the predecessor and `RetrySend` fires at 3.
- `WitnessAuthorityRestored`: `SendRenewal` at 0, the copy lands at 5, `AcknowledgeRenewal` at 5
  confirms deadline 4, and authority stays false. `ScheduleNextBeat` gives beat 2, so
  `SendRenewal` starts the second renewal at 5 without authority. It lands and commits deadline
  9, and authority returns.
- `WitnessLateBeforeReclaim`: `SendRenewal` → `Cancel` → `LocalTerminalize` →
  `LateDeliveryLands` → `GCFence` → `Reclaim`. The late copy extends the old body while authority
  stays false.
- `WitnessLateAfterSuccessor`: `SendRenewal` → `Cancel` → `LocalTerminalize` → `SuccessorClaim` →
  `LateDeliveryRefused`. The successor token is unchanged and old authority stays false.

`WitnessRetryPastDeadline` and `WitnessAuthorityRestored` were checked against a scratch copy
with the budget guards restored (`CanStartRequest` on both sends, `clock <= 3` on both commits).
Both came back `green`, which the runner rejects for a witness row. `WitnessDirectRetry` still
fired as a control.

## Not modelled {#not-modelled}

- Clock drift: the observer waits exactly one lease on the same clock. `CaCasMountCore` owns the
  observer's timing with drift.
- Write admission margin: authority is `clock < confirmedDeadline`, without the time a write
  reserves.
- Retry spacing, the memory guard, reporting.
- A third renewal: the second commit ends the renewal sequence.

## Reproduction commands {#reproduction-commands}

Run from `docs/superpowers/models`:

```bash
TLC_JAVA_OPTS=-Xmx16g nice -n 10 ./run_mountrenewretry.sh > ../../../tmp/run_mountrenewretry.log 2>&1
```

The command exited `0`. Its final lines were:

```text
EXECUTED ROWS: 19/19
ALL EXPECTATIONS MET
```

## Abstraction boundary {#abstraction-boundary}

No formal refinement to atomic `CaCasMountCore.Renew` is claimed. The focused model owns landed but
locally unconfirmed states that the atomic action abstracts away. When the focused operation
reaches local `Committed`, its durable body/token and deadline satisfy the atomic postconditions;
terminal focused outcomes trip the fence at the existing remount boundary. This correspondence is
an audit aid, not a machine-checked refinement.

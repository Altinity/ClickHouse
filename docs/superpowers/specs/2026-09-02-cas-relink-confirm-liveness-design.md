---
description: 'Design for making the CAS fetch-by-relink confirm live under sustained write load: rule 3 refuses only the sent transaction that touches the queried ref, not every activity of the lane'
sidebar_label: 'CAS relink confirm liveness'
sidebar_position: 1
slug: /superpowers/specs/cas-relink-confirm-liveness-design
title: 'CAS relink confirm liveness design'
doc_type: 'design'
---

# CAS relink confirm liveness design {#cas-relink-confirm-liveness-design}

Status: revision 3 of 2026-09-02. Revision 1 relied on the part state machine (gate 0) to carry the
removal argument; review found the counterexample of a live repoint that retires a blob edge while the
part stays `Active`, and revision 1 is withdrawn. Revision 2 (rule 3 scoped to the sent transaction)
passed two independent consults (one on another model) on the design itself; this revision folds in
their prose findings: the model extension is now specified, the existing rule-3 tests that the design
reverses are named with their new expectations, and the citations are corrected. The TLA+ variant and
the code change remain to be done. Ledger finding F11, backlog item `[relink-confirm-lane-livelock]`
in `docs/superpowers/cas/BACKLOG/gcs.md`.

## The problem in one paragraph {#problem}

Same-pool replication transfers only a part's manifest. The receiver publishes its own ref over the
sender's blobs (T1, durable), asks the sender one read-only question, "do you still hold exactly this
manifest for this part?" (T2), and promotes only on `Yes` (T3). On the live GCS stand on 2026-09-02,
two replicas answered each other `Unknown` almost every time for forty minutes: both replication queues
wedged at 1.5-1.7k entries, the replicas diverged to 123k against 166k rows, and the soak died on
`SYSTEM SYNC REPLICA`. Nothing was lost; once one side stopped fetching the other drained in two
minutes. The refusal came from rule 3 of `CasRefLedger::confirmExactRef`
(`Pool/CasRefLedger.cpp:480`):

```cpp
if (rt.lane_state != RefLaneState::Ready || !rt.pending.empty() || rt.leader_active)
    return ConfirmAnswer::Unknown;
```

It refuses a question about part P whenever anything at all is happening in P's namespace: a queued
append for another part, a leader tenure, a `PUT` in flight, the checkpoint frontier being published.
Every replica is also a receiver, and each failed fetch appends two records to its own lane (a
precommit at T1 and its removal at abort), so under load neither side ever sees the other's lane idle.
On GCS the busy stretch is long because every flush ends with a token-CAS on the namespace's `_ckpt`,
which GCS limits to about one mutation per second per object, and `RefLaneState::Writing` spans the
ref-log `PUT`, that publication with its backoff, and the install. On RustFS in a LAN the same stretch
is milliseconds, which is why no earlier soak saw the defect.

## What rule 3 protects {#safety}

One hazard: the sender's in-memory committed row lags its own durable journal. The leader does not
hold `state_mutex` across the `PUT`, and a transaction is installed into the row only after its
checkpoint frontier is published, so from the moment the store applies a transaction until the leader
installs it the row is stale. If that transaction retired the last protector of a blob (a whole-part
removal, or a repoint of a live part's ref from manifest `m1` to `m2` that drops a blob `m1` referenced)
and became durable before the receiver's T1, GC may have decided the blob's deletion before the
receiver's `+1` existed. A confirm that reads the stale row answers `Yes` about `m1`, and the receiver
promotes a manifest whose blob is gone. `docs/superpowers/models/CaRelinkConfirmCore.tla` records this
as the `_sab_stalecache` trace; `SenderAdmit(Other)` in that trace is exactly the live repoint, which
happens without any change of the part's `MergeTree` state (`ContentAddressedTransaction.cpp:352-405`,
standalone writes and removals on a committed ref through `publishStaging` and `repointRef`).

The hazard is therefore about the transaction that has been sent and not yet installed. It is not
about queued appends that were never sent, and it is not about other refs. A queued removal of the
queried ref that has not been sent becomes durable only after the confirm answers (T2), and the
receiver's `+1` was durable at T1 < T2, so the blob is protected without a gap; the old rule's refusal
in that case (`gtest_cas_confirm_exact_ref.cpp:467-503` calls it "the TOCTOU this design closes") was
conservatism, not a safety requirement, and this design reverses it deliberately.

The model's rule 4, the apply-pending poison, is `RefLaneState::NeedsRecovery` in the code (entered
through `requireRecovery`); the confirm covers it through `lane_state` alone, and this design keeps
that.

## Design {#design}

### Rule 3, ref-scoped {#rule-3}

The sent transaction is already in memory: `RefTableRuntime::append_attempt` is armed under
`state_mutex` immediately before the first send (`CasRefLedger.cpp:3592`), retained through
`Writing` and `Wedged`, and swapped out in the same critical section that installs the transaction and
restores `Ready` (the `completed_attempt` swap in `commitRefChunk`). It is built in `prepareRefChunk`
(`CasRefLedger.cpp:3213-3216`) from the structured `chunk_txn`, whose `RefOp`s name every ref they touch:
`OwnerTransition` through `old_binding.ref_name` and `new_binding.ref_name`, `SetPublishedAt` through
`ref_name`; `NamespaceBirth`, `RemoveNamespace` and `EpochSeal` touch the whole namespace.

`RefAppendAttempt` gains the set of ref names the transaction touches and a `touches_namespace` flag.
Production has exactly one fill site, `prepareRefChunk`, from `chunk_txn.ops`. The derivation is a
`switch` over `RefOpKind` with no `default`, so a new kind is a compile error rather than a silent
miss: `OwnerTransition` contributes `old_binding.ref_name` and `new_binding.ref_name` whenever each is
present, which covers its four legal shapes (`CasRefProtocol.cpp:112-154`: add-precommit, remove-precommit,
remove-committed, promote), `SetPublishedAt` contributes `ref_name`, and `NamespaceBirth`,
`RemoveNamespace`, `EpochSeal` set `touches_namespace`. Recovery does not re-arm attempts: it copies the
retained one for adjudication (`CasRefLedger.cpp:1434`) and clears it at install (`:1638-1640`), so the
set travels with the attempt. The test seam `forceWedgeForTest` (`:1866-1878`) supplies bytes that need
not decode and sets `touches_namespace`. Rule 3 becomes:

```cpp
switch (rt.lane_state)
{
    case RefLaneState::Ready:
        break;
    case RefLaneState::Writing:
    case RefLaneState::Wedged:
        if (!rt.append_attempt || rt.append_attempt->touches_namespace
            || rt.append_attempt->touched_refs.contains(ref_name))
            return ConfirmAnswer::Unknown;
        break;
    case RefLaneState::NeedsRecovery:
    case RefLaneState::Closed:
    case RefLaneState::Faulted:
        return ConfirmAnswer::Unknown;
}
```

`pending` and `leader_active` no longer refuse. A sent transaction that does not touch the queried ref
cannot change that ref's binding or the blobs its manifest protects, so the row for that ref is exactly
as authoritative as it is on an idle lane. A sent transaction that does touch it refuses, as today.
Rules 2 (resident and recovered), 5 (exact committed-row equality) and 6 (mount fence, evaluated last)
are unchanged, as are the `try_to_lock` on `state_mutex` and the zero-I/O contract. Everything rule 3
now reads (`lane_state`, `append_attempt`, `state`) lives under `state_mutex`, so `ref_queue_mutex` is
no longer part of the rule-3 argument; the confirm keeps holding it for the O(1) slot lookup, and the
comment that credits the two-mutex hold with making `Yes` sound is corrected (see the documentation
list). Gate 0 in `DataPartsExchange.cpp` stays what it is today, an availability filter; nothing in
this design promotes it.

### Why this is the whole change {#scope-of-change}

The livelock needs the refusal to be table-wide. Scoping it to the sent transaction's own refs is the
smallest change that removes that property while keeping every refusal the safety argument needs:

- the queued appends were never sent, so no store state can disagree with the row because of them;
- the leader's carve, build and post-install work happen with no transaction in flight;
- the frontier publication happens with the attempt still armed, so it stays inside the refusal for
  the refs that transaction touches;
- `Wedged` keeps the attempt and keeps refusing for its refs, since its fate is unknown;
- `NeedsRecovery`, `Closed` and `Faulted` refuse for everything, as today.

No caller changes, no new lane state, no new object, no wait, no timer.

### Committed-ref mutation inventory {#inventory}

Recorded so the next reader does not have to redo the walk, and because revision 1 got it wrong. The
part state at the moment the append is issued is irrelevant to this design; what matters is that every
mutation of a committed ref goes through the lane and is therefore visible in `append_attempt` while
it is in flight.

| Path | How the ref is mutated | Sent through the lane |
|---|---|---|
| Whole-part removal (`IMergeTreeDataPart::remove`, from `clearPartsFromFilesystem` on `Deleting`, from `removeIfNeeded` for `Temporary` and `DeleteOnDestroy`) | `delete_tmp_*` rename via `republishRef`, then the ref dropped in `ContentAddressedTransaction::removeDirectory` | yes |
| Live repoint of a committed ref (standalone write or removal inside a committed part, `publishStaging` → `repointRef`) | `OwnerTransition` from the old manifest to the new one, part stays `Active`/`Outdated` | yes |
| Renames of committed parts (`delete_tmp_*`, merge and mutation results, RENAME TABLE, force-detach) | `republishRef`: destination published first, source dropped second | yes, two transactions |
| DETACH / ATTACH / MOVE | detached clone or new part published before the source is outdated or removed; MOVE's source becomes `DeleteOnDestroy` and is removed as above | yes |
| DROP / TRUNCATE / namespace removal | parts removed as above, then `RemoveNamespace`; the lane is `Closed` | yes |
| FREEZE shadow cleanup, detached cleanup, partial multi-ref rollback (`dropRefIfMatches`) | `OwnerTransition` removals naming their ref | yes |
| Recovery walk's epoch seal (`makeEpochSealTxn`, sent with `slotOccupy`) | no binding change; runs under `recovery_in_progress`, which rule 2 refuses | no, and needs not |

Every binding change is an ordinary lane append and therefore an armed `append_attempt` while in
flight; the one direct ref-log writer outside the lane changes no binding and runs while rule 2 already
refuses. That is the property the design rests on, and it is the property the model must state.

## Verification {#verification}

### Model {#model}

Today's model has no armed state: `SenderAdmit` sets `sPending` and `sLeader` together,
`SenderDurable` is atomic, and rule 3 is `~sPending /\ ~sLeader`. The extension, kept to two booleans
and one witness:

- `sArmed`, set by a new arm step that `SenderDurable` requires and `SenderApply` clears. This mirrors
  "arm before the first send, swap out at install" (`CasRefLedger.cpp:3585-3625`, `:3855-3859`), stated
  in the model as the code invariant it assumes.
- `sTouches`, chosen at admit: whether the admitted transaction touches the queried ref (or the
  namespace). A non-touching transaction leaves `sDurableRef` unchanged. No second ref and no part
  state are introduced.
- Rule 3 becomes `~(sArmed /\ sTouches)`; `sPending` and `sLeader` leave the rule.
- `SabotageStaleCache` drops that conjunct and must still violate `ConfirmedRelinkNeverDangles` (the
  durable `m1 -> m2` repoint with the stale `sCacheRef = m1`); a second, distinct flag forces
  `sTouches = FALSE` for a touching transaction and must violate it too.
- A non-vacuity witness: some behaviour answers `yes` while `sLeader /\ sPending` holds. Without it a
  green `_main` could be the old behaviour.
- The between-chunks state (installed, no attempt, leader still active) is reachable and answers from
  the row.
- Run with `MaxHoles = 0`, as `_main` does. The LIST-completeness caveat recorded in
  `CaRelinkConfirmCore_RESULTS.md` is unchanged and outside this design.

Liveness is argued, not model-checked: with rule 3 scoped to the sent transaction's refs, a confirm
about a ref that is not being mutated reaches rule 5 whenever the lane is resident and not broken, and
the only lock it needs is a `try_to_lock` against the leader's install.

### Consult {#consult}

Two independent consults on revision 2, one on another model, each asked to refute the design,
returned no defect in it (their reports are under `tmp/gcs_live_20260902/`, and their prose findings
are folded into this revision). The model diff, once written, gets the same treatment before the code
task is cut: is `sArmed` set before anything the store can observe; does every transaction that changes
`sDurableRef` have `sTouches` true; does the witness fire.

### Tests {#tests}

- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`. Four existing tests encode the table-wide rule and
  are rewritten with phase-separated expectations rather than deleted:
  `InFlightAppendIsUnknown` (queued same-ref removal, leader parked before carve) becomes `Yes` while
  queued and unsent, `Unknown` from arming through `Writing`, `No` after install;
  `MidTenureChunkBoundaryIsUnknown` (untouched ref between two chunks of one tenure) becomes `Yes`;
  `ConcurrentAppendIsOrderedAfterTheSnapshot` becomes before admission `Yes`, admitted but unsent `Yes`,
  armed `Unknown`, installed `No`, with the same deterministic phase driving;
  `WedgedLaneIsUnknown` stays `Unknown` for every ref because `forceWedgeForTest` marks the attempt
  namespace-wide, and gains a sibling with a real wedged transaction (a `PUT` left `Unresolved` through
  the request-control seam) where the touched ref answers `Unknown` and an unrelated ref `Yes`.
  New: the same-ref stale-row regression, offer `m1`, repoint the live ref to a manifest omitting one
  of `m1`'s blobs, hold the leader after the `PUT` and before install with the existing hooks, publish
  the receiver's `+1` late, assert `Unknown`; and the liveness case, a tenure held open for another ref
  (`pending` non-empty, `leader_active`, `Writing`) while a confirm about an untouched committed ref
  answers `Yes`. The rule 2, 5 and 6 tests are unchanged.
- Completeness of the touched set is a compile-time property (the `switch` with no `default`); a
  per-shape semantic test over the four `OwnerTransition` shapes asserts extraction from both optionals,
  with remove-committed as the case that matters.
- `test_cas_gcs` (fake GCS) gains a two-node case with delayed `_ckpt` writes: continuous inserts on
  both, both replication queues drain. This is the liveness reproduction the unit test cannot give.
- Observability, the minimum the live gate needs to be read: ProfileEvents
  `CASRelinkConfirmRefusedSentTxnTouchesRef`, `CASRelinkConfirmRefusedLaneBroken` and
  `CASRelinkConfirmRefusedStateLockBusy`, and the refusing rule named in the existing
  `Relink confirm is unproven` debug line. Counters for the other rules are optional. Residual `Unknown`
  sources this design does not remove, all bounded: the `try_to_lock` failing while `listRefs` or the
  snapshot publisher hold `state_mutex` or during an install; and a confirm about a ref whose own
  repoint or `SetPublishedAt` is in flight, `Unknown` for one flush. If either counter dominates on the
  live gate, that is a different fix.

### Live gate {#live-gate}

A ten-minute phase-3 soak on the GCS stand (`utils/ca-soak/docker-compose-gcs.yml`,
`--duration 10m`, both replicas, chaos on): both replicas equal to the model at every checkpoint, no
`NO_REPLICA_HAS_PART` storm in `system.replication_queue`, refusal counters dominated by
`SentTxnTouchesRef` and `LaneBroken` only around induced faults. The full two-hour soak runs once, after
every fix of the 2026-09-02 campaign has landed, as the campaign's closing gate rather than per task.
The fake cannot reproduce WAN latency and the provider's rate limit together, so even the short run
stays on the real bucket.

## Documentation to change with the code {#docs}

- `CasRefLedger.h`: the `RefLaneState` text at `:40` ("`Ready` is the only state that ... certifies a
  cached row"), the `ConfirmAnswer` text at `:60` ("busy or non-`Ready` table answers `Unknown`"), the
  `confirmExactRef` declaration comment at `:156-159` ("lane state `Ready`", "the two-mutex hold"), and
  the `RefAppendAttempt` field comments at `:577-608`, which gain the two new fields.
- `CasRefLedger.cpp`: the function-header paragraph at `:426-435`, which says the two-mutex snapshot is
  what makes `Yes` sound and that no admitted removal can coexist with a `Yes`, and the rule 3 comment
  block at `:471-480`. Under this design an admitted, unsent removal does coexist with a `Yes`, safely,
  and the comment must say why.
- The operator-facing description of the confirm under `docs/en/antalya/cas/`, if it states the
  table-wide refusal.

## Rollout {#rollout}

Sender-side relaxation only: an old receiver paired with a new sender sees more `Yes` answers for the
same protocol; a new receiver paired with an old sender sees today's behaviour. No protocol version,
no persisted format change.

## Out of scope {#out-of-scope}

- Receiver-side damping and keeping the prepared relink across retries (decided by the soak after
  this change).
- Coalescing or moving the `_ckpt` committed-frontier publication (backlog `[gcs-hot-control-keys-429]`).
- The GC LIST-completeness assumption the model already records.
- Per-object rate-limit handling on GCS.

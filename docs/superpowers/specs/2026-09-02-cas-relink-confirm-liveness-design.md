---
description: 'Design for making the CAS fetch-by-relink confirm live under sustained write load: rule 3 refuses only the sent transaction that touches the queried ref, not every activity of the lane'
sidebar_label: 'CAS relink confirm liveness'
sidebar_position: 1
slug: /superpowers/specs/cas-relink-confirm-liveness-design
title: 'CAS relink confirm liveness design'
doc_type: 'design'
---

# CAS relink confirm liveness design {#cas-relink-confirm-liveness-design}

Status: revision 2 of 2026-09-02. Revision 1 relied on the part state machine (gate 0) to carry the
removal argument; a review found the counterexample of a live repoint that retires a blob edge while
the part stays `Active`, and revision 1 is withdrawn. This revision awaits the TLA+ re-verification and
the two-model concurrency consult it requires before any code changes. Ledger finding F11, backlog
item `[relink-confirm-lane-livelock]` in `docs/superpowers/cas/BACKLOG/gcs.md`.

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
about queued appends that were never sent, and it is not about other refs.

## Design {#design}

### Rule 3, ref-scoped {#rule-3}

The sent transaction is already in memory: `RefTableRuntime::append_attempt` is armed under
`state_mutex` immediately before the first send (`CasRefLedger.cpp:3592`), retained through
`Writing` and `Wedged`, and swapped out in the same critical section that installs the transaction and
restores `Ready` (the `completed_attempt` swap in `commitRefChunk`). It is built in `prepareRefChunk`
(`CasRefLedger.cpp:3213-3216`) from the structured `chunk_txn`, whose `RefOp`s name every ref they touch:
`OwnerTransition` through `old_binding.ref_name` and `new_binding.ref_name`, `SetPublishedAt` through
`ref_name`; `NamespaceBirth`, `RemoveNamespace` and `EpochSeal` touch the whole namespace.

`RefAppendAttempt` gains the set of ref names the transaction touches and a flag for namespace-wide
transactions. Both are filled where an attempt is built: in `prepareRefChunk` from `chunk_txn.ops`, and
on the recovery path that re-arms a wedged attempt from its durable bytes (`CasRefLedger.cpp:1876`),
which either decodes the transaction to derive the set or marks the attempt namespace-wide. Rule 3
becomes:

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
Rules 2 (resident and recovered), 4 (apply-pending poison), 5 (exact committed-row equality) and 6
(mount fence, evaluated last) are unchanged, as are the two-mutex snapshot, the `try_to_lock` on
`state_mutex`, and the zero-I/O contract. Gate 0 in `DataPartsExchange.cpp` stays what it is today, an
availability filter; nothing in this design promotes it.

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
| FREEZE shadow cleanup, detached cleanup, partial multi-ref rollback (`dropRefIfMatches`) | shadow or not-yet-activated refs, never a live part's ref | yes |

Every row is an ordinary lane append; none bypasses `append_attempt`. That is the property the design
rests on, and it is the property the model must state.

## Verification {#verification}

### Model {#model}

Extend `CaRelinkConfirmCore.tla` with the ref-scoped rule and keep the sabotage discipline:

- Rule 3 becomes: refuse if the sender's sent-but-not-installed transaction (the model's post-durable
  window, and the sent-but-unknown window) touches the queried ref or the namespace; do not refuse on
  queued-but-unsent appends or on the leader tenure alone.
- `SabotageStaleCache` is reformulated as "ignore the sent transaction's ref set" and must still
  produce the counterexample; a new flag "treat the sent transaction as touching no ref" must produce
  it as well.
- Live repoints (`SenderAdmit(Other)`), whole-part removals, and the two-transaction rename are already
  representable as journal transactions with ref sets; no part-state machine is introduced.
- Run with `MaxHoles = 0`, as `_main` does. The LIST-completeness caveat recorded in
  `CaRelinkConfirmCore_RESULTS.md` is unchanged and outside this design.

Liveness is argued, not model-checked: with rule 3 scoped to the sent transaction's refs, a confirm
about a ref that is not being mutated reaches rule 5 whenever the lane is resident and not broken, and
the only lock it needs is a `try_to_lock` against the leader's install.

### Consult {#consult}

Before implementation, two independent consults on this document and the model diff, each asked to
refute the other's verdict. Questions: is there a committed-ref mutation that bypasses the lane and
therefore `append_attempt`; can an attempt's ref set be incomplete for any `RefOp` kind; is there a
window between arming the attempt and the first send, or between install and the swap, where the row
and the attempt disagree; does `Wedged` ever hold an attempt whose ref set is stale.

### Tests {#tests}

- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`: the regression the review asked for, same ref:
  offer `m1`, repoint the live ref to a manifest omitting one of `m1`'s blobs, hold the leader after
  the `PUT` and before install with the existing test hooks, publish the receiver's `+1` late, and
  assert the confirm about `m1` answers `Unknown`. Then the liveness case: with a tenure held open for
  another ref (`pending` non-empty, `leader_active`, `Writing`), a confirm about an untouched committed
  ref answers `Yes`. Then the namespace-wide case: an epoch-seal attempt in flight refuses every ref.
  The existing rule 2, 4, 5, 6 tests are unchanged.
- A unit test that `touched_refs` is complete for every `RefOpKind`, driven from a table of one op per
  kind, so a future kind without a mapping fails the test rather than the confirm.
- `test_cas_gcs` (fake GCS) gains a two-node case with delayed `_ckpt` writes: continuous inserts on
  both, both replication queues drain. This is the liveness reproduction the unit test cannot give.
- Observability: a ProfileEvent per refusal reason (`CASRelinkConfirmRefusedNotResident`,
  `...NotRecovered`, `...StateLockBusy`, `...SentTxnTouchesRef`, `...LaneBroken`, `...RowMismatch`,
  `...Fenced`), and the reason named in the existing `Relink confirm is unproven` debug line. The
  `try_to_lock` failure on `state_mutex` gets its own reason rather than hiding under another.

### Live gate {#live-gate}

A ten-minute phase-3 soak on the GCS stand (`utils/ca-soak/docker-compose-gcs.yml`,
`--duration 10m`, both replicas, chaos on): both replicas equal to the model at every checkpoint, no
`NO_REPLICA_HAS_PART` storm in `system.replication_queue`, refusal counters dominated by
`SentTxnTouchesRef` and `LaneBroken` only around induced faults. The full two-hour soak runs once, after
every fix of the 2026-09-02 campaign has landed, as the campaign's closing gate rather than per task.
The fake cannot reproduce WAN latency and the provider's rate limit together, so even the short run
stays on the real bucket.

## Documentation to change with the code {#docs}

- `CasRefLedger.h`: the `RefLaneState` text ("`Ready` is the only state that admits a new append or
  certifies a cached row") and the `confirmExactRef` declaration comment describe the table-wide rule
  and must describe the ref-scoped one.
- `CasRefLedger.cpp`: the rule 3 comment block.
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

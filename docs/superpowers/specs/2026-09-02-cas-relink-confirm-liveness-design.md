---
description: 'Design for making the CAS fetch-by-relink confirm live under sustained write load: rule 3 refuses only a broken lane, and the part state machine carries the removal argument'
sidebar_label: 'CAS relink confirm liveness'
sidebar_position: 1
slug: /superpowers/specs/cas-relink-confirm-liveness-design
title: 'CAS relink confirm liveness design'
doc_type: 'design'
---

# CAS relink confirm liveness design {#cas-relink-confirm-liveness-design}

Status: approved in discussion on 2026-09-02, awaiting the TLA+ re-verification and the two-model
concurrency consult that this document requires before any code changes. Ledger finding F11,
backlog item `[relink-confirm-lane-livelock]` in `docs/superpowers/cas/BACKLOG/gcs.md`.

## Problem {#problem}

Same-pool replication transfers only a part's manifest. The receiver publishes its own ref over the
sender's blobs (T1, durable), asks the sender one read-only question, "do you still hold exactly this
manifest for this part?" (T2), and promotes only on `Yes` (T3). Any other answer aborts the prepared
relink and re-queues the fetch.

On the live GCS stand on 2026-09-02, two replicas answered each other `Unknown` almost every time for
forty minutes. Both replication queues wedged at 1.5-1.7k entries, the replicas diverged to 123k
against 166k rows, and the soak died on `SYSTEM SYNC REPLICA`. Nothing was lost: once one side stopped
fetching, the other drained in two minutes, and the stand converged to identical row counts.

The refusal came from rule 3 of `CasRefLedger::confirmExactRef` (`Pool/CasRefLedger.cpp:480`):

```cpp
if (rt.lane_state != RefLaneState::Ready || !rt.pending.empty() || rt.leader_active)
    return ConfirmAnswer::Unknown;
```

The rule is table-scoped: a question about part P is refused because any other mutation of the same
namespace is queued, in flight, or waiting for its checkpoint frontier. Every replica is also a
receiver, and each failed fetch appends two records to its own lane (a precommit at T1, its removal at
abort), so under load neither side ever observes the other's lane quiet. On GCS the window is long
because every flush ends with a token-CAS on the namespace's `_ckpt`, which GCS limits to about one
mutation per second per object; `RefLaneState::Writing` spans the ref-log `PUT`, that checkpoint
publication with its backoff, and the apply. On RustFS in a LAN the same window is milliseconds, which
is why no earlier soak showed the defect.

## What rule 3 protects, and what actually protects it {#safety}

Rule 3 exists for one hazard: the sender's in-memory committed row can lag its own durable journal.
The leader does not hold `state_mutex` across the `PUT`, so between the moment GCS applies a removal
and the moment the leader applies it to the row, a confirm reads a stale row and answers `Yes` about a
binding that is already durably gone. If that removal became durable before the receiver's T1, the
blobs had no protector in between, GC may have reclaimed them, and the receiver promotes a dangling
part. `docs/superpowers/models/CaRelinkConfirmCore.tla` shows this with `SabotageStaleCache`.

The hazard is real, but the sender already knows it is about to remove the part long before the
journal does, and the confirm already reads that knowledge. Gate 0 in
`DataPartsExchange.cpp:261` answers `No` unless the part is `Active` or `Outdated` on the routed disk.
The ordering that makes gate 0 a proof rather than a filter is enforced by the code that removes parts:

- The only entry to physical removal is `MergeTreeData::asMutableDeletingPart`
  (`MergeTreeData.cpp:3479`), which throws `LOGICAL_ERROR` unless the part is `Deleting` or
  `DeleteOnDestroy`. `Temporary` parts are moved to `Deleting` before removal; `DeleteOnDestroy` is the
  state of a part moved to another disk.
- The durable ref of a committed part is dropped only inside that removal: `remove()` renames the
  directory to `delete_tmp_*` and removes it recursively, and the CAS transaction's `removeRecursive`
  drops the ref (`ContentAddressedTransaction.cpp:1075`).
- Renames of committed parts (`delete_tmp_*`, a merge or mutation result, DETACH, ATTACH) go through
  `CachedPartFolderAccess::republishRef` (`PartFolderAccess.cpp:506`): the destination ref is published
  first, the source dropped second. Between the two the blobs have two protectors; the source is a
  staged ref, a `Deleting` part, or a detached ref with no part object.
- A repoint of a live part's ref to another manifest can only shrink the sender's own protection of
  some blobs. The receiver's ref, durable since T1, keeps them alive regardless; rule 5 compares the
  exact `ManifestRef` in any case.
- `rollbackDeletingParts` returns a part to `Outdated` only after `remove()` threw. If the drop was
  never sent, the row and the journal agree and `Yes` is correct. If it landed, the lane either applied
  it (rule 5 answers `No`) or could not resolve it and entered `NeedsRecovery` (rule 3 answers
  `Unknown`). A definite failure after a durable drop leaves the row already applied.

So for every path that can remove a committed part's ref, the part has left `{Active, Outdated}`
before the removal is sent, and gate 0 refuses before the row can mislead. The lane's `pending`,
`leader_active` and `Writing` add nothing to that argument.

## Design {#design}

### Rule 3 {#rule-3}

Rule 3 refuses only a lane that cannot certify anything:

```cpp
if (rt.lane_state == RefLaneState::Wedged || rt.lane_state == RefLaneState::NeedsRecovery
    || rt.lane_state == RefLaneState::Closed || rt.lane_state == RefLaneState::Faulted)
    return ConfirmAnswer::Unknown;
```

`Writing`, a non-empty `pending`, and `leader_active` no longer refuse. Rules 2 (resident and
recovered), 4 (apply-pending poison), 5 (exact committed-row equality) and 6 (mount fence, evaluated
last) are unchanged, as are the two-mutex snapshot and the zero-I/O contract. The `try_to_lock` on
`state_mutex` stays: the leader's brief apply and the confirm still never block each other.

### Gate 0 as a proof {#gate-0}

Gate 0 keeps its code and gains its contract. The invariant it rests on is stated at the gate and at
`asMutableDeletingPart`:

> The durable ref of a committed part is removed only from `IMergeTreeDataPart::remove`, which runs
> only on a part in `Deleting` or `DeleteOnDestroy`, or as the second step of `republishRef` whose
> first step already published the destination ref.

A new removal path that violates this ordering breaks the confirm's safety; the TLA+ variant below is
the place where such a change is caught, and the two comments are the place where the next reader
learns why the order matters.

### What does not change {#unchanged}

- The protocol order publish, confirm, promote, and the taxonomy of receiver outcomes.
- The receiver's abort path. Receiver-side damping (a per-source circuit breaker) stays a separate
  item, to be decided by the soak after this change.
- The lane's own state machine, the checkpoint frontier publication, and the `Writing` state's role
  for local certification. Coalescing `_ckpt` publications is a separate design.

## Verification {#verification}

### Model {#model}

Extend `CaRelinkConfirmCore.tla` rather than editing rule 3 in place:

- Model the sender's part state as a variable with the transitions `Active → Outdated → Deleting →
  Removed` (and `Deleting → Outdated` for rollback), with the constraint that a removal transaction
  may enter the journal only while the part is `Deleting`. Gate 0 becomes a modelled predicate, not an
  out-of-scope assumption.
- Replace rule 3 with the broken-lane predicate above.
- Keep every existing sabotage flag and its required counterexample. Rename `SabotageStaleCache` to
  the property it now tests, a removal admitted while the part is still `Active`/`Outdated`, and
  require that it violates `ConfirmedRelinkNeverDangles`.
- Run with `MaxHoles = 0`, as `_main` does today. The LIST-completeness caveat recorded in
  `CaRelinkConfirmCore_RESULTS.md` is unchanged and outside this design.

Liveness is argued, not model-checked: with rule 3 reduced to broken-lane states, a confirm about a
part that is not being removed reaches rule 5 whenever the lane is resident and not wedged, and the
only lock it needs is a `try_to_lock` against the leader's apply.

### Consult {#consult}

Before implementation, two independent model consults on this document and the model diff, each
asked to refute the other's verdict, per the standing rule for hard-concurrency changes. The
questions to put to them: is there a removal path outside `asMutableDeletingPart`; can a
`republishRef` source drop land while the source part is `Active` or `Outdated`; does dropping
`Writing` from rule 3 admit any answer the current rule refuses for a reason the ordering invariant
does not cover.

### Tests {#tests}

- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`: with a tenure held open by the existing
  test hooks (`ref_pre_tenure_hook_for_test`, `carve_hook_for_test`) so that `pending` is non-empty,
  `leader_active` is true and `lane_state` is `Writing`, a confirm about an unrelated committed ref
  answers `Yes`; the same confirm answers `Unknown` when the lane is `Wedged`, `NeedsRecovery`,
  `Closed` or `Faulted`; the existing rule 2, 4, 5, 6 tests are unchanged and still pass.
- A death or `LOGICAL_ERROR` test that `asMutableDeletingPart` rejects `Active` and `Outdated`, if none
  exists; it is the runtime form of the invariant.
- The `test_cas_gcs` fake-GCS integration test gains a case where the fake delays `_ckpt` writes: two
  nodes replicate under continuous inserts and both queues drain.
- Observability used by the tests and by operators: a ProfileEvent per refusing rule
  (`CASRelinkConfirmRefusedNotResident`, `...NotRecovered`, `...LaneBroken`, `...RowMismatch`,
  `...Fenced`) and the refusing rule named in the existing `Relink confirm is unproven` debug line.

### Live gate {#live-gate}

A ten-minute phase-3 soak on the GCS stand (`utils/ca-soak/docker-compose-gcs.yml`,
`--duration 10m`, both replicas, chaos on) after this change: both replicas equal to the model at every
checkpoint, no `NO_REPLICA_HAS_PART` storm in `system.replication_queue`, and the refusal counters
showing broken-lane refusals only around induced faults. The full two-hour soak runs once, after every
fix of the 2026-09-02 campaign has landed (this design, the catalog write path, the `_ckpt` publication
change), as the campaign's closing gate rather than per task. The fake cannot reproduce WAN latency and
the provider's rate limit together, so even the short run stays on the real bucket.

## Rollout {#rollout}

The change is a relaxation on the sender side only and needs no protocol version: an old receiver
paired with a new sender sees more `Yes` answers, a new receiver paired with an old sender sees
today's behaviour. No persisted format changes.

## Out of scope {#out-of-scope}

- Receiver-side damping and keeping the prepared relink across retries.
- Coalescing or moving the `_ckpt` committed-frontier publication (backlog `[gcs-hot-control-keys-429]`).
- The GC LIST-completeness assumption the model already records.
- Any change to the per-object rate limit handling on GCS.

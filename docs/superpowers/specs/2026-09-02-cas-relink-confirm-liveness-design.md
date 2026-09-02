---
description: 'Design for making the CAS fetch-by-relink confirm live under sustained write load: rule 3 refuses only while a queued or in-flight mutation names the queried ref, read from the MutationScope every lane item already carries'
sidebar_label: 'CAS relink confirm liveness'
sidebar_position: 1
slug: /superpowers/specs/cas-relink-confirm-liveness-design
title: 'CAS relink confirm liveness design'
doc_type: 'design'
---

# CAS relink confirm liveness design {#cas-relink-confirm-liveness-design}

Status: revision 8 of 2026-09-02, the simplification with the wedge hole closed. Revisions 1 to 7 and
their reviews are recorded in [F11 spec consults](/superpowers/cas/f11-spec-consults-2026-09-02); their
result is one fact this revision rests on: the hazard is a mutation of the queried ref that is queued or
in flight, and every such mutation already names its ref. Revision 7 also exempted `Wedged`; a consult
showed that a wedged chunk's items are completed before the tenure ends, so nothing but the lane state
records a wedged transaction, and `Wedged` keeps refusing. Ledger finding F11, backlog item `[relink-confirm-lane-livelock]`
in `docs/superpowers/cas/BACKLOG/gcs.md`.

## The problem {#problem}

Same-pool replication transfers only a part's manifest. The receiver publishes its own ref over the
sender's blobs (T1, durable), asks the sender one read-only question, "does ref P still bind exactly
manifest m1?" (T2), and promotes only on `Yes` (T3). On the live GCS stand on 2026-09-02, two replicas
answered each other `Unknown` almost every time for forty minutes: both replication queues wedged at
1.5-1.7k entries, the replicas diverged to 123k against 166k rows, and the soak died on
`SYSTEM SYNC REPLICA`. Nothing was lost; once one side stopped fetching the other drained in two
minutes. The refusal came from rule 3 of `CasRefLedger::confirmExactRef`
(`Pool/CasRefLedger.cpp:480`):

```cpp
if (rt.lane_state != RefLaneState::Ready || !rt.pending.empty() || rt.leader_active)
    return ConfirmAnswer::Unknown;
```

It refuses a question about P whenever anything at all is happening in P's namespace. Every replica is
also a receiver, and each failed fetch appends two records to its own lane (a precommit at T1 and its
removal at abort), so under load neither side ever sees the other's lane idle. On GCS the busy stretch is
long because every flush ends with a token-CAS on the namespace's `_ckpt`, limited to about one mutation
per second per object; on RustFS in a LAN it is milliseconds, which is why no earlier soak saw this.

## What rule 3 protects {#safety}

One hazard: the sender's in-memory committed row lags its durable journal while a transaction is in
flight (the leader does not hold `state_mutex` across the `PUT`, and a transaction is installed into the
row only after its checkpoint frontier is published). If that transaction removes P's ref, or repoints
it from m1 to a manifest m2 that no longer references one of m1's blobs, and it became durable before the
receiver's T1, GC may have decided that blob's deletion before the receiver's `+1` existed; a `Yes` read
off the stale row then lets the receiver promote a manifest whose blob is gone.
`docs/superpowers/models/CaRelinkConfirmCore.tla` records this as `_sab_stalecache`.

The two cases behave differently at the `MergeTree` level and that is worth stating once. A whole-part
removal is preceded by the part leaving `{Active, Outdated}`: the only entry to physical removal,
`MergeTreeData::asMutableDeletingPart` (`MergeTreeData.cpp:3479`), admits only `Deleting` and
`DeleteOnDestroy`, and the ref drop happens inside `remove()`. Gate 0 in `DataPartsExchange.cpp:261`
(part not `Active`/`Outdated` on the routed disk → `No`) therefore already answers correctly for
removals. A repoint of a live part's ref (a write or removal of a file inside a committed part,
`ContentAddressedTransaction.cpp:352-405` through `publishStaging` and `repointRef`) keeps the part
`Active`, correctly, because the part is being changed, not removed; gate 0 cannot see it, and this is
the one case that needs a marker set before the `PUT`. Renames (`republishRef`, `PartFolderAccess.cpp:506`)
publish the destination ref before dropping the source and need nothing.

## Design {#design}

### The marker already exists {#marker}

Every mutation admitted to a namespace lane carries a `MutationScope` (`Pool/CasRefProtocol.h:63-75`):
`Ref{ref_name}` for a mutation of exactly one ref, `WholeShard` for `dropNamespace` and anything else
that touches refs wholesale. Every caller of `appendRefOps` supplies it (`CasPartWriteTxn.cpp:657, 769,
1044`, `CasPool.cpp:1526`, `PartFolderAccess.cpp:660`, `CasRefLedger.cpp:4672, 4729, 4772, 5149`), and the
lane already depends on it: the batch builder admits at most one mutation per ref name per flush and
carves `WholeShard` solo (`CasRefLedger.cpp:2880-2935`). The scope is recorded at admission, under
`ref_queue_mutex`, before anything is sent. So "a change of P is queued or in flight" is knowledge the
sender already holds from the moment it decides to change P until the change is installed.

### Rule 3 {#rule-3}

```cpp
if (rt.lane_state != RefLaneState::Ready && rt.lane_state != RefLaneState::Writing)
    return ConfirmAnswer::Unknown;                       /// Wedged, NeedsRecovery, Closed, Faulted
if (rt.lane_state == RefLaneState::Writing && rt.carved.empty())
    return ConfirmAnswer::Unknown;                       /// cannot happen; fail closed if it ever does
for (const auto & item : rt.pending)
    if (scopeCovers(item->scope, ref_name))
        return ConfirmAnswer::Unknown;                   /// queued, not yet carved
for (const auto & item : rt.carved)
    if (scopeCovers(item->scope, ref_name))
        return ConfirmAnswer::Unknown;                   /// carved into a chunk, not yet installed
```

with `scopeCovers` true for `WholeShard` and for `Ref{name}` when `name == ref_name`. `Writing` no longer
refuses by itself; `leader_active` and a non-empty `pending` no longer refuse by themselves. `Wedged`
keeps refusing table-wide: when a `PUT` comes back ambiguous, `commitRefChunk` completes the chunk's
items with an error before the tenure ends (`CasRefLedger.cpp:3634-3644`, `:3993-4006`), so the possibly
durable transaction is recorded nowhere but in `append_attempt` and the lane state, and the row for its
ref may be stale until the next flush or a remount adopts or discards it. The live stand recorded
`wedged_namespace_count = 0` throughout the livelock, so this refusal costs nothing F11 needs. Rules 2
(resident and recovered), 5 (exact committed-row equality) and 6 (mount fence, evaluated last) are
unchanged, as are the `try_to_lock` on `state_mutex` and the zero-I/O contract. The model's rule 4, the
apply-pending poison, is `NeedsRecovery` in the code and stays covered by the first line. Gate 0 stays
what it is, an availability filter that happens to be exact for removals.

### Two small code changes {#carved}

**`carved`.** The confirm can see `rt.pending`, but the carve moves items out of it: in one continuous
`ref_queue_mutex` hold the leader copies the selected front items into its local `owned_items` and pops
them (`CasRefLedger.cpp:2917-2935`), and they are completed and erased only at the tenure's exit guard,
also under `ref_queue_mutex` (`:2146-2166`). Between carve and completion a mutation is invisible to
anyone holding only the runtime. The runtime gains `rt.carved`, a vector of the same
`shared_ptr<RefMutationItem>`s, appended at the carve and cleared at the exit guard. Items of a chunk
stay in it until the exit guard even after their install, and items that failed validation before any
send stay in it too: both are over-refusal for one tenure, never under-refusal. The failure arms of
`flushRefBatch` that pop all of `pending` and complete the items with an error (`carve_all_pending`,
`:2731-2895`) sent nothing and need no mirror. `forceWedgeForTest` (`:1866-1878`) is unchanged: it
produces `Wedged`, which refuses by lane state.

**Scope validation.** `MutationScope` becomes safety-bearing, so it is checked where it was a hint
before: in `flushRefBatch` step 3 (`:3060-3120`, before anything durable) a `Ref{name}` item whose
built ops carry an `OwnerTransition` binding or a `SetPublishedAt` for a different ref fails, that item
only, with `LOGICAL_ERROR`. Every production caller already passes the exact ref its ops mutate; the
only offender in the tree is a test helper (`gtest_cas_confirm_exact_ref.cpp:554-557`, one item over
1500 refs declared as `Ref{prefix}`), which is rewritten to one ref with 1500 manifests.

Nothing else changes: no new field on the attempt, no new argument on `appendRefOps`, no part-object
flag, no wait, no timer.

### Why this is enough, and why it is not too much {#argument}

- A mutation of P admitted after the confirm's snapshot is ordered after T2 by `ref_queue_mutex`, and
  the receiver's `+1` is durable at T1 < T2, so the blob it protects never loses its last protector:
  this is the protocol's existing argument and it does not depend on the lane being idle.
- A mutation of P admitted before the snapshot is in `pending` or `carved` until installed, and refuses.
  After install the row is current and rule 5 answers. If its `PUT` came back ambiguous, the lane is
  `Wedged` and refuses until the transaction is adopted or discarded.
- A mutation of another ref, wherever it is, cannot change P's binding or the blobs P's manifest
  protects; the row for P is exactly as authoritative as on an idle lane.
- `WholeShard` refuses everything, so `dropNamespace` and any future multi-ref mutation stay safe by
  default.
- The refusal starts at admission rather than at the first send. That is earlier than the argument
  strictly needs (a queued, unsent mutation has no durable effect) and it is deliberate: it costs one
  retry on the queried ref only, it keeps the existing same-ref tests' expectations, and it removes any
  reasoning about send and install phases from the confirm.

## Verification {#verification}

### Model {#model}

Three modules encode the old table-wide contract; all three change, and the change is the same
sentence in each: certification is refused while a queued or in-flight mutation touches the certified
identity, and only then, plus the broken lane states.

`CaRelinkConfirmCore.tla`: today `SenderAdmit(nb)` admits only a transaction that retires the sender's
edge, and rule 3 is `~sPending /\ ~sLeader`. Add a second admitted shape, `noop`, whose journal record
is `NsNoise`'s existing edge-neutral op and which leaves `sDurableRef` unchanged; `sTouches ==
sShape = "touching" /\ ~SabotageTouchBlind`; rule 3 becomes `~(sPending /\ sTouches)`, `sLeader` leaves
the rule. `SenderApply`'s guard becomes shape-aware (today it requires `sDurableRef # Token`, which a
`noop` never satisfies, so a `noop` tenure could never close). Sabotages: `SabotageStaleCache` drops the
conjunct and must still violate `ConfirmedRelinkNeverDangles`; `SabotageTouchBlind` must violate it
too. One witness, a history flag set inside `RConfirm`: `sawYesWhilePendingNoop` (answer `yes` while
`sPending /\ ~sTouches`); `_main` must reach it, or a green run could be the old behaviour.
`nextId <= MaxId` stays on every step that writes a record. No arm or install phase is introduced: the
model's `sPending` spans admission to apply, which is the interval `pending` plus `carved` cover in the
code for a tenure that completes, and a wedged tenure is the model's `sPoison`, refused by lane state.
`MaxHoles = 0` as in `_main`; the LIST-completeness caveat of `CaRelinkConfirmCore_RESULTS.md` is
unchanged and outside this design.

`CaRefLaneCore.tla` and `CaRelinkLaneComposition.tla`: `Certify` (`CaRefLaneCore.tla:714-722`) and
`ConfirmSource` (`CaRelinkLaneComposition.tla:111`) require `lane = "Ready"`. Both become "lane is
`Ready`, or lane is `Writing` and the outstanding mutation does not touch the identity"; `Wedged` and
the broken states stay excluded. The lane core derives the touch from its existing same-binding writes
(`attempt.binding # cache_binding`) and keeps `CurrentRuntime` and the identity's cache/durable
currency; the composition, which has no transaction content, takes the touch as a nondeterministic
parameter of `StartWrite`. Their blocked-certify sabotages (`SabotageCertifyBlocked`,
`SabotageConfirmBlocked`) become "certify while the outstanding mutation touches the identity", and each
module gains one witness for a certification outside `Ready` (`W_CertifiedWhileOutstanding`,
`W_ConfirmedOutsideReady`), because their existing witnesses are satisfied by `Ready` certifications.
`run_reflane.sh` and `run_relinklane.sh` rerun; `CaRefLaneCore_RESULTS.md` (`:43`, `:68-91`) and
`CaRelinkConfirmCore_RESULTS.md` (`:126-142`) are rewritten from the runs. The remaining modelling
details (where the lane core keeps the touch across `resolver_attempt`, which binding updates become
identity-conditional) belong to the model author; the batteries are the check.

### Tests {#tests}

`src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` encodes the old rule in four tests. Two keep their
expectations, two flip:

- `InFlightAppendIsUnknown` (queued removal of the same ref, leader parked before carve): stays
  `Unknown`, now by scope rather than by `pending` being non-empty.
- `ConcurrentAppendIsOrderedAfterTheSnapshot` (same ref, three phases): stays `Yes` → `Unknown` →
  `No`.
- `MidTenureChunkBoundaryIsUnknown` (an untouched ref between two chunks of one tenure): becomes
  `Yes`, and is renamed accordingly.
- `WedgedLaneIsUnknown` (synthetic wedge via `forceWedgeForTest`): unchanged, `Unknown` for every ref
  by lane state; gains a sibling with a real wedged transaction (`ChunkFaultBackend::Mode::Unresolved`
  with a single-attempt budget, as `gtest_cas_ref_install_safety.cpp` already uses) where both the
  wedged ref and an unrelated ref answer `Unknown`, the first because its transaction may be durable,
  the second because the refusal is by lane state.

New tests: the same-ref stale-row regression (offer m1, repoint the live ref to a manifest omitting one
of m1's blobs, hold the leader after the `PUT` and before install with the existing hooks, publish the
receiver's `+1` late, assert `Unknown`); the liveness case (a tenure held open for another ref while a
confirm about an untouched committed ref answers `Yes`); `carved` bookkeeping (an item is visible from
carve to completion, and a completed tenure leaves `carved` empty); and scope validation (a `Ref{X}`
item whose ops name Y fails before durability, with `LOGICAL_ERROR`). The rule 2, 5 and 6 tests are
unchanged. The file header at `:40-46`, which says no `Yes` may coexist with an admitted removal, is
rewritten: no `Yes` may coexist with an admitted mutation *of that ref*.

`test_cas_gcs` (fake GCS) gains a two-node case with delayed `_ckpt` writes: continuous inserts on both,
both replication queues drain. This is the liveness reproduction the unit tests cannot give.

Observability, the minimum the live gate needs to be read: ProfileEvents
`CASRelinkConfirmRefusedRefMutationInFlight`, `CASRelinkConfirmRefusedLaneWedged`,
`CASRelinkConfirmRefusedLaneBroken`, `CASRelinkConfirmRefusedStateLockBusy`, plus a ledger-side
`LOG_TRACE` naming the refusing rule. The attribution stays inside the ledger: `ConfirmAnswer` is a
three-value enum crossing two interfaces, and widening it would be the API change this design avoids.
Residual `Unknown` sources this design leaves, all bounded: the `try_to_lock` failing while `listRefs` or
the snapshot publisher hold `state_mutex` or during an install; a confirm about a ref whose own mutation
is queued or in flight, one flush; and a wedged lane until its next flush or remount. If any counter
dominates on the live gate, that is a different fix.

### Live gate {#live-gate}

A ten-minute phase-3 soak on the GCS stand (`utils/ca-soak/docker-compose-gcs.yml`, `--duration 10m`,
both replicas, chaos on): both replicas equal to the model at every checkpoint, no
`NO_REPLICA_HAS_PART` storm in `system.replication_queue`, refusal counters dominated by
`RefMutationInFlight`, with `LaneWedged` and `LaneBroken` only around induced faults. The full two-hour soak runs once,
after every fix of the 2026-09-02 campaign has landed, as the campaign's closing gate rather than per
task. The fake cannot reproduce WAN latency and the provider's rate limit together, so even the short run
stays on the real bucket.

## Documentation to change with the code {#docs}

- `CasRefLedger.h`: `RefLaneState` text at `:40` ("`Ready` is the only state that ... certifies a cached
  row"), `ConfirmAnswer` text at `:60`, the `confirmExactRef` declaration comment at `:156-159`, and the
  `RefTableRuntime` comment where `carved` is added.
- `CasRefLedger.cpp`: the function-header paragraph at `:426-435` (the two-mutex snapshot is still what
  orders admission against the snapshot; it no longer claims that no admitted removal can coexist with a
  `Yes`, since an admitted removal of another ref now does), the rule 3 comment block at `:471-480`, the
  carve and exit-guard comments where `carved` is maintained, and the `flushRefBatch` step-3 comment
  where the scope is validated.
- `Pool/CasRefProtocol.h:56-60`: the `MutationScope` comment, which calls the scope a batching hint and
  must say it is also what the confirm reads.
- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp:40-46` (see the tests section).
- `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md:126-142`, `CaRefLaneCore_RESULTS.md:43` and
  `:68-91`, and the models README (`CaRelinkConfirmCore.tla` row at `:141`, "lane quiescence", and its
  prose block at `:374-395`).

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

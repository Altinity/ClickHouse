---
description: 'Design for making the CAS fetch-by-relink confirm live under sustained write load: rule 3 refuses only the sent transaction that touches the queried ref, not every activity of the lane'
sidebar_label: 'CAS relink confirm liveness'
sidebar_position: 1
slug: /superpowers/specs/cas-relink-confirm-liveness-design
title: 'CAS relink confirm liveness design'
doc_type: 'design'
---

# CAS relink confirm liveness design {#cas-relink-confirm-liveness-design}

Status: revision 6 of 2026-09-02. Revision 1 relied on the part state machine (gate 0) to carry the
removal argument; review found the counterexample of a live repoint that retires a blob edge while the
part stays `Active`, and revision 1 is withdrawn. Revision 2 (rule 3 scoped to the sent transaction)
passed two independent consults, one on another model, on the design itself; revisions 3 to 6 fold in
their prose findings: the model extension is specified transition by transition, the two companion lane
modules that encode the old `Ready`-only certification contract are in scope, the existing rule-3 tests
the design reverses are named with their new expectations, and citations are corrected. The reports are
kept verbatim in [F11 spec consults](/superpowers/cas/f11-spec-consults-2026-09-02). The TLA+ work and
the code change remain to be done. Ledger finding F11, backlog item `[relink-confirm-lane-livelock]` in
`docs/superpowers/cas/BACKLOG/gcs.md`.

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

Three modules encode the old contract and all three change.

**`CaRelinkConfirmCore.tla`.** Today `SenderAdmit` admits only a transaction that retires the sender's
edge (`sTarget` is `Other` or `"none"`), `SenderDurable` always emits that `del` record and requires
`sDurableRef = Token`, `SenderApply` requires `sDurableRef # Token` and clears `sPending` and `sLeader`
together; rule 3 is `~sPending /\ ~sLeader`. There is no transaction that touches nothing, no armed
state, and no state "installed, tenure still open". The extension, stated as the transitions the model
task implements:

| Variable | Values | Meaning |
|---|---|---|
| `sPhase` | `idle`, `admitted`, `armed`, `durable`, `installed` | where the admitted transaction is; replaces the `sPending` boolean as the phase carrier |
| `sShape` | `none`, `touching`, `noop` | whether the admitted transaction retires the sender's edge (today's transaction) or is edge-neutral (the stand-in for a chunk about another ref; its journal record is `NsNoise`'s existing `noop` op with `src = NoiseSrc`); `none` while idle, for `TypeOK` |
| `sLeader` | boolean | unchanged meaning: a tenure is open |

`sTouches` is an operator, not a variable: `sTouches == sShape = "touching" /\ ~SabotageTouchBlind`.

| Step | Guard | Effect |
|---|---|---|
| `SenderAdmit(shape, nb)` | `sPhase = idle`, `sFence`, `~sPoison`, `sDurableRef = Token`, `nextId <= MaxId` | `sPhase' = admitted`, `sShape' = shape`, `sTarget' = nb`, `sLeader' = TRUE`; for `noop` the target is fixed (`nb = Token`) so TLC does not explore admits that differ only in a dead `sTarget` |
| `SenderArm` | `sPhase = admitted` | `sPhase' = armed` (the code's arm-before-first-send at `CasRefLedger.cpp:3585-3625`) |
| `SenderDurable` | `sPhase = armed`, `nextId <= MaxId` | `touching`: journal gets the `del` record, `sDurableRef' = sTarget`; `noop`: journal gets a `noop` record, `sDurableRef` unchanged; both: `nextId' = nextId + 1`, `sPhase' = durable` |
| `SenderInstall` | `sPhase = durable` | `sCacheRef' = sDurableRef`, `sPhase' = installed`; `sLeader` unchanged (the code's install-and-swap at `:3855-3859`) |
| `SenderTenureEnd` | `sPhase = installed` | `sPhase' = idle`, `sShape' = none`, `sLeader' = FALSE` (the code's tenure exit at `:2165`) |
| `SenderPoison` | `sPhase = durable` | `sPoison' = TRUE`, `sPhase' = idle`, `sLeader' = FALSE`; the current `sDurableRef # Token` conjunct goes, so a `noop` install can poison too |
| `FenceLoss` | as today, with `sPending` read as `sPhase # idle` | unchanged |

`sArmed` is `sPhase \in {armed, durable}`: the attempt is armed from before the first send until install,
which is exactly the interval in which the store may hold a transaction the row does not reflect. Rule 3
becomes `~(sArmed /\ sTouches)`. Sabotages: `SabotageStaleCache` drops the rule-3 conjunct entirely and
must still violate `ConfirmedRelinkNeverDangles` (the durable `m1 -> m2` repoint with the stale
`sCacheRef = m1`); `SabotageTouchBlind` forces `sTouches = FALSE` for a `touching` transaction and must
violate it too. Three reachability witnesses, each a history flag set inside `RConfirm` at the moment the
answer is computed, so the answer and the sender state are the same state: `sawYesArmedNoop` (answer
`yes` while `sArmed /\ ~sTouches`), `sawYesBetweenChunks` (answer `yes` while `sPhase = installed /\
sLeader`), `sawUnkArmedTouching` (answer `unknown` while `sArmed /\ sTouches`). `_main` must reach all
three; without the first two a green run could be the old behaviour. Run with `MaxHoles = 0`, as `_main`
does. The LIST-completeness caveat recorded in `CaRelinkConfirmCore_RESULTS.md` is unchanged and outside
this design.

**`CaRefLaneCore.tla`.** The lane model's `Certify` step (`:714-722`) protects three things at once:
`lane = "Ready"`, `CurrentRuntime` (this runtime still holds the namespace's authority), and the row's
currency (`cache_id = durable_id /\ cache_binding = durable_binding`). Only the first is what this design
changes, and it is widened, not removed: `Certify` is enabled iff
`CurrentRuntime /\ (lane = "Ready" \/ (Outstanding /\ ~attempt_touches_identity))`, where `Outstanding`
is the existing `lane \in {"Writing", "Wedged"}` (`:144`). `NeedsRecovery`, `Closed` and `Faulted` stay
excluded, exactly as the code's rule-3 switch refuses them; writing the guard as
`~(Outstanding /\ touches)` alone would admit them vacuously. `attempt_touches_identity` is derived, not
chosen: the attempt is `[id, token, binding]` (`:38`) and `StartWrite` already ranges over every binding
including the current one (`:966`), so `attempt_touches_identity == attempt # NoAttempt /\
attempt.binding # cache_binding`; a same-binding write is the model's non-touching transaction, and
`WriteLands` (`:237-238`) and `InstallCommitted` (`:252-253`) leave both bindings unchanged for it.
`bad_certification` records a certification whose identity's cache binding differs from its durable
binding; global `cache_id = durable_id` is not kept as the currency test, because a landed non-touching
transaction legitimately advances `durable_id` ahead of the certified identity's row. The flag
`SabotageCertifyBlocked` (`:26`) becomes "certify while the outstanding attempt touches the identity",
the invariant `CertifiedViewIsCurrent` (`:1083`) keeps its name and reads the new predicate, the config
`CaRefLaneCore_sab_certifyblocked.cfg` must still violate it, a new witness
`W_CertifiedWhileOutstanding` (a flag set in `Certify` when `Outstanding`) gets its own `_witness_*`
config that must be violated, and `run_reflane.sh` reruns the battery. Without the witness a rewrite
that left the `Ready`-only guard in place would pass every config.

**`CaRelinkLaneComposition.tla`.** The composition is a separate abstract model of the seam, not an
instance of the lane module, and it has no transaction content, so the touch flag is a nondeterministic
parameter of `StartWrite` (`:54-59`): a boolean `attempt_touches_source`, chosen when the lane leaves
`Ready` and cleared when it returns. The flag is semantically inert in the composition (`CommitWrite`
`:62-67` and `DeleteSource` `:175-181` ignore it), so the seam is checked only through the sabotage and
the witness, as it is today. `ConfirmSource` (`:111`) is enabled iff
`source_exists /\ (lane = "Ready" \/ (lane \in {"Writing", "Wedged"} /\ ~attempt_touches_source))`;
`RefuseBlockedConfirmation` (`:120`) is its exact complement, so a write that runs `Writing -> Wedged ->
Closed` (`CloseLane` `:70-76`) cannot confirm in `Closed`; `ConfirmWhileBlocked` (`:128`) under
`SabotageConfirmBlocked` certifies while the attempt touches the source or the lane is broken;
`ConfirmationRequiresReady` (`:209`) is renamed to say what it now checks; a new witness
`W_ConfirmedOutsideReady` (set in `ConfirmSource` when `lane # "Ready"`) gets its own config that must
be violated, since the existing `W_Confirmation` (`:214`) is satisfied by a `Ready` confirmation.
`run_relinklane.sh` reruns the battery, and `CaRefLaneCore_RESULTS.md` (`:43`, `:68-91`) is rewritten
from both runs. A green `CaRelinkConfirmCore` alone is not the model gate; all three modules are.

## Consult {#consult}

Two independent consults on revision 2, one on another model, each asked to refute the design,
returned no defect in it; their prose findings are folded into revisions 3 to 6, and the reports are
kept in [F11 spec consults](/superpowers/cas/f11-spec-consults-2026-09-02). The model diff, once
written, gets the same treatment before the code task is cut: is `sArmed` set before anything the store
can observe; does every transaction that changes `sDurableRef` have `sTouches` true; do all three
witnesses fire; does the composition model's rewritten seam still catch its three sabotages.

### Tests {#tests}

- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp`. Four existing tests encode the table-wide rule and
  are rewritten with phase-separated expectations rather than deleted:
  `InFlightAppendIsUnknown` (queued same-ref removal, leader parked before carve) becomes `Yes` while
  queued and unsent, `Unknown` from arming through `Writing`, `No` after install;
  `MidTenureChunkBoundaryIsUnknown` (untouched ref between two chunks of one tenure) becomes `Yes`;
  `ConcurrentAppendIsOrderedAfterTheSnapshot` becomes before admission `Yes`, admitted but unsent `Yes`,
  armed `Unknown`, installed `No`, with the same deterministic phase driving;
  `WedgedLaneIsUnknown` stays `Unknown` for every ref because `forceWedgeForTest` marks the attempt
  namespace-wide, and gains a sibling with a real wedged transaction (`ChunkFaultBackend::Mode::Unresolved`
  with a single-attempt budget, as `gtest_cas_ref_install_safety.cpp` already uses) where the touched
  ref answers `Unknown` and an unrelated ref `Yes`. The file header at `:40-46`, which states that no
  `Yes` may coexist with an admitted removal, is rewritten with the new phase table.
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
- `CasRefLedger.h:616`: the `prepared_attempt` field comment enumerates the attempt's fields as
  "COMPLETE" and gains the two new ones.
- `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp:40-46`: the file header (see the tests section).
- `docs/superpowers/models/CaRelinkConfirmCore_RESULTS.md:126-142`: the `_sab_stalecache` narrative
  describes rule 3 as tenure-wide; rewritten with the rerun (`:85-92` states the per-ref effect and
  stays true). `CaRefLaneCore_RESULTS.md:43` and `:68-91`: the `Ready`-only seam. The models README:
  the `CaRelinkConfirmCore.tla` row (`:141`, "lane quiescence") and its prose block (`:374-395`); the
  README has no row for `CaRelinkLaneComposition.tla`, and the `CaRefLaneCore.tla` row does not state
  the seam.
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

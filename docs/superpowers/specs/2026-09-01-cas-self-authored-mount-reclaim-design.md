---
description: 'Design for reclaiming a CAS mount lease immediately when the body in the mount slot is byte-identical to one this runtime wrote, replacing a full observation wait.'
sidebar_label: 'Self-authored mount reclaim'
sidebar_position: 42
slug: /superpowers/specs/cas-self-authored-mount-reclaim
title: 'CAS self-authored mount reclaim'
doc_type: 'guide'
---

# CAS self-authored mount reclaim {#cas-self-authored-mount-reclaim}

> **Prerequisites revised 2026-09-02** by `2026-09-02-cas-backend-token-contract-design.md`. That
> contract makes bytes and token come from one response and removes constructible tokens, so the
> directional-read tripwire and the paired-read prerequisite below are no longer needed; the
> byte-recognition rule and the driver-ownership capability stand. The Prerequisites section is to be
> rewritten against it once it lands.

Revision 5. Revisions 1 through 4 were reviewed and rejected;
[what earlier revisions got wrong](#what-earlier-revisions-got-wrong) records the traps, because they
are the ones a reader is most likely to fall into again.

## Problem {#problem}

A server that loses its mount lease because the object store stalled cannot take it back for
`mountObservationThresholdMs` = `ttl_ms + ttl_ms/20 + poll_interval_ms`. At the defaults — `ttl_ms`
30,000 and `poll_interval_ms` = `mount_renew_period/2` = 5,000 — that is **36.5 s**, and the elapsed
wait is longer still, because it is poll-quantised and an observation restart begins it again. It is
paid even though the same live process still holds every piece of evidence about what it last wrote.

Measured 2026-09-01 in scenario S03 at `--scale full`: a heartbeat write hung 23.7 s against a 30 s
TTL, the renewal ended without retained authority and fenced the mount locally, and every operation
then failed with `Code: 210 ... mount lease not held` while the self-remount produced
`mount_conflict` / `live_double_start` every five seconds.

Nothing here is broken. `claimMountAwaitingExpiry` is doing exactly what it documents: a stale lease
from a prior incarnation and a genuinely live twin are indistinguishable from a bare read, so it
watches the write-token on its own monotonic clock until the holder provably cannot still be within
its lease. The cost is the full threshold, paid for a distinction that does not exist in this case.

A second, quieter cost has the same root. Each remount attempt allocates a **fresh** `writer_epoch`
before claiming (`CasPool.cpp`, step `writer_epoch_allocate`), and seven further steps run after the
claim succeeds — `keeper_install`, `keeper_start`, `publish_writer_epoch`, `cancel_ref_recovery`,
`quiesce_ref_tables`, `arm_fence`, `publish_live`, with an optional `keeper_redo` before the arm.
`keeper_start`, the recovery cancellation, the quiesce and the redo can throw; the epoch publication
and `noteRemounted` cannot, and `armMountFence` only through its test hook. On a throw the attempt
returns `false`, and the next attempt reads a body **this same process wrote seconds ago**, at the
previous epoch — same `server_uuid`, different `writer_epoch`, not fenced, not clean — and pays the
full observation wait for it. Today that is unavoidable: nothing survives the failed attempt that
could say otherwise.

## The rule {#the-rule}

**State, held by `CasMountRuntime`:** a set of the exact bytes of mount-slot bodies this runtime has
written that could still be the current body. Nothing else. In particular **no token is remembered**;
see [why recognition is by bytes alone](#why-bytes-alone).

**On remount, at each poll of the observation loop**, having read the slot's bytes and token: the body
is **recognised** when the bytes read equal one of the retained bodies. On a recognised body, and only
while [the gate](#the-gate) holds, reclaim — an ordinary `putOverwrite` of a fresh body against **the
token just read**.

The read changes no state. State moves on writes only:

| event | effect on the set |
|---|---|
| immediately before the first physical attempt of any mount-slot write | add the exact bytes about to be sent |
| a write of ours confirms | the slot holds that body; the set becomes exactly that one body |
| the set exceeds its cap | drop the oldest |
| anything else, including every ambiguous or failed outcome | unchanged |

Retention is the safe default and dropping is the only thing that can go wrong, so the table has one
rule for adding, one for pruning, and no per-outcome classification at all. A retained body that never
landed can only fail to match. See [why there is no cleverer prune](#no-cleverer-prune).

### Why recognition is by bytes alone {#why-bytes-alone}

Every holder-originated mount body carries `write_attempt_id`, a fresh random 128-bit value minted per
body — in `makeMountBody` (`CasServerRoot.cpp:838`), in `MountLeaseKeeper::encodeBody` as fed by
`start` (`:1547`) and `renew` (`:1694`), and in the farewell (`:1833`). Two different writers therefore
cannot produce identical bytes, and byte equality with a write we sent is an authorship test that needs
no field added to the format.

A remembered token is **not** such a test, and revision 4's belief that it was is the defect that
produced this revision. `tokenFromWriteResult` in the ETag dialect, when the write response carries no
ETag, issues a **fresh `HEAD` and returns whatever is current then**, with nothing asserting that it
still belongs to our body (`CasObjectStorageBackend.cpp:842-866`). So a token we recorded as "ours"
can be a live twin's:

1. we write body A against token `t0`; A commits;
2. the response carries no ETag, so the fallback `HEAD` is issued, and it stalls;
3. a same-uuid twin observes A stable, claims with body B, receives `tB`, and arms authority;
4. our fallback `HEAD` returns `tB`, and we record it as the token of our own write;
5. a later remount recognises `(B, tB)` by token and overwrites a holder that still has authority.

This needs no illegal read ordering; it is a post-write attribution race. Recognising by bytes removes
the dependency rather than deferring it: the token is used only as the `If-Match` precondition, taken
from the read that just produced the bytes, where attribution is not in question.

Today the same fallback token flows into `MountLeaseKeeper::last_token`, where being wrong is
fail-safe — the next renewal's condition simply fails and the mount fences. Turning it into a
*recognition* input is what would have made it fail-dangerous. The underlying API asymmetry — the
generation dialect reports that a write cannot be attributed while the ETag dialect silently guesses —
is real and worth fixing, and is filed separately; this design does not wait for it.

Comparison is against the bytes **as sent**, retained verbatim, never a re-encoding. A re-encode can
differ innocently — `getFQDNOrHostName` alone can resolve differently — and a false mismatch silently
costs the fast path.

### Why there is no cleverer prune {#no-cleverer-prune}

Revision 4 also proposed dropping the bytes of a write the controller had definitively rejected. There
is no verdict to build that on. `putOverwriteControlled` returns only `Committed`, `Conflict` and
`Unresolved`; deterministic failures are **rethrown**, not reported. Guessing from the exception is
unsafe in the exact case that matters: a conditional `PUT` can commit and *then* `tokenFromWriteResult`
throw `CORRUPTED_DATA` while attributing an invalid generation, which the controller classifies as a
deterministic local failure and rethrows — so "it threw deterministically" can be true of a write whose
body is sitting in the slot. `DefiniteFailureAfterAmbiguity` must never prune either: the controller
reports it as `Unresolved` precisely because an earlier attempt may still land. And `claimMount`, the
keeper's claim and the farewell call the backend directly and produce no controller diagnostics at all.

So the only prune is the cap, and the cap is safe in the direction that matters: dropping a body can
lose a fast path, never grant a wrong reclaim. A cap of 16 bodies is a few kilobytes and far above what
a healthy remount produces. Acceptance records the exception rather than hiding it: a run that exceeds
the cap and then reads the evicted body waits, and that is correct behaviour, not a regression.

## The gate {#the-gate}

The hazard to exclude is **two runtimes holding mutation authority at the same time**, not two bodies
racing for the slot. A compare-and-swap on the mount slot invalidates the previous holder's cached
token but does not touch its local authority: `mayMutate` reads a local latch and a local
`CLOCK_BOOTTIME` deadline (`CasMountRuntime.cpp:92`) and learns nothing until its own next renewal
fails.

Recognition establishes that the only runtime that could derive authority from this body is this one.
The gate establishes that this one holds none. It is **not** a `mayMutate` sample:

- `MountFence` initialises permissively — `deadline_boot_ms` is `UINT64_MAX` with `lost = false`
  (`CasMountRuntime.h:117`) — so `mayMutate` is *true* before a lease was ever held;
- a sampled `false` can become `true` again before the reclaim's write is issued, and the observation
  loop sleeps between polls for tens of seconds;
- `tryRemountOnce`'s step 0 calls `noteLeaseLost`, which moves the lifecycle but does not set
  `mount_fence.lost`.

### The capability has to be built {#capability-must-be-built}

Revision 4 claimed the background worker "already owns `Parked` across its callback". It does not. The
remount loop sets `renewal_driver_state` to `ParkRequested`/`Parked` under `driver_mutex`, leaves the
lock scope, and calls `remount_attempt` with no lease object (`CasMountRuntime.cpp:773-792`); `Parked`
is a sampled enum value. The existing
`DriverLease` is private (`CasMountRuntime.h:359`), and `admitKeeperCall` requires an `Active` keeper
(`CasMountRuntime.cpp:324`) — which, after a terminal renewal, there is not. So this is a new admission
protocol, not existing plumbing threaded through.

What must be built:

- **A move-only ownership object**, mintable only by the code that parked the driver and admitted from
  `Parked` **without** requiring a keeper, since the reclaim runs before `installKeeper`. It must be
  passable into the claim and impossible for the claim to construct.
- **Revocation, not just possession.** `stopBackgroundWorkers` can set `workers_stop_requested` and
  overwrite the state while an operation runs outside the mutex — and `remount_attempt` runs exactly
  there (`CasMountRuntime::stopBackgroundWorkers`, `CasMountRuntime.cpp:834`), so holding a C++ object
  does not by itself prove continued ownership.
  This is not hypothetical during `FORGET`: the code explicitly permits an in-flight remount to reach
  `armMountFence` after terminal intent and the first fence trip (`CasPool.cpp:1064`).
- **A re-check immediately before the reclaim's `putOverwrite`**, of both latched loss and live
  ownership, because the observation loop sleeps and a snapshot taken at the decision is not a fact at
  the write.

Today no ordinary production path calls `armMountFence` concurrently with a claim — `remount_mutex`
serialises `tryRemountOnce`, and the only production re-arms are startup and the post-claim remount
step. That is a call-graph fact, not an enforced one: `armMountFence` takes no driver ownership. The
capability is what turns it into an enforced one.

### Where the gate holds {#where-the-gate-holds}

| call site | gate | note |
|---|---|---|
| background self-remount | obtainable, once the capability exists | the worker parks the driver and can mint it there |
| first open | refused | `lost = false`; nothing retained to present either |
| `NoWait` decommission mount | refused | same |
| `mountWritable`'s fence-recovery loop | refused | `lost = false`; the `gc_fenced` certificate is the fast path there |
| direct/forced `tryRemountOnce` | refused unless it acquires ownership explicitly | it owns nothing today |

Where the gate does not hold, the state is not offered and the observation wait applies.

## Safety {#safety}

**Recognition identifies exactly one possible authority-holder.** The bytes at the slot are
byte-identical to a write we sent, and no other writer can produce them. The gate says that writer
holds no authority.

**The conditional write closes the race window.** The read is a hint; `If-Match` is the arbiter. If one
of our other unresolved writes lands between our read and our write, our token is stale and the reclaim
fails; the next poll recognises the new body and reclaims against the token then current. If we win,
the old holder's cached token is consumed and its next write fails forever.

Three orderings, all safe:

| ordering | outcome |
|---|---|
| our reclaim lands first | the old keeper's cached token is stale; its next renewal fails the guard and it never writes again |
| our own unresolved write lands first | our token is stale, the reclaim fails; the next poll recognises its bytes and reclaims |
| a twin's claim races ours | exactly one compare-and-swap wins; the loser re-reads and finds a body it cannot recognise |

**Scope is the runtime, not the process.** Every `Pool::open` constructs its own `CasMountRuntime`, and
there is no process-global registry. The claim quantifies over the certifying runtime and every other
runtime that could write the same mount key.

**GC's fence-out.** GC rewrites the observed body with `gc_fenced = true` and `seq + 1`
(`CasServerRoot.cpp:1211-1215`), so the bytes no longer match ours. A fenced body therefore reaches the
`gc_fenced` certificate, which is checked first. Keeping that order is about classification fidelity —
recording `Fenced` rather than `SelfAuthored`, so a GC fence-out stays visible — not about safety.

**The same-epoch branch is untouched.** `claimMount`'s same-`writer_epoch` branch
(`CasServerRoot.cpp:936`) refreshes in place and checks no process identity at all. It is out of reach
here: the remount path allocates a fresh `writer_epoch` before claiming, so a body this runtime authored
is always at a *previous* epoch. This design changes only the same-uuid / different-epoch branch.

## What this is, and is not, a deferral of {#relation-to-resolved-by-get}

`CasRequestController::putOverwriteControlled` already resolves an ambiguous conditional write by
reading the key back and comparing the **exact bytes it sent**, adopting the token it reads
(`CasRequestControl.cpp:678-687`, `diagnostics.resolved_by_get`). Byte equality is therefore already
this codebase's accepted proof of authorship for a write whose response was lost, and this design
inherits settled, in-production comparison semantics rather than arguing them from first principles.

But it is not the same operation, and revision 4 overstated the kinship:

- the in-call leg requires `got->token != expected` as well as byte equality; this design has no
  `expected`. Under the mount-specific facts — a fresh `write_attempt_id` per body, and every content
  change producing a new token — dropping the clause admits nothing, but it does mean the two are not
  the same predicate;
- the in-call resolve runs while the caller still holds its lease and produces a *verdict about a write*.
  This runs after a fence loss, under a fresh epoch, and produces a **new authority-creating write**.
  That is why the gate and the recognition argument above have to stand on their own.

What the two do share is a dependency on the directional read, below.

## Prerequisites {#prerequisites}

Two conditions must be in place before the fast path is enabled. Neither is established by this
design's own code.

### An empty token must not become an unconditional write {#prerequisite-empty-token}

`Token{}` passes the conditional-write type check, `CasObjectStorageBackend` copies the empty value into
`object_storage_write_if_match`, and both S3 and Azure add `If-Match` only when that string is non-empty
(`WriteBufferFromS3.cpp:656`, `WriteBufferFromAzureBlobStorage.cpp:234`). The "conditional" reclaim would
then be an unconditional clobber of a live twin. Empty tokens are reachable on reads too: `nativeHead`
rejects an empty token only when `native_token_type == TokenType::Generation`
(`CasObjectStorageBackend.cpp:149-158`), so an ETag-mode `HEAD` passes an empty value straight through.

This is `[empty-token-unconditional-write-guard]`, already in the backlog at P2. It is a **prerequisite
of this work, not a citation**: `Backend::putOverwrite` must fail closed on an empty expected token
first. Emulated and in-memory backends already reject an empty token, so no test on them can catch this
— the guard needs its own native-backend test. The reclaim additionally refuses, with its own message,
rather than presenting an empty token to a backend that will refuse it anyway.

### The read must never pair older bytes with a newer token {#prerequisite-directional-read}

`ObjectStorageBackend::get` is a `HEAD` followed by a `GET`, so a replacement racing the read window
returns a mixed pair. The implementation states — and every existing consumer relies on — that the mix
is always `(bytes_newer, token_older)` and never the reverse, because `HEAD` strictly precedes `GET`
(`CasObjectStorageBackend.cpp:588-609`).

That direction is contained: we recognise newer bytes, reclaim against an older token, the conditional
write fails, and the next poll retries. The reverse direction is **not** contained: we would recognise
our own older bytes, take a live twin's newer token, and the conditional write would *succeed*.
`If-Match` cannot help — the token is genuinely current.

This is the same dependency `resolved_by_get` already carries, so with bytes-only recognition this
design adds no new logical assumption — but it adds a consumer and an invocation surface, and it must
be named, because a caching or replaying proxy in front of the store, or any `get` reordered to
`GET`-then-`HEAD`, breaks it silently. See [Tripwires](#tripwires).

## Where it plugs in {#where-it-plugs-in}

`claimMount` takes the retained bodies by value alongside `proven_dead_token`, plus the ownership
capability and a `pre_send` recorder. It stays a pure decision function and returns the token of a
successful write; a read gives the caller nothing to record, because the read records nothing.
`claimMountAwaitingExpiry` re-reads the set from the runtime on every poll rather than capturing it
once, so a write that lands mid-loop shortens the wait. `MountPriorState` gains `SelfAuthored`.

**Every writer records before it sends.** Today `claimMount` mints its body internally, at
`makeMountBody`, and sends it in the same breath — a caller cannot record those bytes in time, and a
write that throws after landing would leave nothing behind. So every mount-slot writer takes the
`pre_send` recorder and invokes it with the exact bytes immediately before the first physical attempt.
The holder-originated writers are: `claimMount`'s fresh mint, same-epoch refresh and reclaim branches;
`MountLeaseKeeper::claim`'s mint and adoption; `MountLeaseKeeper::renew`; and the farewell in
`MountLeaseKeeper::terminate`. GC's fence-out and decommission's token-exact delete are not ours.

A missed recording is safe — it costs the fast path and nothing else — which is what makes this rule
enforceable by a tripwire rather than by a proof. The reclaim is itself a recorded write, which is why
the state is a set: revision 3 kept one body, and the reclaim's own recording evicted the unresolved
renewal it existed to recover.

## Observability {#observability}

The `watermark_renew` event is unchanged: `attempts_sent`, `elapsed_ms`,
`remaining_confirmed_budget_ms`, `unresolved_reason`, `deadline_source` and `stop_cause` are what made
the S03 diagnosis possible. `diagnostics.resolved_by_get` and `observed_bytes` already say whether the
controller's in-call resolution succeeded; a deferred recognition is the same comparison arriving later,
and recording it lets the two be compared.

Three additions, one per branch of the reclaim decision:

**Reclaimed.** A `mount_claim` with prior state `SelfAuthored`. Worth recording alongside it: whether
the recognised body was the last one we confirmed or one whose outcome we never learned — the latter
means a write we gave up on actually landed, which is evidence about the store obtainable no other way.

**Refused, and why.** The body is ours by `server_uuid` but no retained body matched — a restart, a
twin, or a body evicted by the cap — and the observation wait now applies for about
`ttl_ms + ttl_ms/20 + poll_interval_ms`. The gate's own refusal — loss not latched, or no ownership
capability — is visible only to the caller, so the caller logs that one; `claimMount` cannot tell it
from a restart and must not pretend to.

**Refused because the body is foreign** — as today, naming the holder.

One correction while this work is in the claim path. `describeMountHolder`'s comment
(`CasServerRoot.cpp:852`) states that the mount-audit sink is not yet installed during `Pool::open`, so
that at first open these refusal messages are the only holder-identity carrier. That is stale:
`ContentAddressedMetadataStorage` populates `pool_config.event_sink` before calling open
(`ContentAddressedMetadataStorage.cpp:797`), and `Pool::open` installs it (`CasPool.cpp:511`) before
`mountWritable` (`CasPool.cpp:529`) and its claim. First-open claim and conflict events do reach
`system.cas_log`. Delete the stale sentence — revision 1 believed it and proposed startup wiring for a
gap that does not exist.

## Verification {#verification}

### A backend that can defer and duplicate {#deferred-backend}

The dangerous orderings are about **when a physical write is delivered**, and no existing mount gtest
backend can express them: `InMemoryBackend` and its subclasses apply writes synchronously. So the first
piece of work is a test backend that can hold a `putOverwrite`, land it at a point the test chooses,
deliver its response separately or not at all, queue two independently deliverable copies of one logical
write, and return a write response with no ETag so the fallback-`HEAD` path is reachable.

Read-pairing direction is deliberately **not** tested by making a fake backend produce the illegal pair:
a test that asserts the unsafe outcome cannot also be the alarm for it. See
[the four separate artifacts](#read-pairing-artifacts) below.

### Unit tests {#unit-tests}

In `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp`:

1. slot holds the body our last confirmed write put there → instant reclaim, prior state `SelfAuthored`;
2. our unresolved write landed → its bytes are recognised → instant reclaim against the token just read;
3. our unresolved write did not land → the previously confirmed body is recognised → instant reclaim;
4. bytes match nothing retained → no reclaim;
5. same `server_uuid`, different `pid`, empty set → no reclaim, and the log names the restart;
6. foreign body → no reclaim;
7. loss not latched (as at first open) → the state is not offered, observation wait;
8. no ownership capability presented → the state is not offered; and ownership **revoked between the
   decision and the write** (`Stopping`, or terminal `FORGET` intent) → the write must not be issued;
9. a `gc_fenced` body derived from ours → classified `Fenced`; its bytes do not match;
10. remount fails after a successful claim; the next attempt recognises the body that claim wrote;
11. a write commits and the response carries **no ETag** → the fallback `HEAD` returns a token that a
    twin's write made current → nothing is recorded that could recognise that twin's body, and no
    reclaim of it occurs. This is the revision 4 blocker, as a permanent regression test;
12. the retained bytes are the bytes sent, not a re-encoding: the body must be one whose canonical
    re-encode differs — an unknown field, or a changed hostname source — since a canonical round trip of
    an unchanged body would produce identical bytes and prove nothing;
13. an ambiguous renewal, then an ambiguous reclaim, then the renewal lands → the renewal's body is
    still retained and the next poll recognises it;
14. more unresolved bodies than the cap → the oldest is evicted, the evicted body is then the current
    one, and the run waits rather than reclaiming.

Late-delivery cases, requiring the test backend:

15. an old write issued before terminalization, delivered after the reclaim → fails its stale token;
16. two outstanding copies of one write, one landing before and one after the reclaim → at most one lands;
17. an old write landing between the reclaim's read and its write → the reclaim fails, the next poll recognises its bytes;
18. an unresolved renewal still deliverable while an ambiguous reclaim is outstanding, both conditional on the same token → exactly one lands, and the loop converges;
19. GC's fence write racing the reclaim, in both orders → the loser's token is stale; GC reclassifies;
20. an ambiguous `claimMount`, an ambiguous keeper adoption and an ambiguous farewell each leave usable bytes;
21. after a hard kill and restart, **no** `SelfAuthored` claim occurs — a negative assertion, because
    "the existing tests stay green" does not prove the new state was unavailable.

### The read-pairing property, as four separate artifacts {#read-pairing-artifacts}

Revision 4 asked one test to be both green in CI and an alarm. Split it:

- a **production-backend test** that `get` issues its `HEAD` strictly before its `GET`, so a reordering
  refactor fails;
- a **backend-contract test** that a token is never reused across different content;
- a **model sabotage** that permits the illegal pairing and must violate safety, so the consequence is
  proved rather than asserted in prose;
- a **native-backend test** that an empty expected token is refused rather than becoming an
  unconditional write.

### Model {#model}

**A focused TLA+ extension of `CaMountRenewRetryCore`, repaired before it is extended.** Two defects
make it unable to express this design as it stands:

- `localAuthority` is a single global flag, and `SamePairTwin` (`CaMountRenewRetryCore.tla:610`),
  `GCFence` (`:624`), `SuccessorClaim` (`:637`) and `ForeignHolderWrite` (`:651`) each clear it — so the
  model asserts that another writer's durable write revokes the modelled runtime's local authority,
  which is exactly the premise revision 1 was rejected for, baked in. Authority must become
  per-incarnation, and only that incarnation's own fence loss or deadline may clear it. `GCFence` is the
  one action that may legitimately clear another's, and only if given the elapsed-authority guard it
  currently lacks;
- it carries **one** logical request tuple, with `outstanding` counting physical copies of only that
  tuple (`:149`, `:339`), so it cannot represent an unresolved renewal and a reclaim at once — the
  interleaving this design most needs to check.

Once repaired, add: per-incarnation local authority with its own deadline; per-incarnation retained
bodies; several concurrent logical writes with distinct bodies; empty tokens and the conditionality loss
they cause; the ownership capability as a first-class object with acquisition, possession, revocation
and release, including `Stopping` or terminal intent racing a sleeping claim; `armMountFence` as a
separate transition that can occur between the gate snapshot and the reclaim; a restarted incarnation
with no retained state; and the reclaim as **two** actions — a gate snapshot followed by a delayed
conditional write — so the stale-sample race is reachable rather than assumed away.

Safety invariants: **no two incarnations hold mutation authority**; **a live twin's body is never
reclaimed**; and **no incarnation reclaims while it still retains authority itself**. The third is needed
because the first two are both satisfied when the reclaimer and the stale authority are the same
incarnation, which is precisely the gate this design adds.

Token reuse is a **sabotage configuration, never part of the safe model**. The backend contract forbids
reuse across different content (`CasBackend.h:226`); enabling it in the honest model would make any
token-derived reasoning trivially invalid and prove nothing.

Sabotages must be checked against the property they actually break, and a recovery sabotage needs a
stated temporal property or a history invariant with a required trigger — making a witness unreachable
is not by itself a TLC violation, so a recovery sabotage checked only against the safety invariants
passes while doing exactly the damage it models:

| sabotage | property it must violate |
|---|---|
| reclaim without recognition | safety |
| reclaim while the certifying incarnation retains authority | safety (third invariant) |
| reclaim on a gate snapshot rather than a live check at the write | safety |
| treat an empty token as conditional | safety |
| permit the illegal mixed-read direction | safety |
| reuse a token across different content | safety |
| clear retained bytes on an ambiguous outcome | recovery — stated as a temporal property |
| compare a rebuilt body instead of the sent bytes | recovery — stated as a temporal property |
| record the bytes after sending rather than before | recovery — stated as a temporal property |
| retain a single body instead of a set | recovery — stated as a temporal property |

Witnesses: fast reclaim when the unresolved write landed; fast reclaim when it did not; fast reclaim on
the retry after a remount failed past its claim.

### Integration and soak {#integration-and-soak}

Extend `tests/integration/test_cas_mount_renewal_retry`, which already exercises the renewal path under
injected faults. Soak: S39 (`lease fault tolerance`) and S03 at `--scale full`, where this was found.
The limit stated honestly: soak measures recovery latency, not concurrent mutation authority, and would
pass with a central safety defect intact. It is a regression net, not evidence of correctness.

## Assumptions {#assumptions}

**Byte equality implies authorship, because every holder body carries a fresh random 128-bit
`write_attempt_id`.** This is the uniqueness assumption the renewal specification already states for the
protocol as a whole — "random 128-bit IDs are treated with the same uniqueness assumption already used
for server and operation identities" — so this design adds no new one, and `resolved_by_get` already
rests on it. It does depend on the field surviving; see [Tripwires](#tripwires).

**A cloned process defeats it, and today's baseline is already unbounded.** A restored VM snapshot
yields two runtimes with the same `server_uuid`, the same retained state and the same PRNG state, so
they mint the same `write_attempt_id`. That alone is not enough for byte-identical bodies: wall time,
hostname, PID, `seq` and the watermark are encoded too, and the clones must stay in lockstep on all of
them. Where they do, one clone's write lands, the other loses `If-Match`, reads that identical body, and
`resolved_by_get` reports `Committed` to it as well — both extend local authority, and the cycle repeats
with no bound. Revisions 2 and 3 asserted the opposite baseline, that the token guard fences the loser
after roughly one renew period; it does not. Cloning already breaks single-writer exclusivity, and this
design does not measurably worsen it.

That is not a reason to relax. It is a reason to record separately that **`resolved_by_get` under
lockstep clones is what removes the bound** — a finding this work produced and does not fix.

## Acceptance {#acceptance}

1. After a store-induced lease loss with no restart, recovery contains no observation wait. Measured
   from `system.cas_log` as the interval between the `watermark_renew` failure and the pool's
   `TransientNotLive -> Live` transition — **not** the successful `mount_claim`, which precedes the
   post-claim steps that can fail while the disk still refuses every write. Two stated exceptions: a run
   whose current body was evicted by the cap waits, correctly; and `system.cas_log` delivery is
   best-effort and can drop under a full queue, so a missing row is not a failed run — the server log
   carries the same transitions.
2. That recovery's `mount_claim` records prior state `SelfAuthored`.
3. A remount that fails after its claim reclaims on the retry without an observation wait.
4. A restart still waits, the log says so before the wait begins, and no `SelfAuthored` claim occurs.
5. A hard-killed member still waits.
6. `Backend::putOverwrite` refuses an empty expected token, with a native-backend test.
7. The ownership capability cannot be constructed by a caller that does not own the driver, and a
   revocation between the decision and the write stops the write.
8. The repaired model's invariants hold under every configuration, and every sabotage violates the
   property the table assigns it.
9. S03 at `--scale full` no longer fails with `Code: 210` for this reason.

## Tripwires {#tripwires}

Four conditions this design depends on. If any stops holding, it is invalid.

**Every mount-slot mutation is an atomic conditional create, a conditional overwrite against an expected
token, or a token-exact delete; every successful mutation changes the token; and no expected token is
ever empty.** Revision 2 stated the first two and was still wrong, because an empty token makes the
native overwrite unconditional.

**A `get` never pairs older bytes with a newer token.** The safety argument uses the conditional write to
contain a mixed read, and containment works in one direction only. A caching or replaying proxy, or a
`get` reordered to `GET`-then-`HEAD`, breaks it with no visible symptom.

**Every holder-originated mount body carries a fresh random per-body identifier.** Remove or deduplicate
`write_attempt_id` and byte equality stops implying authorship, silently.

**Every mount-slot writer records its bytes before its first physical attempt, and the recorded bytes are
the bytes sent.** A writer that skips this, or a refactor that compares a rebuild instead, does not fail —
it quietly returns to the observation wait.

## Out of scope {#out-of-scope}

**Write-token provenance in the backend API.** The generation dialect reports that a write cannot be
attributed; the ETag dialect silently substitutes a later, unrelated `HEAD`. Aligning them means putting
provenance in `PutResult`, not making the ETag path throw — a `nullopt` is structural for backends with
no write-time token at all, such as local files. Filed separately; this design removes its dependency on
the answer rather than waiting for it.

**`resolved_by_get` under lockstep clones.** Recorded above; file it. This design does not depend on it
being fixed.

**The renewal loop's own bounds.** ch2 stopped with 1,969 ms of confirmed budget unspent and
`stop_cause = continue`, and one attempt ran 23.7 s against #2244's documented 5 s per-attempt PUT
timeout. Both are about how long a renewal tries, not about who may reclaim. Tracked in
`[renewal-gives-up-with-budget-left]`.

**Renaming `started_at_ms`.** The field is stamped with the time each body is built, not with process
start, and `system.content_addressed_mounts` surfaces it as `started_at` described as "Time when the
lease started" — which is what led revision 1 to use it as a process-incarnation identifier. This design
needs no such field, so the rename to `body_written_at_ms` is ordinary documentation debt. File it; do
not fold it in.

**`[decommission-waits-on-the-wrong-predicate]`.** A hard-killed member has nothing to present, so
nothing here helps it. Its own entry warns against weakening the certificate rule to close it; that
warning applies here too — this adds a way to recognise our own body, it does not relax the rule.

**#2243's trigger, the leaked mount thread on `DROP TABLE`, and the per-step remount state machine.**
Separate subsystems, unchanged here. This design removes one of the per-step machine's motivations — the
cost of a failed attempt — but not the machine's own case.

## What this closes {#what-this-closes}

- #2244's deferred follow-up item on whether an own ambiguous claim preserves or resets token-stability
  observation: it preserves it, by retaining the bytes, and the read resolves the ambiguity.
- The failed-remount-attempt cost described in [Problem](#problem).
- `[empty-token-unconditional-write-guard]`, as a prerequisite rather than a side effect.

It does **not** close the per-step remount state machine, which remains #2244's open follow-up.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

Kept because each error is cheaper to read than to repeat. The through-line is that four revisions in a
row reached for extra machinery where the answer was to trust less: an identity field, then a possession
set, then a remembered token, then a per-outcome prune — each removed by the next revision.

**Revision 1** used `started_at_ms` as a process-incarnation identifier, though every renewal restamps
it. It claimed the store supplies the mutual exclusion — a compare-and-swap serialises the slot but does
not revoke the previous holder's local authority. And it declined a focused TLA+ model by arguing from a
false choice between the 1,670-line `CaCasMountCore` and nothing.

**Revision 2** added a `writer_incarnation_id` field and a possession set of attempt ids — both
unnecessary, since `write_attempt_id` already makes bodies unique. It guarded the remembered token
against emptiness but not the observed one used as the write's precondition. It asserted that at most
one write can be unresolved at a time, confusing "one logical call in progress" with "one physically
deliverable request". It called the token check assumption-free and `mayMutate` a sufficient gate. And it
kept, as an alternative, shortening the wait from our own send time — which says nothing about whose
lease is in the slot.

**Revision 3** mutated state on the read path, and retained one body rather than a set, so the reclaim's
own recording evicted the unresolved renewal it existed to recover. It claimed the conditional write
contains a mixed read in both directions. It keyed the lifecycle on `attempts_sent`, which counts backend
entry rather than a physical send. And it asserted a cloning baseline bounded by one renew period.

**Revision 4** recognised a body by a remembered token, which `tokenFromWriteResult`'s fallback `HEAD`
can attribute to someone else's write entirely — the blocker this revision removes by recognising bytes
alone. It claimed the driver-ownership capability already existed; it does not, and building it needs a
new admission protocol with revocation. It proposed pruning on a definitive rejection the controller
never reports. And it asked one test to be both green in CI and the alarm for the property it tests.

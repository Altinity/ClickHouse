---
description: 'Design for reclaiming a CAS mount lease immediately when the body in the mount slot is one this runtime wrote, by deferring the request controller''s own exact-byte resolution past the call boundary, replacing a full observation wait.'
sidebar_label: 'Self-authored mount reclaim'
sidebar_position: 42
slug: /superpowers/specs/cas-self-authored-mount-reclaim
title: 'CAS self-authored mount reclaim'
doc_type: 'guide'
---

# CAS self-authored mount reclaim {#cas-self-authored-mount-reclaim}

Revision 4. Revisions 1, 2 and 3 were reviewed and rejected;
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
Several of them can throw (`keeper_start`, the recovery cancellation and the quiesce most readily;
the epoch publication and `noteRemounted` cannot, and `armMountFence` only through its test hook), and
the attempt then returns `false`. The next attempt therefore reads a body **this same process wrote
seconds ago**, at the previous epoch — same `server_uuid`, different `writer_epoch`, not fenced, not
clean — and pays the full observation wait for it. Today that is unavoidable: nothing survives the
failed attempt that could say otherwise.

## This is the controller's own resolution, deferred {#deferred-resolution}

The mechanism is not new here. `CasRequestController::putOverwriteControlled` already resolves an
ambiguous conditional write by reading the key back and comparing the **exact bytes it sent**: on a
match with a changed token it reports `Committed` and adopts the token it just read
(`CasRequestControl.cpp:680`, `diagnostics.resolved_by_get`). Byte equality is already this
codebase's accepted proof of authorship for a write whose response was lost.

What this design changes is **when** that resolution may run. Today it runs inside the call, bounded
by the operation deadline and the lease-safety gate — so precisely when the store is stalling, the
resolve stalls with it, the call ends `Unresolved`, and the bytes are discarded along with everything
they could still have proven. That is S03. Retaining the bytes gives the same resolution a second
chance later, from the remount loop, once the store has recovered.

Two consequences worth stating up front. The comparison semantics are settled and already in
production, so this design does not have to justify them from first principles. And no relaxation of
the controller's conservatism is needed: `FenceLostPostWrite` deliberately withholds the token of a
write it knows committed, and retained bytes recover that information later without the controller
ever claiming a commit it must not claim.

## The rule {#the-rule}

**State, held by `CasMountRuntime`:**

- `confirmed_token` — the token the store returned for our last **confirmed** mount-slot write;
- `unresolved_bodies` — the exact bytes of mount-slot writes we have sent whose outcome we do not
  know.

Both are held together for the duration of an unresolved write, and that pairing is the whole design:
the previous token covers "the write did not land", the bytes cover "it did". Sending a write does not
touch `confirmed_token`; only a confirmation does.

**On remount, at each poll of the observation loop**, having read the slot's bytes and token, the body
is **recognised** when either

- the token read is non-empty and equals `confirmed_token`, or
- the bytes read equal one of `unresolved_bodies`.

On a recognised body, and only while [the gate](#the-gate) holds, reclaim: an ordinary `putOverwrite`
of a fresh body against **the token just read**.

**The read path changes no state.** It does not adopt the token it read, and it does not drop the
bytes it matched. State moves only when a write of ours confirms, which is the one moment we learn a
token that is certainly paired with bytes we certainly wrote. Revision 3 mutated state on the read
and lost its own evidence two different ways; see
[what earlier revisions got wrong](#what-earlier-revisions-got-wrong).

### Why no new identity field is needed {#no-new-identity-field}

Every holder-originated mount body carries `write_attempt_id`, a fresh random 128-bit value minted per
body — in `makeMountBody` (`CasServerRoot.cpp:838`), in `MountLeaseKeeper::encodeBody` as fed by
`start` (`:1547`) and `renew` (`:1694`), and in the farewell (`:1833`). Two different writers therefore
cannot produce identical bytes, and byte equality with a write we sent is already an authorship test.
The discriminator is in the format; nothing needs adding to it.

Comparison is against the bytes **as sent**, retained verbatim, never against a re-encoding. A
re-encode can differ innocently — `getFQDNOrHostName` alone can resolve differently — and a false
mismatch silently costs the fast path.

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
  loop runs for tens of seconds;
- `tryRemountOnce`'s step 0 calls `noteLeaseLost`, which moves the lifecycle but does not set
  `mount_fence.lost`.

So the fast path is admitted only while **both** hold:

- this runtime has **latched** a mount loss — `mount_fence.lost` set and its fence generation bumped —
  rather than merely never having armed a fence; and
- the caller **holds** renewal-driver ownership, as an unforgeable capability, across the whole claim.
  Sampling the driver-state enum is not enough: the enum can be read by a caller that owns nothing.
  The remount callback must receive the `Parked`/`Dormant` lease it already runs under, and present
  it; a caller that cannot present one does not get the fast path.

Where the gate does not hold, the state is simply not offered and the observation wait applies. By
call site:

| call site | gate | note |
|---|---|---|
| background self-remount | obtainable | the worker already owns `Parked` across its callback |
| first open | refused | `lost = false`; nothing retained to present either |
| `NoWait` decommission mount | refused | same |
| `mountWritable`'s fence-recovery loop | refused | `lost = false`; the `gc_fenced` certificate is the fast path there |
| direct/forced `tryRemountOnce` | refused unless it acquires ownership explicitly | it owns no lease today |

## Safety {#safety}

**Recognition identifies exactly one possible authority-holder.** Either the store returned this token
to us on a confirmed write, or the bytes at it are byte-identical to a write we sent. Under either, the
only runtime that could derive authority from this body is this one — and the gate says this one holds
none.

**The conditional write closes the race window.** The read is a hint; `If-Match` is the arbiter. If our
unresolved write lands between our read and our write, our token is stale and the reclaim fails; the
next poll recognises its bytes and reclaims against the token then current. If we win, the old
holder's cached token is consumed and its next write fails forever.

Three orderings, all safe:

| ordering | outcome |
|---|---|
| our reclaim lands first | the old keeper's cached token is stale; its next renewal fails the guard and it never writes again |
| our own unresolved write lands first | our token is stale, the reclaim fails; the next poll recognises its bytes and reclaims |
| a twin's claim races ours | exactly one compare-and-swap wins; the loser re-reads and finds a body it cannot recognise |

**Scope is the runtime, not the process.** Every `Pool::open` constructs its own `CasMountRuntime`,
and there is no process-global registry. The claim quantifies over the certifying runtime and every
other runtime that could write the same mount key.

**GC's fence-out.** GC rewrites the observed body with `gc_fenced = true` and `seq + 1`
(`CasServerRoot.cpp:1211-1215`), so the bytes no longer match ours and the token it consumed no longer
matches `confirmed_token`. A fenced body therefore reaches the `gc_fenced` certificate, which is
checked first. Keeping that order is about classification fidelity — recording `Fenced` rather than
`SelfAuthored`, so a GC fence-out stays visible — not about safety.

**The same-epoch branch is untouched.** `claimMount`'s same-`writer_epoch` branch
(`CasServerRoot.cpp:936`) refreshes in place and checks no process identity at all. It is out of reach
here: the remount path allocates a fresh `writer_epoch` before claiming, so a body this runtime
authored is always at a *previous* epoch. This design changes only the same-uuid / different-epoch
branch.

## Prerequisites {#prerequisites}

Two conditions must be in place before the fast path is enabled. Neither is optional and neither is
established by this design's own code.

### An empty token must not become an unconditional write {#prerequisite-empty-token}

`Token{}` passes the conditional-write type check, `CasObjectStorageBackend` copies the empty value
into `object_storage_write_if_match`, and both S3 and Azure add `If-Match` only when that string is
non-empty (`WriteBufferFromS3.cpp:656`, `WriteBufferFromAzureBlobStorage.cpp:234`). The "conditional"
reclaim would then be an unconditional clobber of a live twin. Empty tokens are reachable:
`tokenFromWriteResult`'s `HEAD` fallback can produce one on a `Done` write, and reads admit one too —
`nativeHead` rejects an empty token only when `native_token_type == TokenType::Generation`
(`CasObjectStorageBackend.cpp:153`), so an ETag-mode `HEAD` passes an empty value straight through.

This is `[empty-token-unconditional-write-guard]`, already in the backlog at P2. It is a
**prerequisite of this work, not a citation**: `Backend::putOverwrite` must fail closed on an empty
expected token first. Emulated and in-memory backends already reject an empty token, so no test on
them can catch this — the guard needs its own native-backend test. The reclaim additionally refuses,
with its own message, rather than presenting an empty token to a backend that will refuse it anyway.

### The read must never pair newer bytes with a newer token {#prerequisite-directional-read}

`ObjectStorageBackend::get` is a `HEAD` followed by a `GET`, so a replacement racing the read window
returns a mixed pair. The implementation states — and every existing consumer relies on — that the
mix is always `(bytes_newer, token_older)` and never the reverse, because `HEAD` strictly precedes
`GET` (`CasObjectStorageBackend.cpp:588-609`).

That direction is contained: we recognise newer bytes, reclaim against an older token, the conditional
write fails, and the next poll retries. The reverse direction is **not** contained: we would recognise
our own older bytes, take a live twin's newer token, and the conditional write would *succeed*,
overwriting a holder that still has authority. `If-Match` cannot help — the token is genuinely current.

So this design depends on the directional property, and depends on it for safety rather than for
liveness. It is the same dependency `resolved_by_get` already carries on the renewal path, so this
adds no new exposure — but it must be named, because a caching or replaying proxy in front of the
store, or any `get` reordered to `GET`-then-`HEAD`, breaks it silently. See
[Tripwires](#tripwires); the model must constrain its mixed-read action to the one legal direction,
so that a future change permitting the other produces a counterexample rather than nothing.

## State lifecycle {#state-lifecycle}

Both values are owned by `CasMountRuntime`, read and written under its own mutex. The lifecycle has
one rule and two prunes.

**The rule: state changes only on a confirmed write of ours.** On confirmation with a non-empty token,
`confirmed_token` becomes that token and `unresolved_bodies` is emptied — the slot is at a token we
know, so no earlier body of ours can still be current. On confirmation with an empty token,
`confirmed_token` is cleared and the bytes are **kept**: we know the content, not the token, and a
later read can still recognise it.

Everything else retains. This is deliberately blunter than revision 3's per-outcome table, which tried
to key on `attempts_sent` and got it wrong in both directions: `attempts_sent` is incremented and
`PutStarted` emitted *before* `Backend::putOverwrite` is called (`CasRequestControl.cpp:601-605`), so a
purely local rejection still counts as an attempt, while `claimMount`, `MountLeaseKeeper::claim` and
the farewell call the backend directly and produce no such diagnostics at all. Retention is the safe
default: a retained body that never landed can only fail to match.

**Prune 1 — a definite failure.** A write the controller reports as definitively rejected — not
ambiguous — provably never applied, and its bytes are dropped. This keeps the set to genuinely
ambiguous writes, which are rare.

**Prune 2 — a bound.** The set is capped, oldest dropped first. Dropping can only lose a fast path,
never grant a wrong reclaim, so the cap is a fail-safe bound rather than a fallback. A cap of 16
bodies is a few kilobytes and is far above what a healthy remount produces.

**Every writer records before it sends.** Today `claimMount` mints its body internally, at
`makeMountBody`, and sends it in the same breath — a caller cannot record those bytes in time, and a
write that throws after landing would leave nothing behind. So every mount-slot writer takes a
`pre_send` recorder and invokes it with the exact bytes immediately before the first physical attempt.
The holder-originated writers are: `claimMount`'s fresh mint, same-epoch refresh and reclaim branches;
`MountLeaseKeeper::claim`'s mint and adoption; `MountLeaseKeeper::renew`; and the farewell in
`MountLeaseKeeper::terminate`. GC's fence-out and decommission's token-exact delete are not ours.

A missed recording is safe — it costs the fast path and nothing else — which is what makes this rule
enforceable by a tripwire rather than by a proof.

**The reclaim is itself a recorded write.** This is what revision 3 got wrong: with a single retained
body, the reclaim's own `pre_send` evicted the unresolved renewal it was trying to recover from, so
the "our own unresolved write lands first" ordering became unrecoverable and its own test could not
pass. A set, not a slot.

## Where it plugs in {#where-it-plugs-in}

`claimMount` takes the pair by value alongside `proven_dead_token`, plus the driver-ownership
capability and a `pre_send` recorder. It stays a pure decision function and returns the token of a
successful write; it returns nothing for the caller to record from a *read*, because the read records
nothing. `claimMountAwaitingExpiry` re-reads the pair from the runtime on every poll rather than
capturing it once, so a write that lands mid-loop shortens the wait. `MountPriorState` gains
`SelfAuthored`.

## Observability {#observability}

The `watermark_renew` event is unchanged: `attempts_sent`, `elapsed_ms`,
`remaining_confirmed_budget_ms`, `unresolved_reason`, `deadline_source` and `stop_cause` are what made
the S03 diagnosis possible. `diagnostics.resolved_by_get` and `observed_bytes` already exist and say
whether the controller's in-call resolution succeeded; a deferred recognition is the same fact
arriving later, and should be recorded so the two can be compared.

Three additions, one per branch of the reclaim decision:

**Reclaimed.** A `mount_claim` with prior state `SelfAuthored`, saying whether the token matched
directly or the bytes were recognised. The distinction is worth recording: a byte match means a write
we gave up on actually landed — evidence about the store that is otherwise unobtainable.

**Refused, and why.** The body is ours by `server_uuid` but nothing was recognised. Today this is an
undifferentiated `live_double_start`. It must name the reason and the cost: neither the token nor any
retained body matched (a restart, a twin, or a body we no longer hold), and the observation wait now
applies for about `ttl_ms + ttl_ms/20 + poll_interval_ms`. The gate's own refusal — loss not latched,
or no driver-ownership capability — is visible only to the caller, so the caller logs that one;
`claimMount` cannot tell it from a restart and must not pretend to.

**Refused because the body is foreign** — as today, naming the holder.

One correction while this work is in the claim path. `describeMountHolder`'s comment
(`CasServerRoot.cpp:852`) states that the mount-audit sink is not yet installed during `Pool::open`,
so that at first open these refusal messages are the only holder-identity carrier. That is stale:
`ContentAddressedMetadataStorage` populates `pool_config.event_sink` before calling open
(`ContentAddressedMetadataStorage.cpp:797`), and `Pool::open` installs it (`CasPool.cpp:511`) before
`mountWritable` (`CasPool.cpp:529`) and its claim. First-open claim and conflict events do reach
`system.cas_log`. Delete the stale sentence — revision 1 believed it and proposed startup wiring for a
gap that does not exist.

## Verification {#verification}

### A backend that can defer, duplicate and skew {#deferred-backend}

The dangerous cases are about **when a physical write is delivered** and **how a read pairs bytes with
a token**, and no existing mount gtest backend can express either: `InMemoryBackend` and its subclasses
apply writes synchronously and read atomically. So the first piece of work is a test backend that can
hold a `putOverwrite`, land it at a point the test chooses, deliver its response separately or not at
all, queue two independently deliverable copies of one logical write, return a mixed `(bytes, token)`
pair in **either** direction, and reuse a token value. Without it, the tests below only restate what
synchronous, atomic code already guarantees.

### Unit tests {#unit-tests}

In `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp`:

1. slot token equals `confirmed_token` → instant reclaim, prior state `SelfAuthored`;
2. our unresolved write landed: bytes recognised → instant reclaim against the token just read;
3. our unresolved write did not land → the previous `confirmed_token` still matches, instant reclaim;
4. bytes differ and token differs → no reclaim;
5. same `server_uuid`, different `pid`, empty state → no reclaim, and the log names the restart;
6. foreign body → no reclaim;
7. loss not latched (as at first open) → the state is not offered, observation wait;
8. no driver-ownership capability presented → the state is not offered — and, separately, that
   ownership is **held across** the conditional write, not merely sampled before it: the test releases
   ownership between the decision and the write and the write must not be issued;
9. a `gc_fenced` body derived from ours → classified `Fenced`; neither bytes nor token match;
10. remount fails after a successful claim; the next attempt reclaims on the recorded token;
11. a write confirmed with an empty token → bytes retained, no token reclaim; and a later read that
    recognises those bytes **while returning an empty token** → refused, and the bytes are still held;
12. the retained bytes are the bytes sent, not a re-encoding: the body must be one whose canonical
    re-encode differs — an unknown field, or a changed hostname source — since a canonical round trip
    of an unchanged body would produce identical bytes and prove nothing;
13. an ambiguous renewal, then an ambiguous reclaim, then the renewal lands → the renewal's body is
    still retained and the next poll recognises it (the ordering revision 3 could not satisfy);
14. more unresolved bodies than the cap → the oldest is dropped, and the drop costs only the fast path.

Late-delivery and read-pairing cases, all requiring the test backend:

15. an old write issued before terminalization, delivered after the reclaim → fails its stale token;
16. two outstanding copies of one write, one landing before and one after the reclaim → at most one lands;
17. an old write landing between the reclaim's read and its write → the reclaim fails, the next poll recognises its bytes;
18. an unresolved renewal still deliverable while an ambiguous reclaim is outstanding, both conditional on the same token → exactly one lands, and the loop converges;
19. GC's fence write racing the reclaim, in both orders → the loser's token is stale; GC reclassifies;
20. an ambiguous `claimMount`, an ambiguous keeper adoption, and an ambiguous farewell each leave usable bytes;
21. a mixed read in the legal direction (`bytes_newer, token_older`) → the reclaim fails closed and recovers;
22. a mixed read in the illegal direction (`bytes_older, token_newer`) → **documents the exposure**: this
    test exists to fail loudly if a backend ever produces it, and is the executable form of the
    directional tripwire;
23. a reused token value against different bytes → the backend contract is violated and the reclaim must
    not be the thing that discovers it silently;
24. a native-backend test that an empty expected token is refused rather than becoming an unconditional write;
25. after a hard kill and restart, **no** `SelfAuthored` claim occurs — a negative assertion, because
    "the existing tests stay green" does not prove the new state was unavailable.

### Model {#model}

**A focused TLA+ extension of `CaMountRenewRetryCore`, repaired before it is extended.** Two defects
make it unable to express this design as it stands:

- `localAuthority` is a single global flag, and `SamePairTwin`, `GCFence`, `SuccessorClaim` and
  `ForeignHolderWrite` each **clear** it (`CaMountRenewRetryCore.tla:615`, `:628`, `:642`, `:656`) — so
  the model asserts that another writer's durable write revokes the modelled runtime's local authority,
  which is exactly the premise revision 1 was rejected for, baked in. Authority must become
  per-incarnation, and only that incarnation's own fence loss or deadline may clear it. `GCFence` is
  the one action that may legitimately clear another's, and only if it is given the elapsed-authority
  guard it currently lacks;
- it carries **one** logical request tuple, with `outstanding` counting physical copies of only that
  tuple (`:149`, `:339`), so it cannot represent an unresolved renewal and a reclaim at once — the
  interleaving this design most needs to check.

Once repaired, add: per-incarnation local authority with its own deadline; per-incarnation retained
bodies and confirmed token; several concurrent logical writes with distinct bodies; empty tokens and
the conditionality loss they cause; a **directional** mixed read, legal direction only; explicit token
reuse, so that ABA is tested rather than assumed away by distinct bodies; a restarted incarnation with
no retained state; and the reclaim as **two** actions — an evidence-and-gate snapshot followed by a
delayed conditional write — so the stale-sample race is reachable rather than assumed away.

Safety invariants: **no two incarnations hold mutation authority**; **a live twin's body is never
reclaimed**; and **no incarnation reclaims while it still retains authority itself**. The third is
needed because the first two are both satisfied when the reclaimer and the stale authority are the
same incarnation, which is precisely the gate this design adds.

Sabotages must be checked against the property they actually break, because one checked against the
wrong property passes and proves nothing:

| sabotage | property it must violate |
|---|---|
| reclaim without recognition | safety |
| reclaim while the certifying incarnation retains authority | safety (third invariant) |
| reclaim on a gate sample taken before the write | safety |
| treat an empty token as conditional | safety |
| permit the illegal mixed-read direction | safety |
| clear retained bytes on an **ambiguous** terminal outcome | recovery |
| compare a rebuilt body instead of the sent bytes | recovery |
| record the bytes after sending rather than before | recovery |
| retain a single body instead of a set | recovery (revision 3's defect) |

Witnesses: fast reclaim when the unresolved write landed; fast reclaim when it did not; fast reclaim
on the retry after a remount failed past its claim.

### Integration and soak {#integration-and-soak}

Extend `tests/integration/test_cas_mount_renewal_retry`, which already exercises the renewal path
under injected faults. Soak: S39 (`lease fault tolerance`) and S03 at `--scale full`, where this was
found. Note the limit honestly: soak measures recovery latency, not concurrent mutation authority, and
would pass with a central safety defect intact. It is a regression net, not evidence of correctness.

## Assumptions {#assumptions}

**Byte equality implies authorship, because every holder body carries a fresh random 128-bit
`write_attempt_id`.** This is the uniqueness assumption the renewal specification already states for
the protocol as a whole — "random 128-bit IDs are treated with the same uniqueness assumption already
used for server and operation identities" — so this design adds no new one, and `resolved_by_get`
already rests on it. It does depend on the field surviving; see [Tripwires](#tripwires).

**A cloned process defeats it, and today's baseline is already unbounded.** A restored VM snapshot
yields two runtimes with the same `server_uuid`, the same retained state, and the same PRNG state.
Revisions 2 and 3 both claimed a baseline that does not exist: that the store's token guard fences
whichever clone loses, so the overlap is short and self-correcting. It is not. With identical PRNG
state the clones build **byte-identical** renewal bodies; one lands, the other loses `If-Match`, reads
that identical body, and `resolved_by_get` reports `Committed` to it as well. Both extend local
authority, and the cycle repeats. So cloning already breaks single-writer exclusivity without bound,
and this design does not measurably worsen it.

That is not a reason to relax. It is a reason to record separately that **`resolved_by_get` is what
makes cloning non-self-correcting** — a finding this work produced and does not fix.

Nothing else weakens. A restart with `pid` reuse in the same millisecond recognises nothing, because
the retained bytes and token are gone.

## Acceptance {#acceptance}

1. After a store-induced lease loss with no restart, recovery contains no observation wait. Measured
   from `system.cas_log` as the interval between the `watermark_renew` failure and the pool's
   `TransientNotLive -> Live` transition — **not** the successful `mount_claim`, which precedes the
   post-claim steps that can fail while the disk still refuses every write. `system.cas_log` delivery
   is best-effort and can drop under a full queue, so a missing row is not a failed run; the server
   log carries the same transitions.
2. That recovery's `mount_claim` records prior state `SelfAuthored`.
3. A remount that fails after its claim reclaims on the retry without an observation wait.
4. A restart still waits, the log says so before the wait begins, and no `SelfAuthored` claim occurs.
5. A hard-killed member still waits.
6. `Backend::putOverwrite` refuses an empty expected token, with a native-backend test.
7. The repaired model's invariants hold under every configuration, and every sabotage violates the
   property the table assigns it.
8. S03 at `--scale full` no longer fails with `Code: 210` for this reason.

## Tripwires {#tripwires}

Four conditions this design depends on. If any stops holding, it is invalid.

**Every mount-slot mutation is an atomic conditional create, a conditional overwrite against an
expected token, or a token-exact delete; every successful mutation changes the token; and no expected
token is ever empty.** Revision 2 stated the first two and was still wrong, because an empty token
makes the native overwrite unconditional.

**A `get` never pairs older bytes with a newer token.** The safety argument uses the conditional write
to contain a mixed read, and containment works in one direction only. A caching or replaying proxy, or
a `get` reordered to `GET`-then-`HEAD`, breaks it with no visible symptom. Test 22 is its executable
form.

**Every holder-originated mount body carries a fresh random per-body identifier.** Remove or
deduplicate `write_attempt_id` and byte equality stops implying authorship, silently.

**Every mount-slot writer records its bytes before its first physical attempt, and the recorded bytes
are the bytes sent.** A writer that skips this, or a refactor that compares a rebuild instead, does not
fail — it quietly returns to the observation wait.

## Out of scope {#out-of-scope}

**The renewal loop's own bounds.** ch2 stopped with 1,969 ms of confirmed budget unspent and
`stop_cause = continue`, and one attempt ran 23.7 s against #2244's documented 5 s per-attempt PUT
timeout. Both are real and both are about how long a renewal tries, not about who may reclaim. Tracked
in `[renewal-gives-up-with-budget-left]`.

**`resolved_by_get` under process cloning.** Recorded above; file it. Fixing it is not this design's
job, and this design does not depend on it being fixed.

**Renaming `started_at_ms`.** The field is stamped with the time each body is built, not with process
start, and `system.content_addressed_mounts` surfaces it as `started_at` described as "Time when the
lease started" — which is what led revision 1 to use it as a process-incarnation identifier. This
design needs no such field, so the rename to `body_written_at_ms` is ordinary documentation debt. File
it; do not fold it in.

**`[decommission-waits-on-the-wrong-predicate]`.** A hard-killed member has nothing to present, so
nothing here helps it. Its own entry warns against weakening the certificate rule to close it; that
warning applies here too — this adds a way to recognise our own body, it does not relax the rule.

**#2243's trigger, the leaked mount thread on `DROP TABLE`, and the per-step remount state machine.**
Separate subsystems, unchanged here. This design removes one of the per-step machine's motivations —
the cost of a failed attempt — but not the machine's own case.

## What this closes {#what-this-closes}

- #2244's deferred follow-up item on whether an own ambiguous claim preserves or resets token-stability
  observation: it preserves it, by retaining the bytes, and the read resolves the ambiguity.
- The failed-remount-attempt cost described in [Problem](#problem).
- `[empty-token-unconditional-write-guard]`, as a prerequisite rather than a side effect.

It does **not** close the per-step remount state machine, which remains #2244's open follow-up.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

Kept because each error is cheaper to read than to repeat.

**Revision 1** used `started_at_ms` as a process-incarnation identifier, though every renewal restamps
it, so the check would have rejected this process's own latest body. It claimed the store supplies the
mutual exclusion — a compare-and-swap serialises the slot but does not revoke the previous holder's
local authority. And it declined a focused TLA+ model by arguing from a false choice between the
1,670-line `CaCasMountCore` and nothing.

**Revision 2** added a `writer_incarnation_id` field and a possession set of attempt ids — both
unnecessary, since `write_attempt_id` already makes bodies unique and the bytes are already in hand. It
guarded the *remembered* token against emptiness but not the *observed* one used as the write's
precondition. It asserted that at most one write can be unresolved at a time, confusing "one logical
call in progress" with "one physically deliverable request", so its own `LOGICAL_ERROR` would have
fired on the fast path it designed. It called the token check assumption-free, and called `mayMutate` a
sufficient gate. And it recorded, as an alternative worth keeping, shortening the wait from our own
send time — which does not work: without recognising the body, a send-time bound says nothing about
whose lease is in the slot.

**Revision 3** mutated state on the read path, adopting the token it read as `confirmed_token` and
dropping the matched bytes. Under the legal mixed read that discards the only evidence and forces the
full wait; when the matching read returns an empty token it installs an unusable token and drops the
bytes as well. It retained one body rather than a set, so the reclaim's own recording evicted the
unresolved renewal it existed to recover — making its own stated recovery ordering, and its own test
for it, impossible. It claimed the conditional write contains a mixed read in both directions; it
contains one. It keyed the lifecycle on `attempts_sent`, which counts backend entry rather than a
physical send and is absent from three of the writers. It missed `ForeignHolderWrite` in the model
repair. And it asserted a cloning baseline bounded by one renew period, which `resolved_by_get`
refutes.

---
description: 'Design for reclaiming a CAS mount lease immediately when the body in the mount slot is one this runtime wrote, recognised by a remembered ETag or by the exact bytes of an in-flight write, replacing a full observation wait.'
sidebar_label: 'Self-authored mount reclaim'
sidebar_position: 42
slug: /superpowers/specs/cas-self-authored-mount-reclaim
title: 'CAS self-authored mount reclaim'
doc_type: 'guide'
---

# CAS self-authored mount reclaim {#cas-self-authored-mount-reclaim}

Revision 3. Revisions 1 and 2 were both reviewed and rejected;
[what the earlier revisions got wrong](#what-earlier-revisions-got-wrong) records the traps, because
they are the ones a reader is most likely to fall into again.

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
`quiesce_ref_tables`, `arm_fence`, `publish_live`, with an optional `keeper_redo` before the arm. Any
of them can throw, and the attempt returns `false`. The next attempt therefore reads a body **this
same process wrote seconds ago**, at the previous epoch — same `server_uuid`, different
`writer_epoch`, not fenced, not clean — and pays the full observation wait for it. Today that is
unavoidable: nothing survives the failed attempt that could say otherwise.

## The rule {#the-rule}

Remember two things about our own writes, and one reclaim follows from them.

**State, held by `CasMountRuntime`:**

- `confirmed_token` — the token the store returned for our last **confirmed** mount-slot write;
- `inflight_body` — the exact bytes of a mount-slot write we have sent and whose outcome we do not
  know; empty when there is none.

Both are held **at the same time** for exactly the duration of an unresolved write, and that pairing
is the whole design: the previous token covers "the write did not land", the bytes cover "it did".
Sending a write does not touch `confirmed_token`; only a confirmation does.

**On remount, at each poll of the observation loop**, having read the slot's bytes and token:

1. If `inflight_body` is non-empty and equals the bytes just read, then our unresolved write landed.
   We have just learned its token: adopt the token we read as `confirmed_token` and drop
   `inflight_body`.
2. If `confirmed_token` is non-empty and equals the token just read, reclaim: an ordinary
   `putOverwrite` of a fresh body against that token.

Step 1 is a repair step for a token we could not otherwise know. Step 2 is the only decision, and its
condition is one line: **the slot's token is one we know identifies a body we wrote.**

### Why no new identity field is needed {#no-new-identity-field}

Every holder-originated mount body already carries `write_attempt_id`, a fresh random 128-bit value
minted per body — in `makeMountBody` (`CasServerRoot.cpp:838`), in `MountLeaseKeeper::encodeBody`
(fed by `renew`), and in the farewell (`CasServerRoot.cpp:1833`). So two different writers cannot
produce identical bytes, and byte equality with something we sent is already an authorship test. The
discriminator is in the format; nothing needs adding to it.

Comparison is against the bytes **as sent**, retained verbatim, never against a re-encoding. A
re-encode can differ innocently — `getFQDNOrHostName` alone can resolve differently — and a false
mismatch silently costs the fast path.

## Safety {#safety}

The hazard to exclude is **two runtimes holding mutation authority at the same time**, not two bodies
racing for the slot. A compare-and-swap on the mount slot invalidates the previous holder's cached
token but does not touch its local authority: `mayMutate` reads a local latch and a local
`CLOCK_BOOTTIME` deadline (`CasMountRuntime.cpp:92`) and learns nothing until its own next renewal
fails. So the argument has to be made in terms of authority.

**Step 1 — the certificate identifies exactly one possible authority-holder.** We reclaim only
against a token we know belongs to a body we wrote: either directly, because the store returned it to
us on a confirmed write, or because the bytes at that token are byte-identical to a write we sent.
Under either, the only runtime that could derive authority from this body is this one.

**Step 2 — this runtime provably holds none.** This is a real precondition and it is **not** implied
by the code today. `MountFence` initialises permissively — `deadline_boot_ms` is `UINT64_MAX` with
`lost = false` (`CasMountRuntime.h:117`) — so `mayMutate` is *true* before a lease was ever held, and
therefore true during first open and during the `NoWait` decommission mount. And `tryRemountOnce`'s
step 0 calls `noteLeaseLost`, which moves the lifecycle but does not set `mount_fence.lost`, so a
forced or direct remount is not fenced by construction either.

So the gate is explicit, and sampling `mayMutate` is not it. The fast path is admitted only while
**both** hold:

- this runtime has **latched** a mount loss — `mount_fence.lost` is set and its fence generation has
  been bumped — rather than merely never having armed a fence; and
- the caller **holds** the renewal driver in `Parked` or `Dormant` for the whole claim, so no keeper
  of this runtime can confirm a renewal and re-arm authority underneath the decision.

A naked `!mayMutate()` sample fails both ways: it is true at first open when no lease was ever held,
and a sampled false can become true again before the reclaim's write is issued. Holding driver
ownership across the claim is what makes the sample stable; the latch is what makes it mean what we
want.

Where the gate does not hold, the state is simply not offered and the observation wait applies —
first open and the `NoWait` decommission claim both take that path, and neither has anything to
present anyway.

**Step 3 — the store closes the race window.** The read is a hint; `If-Match` is the arbiter. If our
unresolved write lands between our read and our write, our token is stale and the reclaim fails, and
we retry — now matching its bytes. If we win, the old holder's cached token is consumed and its next
write fails forever. This also absorbs the native backend's non-atomic read: `get` is a `HEAD`
followed by a `GET`, so bytes and token can come from different versions
(`CasObjectStorageBackend.cpp:588`); a mispaired hint can only make the conditional write fail.

Three orderings, all safe under those steps:

| ordering | outcome |
|---|---|
| our reclaim lands first | the old keeper's cached token is stale; its next renewal fails the guard and it never writes again |
| our own unresolved write lands first | our token is stale, the reclaim fails; the next poll matches its bytes, learns its token, and reclaims |
| a twin's claim races ours | exactly one compare-and-swap wins; the loser re-reads and finds a body it cannot recognise |

**Scope is the runtime, not the process.** Every `Pool::open` constructs its own `CasMountRuntime`,
and there is no process-global registry. The claim quantifies over the certifying runtime and every
other runtime that could write the same mount key.

**GC's fence-out.** GC fences by rewriting the observed body with `gc_fenced = true`, copying
`write_attempt_id` — but it changes `gc_fenced` and `seq`, so the bytes no longer match ours, and it
consumes the token, so `confirmed_token` no longer matches either. A fenced body therefore reaches
the `gc_fenced` certificate, which is checked first. Keeping that order is about classification
fidelity — recording `Fenced` rather than `SelfAuthored`, so a GC fence-out stays visible — not about
safety.

**The same-epoch branch is untouched.** `claimMount`'s same-`writer_epoch` branch
(`CasServerRoot.cpp:936`) refreshes in place and checks no process identity at all. It is out of
reach here: the remount path allocates a fresh `writer_epoch` before claiming, so a body this runtime
authored is always at a *previous* epoch. This design changes only the same-uuid / different-epoch
branch.

## Prerequisite: an empty token must not become an unconditional write {#prerequisite-empty-token}

Nothing above holds if the token can be empty. `Token{}` passes the conditional-write type check,
`CasObjectStorageBackend` copies the empty value into `object_storage_write_if_match`, and both S3
and Azure add `If-Match` only when that string is non-empty (`WriteBufferFromS3.cpp:656`,
`WriteBufferFromAzureBlobStorage.cpp:234`). The "conditional" reclaim would then be an unconditional
clobber of a live twin — the exact violation Step 3 is supposed to exclude. Empty tokens are
reachable: `tokenFromWriteResult`'s `HEAD` fallback can produce one on a `Done` write.

This is `[empty-token-unconditional-write-guard]`, already in the backlog at P2. It is a
**prerequisite of this work, not a citation**: `Backend::putOverwrite` must fail closed on an empty
expected token before this design's reclaim is enabled. Emulated and in-memory backends already
enforce an empty token, so no test on them can catch this — the guard needs its own native-backend
test.

The same reachability shapes the state rules below: a write confirmed with an empty token yields no
usable `confirmed_token`, and the design keeps its bytes instead of pretending it learned anything.

## State lifecycle {#state-lifecycle}

Both values are owned by `CasMountRuntime`, which outlives every keeper it installs, and are read and
written under its own mutex.

| when | `inflight_body` | `confirmed_token` |
|---|---|---|
| immediately before the first physical attempt of any mount-slot write | set to the exact bytes about to be sent | unchanged — the previous token is what covers "the write did not land" |
| the write confirms with a non-empty token | cleared | set to that token |
| the write confirms with an empty token | **kept** | cleared |
| the write terminates having sent nothing (`attempts_sent == 0`) | cleared | unchanged |
| the write terminates ambiguously | kept | unchanged |
| a remount read matches `inflight_body` | cleared | set to the token just read |

Two consequences to state rather than discover:

**A second send overwrites the first.** If a renewal ends ambiguously and the reclaim that follows is
also ambiguous, only the reclaim's bytes are retained. Should the slot then hold the renewal's body,
it is not recognised and the observation wait applies. This is a deliberate simplification: keeping
one body costs nothing and can only ever lose a fast path, never grant a wrong one. A set of bodies
would close the gap and is not worth its bookkeeping until this case is observed.

**Every writer must record before it sends.** Today `claimMount` mints its body internally, at
`makeMountBody`, and sends it in the same breath — a caller cannot record those bytes in time, and a
write that throws after landing would leave nothing behind. So every mount-slot writer takes a
`pre_send` recorder and invokes it with the exact bytes immediately before the first physical
attempt. The holder-originated writers are: `claimMount`'s fresh mint, same-epoch refresh and reclaim
branches; `MountLeaseKeeper::claim`'s mint and adoption; `MountLeaseKeeper::renew`; and the farewell
in `MountLeaseKeeper::terminate`. GC's fence-out and decommission's token-exact delete are not ours.

A missed recording is safe — it costs the fast path and nothing else — which is what makes this rule
enforceable by a tripwire rather than by a proof.

## Where it plugs in {#where-it-plugs-in}

`claimMount` takes the pair by value alongside `proven_dead_token`, and a `pre_send` recorder. It
stays a pure decision function: it returns, in `MountClaimResult`, the token of a successful write and
an optional `learned_token` set when the read matched `inflight_body`, and the runtime records both.
`claimMountAwaitingExpiry` re-reads the pair from the runtime on every poll rather than capturing it
once, so a write that lands mid-loop shortens the wait. `MountPriorState` gains `SelfAuthored`.

## Observability {#observability}

The `watermark_renew` event is unchanged: `attempts_sent`, `elapsed_ms`,
`remaining_confirmed_budget_ms`, `unresolved_reason`, `deadline_source` and `stop_cause` are what made
the S03 diagnosis possible.

Three additions, one per branch of the reclaim decision:

**Reclaimed.** A `mount_claim` with prior state `SelfAuthored`, saying whether the token was already
known or was learned by matching `inflight_body`. The distinction is worth recording: a match means a
write we gave up on actually landed — evidence about the store that is otherwise unobtainable.

**Refused, and why.** The body is ours by `server_uuid` but nothing matched. Today this is an
undifferentiated `live_double_start`. It must name the reason and the cost: the token differs and no
in-flight bytes matched (a restart, a twin, or a body we no longer remember), and the observation wait
now applies for about `ttl_ms + ttl_ms/20 + poll_interval_ms`. The gate's own refusal — loss not
latched, or driver ownership not held — is visible only to the caller, so the caller logs that one;
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

### A backend that can defer and duplicate a write {#deferred-backend}

The dangerous cases are all about **when a physical write is delivered**, and no existing mount gtest
backend can express them: `InMemoryBackend` and its subclasses apply a write synchronously. So the
first piece of work is a `DeferredMountWriteBackend` that can hold a `putOverwrite`, land it at a
point the test chooses, deliver its response separately or not at all, and queue two independently
deliverable copies of one logical write. Without it, the tests below only restate what synchronous
code already guarantees.

### Unit tests {#unit-tests}

In `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp`:

1. slot token equals `confirmed_token` → instant reclaim, prior state `SelfAuthored`;
2. our unresolved write landed: bytes match → token learned, instant reclaim, recorded as a match;
3. our unresolved write did not land → the previous `confirmed_token` still matches, instant reclaim;
4. bytes differ and token differs → no reclaim;
5. same `server_uuid`, different `pid`, empty state → no reclaim, and the log names the restart;
6. foreign body → no reclaim;
7. loss not latched (fence never armed, as at first open) → the state is not offered, observation wait;
8. driver ownership not held → the state is not offered;
9. a `gc_fenced` body derived from ours → classified `Fenced`, and neither bytes nor token match;
10. remount fails after a successful claim; the next attempt reclaims on the recorded token;
11. a write confirmed with an empty token → no reclaim on the token, and the bytes are still held;
12. a re-encoded body is compared instead of the sent bytes → the match must fail (a guard against a
    future refactor substituting a rebuild for the retained string).

Late-delivery cases, all requiring the deferred backend:

13. an old write issued before terminalization, delivered after the reclaim → fails its stale token;
14. two outstanding copies of one write, one landing before and one after the reclaim → at most one lands;
15. an old write landing between the reclaim's read and its write → the reclaim fails and the next poll matches its bytes;
16. an unresolved renewal still deliverable while an ambiguous reclaim is outstanding, both conditional on the same token → exactly one lands, and the loop converges;
17. GC's fence write racing the reclaim, in both orders → the loser's token is stale; GC reclassifies;
18. an ambiguous `claimMount`, an ambiguous keeper adoption, and an ambiguous farewell each leave usable bytes;
19. a native-backend test that an empty expected token is refused rather than becoming an unconditional write.

### Model {#model}

**A focused TLA+ extension of `CaMountRenewRetryCore`, and the model must be repaired before it is
extended.** Two defects make it unable to express this design as it stands:

- `localAuthority` is a single global flag, and `SamePairTwin`, `GCFence` and `SuccessorClaim` each
  **clear** it (`CaMountRenewRetryCore.tla:615`, `:628`, `:642`) — so the model asserts that another
  writer's durable write revokes the modelled runtime's local authority, which is exactly the premise
  revision 1 was rejected for, baked in. Authority must become per-runtime, and only its own
  fence-loss or deadline may clear it (`GCFence` is the one that legitimately may, and only because
  GC waits out the authority interval first);
- it carries **one** logical request tuple, with `outstanding` counting physical copies of only that
  tuple (`:149`, `:339`), so it cannot represent an unresolved renewal and a reclaim at once — the
  interleaving this design most needs to check.

Once repaired, add: per-runtime local authority with its own deadline; per-runtime retained bytes and
confirmed token; several concurrent logical writes with distinct bodies; empty tokens and the
conditionality loss they cause; a non-atomic bytes/token read; a restarted runtime with no retained
state; and — critically — the reclaim as **two** actions, an evidence-and-gate snapshot followed by a
delayed conditional write, so the stale-sample race is reachable rather than assumed away.

Safety invariants: **no two runtimes hold mutation authority**, and **a live twin's body is never
reclaimed**. "Only one compare-and-swap updates the slot" is already proven and is not the property at
issue.

Some sabotages break safety and some break only recovery, and the plan must say which, because a
sabotage checked against the wrong property passes and proves nothing:

| sabotage | property it must violate |
|---|---|
| reclaim without a matching token or bytes | safety |
| reclaim while authority is not latched lost | safety |
| reclaim on a sample taken before authority was released | safety |
| treat an empty token as conditional | safety |
| compare a rebuilt body instead of the sent bytes | recovery |
| clear the retained bytes on a terminal outcome | recovery |
| record the bytes after sending rather than before | recovery |

Witnesses: fast reclaim when the unresolved write landed; fast reclaim when it did not; fast reclaim
on the retry after a remount failed past its claim.

### Integration and soak {#integration-and-soak}

Extend `tests/integration/test_cas_mount_renewal_retry`, which already exercises the renewal path
under injected faults. Soak: S39 (`lease fault tolerance`) and S03 at `--scale full`, where this was
found.

## Assumptions {#assumptions}

**Byte equality implies authorship, because every holder body carries a fresh random 128-bit
`write_attempt_id`.** This is the uniqueness assumption the renewal specification already states for
the protocol as a whole — "random 128-bit IDs are treated with the same uniqueness assumption already
used for server and operation identities" — so this design adds no new one. It does depend on that
field surviving; see [Tripwires](#tripwires).

**A cloned process defeats it, and the honest accounting is worse than "one more way in".** A restored
VM snapshot yields two runtimes with the same `server_uuid`, the same retained bytes, and the same
PRNG state. Today that case is already broken and it is worth being precise about how: both clones
resume with `mount_fence.lost` clear and a `CLOCK_BOOTTIME` deadline in the future, so both admit
mutations immediately — the store's token guard fences whichever clone loses its **next renewal**, so
the overlap is bounded by roughly one renew period and is self-correcting. With this design the loser,
once fenced, can match the winner's freshly renewed body and take the lease from a healthy holder, so
the overlap becomes up to a full TTL and is no longer self-correcting. Cloning a running server that
holds a CAS mount is outside supported operation, but this is a real degradation of what happens
anyway, and it belongs in the mount documentation rather than only here.

Nothing else weakens. A restart with `pid` reuse in the same millisecond recognises nothing, because
the retained bytes and token are gone.

## Acceptance {#acceptance}

1. After a store-induced lease loss with no restart, recovery contains no observation wait. Measured
   from `system.cas_log` as the interval between the `watermark_renew` failure and the pool's
   `TransientNotLive -> Live` transition — **not** the successful `mount_claim`, which precedes seven
   further steps that can each fail while the disk still refuses every write. `system.cas_log`
   delivery is best-effort and can drop under a full queue, so a missing row is not a failed run;
   the server log carries the same transitions.
2. That recovery's `mount_claim` records prior state `SelfAuthored`.
3. A remount that fails after its claim reclaims on the retry without an observation wait.
4. A restart still waits, and the log says so, naming the reason, before the wait begins.
5. A hard-killed member still waits — no code path change, verified by the existing tests staying green.
6. `Backend::putOverwrite` refuses an empty expected token, with a native-backend test.
7. The repaired model's invariants hold under every configuration, and every sabotage violates the
   property the table assigns it.
8. S03 at `--scale full` no longer fails with `Code: 210` for this reason.

## Tripwires {#tripwires}

Three conditions this design depends on. If any stops holding, it is invalid.

**Every mount-slot mutation is an atomic conditional create, a conditional overwrite against an
expected token, or a token-exact delete; every successful mutation changes the token; and no expected
token is ever empty.** Revision 2 stated the first two and was still wrong, because an empty token
makes the native overwrite unconditional — which is why the third clause is not decoration. (Revision
1 was wronger: it claimed every write is a `putOverwrite` against a token read immediately before it,
false at the fresh claim's `putIfAbsent` and at renewal's and farewell's cached `last_token`.)

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

**Renaming `started_at_ms`.** The field is stamped with the time each body is built, not with process
start, and `system.content_addressed_mounts` surfaces it as `started_at` described as "Time when the
lease started" — which is what led revision 1 to use it as a process-incarnation identifier. This
design no longer needs any such field, so the rename to `body_written_at_ms` is now ordinary
documentation debt rather than part of this work. File it; do not fold it in.

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
it, so the check would have rejected this process's own latest body. It also claimed the store supplies
the mutual exclusion — a compare-and-swap serialises the slot but does not revoke the previous holder's
local authority, so a false recognition admits two writers. And it declined a focused TLA+ model by
arguing from a false choice between the 1,670-line `CaCasMountCore` and nothing.

**Revision 2** answered those by adding a `writer_incarnation_id` field and a possession set of attempt
ids — both unnecessary, since `write_attempt_id` already makes bodies unique and the bytes are already
in hand. It guarded the *confirmed* token against emptiness but not the *observed* one used as the
write's precondition, which is the unconditional-clobber blocker above. It asserted that at most one
write can be unresolved at a time, confusing "one logical call in progress" with "one physically
deliverable request", so its own `LOGICAL_ERROR` would have fired on the very fast path it designed.
It claimed the token certificate needed no assumptions, though token non-reuse is a backend contract
the capability probe does not verify. It called `mayMutate` a sufficient gate. And it recorded, as an
alternative worth keeping, shortening the wait from our own send time — which does not work: without
recognising the body, a send-time bound says nothing about whose lease is in the slot.

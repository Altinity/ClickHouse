---
description: 'Design for reclaiming a CAS mount lease immediately when this runtime incarnation can prove it authored the body in the mount slot, replacing a full observation wait.'
sidebar_label: 'Self-authored mount reclaim'
sidebar_position: 42
slug: /superpowers/specs/cas-self-authored-mount-reclaim
title: 'CAS self-authored mount reclaim'
doc_type: 'guide'
---

# CAS self-authored mount reclaim {#cas-self-authored-mount-reclaim}

Revision 2. Revision 1 was reviewed and rejected; [what revision 1 got wrong](#what-revision-1-got-wrong)
records why, because two of its errors are the ones a reader is most likely to repeat.

## Problem {#problem}

A server that loses its mount lease because the object store stalled cannot take it back for
`mountObservationThresholdMs` = `ttl_ms + ttl_ms/20 + poll_interval_ms`. At the defaults — `ttl_ms`
30,000 and `poll_interval_ms` = `mount_renew_period/2` = 5,000 — that is **36.5 s**, and it is paid
even though the same live process still holds every piece of evidence about what it last wrote.

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
`quiesce_ref_tables`, an optional `keeper_redo`, `arm_fence`, `publish_live`. Any of them can throw,
and the attempt returns `false`. The next attempt therefore reads a body **this same process wrote seconds
ago**, at the previous epoch — same `server_uuid`, different `writer_epoch`, not fenced, not clean —
and pays the full 36.5 s observation wait for it. Today that is unavoidable; nothing survives the
failed attempt that could say otherwise.

## What revision 1 got wrong {#what-revision-1-got-wrong}

Two errors, both worth stating so they are not reintroduced.

**It used `started_at_ms` as a process-incarnation identifier.** The field's name says process start;
its value is the time that particular body was built. `makeMountBody` stamps it from the caller's
`now_ms` (`CasServerRoot.cpp:845`), `MountLeaseKeeper::encodeBody` restamps it from each operation's
`wall_ms` (`CasServerRoot.cpp:1452`), and the farewell restamps it again (`CasServerRoot.cpp:1829`).
A check that compared it against a real process-start timestamp would reject this process's own
latest body on every renewal.

**It claimed the store supplies the mutual exclusion.** It does not, at the level that matters. A
successful compare-and-swap on the mount slot invalidates the previous holder's cached token, but it
does not touch that holder's **local** write authority: `mayMutate` reads a local latch and a local
`CLOCK_BOOTTIME` deadline (`CasMountRuntime.cpp:92`) and learns nothing until its own next renewal
fails. So a *false* authorship match yields A holding authority until its deadline while B arms a new
fence — two runtimes admitting durable writes at once, with a serialized mount slot the whole time.
The existing `gc_fenced` certificate is not safe for the reason revision 1 gave; it is safe because
GC waits out the full authority interval before fencing.

The correction is structural, not a patch: the certificate must establish that **no runtime holds
mutation authority**, and the reasoning that gets there is load-bearing and must be modelled.

## The certificate {#the-certificate}

Add a fourth certificate of death to `claimMount`: **the body in the mount slot was written by this
runtime incarnation, and this runtime incarnation currently holds no mutation authority.**

It has two independent acceptance tiers. The first needs no assumptions at all; the second covers the
case the first cannot reach.

**Tier A — token identity.** The token the store returns for the slot equals the token our last
**confirmed** mount-slot write returned, and that token is non-empty. The store minted it; any write
by anyone since would have replaced it. So the slot holds exactly the bytes we last wrote and nothing
has written since. This is an absolute certificate — it rests on the store's conditional-write
contract and on nothing we reason about.

The non-empty requirement is not decoration. `tokenFromWriteResult`'s `HEAD` fallback can put an
empty `Token` into `MountLeaseKeeper::last_token`, and `Token{}` currently passes the conditional-write
type check — see `[empty-token-unconditional-write-guard]`. An empty confirmed token compared against
an empty observed token would match two unrelated writes. Tier A rejects it.

**Tier B — authorship possession.** The slot body's `writer_incarnation_id` equals ours, **and** its
`write_attempt_id` is one we hold: either our last confirmed id, or the id of a write we published
and never resolved. This is what covers the write that timed out and may still have landed — the
store mints the token, so an unconfirmed write's token is unknowable, but its authorship is known
before the request is sent.

Tier B rests on one assumption, stated in full in [Assumptions](#assumptions): that two concurrently
live runtimes never mint the same `writer_incarnation_id`.

Between them, all three readings of the slot resolve with no wait: our last confirmed body still
there (Tier A), our unresolved write having landed (Tier B), or someone else's body (no certificate,
observation wait as today).

### Identity is possession, not `pid` and not a timestamp {#identity-is-possession}

`pid` and `started_at_ms` play no part in the certificate. They cannot: `pid` is reused, and
`started_at_ms` is a body timestamp. What discriminates a restart is that a restarted process **has
nothing to present** — the incarnation id and the possessed attempt ids live in memory and die with
the process. There is no "was I restarted?" flag to compute or get wrong.

This needs one new durable field, `writer_incarnation_id`, minted once per `CasMountRuntime` and
carried by every holder-originated body. A body that predates the field decodes it as zero, and zero
never matches — the fail-closed default. Consistent with the pre-release rule, no compatibility
scaffolding is written for it.

Separately and in its own commit: `started_at_ms` is renamed to `body_written_at_ms` across the wire
codec, `CasInspect`, and the `started` column of `system.content_addressed_mounts`. The field's name
is what produced revision 1's blocker; leaving it in place leaves the trap armed.

## Safety {#safety}

The hazard to exclude is **two runtimes holding mutation authority at the same time**, not two bodies
racing for the slot. State the argument in that shape.

**Step 1 — the certificate identifies exactly one possible authority-holder.** Tier A says nothing
has written since our last confirmed write; Tier B says the body carries our incarnation's authorship.
Under either, the only runtime that could derive authority from this body is this one.

**Step 2 — this runtime provably holds none.** The reclaim runs only from the remount path, which is
entered after `tripMountLost` has set `mount_fence.lost` and bumped `fence_generation`, and which
cannot re-arm before `armMountFence` at the very end of the sequence (`CasPool.cpp`, step
`arm_fence`). Between those two points `mayMutate` is false for every caller in this process.

**This premise is enforced by construction, not assumed.** The evidence is not something `claimMount`
can read for itself — it is handed in. So the caller populates it **only** when
`!mount_runtime.mayMutate()` holds, and otherwise hands in an empty `SelfAuthorship`, which certifies
nothing and falls through to the observation wait. There is no path on which authority is held and a
certificate is presented, and the refusal is a release-build behaviour rather than a `chassert`.

**Step 3 — the store closes the race window.** Every mount-slot mutation is atomic and conditional, so
a late physical delivery of an old write cannot straddle the reclaim: it either lands before our
compare-and-swap, in which case we read its token and swap against that, or it finds its expected
token consumed and fails forever. This is what the store gives, and it is one half of the argument
rather than the whole of it.

Three orderings, all safe under those steps:

| ordering | outcome |
|---|---|
| our reclaim lands first | the old keeper's cached token is stale; its next renewal fails the guard and it never writes again |
| our own in-flight write lands first | we read the new token; the body is still ours by Tier B; the reclaim swaps against that token |
| a twin's claim races ours | exactly one compare-and-swap wins; the loser re-reads and finds a body it cannot certify |

**GC's fence-out is not a false positive.** GC fences by rewriting the observed body with
`gc_fenced = true`, copying `write_attempt_id` — so a fenced body can carry our id. It is classified
by the `gc_fenced` certificate, which is checked **first** and is strictly stronger. The check order
in `claimMount` is therefore load-bearing: `gc_fenced`, then the clean marker, then this certificate,
then `proven_dead_token`.

**The same-epoch branch is untouched.** `claimMount`'s same-`writer_epoch` branch
(`CasServerRoot.cpp:936`) refreshes in place and checks no process identity at all. It is out of
reach here: the remount path allocates a fresh `writer_epoch` before claiming, so a body this runtime
authored is always at a *previous* epoch. This design changes only the same-uuid / different-epoch
branch.

## Authorship evidence and its lifecycle {#evidence-lifecycle}

The evidence is owned by `CasMountRuntime`, which outlives every keeper it installs, and is three
values:

- `incarnation_id` — 128 bits from `RAND_bytes` (the source `FileEncryptionCommon` and
  `OAuthFlowRunner` already use, unlike `thread_local_rng`, whose seed is documented as not
  cryptographically secure), minted once in the runtime's constructor, immutable, never persisted
  except inside bodies this runtime authors;
- `confirmed` — `{attempt_id, token}` of the last mount-slot write this runtime confirmed;
- `unresolved` — the attempt id of a write published but not yet resolved; **at most one**.

The bound on `unresolved` is an invariant, not a cap. Two rules produce it:

1. **One mount-slot write is in flight at a time.** The renewal driver is single, and keeper
   replacement requires the driver `Dormant` or `Parked`. Registering a second unresolved id while
   one is outstanding is a `LOGICAL_ERROR`.
2. **A confirmation clears `unresolved`.** If a write confirmed at token `T`, the slot is at `T`, and
   every earlier copy's expected token is consumed and can never land. Nothing older can be the
   current body, so nothing older need be remembered.

The remaining rules answer, one for one, where the evidence must be created and where it must
survive:

| rule | why |
|---|---|
| publish the attempt id **before** the first physical attempt leaves | a write that landed but was never published is invisible to Tier B; ordering is the whole content of this rule |
| a logical write ending with `attempts_sent == 0` clears `unresolved` | it provably never reached the store; the renewal diagnostics already report this distinction |
| a confirmed write sets `confirmed` and clears `unresolved`, under the runtime's own mutex | a remount reader must never see a half-updated pair |
| `claimMount`'s own successful write is evidence | it must return its `PutResult` token on the `Claimed` branches, which today it discards; the deliberate `std::nullopt` on the *race* branches stays as documented |
| `MountLeaseKeeper::start`'s write is evidence on both outcomes | confirmed on success, unresolved when its `putOverwrite` is ambiguous |
| `RenewalTerminal` clears nothing | the evidence belongs to the runtime, not to the keeper that produced it |
| `installKeeper` clears nothing | keeper replacement destroys a keeper, not the runtime's authorship |
| a failed remount step clears nothing | this is precisely the case the second half of [Problem](#problem) describes: the next attempt's fast path depends on the failed attempt's evidence surviving |

One consequence is worth naming because it looks like an accident and is not: if our unresolved write
lands **during** the observation loop, a later poll sees a body Tier B certifies and the wait ends
early. The loop re-presents the evidence on every poll for exactly this reason.

## Where it plugs in {#where-it-plugs-in}

`claimMount` takes one more by-value parameter alongside `proven_dead_token` — a small
`SelfAuthorship` struct carrying the incarnation id, the confirmed pair, and the optional unresolved
id — so the function stays pure and directly testable. `claimMountAwaitingExpiry` threads it through
each poll. `MountClaimResult` gains the token of a successful write, and `MountPriorState` gains
`SelfAuthored`.

## Observability {#observability}

The `watermark_renew` event is unchanged: `attempts_sent`, `elapsed_ms`,
`remaining_confirmed_budget_ms`, `unresolved_reason`, `deadline_source` and `stop_cause` are what made
the S03 diagnosis possible.

Three additions, one per branch of the reclaim decision:

**Reclaimed on the new certificate.** A `mount_claim` with prior state `SelfAuthored`, naming the tier
that matched. The distinction is worth recording: a Tier B match on the unresolved id means a write we
gave up on actually landed — evidence about the store that is otherwise unobtainable.

**Refused, and why.** The body is ours by `server_uuid` but the certificate does not hold. Today this
is an undifferentiated `live_double_start`. It must distinguish the reasons and name the cost: the
observation wait applies and will take about `ttl_ms + ttl_ms/20 + poll_interval_ms`. Two reasons are
visible to `claimMount` — a different `writer_incarnation_id` (a restart, or a twin) and an attempt id
we do not possess. The third, authority not yet released, is visible only to the caller that withheld
the evidence, so the caller logs it; `claimMount` cannot tell that case from a restart and must not
pretend to. This turns an unexplained half-minute stall into an announced one.

**Refused because the body is foreign** — as today, naming the holder.

One correction while this work is in the claim path. `describeMountHolder`'s comment
(`CasServerRoot.cpp:852`) states that the mount-audit sink is not yet installed during `Pool::open`,
so that at first open these refusal messages are the only holder-identity carrier. That is stale:
`ContentAddressedMetadataStorage` populates `pool_config.event_sink` before calling open
(`ContentAddressedMetadataStorage.cpp:797`), and `Pool::open` installs it (`CasPool.cpp:511`) before
`mountWritable` (`CasPool.cpp:534`) and its claim (`CasPool.cpp:661`). First-open claim and conflict
events do reach `system.ca_event_log`. Delete the stale sentence — revision 1 of this document
believed it and proposed startup wiring to fix a gap that does not exist.

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

1. slot holds our last confirmed body, token unchanged → Tier A, instant, prior state `SelfAuthored`;
2. confirmed token is empty → **no** Tier A match, observation wait;
3. our unresolved write landed → Tier B, instant, recorded as an unresolved-id match;
4. our unresolved write never landed → Tier A, instant;
5. body carries our incarnation id but an attempt id we do not possess → no certificate;
6. body carries our attempt id but a different incarnation id → no certificate;
7. same `server_uuid`, different `pid`, empty possession → no certificate, and the log names the restart;
8. foreign body → no reclaim;
9. authority still held at the remount call site → the caller hands in an empty `SelfAuthorship`;
   and, at the level below, `claimMount` given an empty `SelfAuthorship` certifies nothing;
10. a `gc_fenced` body carrying our attempt id → classified `Fenced`, not `SelfAuthored`;
11. remount fails after a successful claim; the next attempt reclaims on Tier A rather than waiting.

Late-delivery cases, all requiring the deferred backend:

12. an old write issued before terminalization, delivered after the reclaim → fails its stale token;
13. two outstanding copies of one write, one landing before and one after the reclaim → at most one lands;
14. an old write landing between the reclaim's `get` and its `putOverwrite` → the reclaim loses and retries;
15. GC's fence write racing the reclaim, in both orders → the loser's token is stale; GC reclassifies.

### Model {#model}

**A focused TLA+ extension, of `CaMountRenewRetryCore` rather than `CaCasMountCore`.** Revision 1
argued from a false choice: extend all 1,670 lines of `CaCasMountCore`, or model nothing.
`CaMountRenewRetryCore` is 878 lines and already models the split-phase conditional write with
outstanding copies, a twin, a successor, GC fence-out and late delivery — including the witnesses
`late_before_reclaim` and `late_after_successor` and the sabotage `double_conditional_landing`. It is
the abstraction the renewal specification asked for, and the reclaim belongs in it.

To add: a per-runtime mutation-authority flag with its local deadline; per-runtime possession of
confirmed and unresolved ids; a restarted runtime with a disjoint possession set and a distinct
incarnation id; the self-reclaim action gated on the certificate and on released authority;
fresh-epoch allocation; and claim-succeeded-then-failed-before-re-arm.

Invariants: **no two runtimes hold mutation authority**, and **a live twin's body is never
reclaimed**. "Only one compare-and-swap updates the slot" is already proven and is not the property at
issue.

Sabotages, each of which must produce a counterexample: `accept_without_possession`,
`keep_possession_across_restart`, `reclaim_before_local_fence`, `clear_possession_on_terminal`,
`publish_after_send`, `ignore_incarnation_id`.

Witnesses: fast reclaim after an unresolved write landed; fast reclaim after it did not; fast reclaim
on the retry after a remount failed past its claim.

Incarnation ids are modelled as symbolically unique — which is the assumption below, made explicit in
the place where it is load-bearing.

### Integration and soak {#integration-and-soak}

Extend `tests/integration/test_cas_mount_renewal_retry`, which already exercises the renewal path
under injected faults. Soak: S39 (`lease fault tolerance`) and S03 at `--scale full`, where this was
found.

## Assumptions {#assumptions}

**Tier B assumes that two concurrently live runtimes never mint the same `writer_incarnation_id`.**
The id is 128 `RAND_bytes` bits drawn once per runtime; ordinary collision is not a practical
concern, and it is not what this paragraph is about. Note what this replaces: `write_attempt_id`
alone would not do, because `newMountWriteAttemptId` draws a UUIDv4 from `thread_local_rng`
(`CasServerRoot.cpp:828`), whose seed `randomSeed` documents as not cryptographically secure.

What is: a **cloned process** — a VM snapshot restored twice, or any duplication that copies memory
after the id is minted — produces two runtimes with the same `server_uuid`, the same incarnation id,
and the same possession set. Either could then certify the other's body and reclaim it while the other
still holds authority, for up to one TTL.

State the cost plainly. Today's protocol survives that case: the token guard fences whichever clone
loses. This certificate does not, and no content-based check can, since the clone's content is by
construction identical. Cloning a running ClickHouse server that holds a CAS mount is outside the
protocol's supported operation, but before this change it was survivable and after it is not. That is
a deliberate trade for 36.5 s of recovery, and it belongs in the mount documentation rather than only
here.

Nothing else weakens. A restart with `pid` reuse in the same millisecond forges nothing, because the
possession set is gone.

## Acceptance {#acceptance}

1. After a store-induced lease loss with no restart, recovery contains no observation wait. Measured
   from `system.ca_event_log` as the interval between the `watermark_renew` failure and the pool's
   `TransientNotLive -> Live` transition — **not** the successful `mount_claim`, which precedes seven
   further steps that can each fail while the disk still refuses every write. The threshold is well
   under the 36.5 s the wait alone costs today; the remaining time is the seven steps, and measuring it
   this way is also the first number we will have for them.
2. That recovery's `mount_claim` records prior state `SelfAuthored`.
3. A remount that fails after its claim reclaims on the retry without an observation wait.
4. A restart still waits, and the log says so, naming the reason, before the wait begins.
5. A hard-killed member still waits — no code path change, verified by the existing tests staying green.
6. The focused model's invariants hold under every configuration, and every sabotage produces a
   counterexample.
7. S03 at `--scale full` no longer fails with `Code: 210` for this reason.

## Tripwires {#tripwires}

Two conditions this design depends on. If either stops holding, it is invalid.

**Every mount-slot mutation is an atomic conditional create, a conditional overwrite against an
expected token, or a token-exact delete, and every successful mutation changes the token.** Revision 1
stated this as "every write is a `putOverwrite` against a token read immediately before it", which is
false in three places: the fresh claim uses `putIfAbsent` (`CasServerRoot.cpp:915`), and renewal and
farewell use a **cached** `last_token` (`CasServerRoot.cpp:1695`, `CasServerRoot.cpp:1835`). The
corrected form is what actually holds, and it is what the argument needs.

**Every mount-slot writer registers its authorship before its first physical attempt.** A new write
path that skips registration does not merely miss the fast path — it can leave a body this runtime
authored that this runtime cannot recognise, which is the observation wait returning silently.

## Out of scope {#out-of-scope}

**The renewal loop's own bounds.** ch2 stopped with 1,969 ms of confirmed budget unspent and
`stop_cause = continue`, and one attempt ran 23.7 s against #2244's documented 5 s per-attempt PUT
timeout. Both are real and both are about how long a renewal tries, not about who may reclaim. Tracked
in `[renewal-gives-up-with-budget-left]`.

**Shortening the observation wait using our own send time.** If our write landed, the lease it created
expires one TTL after we sent it, and we measured that send on our own monotonic clock — so the
residual wait could be `TTL - elapsed` instead of the full threshold, with no new assumption. It is a
genuine alternative to Tier B and it is deliberately not taken here: it changes the wait computation,
which is the most safety-sensitive arithmetic in the mount protocol, for a case Tier B already covers
instantly. Recorded so it is not re-derived as a new idea.

**`[decommission-waits-on-the-wrong-predicate]`.** A hard-killed member can present no certificate, so
nothing here helps it. Its own entry warns against weakening the certificate rule to close it; that
warning applies here too — this adds a certificate, it does not relax the rule.

**#2243's trigger, the leaked mount thread on `DROP TABLE`, and the per-step remount state machine.**
Separate subsystems, unchanged here. This design removes one of the per-step machine's motivations —
the cost of a failed attempt — but not the machine's own case.

## What this closes {#what-this-closes}

- #2244's deferred follow-up item on whether an own ambiguous claim preserves or resets token-stability
  observation: it preserves it, by possession, and the answer is Tier B.
- The failed-remount-attempt cost described in [Problem](#problem).

It does **not** close the per-step remount state machine, which remains #2244's open follow-up.

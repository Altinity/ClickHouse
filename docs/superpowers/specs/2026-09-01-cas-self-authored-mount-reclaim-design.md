---
description: 'Design for reclaiming a CAS mount lease instantly when the slot body was written by this very process incarnation, replacing a full-TTL observation wait.'
sidebar_label: 'Self-authored mount reclaim'
sidebar_position: 42
slug: /superpowers/specs/cas-self-authored-mount-reclaim
title: 'CAS self-authored mount reclaim'
doc_type: 'guide'
---

# CAS self-authored mount reclaim {#cas-self-authored-mount-reclaim}

## Problem {#problem}

A server that loses its mount lease because the object store stalled cannot take it back for
`ttl_ms + ttl_ms/20 + poll_interval_ms` — about 31.5 s at a 30 s TTL — even though it is the same live
process that held the lease and knows its own predecessor epoch is dead.

Measured 2026-09-01 in scenario S03 at `--scale full`: a heartbeat write hung 23.7 s against a 30 s
TTL, the renewal ended without retained authority and fenced the mount locally, and every operation
then failed with `Code: 210 ... mount lease not held` while the self-remount produced
`mount_conflict` / `live_double_start` every five seconds.

Nothing here is broken. `claimMountAwaitingExpiry` is doing exactly what it documents: a stale lease
from a prior incarnation and a genuinely live twin are "indistinguishable from a bare read", so it
watches the write-token on its own monotonic clock until the holder provably cannot still be within
its lease. The cost is the full TTL, paid for a distinction that does not exist in this case.

## The certificate {#the-certificate}

Add a fourth certificate of death: **the body in the mount slot was written by this very process
incarnation.**

It is accepted when the slot body's `server_uuid`, `pid` and `started_at_ms` match this process, and
its `write_attempt_id` equals either

- the id of our last **confirmed** lease write, or
- the id of a write we **attempted and could not confirm**.

On a match, the reclaim is an ordinary token-guarded `putOverwrite` against the token just read,
whatever that token is, with a fresh `writer_epoch`.

**No wire-format change.** `MountLease` already carries `server_uuid`, `writer_epoch`, `hostname`,
`pid`, `started_at_ms`, `seq`, `min_active_build_sequence`, `gc_fenced` and `write_attempt_id`. The
change lives entirely in the reclaim decision.

### Why the second case matters {#unresolved-writes}

An attempt that timed out may still have landed. Keying the certificate on the token alone cannot
cover that: the store mints the token, so an unconfirmed write's token is unknown. Keying it on
**authorship** does, because `write_attempt_id` is minted per holder body and is known before the
write is sent — `#2244`'s renewal design already fixes one immutable body and one `write_attempt_id`
across every physical attempt of one logical renewal.

So all three readings of the slot resolve without a wait: our last confirmed body (the write did not
land), our attempted body (it did), or someone else's (no certificate).

## Safety {#safety}

**A live twin cannot be mistaken for our predecessor.** To hold the mount a twin must write, and any
write mints a new `write_attempt_id` and carries the writer's own `pid`. A slot bearing our
`write_attempt_id` and our `pid` therefore proves no twin has written since.

**The store provides the mutual exclusion, not our reasoning.** Every lease write is a
`putOverwrite` guarded by the token read immediately before it. Three orderings, all safe:

| ordering | outcome |
|---|---|
| our reclaim lands first | the old keeper's cached token is now stale; its next renewal's CAS fails and it can never write again — the same property `gc_fenced` relies on |
| the old in-flight write lands first | the token we read is the new one, the body is still ours by `write_attempt_id`, so the reclaim proceeds against that token |
| a twin's claim races ours | exactly one CAS wins; the loser re-reads and sees a body it did not author |

**A restart cannot forge the certificate.** The confirmed and attempted ids live in the mount runtime
object, which a restart destroys, and `pid`/`started_at_ms` in the slot body are checked against this
process independently. Possession is the anchor; there is no "was I restarted?" flag to get wrong.

**Consequences that remove work rather than adding it.** A hard-killed server presents nothing and
falls back to observation automatically — no special case. A clean shutdown already reclaims instantly
through the existing clean marker (`min_active_build_sequence == UINT64_MAX`) — unchanged.

## Observability {#observability}

Unchanged: the `watermark_renew` event already carries `attempts_sent`, `elapsed_ms`,
`remaining_confirmed_budget_ms`, `unresolved_reason`, `deadline_source` and `stop_cause`. That payload
is what made the S03 diagnosis possible; it stays as is.

Three additions, one per branch of the reclaim decision:

**Reclaimed on the new certificate.** A `mount_claim` with a distinct prior state (`SelfAuthored`,
alongside today's `Fenced` / `Clean` / `UncleanObserved`) naming which id matched — confirmed or
attempted. The distinction is worth recording: an attempted-id match means a write we gave up on
actually landed.

**Refused because this is a restart.** The body is ours by `server_uuid` and epoch but `pid` or
`started_at_ms` differ. Today this is an undifferentiated `live_double_start`. It must say so plainly
and name the cost: a restart cannot use the fast path, the observation wait applies, and it will take
about `ttl_ms + ttl_ms/20 + poll_interval_ms`. This turns an unexplained half-minute stall into an
announced one.

**Refused because the body is foreign** — as today, naming the holder.

Folded in: `[first-open mount claim is invisible to the audit-event trail]`. The `CasEventSink` is
installed after `Store::open` returns, so open-time claims never reach `system.ca_event_log`. Its own
recorded fix — synthesize the claim event once the sink is installed, or install the sink before the
mount step — is startup wiring only, and this work already touches the claim path's observability.

## Verification {#verification}

**No TLA+ model, and this is a deliberate deviation from what #2244's specification requires.** That
document asks for "a focused TLA+ model and an explicitly documented abstraction boundary against
`CaCasMountCore`" for the deferred remount work. The deviation was decided explicitly, on this
reasoning:

- The distributed part does not change. The reclaim is the same token-guarded CAS the existing
  `ObservedReclaim` path performs; only the justification for issuing it differs. `CaCasMountCore` is
  1,670 lines with 27 actions and 84 configurations, and extending it would re-prove an unchanged
  mechanism.
- The new risk is intra-process — whether a fenced keeper can still write — and that is settled by
  the token guard rather than by protocol reasoning, as the safety section shows.

If that reasoning is wrong, it is wrong in a specific and checkable way: it assumes every lease write
is token-guarded. A future change that adds an unguarded lease write invalidates this design, and
that is the tripwire to watch.

**Unit tests** in `src/Disks/tests/gtest_cas_mount_claim_conflicts.cpp`, one per decision branch:

1. slot holds our last confirmed body → instant reclaim, prior state `SelfAuthored`;
2. slot holds our attempted-but-unconfirmed body → instant reclaim, recorded as such;
3. same `server_uuid` and epoch, different `pid` → no fast path, observation wait entered, message names the restart;
4. foreign body → no reclaim;
5. the old keeper attempts a renewal after our reclaim → its CAS fails on the stale token.

Case 5 is the one that matters most: it is the direct check of the property the TLA+ model is being
skipped in favour of.

**Integration:** extend `tests/integration/test_cas_mount_renewal_retry`, which already exercises the
renewal path under injected faults.

**Soak:** S39 (`lease fault tolerance`) and S03 at `--scale full`, where this was found.

## Acceptance {#acceptance}

1. After a store-induced lease loss with no restart, recovery is one compare-and-swap rather than
   ≈31.5 s. Measured from `system.ca_event_log`: the interval between the `watermark_renew` failure
   and the successful `mount_claim`.
2. A restart still waits, and the log says so before the wait begins.
3. A hard-killed member still waits — no code path change, verified by the existing tests staying green.
4. No new user-facing setting, and no change to `MountLease`'s wire format.
5. S03 at `--scale full` no longer fails with `Code: 210` for this reason.

## Out of scope {#out-of-scope}

**The renewal loop's own bounds.** ch2 stopped with 1,969 ms of confirmed budget unspent and
`stop_cause = continue`, and one attempt ran 23.7 s against #2244's documented 5 s per-attempt PUT
timeout. Both are real and both are about how long a renewal tries, not about who may reclaim. Tracked
in `[renewal-gives-up-with-budget-left]`.

**`[decommission-waits-on-the-wrong-predicate]`.** A hard-killed member can present no certificate, so
nothing here helps it. Its own entry frames the decision — test waiting on the wrong predicate versus
a product gap in `NoWait` — and warns against weakening the certificate rule to close it. That
warning applies to this design too: this adds a certificate, it does not relax the rule.

**#2243's trigger, the leaked mount thread on `DROP TABLE`, and the per-step remount state machine.**
Separate subsystems; unchanged here.

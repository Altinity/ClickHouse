---
description: 'Two independent adjudications of whether the GC reduce phase may observe a zero-in-degree blob''s incarnation earlier than the streaming merge reaches it, and the design changes their findings forced.'
sidebar_label: 'GC head read-ahead consult'
sidebar_position: 46
slug: /superpowers/worklogs/cas-gc-head-read-ahead-consult
title: 'GC reduce-phase HEAD read-ahead — two consults'
doc_type: 'guide'
---

# GC reduce-phase HEAD read-ahead — two consults {#cas-gc-head-read-ahead-consult}

Both consults answered the same question independently, neither told of the other: may the `HEAD` that
observes a zero-in-degree blob's incarnation be issued at the start of `fold_reduce`, after ref intake
has frozen the round's cut, instead of at the moment the streaming merge reaches that blob in
`closeBlob`? Both returned `SAFE`. Their raw reports are `tmp/r2-consult-a.md` and `tmp/r2-consult-b.md`
in the working tree; this is what the implementation took from them.

## Consult A {#consult-a}

`SAFE`, subject to three preconditions, all of which `GcReadAhead` already satisfied when the consult
read it:

1. **The prefetch must be a bare `head`, never `head_blob` or `peek_head`.** Both lambdas are
   side-effecting: they emit the candidate trail, bump the condemn counters and schedule the
   condemn-marker write. Running one over a superset would stamp `Condemned` on blobs the round never
   condemns and force a live writer to republish each of them.
2. **Each worker must own its `CasOperation`.** The type carries mutable per-call state and is
   documented single-threaded; the sanctioned fan-out is one `resume(generation())` per task, which is
   what the read-ahead does. Sharing one operation across a pool would be a data race, and a data race
   is not one of the four divergences the design enumerates.
3. **A failed prefetch must never be cached as "absent".** A `HEAD` can stop on a lost fence, an
   exhausted budget or an exhausted policy, and every one of those throws. Recording `nullopt` for such
   a failure would tell `closeBlob` the object is gone and silently skip a real condemnation — the
   fallback-hiding-an-error shape the project forbids. The read-ahead rethrows the worker's exception at
   the take site instead, so a transport failure fails the round exactly where the inline read failed it.

Two findings beyond the preconditions:

- **The benign reading of the stale-token case rests on the writer, not on the collector.** A blob is
  content-addressed, so on a dialect whose etag is derived from content, a naive republication would
  reproduce the condemned etag and the exact-token delete would match a live body. That does not happen
  only because each physical publication mints a fresh envelope, which changes the bytes and therefore
  the etag. The property is load-bearing for this design and belongs written down where someone might
  later "optimize" the envelope away.
- **One consequence the four-outcome table does not name.** A blob observed absent early and present by
  the merge is not condemned this round, and its zero marker is per-generation and dropped on carry, so
  no later round re-examines it unless a delta touches the blob again. For ordinary garbage that is
  correct, because "present by the merge" implies a publisher and therefore an edge. The residue is a
  publication that lands during the reduce phase and then rolls back: that body leaks until a rebuild.
  Today's code has the same leak through a narrower window, so this is a quantitative widening of an
  existing class, not a new one. Recorded in the backlog rather than fixed here.

## Consult B {#consult-b}

`SAFE`. It supplied the discriminator the enumeration needed and found the one place where the design's
headline claim was not literally true.

- **Why four cases are exhaustive.** The `HEAD`'s result reaches the round only as an optional pair of
  size and etag, and the take sites read no other projection of it. Size is a function of the blob hash
  under a fixed pool header length, so it is not an independent dimension: the observable state is
  presence times etag, and the divergence space between an early and a late observation is exactly the
  five-cell product of those two states. There is no sixth cell because there is nothing else to
  observe. The fifth outcome, that the `HEAD` throws rather than returns, is closed by precondition 3
  above.
- **`peek_head`'s HEAD is a decision, not data — and this changed the design.** Its result is the
  supersede branch's own condition, so observing earlier narrows the window in which a republication can
  be seen and genuinely changes which entries supersede. The consequence is benign: a missed supersede
  leaves the stale entry to graduate, its exact-token delete mismatches the new incarnation, and the
  entry is dropped, so reclamation is delayed rather than wrong. The implementation nevertheless leaves
  `peek_head` inline. The property worth having is that only the moment of the fetch moves and never a
  decision, and it is not worth spending on the rare blob that both carries a condemned row and is
  touched again in the same round.
- **The hint must be windowed, and the naive reading of the design is an availability regression.**
  Hinting a whole round's candidate set at phase start would queue a slot and a task per candidate on a
  background thread, bounded by nothing, since no per-round budget caps condemnations. The
  implementation issues hints from inside `head_blob` under the pending-against-window loop the intake
  sites already use, so the requests it can add are bounded by one window past the last candidate the
  merge actually reaches — a superset that overshoots costs a window, not its own size.
- **The superset rule holds, including the case the first draft would have missed.** A key whose deltas
  end in an activation but which a source retirement then clears is a real candidate; retirements are
  therefore folded into the same last-verdict computation, after the deltas, the way the merge applies
  them.

What consult B tried and could not break: content-derived etag reuse, a stale condemned marker over a
live incarnation, a stray condemned meta suppressing destructive work forever, a prefetch observing
state older than the round's cut, a competing leader deleting the body mid-round, a cross-shard take
race, a late worker touching freed state, and a scheduling failure leaving an unsatisfiable slot. It
notes explicitly that its verdict is reasoning rather than experiment, and names the test that would
make it executable: drive the stale-token case and assert that everything except the token value is
unchanged against a sequential run.

## What neither consult covers {#not-covered}

Neither reviewed the candidate-set code or the hint loop, because neither existed when they read the
tree. Neither measured anything. The liveness question consult A leaves open — whether prefetch
overshoot can push a round past its lease — is answered structurally by the windowing above rather than
by measurement, and the measurement is the soak's job.

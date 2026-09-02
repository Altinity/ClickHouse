---
description: 'Four ways to make retry coverage a property of the build rather than of review attention, written after an audit found twenty-three conditional-write call sites with no retry above them'
sidebar_label: 'Retry coverage by construction'
sidebar_position: 1
slug: /superpowers/cas/retry-coverage-by-construction
title: 'Making retry coverage structural'
doc_type: 'design'
---

# Making retry coverage structural {#retry-coverage-by-construction}

> **Superseded (2026-09-03).** Proposals 1, 3 and 4 are absorbed into revision 5 of
> `../specs/2026-09-02-cas-backend-token-contract-design.md`: `Backend` becomes private to
> `CasRequests`, every call names a `Retry` policy, `ThrottlingBackend` gates the suite, and
> control-plane reads move onto the single-attempt client under the CAS engine's deadline. Proposal 2
> (an ambient context) was rejected there in favour of an explicit policy parameter with no default.
> This note is kept as the argument; the spec is the design.

Written 2026-09-02, after the [retry-coverage audit](/superpowers/cas/gcs-retry-coverage-audit-2026-09-02)
found twenty-three production call sites issuing a conditional write with nothing above them that
retries. The audit answers *where*; this note answers *how not to have to ask again*.

## The class of defect {#defect-class}

The three conditional-write primitives on `Backend` — `putIfAbsent`, `putOverwrite`, `casPut` — are
pinned to a single HTTP attempt on purpose. `conditionalWriteSettings` selects the single-attempt
client so that a transparently retried conditional write can never cross a mount-lease boundary: the
retry that matters here is not "send it again", it is "resolve the ambiguous attempt by an exact read,
re-check the fence, then decide". That is what `CasRequestController` does, and it is why the retry
belongs above the primitive rather than inside it or beneath it.

So the primitives are safe only through a wrapper, and they are callable without one. That is the
defect class, and it is not fixed by finding the twenty-three sites: it is fixed by making the
twenty-fourth impossible to write.

## What will not work {#rejected}

**Pushing the retry down into the object-storage client.** Its safety rests on resolving an ambiguous
attempt by reading back the exact expected bytes. The client does not know what was expected, so it
cannot distinguish "my own earlier attempt landed" from "somebody else took the key". A blind retry of
`putIfAbsent` turns a lost response into a false conflict.

**A lint or a grep gate with an allowlist.** It catches the next regression and then rots: every
legitimate addition widens the list, and a widened list stops being read. The compiler does not rot.

## Four changes, in the order they pay off {#proposals}

### 1. Make the raw primitives uncallable outside the controller {#uncallable}

Turn the three primitives into **private virtuals** on `Backend` and expose them only through a small
handle whose constructor is private and befriended to `CasRequestController`. Every direct call then
fails to compile, and the compiler enumerates the sites for you.

This is cheap for implementers and expensive only for callers, which is the right way round. C++
applies access control to *calling*, not to *overriding*: a subclass may override a private virtual
without naming it. The three production backends and the test doubles in the gtest suites therefore
need no change at all. Only the call sites move, and a call site that cannot supply a fence and a
deadline is precisely a call site that has not thought about retry.

### 2. Make the controlled call as short to write as the raw one {#ambient-context}

Part of why sites skipped the controller is friction: the raw call is one line, the controlled one
needs a budget, a deadline and a fence check assembled at the point of use. A wall without a door
produces paths around it. Give the mount a request context carrying fence, deadline and budget, and let
the controller pick it up implicitly, so the safe call is the shorter one.

### 3. Make "do we retry" a property the suite checks {#throttling-fake}

The subsystem already has an instrumented backend decorator. Add a sibling that answers a throttling
status on every *n*-th conditional write, and a gate asserting that no user-visible statement fails
under it. A missing retry then shows up in seconds on any build rather than as a fraction of failed
`CREATE TABLE` statements on a live bucket. The fake Google Cloud Storage service used by the
integration suite already has a control surface; a throttling mode belongs there for the end-to-end
half of the same property.

### 4. Propagate one deadline downwards {#deadline-propagation}

The audit found the opposite defect on the paths that *are* retried: the object-storage client retries
up to its own attempt limit with a five-second cap and no jitter, so a persistently throttled request
blocks its thread for roughly forty minutes, and the operation's own deadline cannot preempt a request
already inside that loop. An operation should pass its remaining time down into the request settings,
so that no request outlives the operation that owns it. One deadline, honoured at every layer.

## If only one is done {#recommendation}

Do the first. It converts a review discipline into a build failure, and it is the only one of the four
that removes the possibility rather than the incentive or the delay in noticing. The other three make
the first one comfortable to live with, fast to verify and symmetric with respect to over-retrying.

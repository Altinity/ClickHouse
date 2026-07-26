---
description: What the CAS effort is trying to achieve and what it deliberately refuses, as of the 2026-07-26 consolidation
sidebar_label: Intent
sidebar_position: 0
slug: /superpowers/cas/intent
title: CAS intent — what we want and what we do not
doc_type: reference
---

# CAS intent — what we want and what we do not {#cas-intent}

Written at the 2026-07-26 consolidation, after a round in which eight agents built a protocol and a review
found that composition broke what each part preserved. Its purpose is to be the thing a plan is checked
AGAINST, so that "the plan says so" stops being a sufficient reason to do something.

## What we want {#want}

**Correctness that is demonstrated, not argued.** A guarantee counts when something fails without it. Three
times this week a check passed while looking at nothing — a harness whitelist that silently dropped a class,
a GC observation that returned empty on a schema mismatch, an assert that was vacuous on an empty row set.
A green that cannot go red is not evidence.

**Failure modes that are visible at the moment they happen.** The retention leak survived months of soaks
because an unmatched removal was a silent no-op and fsck called the result "expected". We would rather ship
a loud, imperfect signal than a quiet, correct one.

**A GC that can say where its time went.** Until this week it could not: no timers anywhere, and a round
summary that omitted the round's own duration. "Where did round 33 spend 39 minutes" was unanswerable from
a captured artifact.

**Fail-closed under ambiguity.** An operation that MAY have landed must not be treated as one that did not.
That single sentence is the root of the ref-lane wedge, of the uncertain-precommit fix, and of the
promote's tri-state outcome — three separate defects that were the same mistake.

**A minimal, argued upstream footprint.** Generic MergeTree, Replicated and Keeper code must not learn CAS
concepts. Where an exception was accepted, it is recorded as accepted WITH its reasoning, so it is neither
re-litigated nor mistaken for an oversight.

## What we do not want {#do-not-want}

**Fallbacks that hide errors.** An unclassified failure is not evidence that the alternative path would
fare better. The relink's byte fallback is sound only when the doubt is about the MECHANISM; doubt about
the SOURCE must abort and retry, not silently take a different route to the same peer.

**Tests that pass for the wrong reason.** A relink proved by a flat blob count is worthless, because a byte
fetch onto a content-addressed disk dedups and is also flat. Every proof must be able to fail.

**Scaffolding for compatibility we do not owe.** The pool format is pre-release with no persisted
production data. A format bump is cheap; a dual protocol is not.

**Speculative optimisation of a protocol step.** Protocol steps are not removed as "cheap wins". Where a
cost is suspected — the fold seal being read five times per round, the meta pool having no queue depth —
it is INSTRUMENTED first and decided on data.

**Work started because a plan says so.** Plans have been wrong in every round: a field placement that
silently rebound positional initialisers, a prescribed proof that proved nothing, a step whose own
authorization forbade it. A plan is a hypothesis about how to proceed, and the person executing it is
expected to notice when it is wrong and say so.

**Silence as a failure mode.** If a mismatch between two components can degrade to "nothing observed"
rather than to an error, that is a defect regardless of whether it has bitten yet.

## The one thing that outranks the rest {#no-silent-data-loss}

No path may lose acknowledged data, and no path may delete an object a committed reference still names.
Where we cannot yet guarantee that — the fold cursor advancing over records a round merely OBSERVED — the
gap is named, mechanised in a model, made executable as a test, and carries a commitment to fix before
release. It is not softened into a caveat.

## How to use this document {#how-to-use}

When a plan step and this document disagree, this document wins and the plan is amended. When a reviewer's
remedy and this document disagree, the remedy is checked before it is applied — two of four remedies in the
last review were wrong, and one would have turned a leak into a permanent wedge.

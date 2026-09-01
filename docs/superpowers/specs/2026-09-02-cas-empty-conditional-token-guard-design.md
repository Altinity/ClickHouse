---
description: 'Design for refusing an empty expected token at the CAS conditional-write entry, so a fenced write can never degrade into an unconditional clobber on an ETag store.'
sidebar_label: 'Empty conditional token guard'
sidebar_position: 43
slug: /superpowers/specs/cas-empty-conditional-token-guard
title: 'CAS empty conditional token guard'
doc_type: 'guide'
---

# CAS empty conditional token guard {#cas-empty-conditional-token-guard}

## Problem {#problem}

Every safety-critical CAS mutation is conditional on a token. On an ETag store the condition is
carried by `If-Match`, and `WriteBufferFromS3` adds that header **only when the value is non-empty**
(`WriteBufferFromS3.cpp:656`; `WriteBufferFromAzureBlobStorage.cpp:234` does the same). So an empty
expected token does not produce a condition that fails — it produces **no condition at all**, and the
"fenced" write becomes an unconditional overwrite of whatever is currently there, including a live
holder's object.

Nothing in the backend stops that today. The three conditional entry points validate only the token's
**dialect**:

| entry point | guard | where the value reaches the wire |
|---|---|---|
| `putOverwrite` | `mintingTypeMatches(expected.type)` (`CasObjectStorageBackend.cpp:963`) | `:969` |
| `casPut` (swap form) | `mintingTypeMatches(expected->type)` (`:987`) | `:994` |
| `deleteExact` | `mintingTypeMatches(token.type)` (`:1029`) | `:1040` |

**The hole is exactly one configuration: Native mode on an ETag store.** A default-constructed
`Token{}` has type `ETag`, so on a generation store and in emulated mode the dialect guard already
rejects it. But `tokenForHead` stamps `native_token_type` onto whatever string it is given
(`CasObjectStorageBackend.h:159-162`), so on an ETag store an empty ETag becomes `Token{"", ETag}` — a
**correctly typed, empty-valued** token that passes every existing check.

Empty tokens are reachable, not merely constructible. `tokenFromWriteResult`'s `HEAD` fallback returns
`hr ? hr->token : Token{}` (`CasObjectStorageBackend.cpp:866`), and `nativeHead` validates emptiness
only for generation tokens (`:149-158`). The value then flows into `MountLeaseKeeper::last_token` and
becomes the expected token of the next renewal.

The same file already treats an empty ETag as unusable one function away: `tokenForList` returns
`std::nullopt` for it, on the stated grounds that a token that cannot discriminate is worse than no
token (`CasObjectStorageBackend.h:167-172`). The head-and-write path never got the same treatment.

This is `[empty-token-unconditional-write-guard]`, triaged P2 as "missing guard, not a demonstrated
data-loss path". It is being taken now because it is a prerequisite of the self-authored mount reclaim
design, which turns a conditional write into a safety argument.

## The change {#the-change}

Refuse an empty token value at the same three entry points, immediately after the dialect check, using
each site's existing refusal outcome:

| entry point | refusal |
|---|---|
| `putOverwrite` | `{PutOutcome::PreconditionFailed, {}}` |
| `casPut` | `{CasOutcome::Conflict, {}}` |
| `deleteExact` | `DeleteOutcome::Kind::TokenMismatch` |

The check goes **before** the `Native` / `EmulatedSingleProcess` branch, so the refusal is uniform
across modes even though only the native ETag path can be harmed by it. A uniform guard is cheaper to
reason about than a mode-conditional one, and it makes the emulated backends able to reproduce the
refusal in a test.

`casPut`'s create-if-absent form (`expected == nullopt`) is untouched: it carries no token and rides
`If-None-Match: *`.

## Why refuse rather than throw {#refuse-not-throw}

The backlog entry proposed a `LOGICAL_ERROR`-class throw. Refusing is better, for three reasons.

**It is what the neighbouring guard does.** The dialect check one line above returns each site's
refusal outcome rather than throwing. An empty token is the same kind of fact — a token that can match
nothing — and it should not have a different shape.

**It needs no caller changes, because every caller already handles the outcome.** A refusal is what a
lost condition looks like, and the whole protocol is built to re-validate on one.

**It self-heals through a path that already exists.** Trace the case that matters, a renewal whose
`last_token` is empty:

1. `putOverwriteControlled` calls `putOverwrite`, which now refuses with `PreconditionFailed`;
2. the controller's resolve-by-get reads the slot: the bytes are the *previous* body, not the one this
   renewal is writing, and the token differs from the (empty) expected one, so it reports `Conflict`
   (`CasRequestControl.cpp:703-707`);
3. `MountLeaseKeeper::renew` turns a conflict into a terminal renewal, the mount fences, and the
   remount allocates a fresh `writer_epoch` and claims — obtaining a **fresh, non-empty token**.

So the corrupt token is discarded by the existing recovery, with no new failure mode and no livelock.
A throw would take a different route: `terminalResult` rethrows `LOGICAL_ERROR` rather than converting
it to a terminal renewal result (`CasServerRoot.cpp:1636-1638`), so a `LOGICAL_ERROR` here would
escape the renewal's own containment. `CORRUPTED_DATA` would be converted, but it would still turn a
refusal into an exception on a path whose callers are written for refusals.

## Observability {#observability}

A refusal that looks like an ordinary lost condition would make the anomaly invisible, and an empty
token is never ordinary. Each refusal:

- increments a new `CASConditionalWriteEmptyTokenRefused` profile event, and
- logs once at `Warning` with the key and the operation, rate-limited, saying plainly that a token with
  no value was presented as a condition, that the write was refused rather than sent unconditioned,
  and that the source is an object-storage response that carried no ETag.

The event is the thing to alert on: it is zero in every healthy deployment, and a non-zero value means
the store returned a write or head response with no ETag.

## What this does not do {#what-this-does-not-do}

**It does not stop an empty token being minted.** `tokenForHead` still produces `Token{"", ETag}` and
`tokenFromWriteResult` still falls back to a fresh `HEAD`. This change makes such a token harmless at
the point where it could do damage, which is the whole safety property; making the read and write paths
stop producing it is a larger change with its own consumers to consider.

**It does not address token provenance.** A *non-empty* token from `tokenFromWriteResult`'s fallback
`HEAD` may belong to a different writer's object entirely, and there is already a consumer where that
is not fail-safe — `Gc::acquireOrRenewLease` stores it and the round commit CASes against it. That is
`[write-token-provenance-not-in-the-api]`, and this change neither fixes nor worsens it.

**It does not change the emulated backends' behaviour in practice.** They already refuse an empty
token, by dialect (a default `Token{}` is not `Emulated`) or by value comparison. The guard makes the
refusal explicit and testable there; it changes no outcome.

## Verification {#verification}

**The test that matters must run against the native backend.** Emulated and in-memory backends refuse
an empty token already, so a test on them passes with the defect fully present — that is precisely why
this survived to now. The native path needs a real conditional-write backend (the CA-s3 lane, or a
gtest against the S3-compatible store used by the backend contract tests).

1. **Native, ETag mode, `putOverwrite` with `Token{"", ETag}`** against a key holding someone else's
   object: the object is **unchanged** afterwards, and the call returns `PreconditionFailed`. Without
   the guard this test fails by overwriting — it is the regression test for the defect itself.
2. The same for `casPut`'s swap form and for `deleteExact`: the object survives, the outcome is
   `Conflict` / `TokenMismatch`.
3. `casPut` with `expected == nullopt` still creates: the guard did not catch the create form.
4. A non-empty token still commits, and a non-empty **wrong** token still refuses: the guard did not
   change ordinary conditional behaviour.
5. Generation mode is unaffected: an empty generation token is still refused, now by the value guard
   rather than only by the dialect guard, and the outcome is the same.
6. The profile event increments exactly once per refusal, and the emulated backend can drive that
   assertion without a real store.

**Integration:** the renewal trace above, end to end. Inject an empty `last_token` into a live keeper,
and assert the sequence — refusal, conflict, terminal renewal, fence, remount with a fresh epoch — and
that the mount slot's object is never written unconditionally along the way. This is the test that
proves the self-healing claim rather than asserting it.

## Acceptance {#acceptance}

1. On a native ETag store, no conditional mutation can be issued with an empty expected token, proven
   by a test that overwrites without the guard and does not with it.
2. No caller changed. The three refusal outcomes are the ones callers already handle.
3. A renewal holding an empty token recovers through fence and remount, with the mount object never
   written unconditionally.
4. `CASConditionalWriteEmptyTokenRefused` is zero across the existing test suites, and non-zero in the
   test that provokes it.
5. The CA-s3 lane and the `CAS*` gtest gate stay green.

## Out of scope {#out-of-scope}

**Paired reads.** Taking the token from the `GET` response rather than a preceding `HEAD` removes a
round trip and makes the bytes/token pair atomic by construction. It is the natural companion to this
change and the next step in the same sequence, but it touches five call sites and a new backend read
method; it is not folded in here.

**`[write-token-provenance-not-in-the-api]`** and **`[resolved-by-get-unbounds-clone-overlap]`**, both
filed separately.

**The self-authored mount reclaim design**, which lists this change as a prerequisite.

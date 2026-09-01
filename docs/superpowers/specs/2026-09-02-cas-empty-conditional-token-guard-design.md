---
description: 'Design for rejecting a token that cannot name an exact incarnation at the CAS conditional-mutation entry, so a fenced write can never degrade into an unconditional clobber.'
sidebar_label: 'Exact-token entry guard'
sidebar_position: 43
slug: /superpowers/specs/cas-empty-conditional-token-guard
title: 'CAS exact-token entry guard'
doc_type: 'guide'
---

# CAS exact-token entry guard {#cas-exact-token-entry-guard}

Revision 2. Revision 1 was reviewed and rejected; [what revision 1 got wrong](#what-revision-1-got-wrong)
records why, because two of its errors would have made things worse rather than merely incomplete.

## Problem {#problem}

Every safety-critical CAS mutation is conditional on a token that must name **one exact incarnation**.
Three entry points forward that token, and each validates only its **dialect**:

| entry point | guard | where the value reaches the wire |
|---|---|---|
| `putOverwrite` | `mintingTypeMatches(expected.type)` (`CasObjectStorageBackend.cpp:963`) | `:969` |
| `casPut`, swap form | `mintingTypeMatches(expected->type)` (`:987`) | `:994` |
| `deleteExact` | `mintingTypeMatches(token.type)` (`:1029`) | `:1040` |

A value that passes the dialect check but cannot name an exact incarnation is forwarded anyway. The
sharpest case is the empty string: `WriteBufferFromS3` attaches `If-Match` **only when the value is
non-empty** (`WriteBufferFromS3.cpp:656` and `:746`, its two upload sites;
`WriteBufferFromAzureBlobStorage.cpp:234` does the same). So an empty expected token does not make a condition that fails — it makes **no condition at
all**, and a fenced write becomes an unconditional overwrite of whatever is currently there, including
a live holder's object.

**Reachability.** `tokenForHead` stamps `native_token_type` onto whatever string it is given
(`CasObjectStorageBackend.h:159-162`), and `nativeHead` validates emptiness only for generation tokens
(`:149-158`). `tokenFromWriteResult`'s `HEAD` fallback returns `hr ? hr->token : Token{}`
(`CasObjectStorageBackend.cpp:866`), and that value flows into `MountLeaseKeeper::last_token`. That is
the naturally minted path, and it exists on ETag stores.

Two further sources make this **not** a single-dialect problem, contrary to revision 1:

- **Deserialization.** `TokenFields::build` requires both fields to be *present* but accepts an empty
  value, in any dialect (`CasWireVocab.cpp:110-115`); the binary condemned-row decoder likewise accepts
  `token_len == 0` (`CasBlobInDegree.cpp:205`). A condemned row's token reaches `deleteExact` on GC's
  destructive path.
- **Direct construction.** `Token{"", TokenType::Generation}` passes `mintingTypeMatches` on a
  generation store, and the GCS adapter validates an `If-Match` only when one is present rather than
  requiring a condition on every conditional request — so the PUT goes out unconditioned there too.

**Not every entry point fails the same way, and this is the fact revision 1 missed.** The write paths
put the value in `WriteSettings`, where an empty string means "omit the header". The delete path calls
`request.SetIfMatch(etag)` unconditionally (`S3ObjectStorage.cpp:513`), so an empty token produces a
malformed request rather than an unconditioned one. The codebase already knows this and says so in
`CasProbe`'s cleanup comment: "A deleteExact with the absent HeadResult's EMPTY token is a malformed
conditional op — AWS S3 answers `400 InvalidArgument` (`If-Match cannot be empty`)". **Deletion is
fail-loud today.** Any change there must keep it that way.

This is `[empty-token-unconditional-write-guard]`, triaged P2. It is being taken now because it is a
prerequisite of the self-authored mount reclaim design, which turns a conditional write into a safety
argument.

## The change {#the-change}

One predicate, applied at all three entry points immediately after the dialect check and **before** the
mode branch; two different reactions, chosen by what each site does today.

### The predicate {#the-predicate}

Not "non-empty" — "can only match the exact incarnation it names". Emptiness is the reachable case, but
it is not the only value that fails that test, and drawing the guard at emptiness would leave aliases
that defeat exactness just as completely:

| dialect | accepted |
|---|---|
| Generation | non-empty, all ASCII digits, and already canonical (`value == normalizeTokenValue(value)`) — a quoted `"123"` is unequal to `123` under `Token::operator==` yet the GCS adapter unquotes it, so it can commit against a token it does not equal |
| ETag | non-empty, and not the wildcard `*` after trimming optional whitespace — `If-Match: *` means "any current representation exists" under HTTP semantics, which is precisely not an exact incarnation |
| Emulated | non-empty |

**Deliberately not included: a full entity-tag grammar.** A comma-separated `If-Match` list matches any
listed tag, and Azure documents evaluating multiple values with logical OR — so a list is also not
exact. But S3-compatible stores vary in what they accept, and imposing a strict grammar without a
compatibility survey risks refusing tokens real stores mint. Empty and wildcard are unambiguous and
cost nothing; the list case is recorded in [Out of scope](#out-of-scope) with the survey it needs.

### The reaction, per site {#reaction-per-site}

**`putOverwrite` and `casPut`'s swap form: refuse**, with each site's existing outcome
(`PreconditionFailed` / `Conflict`). Today these silently clobber; any refusal is strictly better, and
the refusal is shaped exactly like the dialect guard one line above, so callers already handle it.

**`deleteExact`: throw.** It must not reuse `TokenMismatch`. `Backend` documents that outcome as proof
that **another incarnation is current** (`CasBackend.h:93-95`), and callers act on it as evidence:

- GC's blob redelete sees `TokenMismatch`, HEADs the key, finds the object present, labels it
  `Replaced`, drops it from the retirement pipeline and forgets its condemn-marker confirmation
  (`CasGc.cpp:683`, `:728`). With an invalid token it may be the *same* undeleted incarnation — a false
  audit record and a permanently leaked blob;
- wholesale prefix cleanup ignores the outcome, counts the object as deleted, and can declare a prefix
  drained (`CasGc.cpp:3434-3441`: the outcome is discarded and `++deleted` is unconditional),
  advancing retention over objects that still exist;
- GC metadata deletion turns from a logged exception into a silent no-op (`CasGcMetaWriter.cpp:72`);
- `CasPlainObjects::casRemoveObject` turns an immediate exception into 100 `HEAD`/refuse cycles before
  `ABORTED` (`CasPlainObjects.cpp:77`).

So a refusal outcome at `deleteExact` would convert a loud malformed-request failure into quiet,
data-affecting misinformation. Throwing `CORRUPTED_DATA` preserves today's shape — the operation fails
loudly — while moving the failure from the wire to the caller's own process, where the message can name
the token and the key. No delete caller changes, because every one of them already propagates or logs
an exception from this call.

## What it does and does not fix {#what-it-fixes}

**Fixed:** no conditional mutation can be issued with a token that cannot name an exact incarnation. On
the write paths that closes a real fail-open hole. On the delete path it turns a remote 400 into a
local, self-describing failure and removes the possibility that a future transport change makes an
invalid delete token silently unconditional.

**Not fixed, and revision 1 claimed otherwise: this is not self-healing in general.** Revision 1 traced
one favourable branch — refusal, resolve-by-get reports `Conflict`, terminal renewal, fence, remount
with a fresh token — and presented it as the outcome. The controller reports `Conflict` only when the
resolving `GET` returns **both** a different token and different bytes (`CasRequestControl.cpp:703-707`).
If the store keeps returning an empty ETag, the resolve reads the same empty token, `got->token ==
expected` holds, and the controller concludes our attempt never applied and retries until its budget is
spent — no conflict, no terminal, no remount. And a remount only obtains a usable token if the store has
started returning one.

The honest statement is narrower: **the refusal always fails closed — no mutation is sent — but what
happens next is the caller's, and for a persistent store anomaly it is a bounded failure rather than a
recovery.** Per caller:

| caller | behaviour under a persistently invalid token |
|---|---|
| `PoolMeta::admitOrValidate` | **unbounded** `for (;;)` conflict loop (`CasPoolMeta.cpp:75`) — a tight livelock; filed separately |
| `CasPlainObjects` | 100 retries, then `ABORTED` |
| ref-catalog mutation, epoch allocation, checkpoint publication | bounded, then fail |
| GC lease acquire/renew | bounded, returns false |
| GC heartbeat | silently ignores the conflict (`CasGc.cpp:4360`) |
| mount-floor fencing | four retries, conservatively classifies the holder as live |

None of these is worse than today's unconditional write in terms of *damage*; several are worse in
terms of *availability*, and the unbounded loop is a genuine new hazard that the guard exposes rather
than creates. It is worth fixing in the same series.

## Observability {#observability}

A refusal that looks like an ordinary lost condition would make the anomaly invisible, and an invalid
token is never ordinary. Each refusal or throw:

- increments a new `CASConditionalWriteInvalidTokenRefused` profile event, and
- logs once at `Warning`, rate-limited, naming the key, the operation and **which rule failed**
  (empty, wildcard, non-canonical generation).

The message must not assert a cause. Revision 1's did — it blamed "an object-storage response that
carried no ETag" — but a decoded record, a directly constructed token and a test seam are equally
possible sources, and a message that names the wrong one sends the reader to the wrong place.

The event is the thing to alert on: zero in every healthy deployment, non-zero means a token that
cannot fence reached a fencing operation.

## Verification {#verification}

Revision 1's plan could false-pass in two ways, and both are fixed here: a fake native backend that
treats an empty `object_storage_write_if_match` as a *failed condition* never exercises
`WriteBufferFromS3`'s omission, and `InMemoryBackend` is a separate implementation that would not run
the guard at all.

**Two layers, both required.**

*No-wire, against `ObjectStorageBackend` with a call-counting object storage:*

1. `putOverwrite`, `casPut` swap form and `deleteExact` with `Token{"", ETag}` on an ETag backend →
   **zero** `writeObject` and zero `removeObjectIfTokenMatches` calls; the two write paths return their
   refusal, `deleteExact` throws;
2. the same with `Token{"", Generation}` on a generation backend — revision 1 wrongly claimed the
   dialect guard already covered this;
3. `Token{"*", ETag}` and `Token{" * ", ETag}` → refused, zero calls;
4. `Token{"\"123\"", Generation}` (non-canonical) → refused, zero calls;
5. `casPut` with `expected == nullopt` still creates — the guard did not catch the create form;
6. a valid token still commits, and a valid **wrong** token still refuses through the ordinary path;
7. the profile event increments exactly once per refusal. This test must use `ObjectStorageBackend` in
   `EmulatedSingleProcess` mode; `InMemoryBackend` does not execute this code, and its
   `setEnforceTokens(false)` seam accepts every token by design (`CasInMemoryBackend.h:110`).

*Against a real S3-compatible store, with distinct old and new bodies:* a conditional overwrite with an
empty token leaves the object **unchanged**. This is the one test that is red without the guard and
green with it, and it is the reason the no-wire layer alone is insufficient.

*Caller-level, for the delete contract:* a delete caller presented with an invalid token must not
record `Replaced` and must not count the object as drained. Assert on GC's redelete path and on the
wholesale prefix cleanup that the entry stays in the pipeline and the counters do not advance.

*Renewal, both branches:* inject an empty `last_token` and (a) let the resolve return a valid token —
conflict, terminal, fence, remount with a fresh token; (b) keep the store returning an empty ETag —
assert the bounded-retry-then-fail outcome and that **no unconditional write is ever issued**. Branch
(b) is the one revision 1 omitted, and it is the one that documents the real behaviour.

*Livelock:* `PoolMeta::admitOrValidate` against a persistently invalid token, asserting the loop is
bounded once its own fix lands.

## Acceptance {#acceptance}

1. No conditional mutation can be issued with a token that fails the predicate, proven by the real-store
   test that overwrites without the guard and does not with it.
2. `deleteExact` fails loudly and is never reported as `TokenMismatch`; no delete caller records
   `Replaced` or counts a drain on an invalid token.
3. No caller of the two write paths changed.
4. `CASConditionalWriteInvalidTokenRefused` is zero across the existing suites and non-zero in the tests
   that provoke it.
5. Both renewal branches behave as documented, and neither issues an unconditional write.
6. The CA-s3 lane and the `CAS*` gtest gate stay green.

## Out of scope {#out-of-scope}

**Rejecting an empty token at decode.** `TokenFields::build` and the condemned-row decoder accept an
empty value; guarding there would fail earlier and name the corrupt record rather than the operation.
It is small and probably right, but it is a second site with its own compatibility question — a
persisted empty token would become an undecodable record rather than a refused operation — so it is
recorded, not folded in.

**An entity-tag grammar for ETag values.** Comma-separated lists are not exact either, and Azure
documents OR-evaluation of multiple values. Needs a compatibility survey across the S3-compatible
stores in the test matrix before it can be enforced.

**`PoolMeta::admitOrValidate`'s unbounded conflict loop.** Exposed by this change, not caused by it;
filed separately and worth fixing in the same series.

**Paired reads.** Taking the token from the `GET` response rather than a preceding `HEAD` removes a
round trip and makes the bytes/token pair atomic by construction. Next in this sequence, not folded in.

**`[write-token-provenance-not-in-the-api]`** and **`[resolved-by-get-unbounds-clone-overlap]`**, both
filed separately.

## What revision 1 got wrong {#what-revision-1-got-wrong}

**It gave `deleteExact` a refusal outcome.** `TokenMismatch` is documented as evidence that another
incarnation is current, and GC acts on it: labels a blob `Replaced`, drops it from the retirement
pipeline, counts prefixes as drained. Deletion is fail-loud today — S3 answers `400 InvalidArgument`
on an empty `If-Match`, as `CasProbe`'s own comment records — so revision 1 would have converted a loud
failure into quiet misinformation and a leaked blob. This is the error worth remembering: the two write
paths and the delete path fail differently at the transport, and a guard that treats them identically
gets one of them backwards.

**It scoped the hole to "Native mode on an ETag store".** True of the naturally minted path, false of
the entry point: a typed empty generation token is accepted by the dialect guard and the GCS adapter
does not require a condition, and decoded tokens admit an empty value in any dialect.

**It claimed self-healing, no new failure mode and no livelock.** It traced one branch of the resolve
and presented it as the outcome. A persistently empty token takes the other branch, and
`PoolMeta::admitOrValidate` turns it into an unbounded loop.

**It drew the predicate at emptiness**, leaving `*` and non-canonical generation values — both of which
defeat exactness as completely as an empty string.

**Its verification could pass with the defect present**, because a fake backend may treat an empty
condition as failed, and because `InMemoryBackend` does not run the guard at all.

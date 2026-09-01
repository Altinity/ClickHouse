---
description: 'Design for a CAS backend token contract that cannot be misused: tokens minted only by the backend from a store response, bytes and token always from one response, one API call per store request.'
sidebar_label: 'Backend token contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend token contract'
doc_type: 'guide'
---

# CAS backend token contract {#cas-backend-token-contract}

This document supersedes `2026-09-02-cas-empty-conditional-token-guard-design.md` and revises the
prerequisites of `2026-09-01-cas-self-authored-mount-reclaim-design.md`. Both were written under the
current contract and spent their revisions guarding symptoms of the defect this document removes.

## Problem {#problem}

Six review rounds across two designs found nine classes of defect in how CAS handles incarnation
tokens:

1. an empty value passes as a token, and on S3 and Azure an empty `If-Match` is **omitted**, so a
   fenced write becomes an unconditional overwrite;
2. `*` and comma-separated lists pass, and mean "any current representation", not one incarnation;
3. a non-canonical generation (`"123"`) is unequal to `123` under `Token::operator==` yet the GCS
   adapter unquotes it, so it can commit against a token it does not equal;
4. a write's token may be the result of a **later, unrelated `HEAD`**, so it can belong to a
   different writer's object — and `Gc::acquireOrRenewLease` commits against it;
5. `get` assembles `{bytes, token}` from a `HEAD` and a `GET`, so the pair is atomic only under a
   directional assumption that a caching proxy breaks silently;
6. dialect is a runtime check at three entry points, not a property of the type;
7. the same invalid input fails differently at the write paths (header omitted) and the delete path
   (malformed request, `400`);
8. decoders accept an empty persisted token in any dialect, and it reaches the destructive
   `deleteExact`;
9. `TokenMismatch` is documented as remote evidence that another incarnation is current, yet the
   existing dialect guard returns it for a **local** refusal (`CasObjectStorageBackend.cpp:1029-1034`)
   — and GC acts on it, labelling blobs `Replaced` and counting prefixes drained.

Eight of the nine have one root: **`Token` is `struct { String value; TokenType type; }`, anyone can
construct one from anything, and the backend accepts what it is handed.** The contract lives in
runtime checks scattered across consumers, and the checks do not agree with each other. Every
previous revision guarded one symptom at one site, and the next review found the same root showing
through a different one.

The ninth — same-content replay — is the only one that is a property of the store rather than of the
type, and it is the only assumption this contract has to state.

A second, independent waste rides along: `Backend::get` **always** issues both a `HEAD` and a `GET`,
though a `GET` returns everything a `HEAD` does plus the body. Every control-object read costs two
requests; `MountLeaseKeeper::claim` costs three; the mount-observation loop pays two every five
seconds. `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by no
caller.

## The contract {#the-contract}

Four sentences. Everything below is their consequence.

> **A `Token` is minted only by the backend, only from a store response, and only when that response
> names exactly one incarnation.** Nothing else can produce one.
>
> **Bytes and a token are paired only when they came from the same response.** There is no operation
> that attaches one response's token to another response's bytes.
>
> **A write returns its committed token if the response carried one, and says plainly if it did not.**
>
> **One backend call is one request to the store.** No call issues two.

Under this contract classes 1–8 are not guarded against; they are unexpressible. Class 9 is named in
[Assumptions](#assumptions).

## The `Token` type {#the-token-type}

`Token` keeps its shape — `value` and `type` — and loses its public constructor. Only `Backend`
implementations can mint one, through one function that applies the minting rules:

| dialect | a value is a token iff |
|---|---|
| Generation | non-empty, all ASCII digits, already canonical (`value == normalizeTokenValue(value)`) |
| ETag | non-empty and not `*` after trimming optional whitespace |
| Emulated | non-empty |

A value that fails is not a token. On a `HEAD` or `GET` response that is a `CORRUPTED_DATA` exception
naming the key — the store returned an identity that cannot identify. On a write response it is
`Committed{nullopt}`, below. There is no third outcome and no fallback.

**Deliberately not included:** a full entity-tag grammar for ETag values. A comma-separated list is
not exact either, but S3-compatible stores vary in what they mint, and a strict grammar without a
compatibility survey risks refusing tokens real stores produce. Empty and wildcard are unambiguous
and cost nothing; the list case is recorded in [Out of scope](#out-of-scope).

Because the backend mints only its own dialect, `mintingTypeMatches` and its three call sites are
deleted. There is no foreign-dialect token to refuse.

**Decoding is minting.** Tokens are persisted in `cas_run` records (`SourceEdgeRecord`) and GC
outcomes. `TokenFields::build` today assembles a `Token` from any two present fields
(`CasWireVocab.cpp:110-115`); the binary condemned-row decoder accepts `token_len == 0`
(`CasBlobInDegree.cpp:205`). Both now call `backend.parseToken(type_word, value)`, the same minting
function, so a persisted empty or foreign token fails to decode with `CORRUPTED_DATA` naming the
record — instead of reaching `deleteExact` as a condition that cannot fence. Per the pre-release rule,
no compatibility path is written for previously persisted invalid values.

## Read surface {#read-surface}

Three operations. Each is one request, and the type says what it carries.

| operation | returns | request | use when |
|---|---|---|---|
| `readSnapshot(key, max_bytes)` | `Snapshot{bytes, token}`, or absent | one `GET`, via `readSmallObjectAndGetObjectMetadata` | the caller needs the bytes **and** their identity: every read-modify-CAS on a control object, the controller's resolve-by-get |
| `openStream(key)` | a `ReadBuffer`, or absent | one `GET` | the caller needs bytes and no identity: staging reads, the `cas_run` scan |
| `head(key)` | `Identity{exists, size, token, attributes}` | one `HEAD` | the caller does **not** want the body: existence, size, a token to delete without reading |

`Snapshot::token` and `Identity::token` are the same unforgeable `Token`, and either is a valid
precondition for a conditional mutation. The difference is what else the caller holds: a `Snapshot`
can be compared **by bytes** to something the caller wrote; an `Identity` cannot, because it has no
bytes. The hazard was never a `HEAD` token as such. It was presenting one as an observation of bytes it
did not see.

`readSnapshot` takes `max_bytes` because `readSmallObjectAndGetObjectMetadata` materialises the body.
Control objects are small; the ref catalog is the one that can grow, and it is already read whole
today. The bound is per object kind, not a limit on the design. The base `IObjectStorage`
implementation returns **empty** metadata (`IObjectStorage.cpp:41-56`); on such a storage a
`readSnapshot` whose response carries no token is the `CORRUPTED_DATA` case above, never a silent
tokenless snapshot.

**What goes.** `get` and its `GetResult` — the `HEAD`-then-`GET` pair, the directional-consistency
comment that justified it (`CasObjectStorageBackend.cpp:588-609`), and the `known_size` plumbing that
existed to avoid a second metadata round trip the first one had already paid for. `GetStreamResult::
token`, produced by a `HEAD` and consumed by nobody. The `Range` parameter on reads: no production
caller passes a non-whole range — every `get` and both `getStream` callers read whole objects — so the
ranged machinery, its size clamp and the `HEAD` the clamp needed are removed rather than redesigned.

**Blobs are not affected, because they were never here.** Column data is read through
`ContentAddressedMetadataStorage::readBlobPayload` → `object_storage->readObject` wrapped in a
`ReadBufferFromFileView` (`ContentAddressedMetadataStorage.cpp:2080`); `MergeTree` seeks by mark and
`ReadBufferFromS3` turns each seek into a ranged `GET`, with the object size supplied from the
`BlobLocation` in the `StoredObject`, so the buffer never falls back to its own `HEAD`. That path
already satisfies the contract: one request per window, size known, no token because none is needed.

## Write surface {#write-surface}

`putIfAbsent`, `putOverwrite` and `casPut` keep their signatures. Their result changes shape:

```
Committed { std::optional<Token> token; }   // nullopt: written, but the response named no incarnation
```

**The committed token comes only from the write response.** `tokenFromWriteResult` loses its second
half: the fallback `HEAD` (`CasObjectStorageBackend.cpp:865-866`) is deleted, and an absent or
invalid response value yields `nullopt`. For S3 in the ETag dialect the response always carries an
ETag, so `nullopt` is an anomaly; for GCS it always carries a generation; for local files it never
does, and `nullopt` there is the truth rather than a defect the old code papered over with a `HEAD`.

A caller that needs a token after `Committed{nullopt}` does `readSnapshot` and compares the bytes to
what it wrote. That is exactly what `putOverwriteControlled` already does as `resolved_by_get`
(`CasRequestControl.cpp:678-687`), now on an atomically paired read and invoked explicitly where it is
needed. Two production consumers change:

- **`MountLeaseKeeper::last_token`** becomes `std::optional<Token>`. A renewal with `nullopt` first
  reads the slot; if the bytes are its own last body, that snapshot's token is the precondition. One
  extra `GET` in the anomalous case, and no way to hold a token that belongs to someone else.
- **`Gc::acquireOrRenewLease`** stores `state_token` straight from the write result
  (`CasGc.cpp:4397`, `:4418`) and the round commit CASes against it (`:944`). With `nullopt` it returns
  `false`: a leader that cannot name its own incarnation is not a leader. Today it would have accepted
  a successor's token from the fallback `HEAD` and committed over it.

## Delete surface {#delete-surface}

`deleteExact(key, token)` is unchanged in shape. What changes is what its outcomes can mean. With no
local refusal left — nothing to check, because an invalid token cannot reach it — `TokenMismatch` and
`NotFound` are **only ever the store's answer**, which is what `CasBackend.h:93-95` already says they
are. GC's redelete, which HEADs on `TokenMismatch` and labels the blob `Replaced`, is now acting on
evidence. It was not before.

## Assumptions {#assumptions}

**One.** A token names one incarnation, and the store never reuses a token for **different** content.
That is the existing `Backend` contract (`CasBackend.h:226`). What the contract does not, and cannot,
promise is that identical content never yields the same token: an S3 ETag is content-derived, so a
restored copy of an old body may carry the old ETag. A design that recognises its own body by bytes
therefore also recognises a restored copy of it. This is an environmental assumption of the whole
protocol — it defeats `proven_dead_token` observation just as much — and it is stated here once so no
downstream design has to rediscover it.

Nothing else is assumed. In particular, no ordering between two requests is assumed anywhere, because
no operation makes two.

## Test seams, named {#seams}

- `CasProbe` constructs a deliberately wrong token to prove enforcement (`CasProbe.cpp:180`). Under
  this contract it obtains one by minting from a real response of a *different* object, which is what
  a wrong token is. No friend access.
- `InMemoryBackend::setEnforceTokens(false)` models a store that accepts every token
  (`CasInMemoryBackend.h:110`). It stays: it is about the store's behaviour, not about constructing
  tokens.
- `EmulatedSingleProcess` mints its own tokens from the object's mtime and goes through the same
  minting function. Its `emu_mutex` is an instance member though the header promises a "process-wide
  mutex" (`CasObjectStorageBackend.h:39` vs `:210`); with one backend per pool that is latent, but the
  comment is corrected in this change because a wrong comment is how revision 1 of the reclaim design
  went wrong.

## Effects on the designs this supersedes or revises {#effects}

**The exact-token entry guard (`2026-09-02`) is superseded entirely.** Its three per-site checks, its
per-site reactions and its dialect-specific predicate are the private constructor. Its finding that
deletion must stay fail-loud is honoured by construction: an invalid token cannot be constructed, so
`deleteExact` never sees one.

**Paired reads are `readSnapshot`.** No separate step.

**`[write-token-provenance-not-in-the-api]` is closed** by `Committed{optional<Token>}`; the GC lease
consumer is fixed here.

**The self-authored mount reclaim (`2026-09-01`, revision 5)** loses its directional-read tripwire and
its paired-read prerequisite, both of which exist to manage a hazard this contract removes. Its
byte-recognition rule stands unchanged, and its remaining environmental assumption is the one stated
above. Its prerequisites section is to be rewritten against this document once this lands.

## Verification {#verification}

**At compile time.** No translation unit outside `Backend/` constructs a `Token` with a value. Today
that is two sites — `TokenFields::build` and a benchmark — and the change makes it zero. This is the
test that matters most and it is not a test: it is the absence of a constructor.

**One call, one request.** Against a call-counting object storage, each of `readSnapshot`,
`openStream`, `head`, `putIfAbsent`, `putOverwrite`, `casPut` and `deleteExact` issues exactly one
request. This is the regression test for the `HEAD`+`GET` pattern returning.

**Minting refuses what it must.** Against an object storage returning a chosen ETag or generation: an
empty value, `*`, `" * "`, a non-canonical generation → `CORRUPTED_DATA` on `head` and `readSnapshot`,
`Committed{nullopt}` on a write; zero conditional requests issued afterwards by any caller handed the
result.

**Decoding refuses what it must.** A `cas_run` record and a GC outcome with an empty token → the
record fails to decode, naming itself; nothing reaches `deleteExact`.

**The two `nullopt` consumers.** `MountLeaseKeeper` with `last_token == nullopt` renews through a
`readSnapshot` whose bytes match its last body, and refuses when they do not; `Gc::acquireOrRenewLease`
returns `false` on `Committed{nullopt}` and no round commit follows.

**Against a real S3-compatible store**, with distinct old and new bodies: a conditional overwrite
cannot be issued without a token, because there is no token to pass — the test exists to fail if a
constructor is ever reintroduced.

**Existing gates.** The CA-s3 lane and the `CAS*` gtest gate stay green. `Cas::Probe` still passes on
every supported store.

## Acceptance {#acceptance}

1. `Token` has no public constructor; `grep` for `Token{` outside `Backend/` and tests returns nothing.
2. `get`, `GetResult`, `GetStreamResult::token`, `Range` on reads, `mintingTypeMatches` and the
   fallback `HEAD` in `tokenFromWriteResult` no longer exist.
3. Every backend operation issues exactly one store request under the call-counting test.
4. `deleteExact` never returns `TokenMismatch` for any reason other than the store's own mismatch.
5. Both `nullopt` consumers behave as specified.
6. The exact-token guard spec is marked superseded, and the reclaim design's prerequisites point here.
7. Existing lanes and gates green.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState` and
`keeper_state` read as calls into ClickHouse Keeper. They are not: the object **renews a mount lease**.
The collision is concrete — `CasRequestControl.h:250` discusses real ZooKeeper retry semantics two
files away from a class named `Keeper`. The precise name is `MountLeaseRenewer` (`installRenewer`,
`startRenewer`, `admitRenewerCall`, `MountLeaseRenewerState`).

Sized: 159 occurrences in 8 non-test files, 249 in tests. Mechanical, and its own commit — a rename
folded into a contract change makes both diffs unreadable.

## Out of scope {#out-of-scope}

**An entity-tag grammar for ETag values**, pending a compatibility survey across the S3-compatible
stores in the test matrix.

**Ranged reads through `Backend`.** No production caller needs one. If one appears, the object size
comes from the caller — CAS always has it — and the API refuses to guess; `Content-Range` parsing and
`416` handling are built then, not now.

**`PoolMeta::admitOrValidate`'s unbounded conflict loop** (`CasPoolMeta.cpp:75`) and
**`resolved_by_get` under lockstep clones** (`[resolved-by-get-unbounds-clone-overlap]`): both
pre-existing, both filed, neither changed here.

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

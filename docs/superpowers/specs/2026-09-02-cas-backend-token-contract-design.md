---
description: 'Design for a CAS backend token contract that cannot be misused: tokens minted only by the backend, bytes and token always from one physical request, every write tokened or refused at mount, one API call per store request.'
sidebar_label: 'Backend token contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend token contract'
doc_type: 'guide'
---

# CAS backend token contract {#cas-backend-token-contract}

Revision 2. Revision 1 was reviewed and rejected; [what revision 1 got wrong](#what-revision-1-got-wrong)
records why. This document supersedes `2026-09-02-cas-empty-conditional-token-guard-design.md` and
revises the prerequisites of `2026-09-01-cas-self-authored-mount-reclaim-design.md`.

## Problem {#problem}

Six review rounds across two designs found nine classes of defect in how CAS handles incarnation
tokens. Every one below was verified against the code by the review of revision 1.

1. an empty value passes as a token, and `WriteBufferFromS3` attaches `If-Match` **only when the value
   is non-empty** (both upload sites), so a fenced write becomes an unconditional overwrite;
2. `*` passes, and means "any current representation", not one incarnation;
3. a non-canonical generation (`"123"`) is unequal to `123` under `Token::operator==`, yet the GCS
   adapter strips the quotes and it commits against a token it does not equal;
4. a write's token may be the result of a **later, unrelated `HEAD`** (`tokenFromWriteResult`'s
   fallback), so it can belong to a different writer's object — and `Gc::acquireOrRenewLease` stores
   it and the round commit CASes against it;
5. `get` assembles `{bytes, token}` from a `HEAD` and a `GET`, so the pair is atomic only under a
   directional assumption that a caching proxy breaks silently;
6. dialect is a runtime check at three entry points, not a property of the type;
7. the same invalid input fails differently at the write paths (header omitted) and the delete path
   (`SetIfMatch` unconditional, `400 InvalidArgument`);
8. decoders accept an empty persisted token in any dialect (`TokenFields::build` checks presence;
   `decodeCondemnedRow` accepts `len == 0`), and a `RetiredEntry::token` reaches the destructive
   `deleteExact`;
9. `TokenMismatch` is documented as remote evidence that another incarnation is current, yet the
   existing dialect guard returns it for a **local** refusal — and GC's redelete labels the blob
   `Replaced`, the prefix cleanup does `++deleted` unconditionally.

Eight of the nine have one root: **`Token` is `struct { String value; TokenType type; }`, anyone can
construct one from anything, and the backend accepts what it is handed.** The ninth — same-content
replay — is a property of the store and is the one assumption this contract states.

A second waste rides along. `Backend::get` always issues a `HEAD` and a `GET`, though a `GET` returns
everything a `HEAD` does plus the body: every control-object read is two requests, the mount-observation
loop pays two every five seconds, and `MountLeaseKeeper::claim` pays two on its mint path and four on
its adopt path. `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by no
caller. `Backend::probeSentinelRaw` is a `HEAD` then a `GET` — three requests on Native.

## The contract {#the-contract}

Four sentences. Everything below is their consequence.

> **A `Token` is minted only by the backend — from a store response, or by adopting a persisted record
> of the backend's own dialect — and only when the value names exactly one incarnation.** Nothing else
> can produce one.
>
> **Bytes and a token are paired only when they came from one physical request.** A read that would
> have to reissue its request fails instead of continuing.
>
> **A writable store returns a token for every write, and the capability probe proves it at mount.** A
> write response without a token at runtime is a store anomaly and an exception, never a value.
>
> **One backend call is one request to the store.**

Under this contract classes 1–8 are not guarded against; they are unexpressible. Class 9 is named in
[Assumptions](#assumptions).

## The `Token` type {#the-token-type}

`Token` keeps `value` and `type`, and both become **private**. It has no default constructor and no
public valued constructor; copy and move stay public so `std::optional<Token>` and containers work;
`operator==` stays defaulted. Two assertions make the property checkable at compile time rather than by
a runtime test that cannot observe the absence of a constructor:

```
static_assert(!std::is_default_constructible_v<Token>);
static_assert(!std::is_constructible_v<Token, String, TokenType>);
```

Revision 1 said "loses its public constructor" and stopped. That is insufficient while the members are
public — `Token t; t.value = "*";` compiles — and while a default constructor exists, because an
always-empty-capable `Token` reproduces defect class 1 inside the type. "Not yet" is spelled
`std::optional<Token>`, and the roughly twenty-five production sites that default-construct a `Token`
member today become optionals, are constructed with a token, or are restructured; the review of revision
1 lists them, and the implementation plan carries that list.

Minting is one protected `Backend::mint(TokenType, String)` declared `friend` by `Token`, so every
backend — the native one, the emulated one, `InMemoryBackend` and its test subclasses, the instrumented
proxy — has exactly one path. It applies the minting rules:

| dialect | a value is a token iff |
|---|---|
| Generation | non-empty and all ASCII digits (which already excludes the quoted form) |
| ETag | non-empty and not `*` after trimming optional whitespace |
| Emulated | non-empty |

A value that fails is not a token. On a `HEAD` or `GET` response that is a `CORRUPTED_DATA` exception
naming the key — the store returned an identity that cannot identify. On a write response it is the
anomaly described in [Write surface](#write-surface). Because the backend mints only its own dialect,
`mintingTypeMatches` and its three call sites are deleted.

**Deliberately not included:** a full entity-tag grammar for ETag values. A comma-separated list is not
exact either, but S3-compatible stores vary in what they mint, and a strict grammar without a
compatibility survey risks refusing tokens real stores produce. Recorded in [Out of scope](#out-of-scope).

**Tests mint through one named seam**, `Token::forTest(TokenType, String)`, so the fifty-odd
constructions across fourteen test files have one thing to migrate to and the production code has
nothing to reach for.

## Persisted tokens {#persisted-tokens}

Tokens are persisted in `cas_run` records (`SourceEdgeRecord`) and GC outcomes, and decoded by code
that has **no backend in hand**: `CasInspect`, the ref-protocol benchmark, the golden-pin tests, and the
format tests. `Formats/` depends on `Primitives/` only. Revision 1's "decoding is minting" would have
threaded a `Backend &` through every decoder and reversed that layering.

So decoders do not produce a `Token`. They produce

```
PersistedToken { String type_word; String value; }
```

— unvalidated, renderable, comparable as strings — and the **only** way from it to a `Token` is
`backend.adopt(PersistedToken) -> std::optional<Token>`, which applies the minting rules and requires
the persisted dialect to be the backend's own. There is exactly one adopt site per consumer, at the
boundary where a persisted token meets a conditional operation: GC's redelete of a `RetiredEntry`, the
orphan nomination, and `CasFsck`'s token compare. `nullopt` there means "this record cannot fence
anything": the consumer skips the operation, keeps the entry, and logs it — never `TokenMismatch`,
never `Replaced`, never a drained count.

Two consequences stated plainly. `CasInspect` and the benchmark do not change; they never needed a
`Token`. And a pool whose persisted tokens carry a foreign dialect — copied between stores — goes from
today's "refused at `deleteExact` as a mismatch, blob silently leaked" to "skipped with a warning naming
the record"; the recovery is `rebuildBaseline`, and the warning must say so.

## Read surface {#read-surface}

Three operations. Each is one request, and the type says what it carries.

| operation | returns | request | use when |
|---|---|---|---|
| `readSnapshot(key, max_bytes, size_hint)` | `Snapshot{bytes, token}`, or absent | one `GET` | the caller needs the bytes **and** their identity |
| `openStream(key, size_hint)` | a `ReadBuffer`, or absent | one `GET` | the caller needs bytes and no identity |
| `head(key)` | `std::optional<Identity{size, token, attributes}>` | one `HEAD` | the caller does **not** want the body |

`Snapshot::token` and `Identity::token` are the same unforgeable `Token`, and either is a valid
precondition for a conditional mutation. A `Snapshot` can additionally be compared **by bytes** to
something the caller wrote; an `Identity` cannot. `head` returns an optional because there is no `Token`
to put in an absent result; the `exists` flag goes away, and the ~30 `head(...).exists` sites become
`if (auto id = head(...))`.

**One physical request, enforced.** On S3, `readSmallObjectAndGetObjectMetadata` drains a
`ReadBufferFromS3` and then reads `getObjectMetadataFromTheLastRequest`. `ReadBufferFromS3::nextImpl`
reissues the `GET` from the current offset on a mid-body failure, without `If-Match` and without
comparing ETags, so a failure at byte `k` yields `[0,k)` of one incarnation, `[k,end)` of another, and
the **second** response's token. Azure's buffer has the same shape. That is the mixed pair the reclaim
design calls uncontained, and revision 1 claimed it away "by construction". The reissue loop is gated by
`S3RequestSetting::max_single_read_retries` (`ReadBufferFromS3.cpp:188`, `:217`); `readSnapshot` issues
its read with that setting at 1, so a mid-body failure is an exception and the caller's own retry does a
fresh `readSnapshot`. One physical request — literally, not approximately. The emulated backend reads
and mints under one mutex and is one-request by construction.

**`max_bytes` is a correctness parameter, not only a memory bound.** `copyDataMaxBytes` throws
`CANNOT_READ_ALL_DATA` when the body exceeds it, so a bound smaller than a grown object is a permanent
read failure for that object. The value is `traitsFor(id).object_cap` for a decoded object — every
format read this way has a non-zero cap, from 64 KiB for a ref checkpoint to 256 MiB for a fold seal —
and `bytes.size() + 1` for a byte-compare resolve. `readGcMaintenanceState` classifies only
`CORRUPTED_DATA` as a corrupt object today and must classify the overflow as well.

**`size_hint` stays, for a reason revision 1 missed.** `known_size` did two things. It avoided a
metadata round trip on the ranged path — the part that goes. And through `casSizedReadSettings` it sized
the read buffer to the object: fold and point reads move about 3.7 KiB where `ReadBufferFromS3`'s
default allocation is about 1 MiB (`CAS_FOLD_READ_SLACK_BYTES`), and `S3ObjectStorage::readObject`
sizes the buffer from `remote_fs_settings.buffer_size`, ignoring its `read_hint`. Deleting the `HEAD`
deletes the only exact size the backend had; the caller supplies a hint from the object kind instead, or
the allocation regresses by two orders of magnitude on the hottest reads.

**`probeSentinelRaw` becomes one `GET`.** Today it is `head` then `get`. A `GET` 404 carries a body that
distinguishes `NoSuchKey` from `NoSuchBucket`, which is the classification the probe needs and the
reason the code's own `RESOURCE_NOT_FOUND` comment gives; a `HEAD` 404 does not. One request suffices.

**What goes.** `get` and `GetResult`; the directional-consistency comment
(`CasObjectStorageBackend.cpp:588-609`); `GetStreamResult::token`; the `Range` parameter on reads — no
production caller passes a non-whole range, verified across all sixty `get` sites and both `getStream`
sites; `known_size` as `HEAD`-avoidance. `InstrumentedBackend`'s `CAS*Get` / `CAS*GetStream` profile
events are renamed to the new operations.

**Blobs are not affected, because they were never here.** Column data is read through
`ContentAddressedMetadataStorage::readBlobPayload` → `object_storage->readObject` wrapped in a
`ReadBufferFromFileView` (`ContentAddressedMetadataStorage.cpp:2080`); `MergeTree` seeks by mark,
`ReadBufferFromS3` turns each seek into a ranged `GET`, and the object size is supplied from the
`BlobLocation` in the `StoredObject`, so the buffer never issues its own `HEAD`. Already one request
per window, size known, no token because none is needed.

**The Native-over-Local test convention ends.** `Mode::Native` over `LocalObjectStorage` is test-only
(production chooses `EmulatedSingleProcess` for `Local`), and `LocalObjectStorage` has no
`readSmallObjectAndGetObjectMetadata` override — the base returns empty metadata, so every
`readSnapshot` there would throw. Those fixtures (`gtest_cas_backend`, `gtest_cas_request_control`,
`gtest_cas_probe`, `gtest_cas_sentinel_probe`, `gtest_cas_s3_staging`, `gtest_cas_backend_generation`)
move to `EmulatedSingleProcess`, which mints tokens; the one test that exists to exercise the write-side
`HEAD` fallback (`CASBackendGeneration.StampedTokenTypeFollowsNativeKind`) is rewritten to assert the
anomaly path below.

## Write surface {#write-surface}

`putIfAbsent`, `putOverwrite` and `casPut` keep their signatures. Their result carries a `Token` — not an
optional:

```
Committed { Token token; }
```

**A writable store returns a token for every write, and the probe proves it.** `runCapabilityProbe`
already depends on this structurally: it takes `t1` from `putIfAbsent`'s result (`CasProbe.cpp:59`) and
uses it as the precondition of step 4 (`:115`), then `t2` and `ct1` likewise. A store whose writes carry
no token cannot get past step 4 today; the change makes that refusal explicit — the probe reports "store
returns no write token" and the pool is not writable. S3 in the ETag dialect always carries an ETag, GCS
always carries a generation, `EmulatedSingleProcess` mints one from a stat under its mutex. Azure is
read-only today (no `removeObjectIfTokenMatches` override, no `SingleAttempt` retry profile), so it never
reaches the probe's write steps.

**At runtime, a write response without a token is an anomaly and throws `CORRUPTED_DATA`.** The
fallback `HEAD` in `tokenFromWriteResult` (`CasObjectStorageBackend.cpp:865-866`) is deleted; nothing
replaces it. This is the same rule as a `HEAD` response that cannot identify.

What the throw means to callers, honestly: the write **may have committed** — the store accepted it and
then failed to name it. For the controller that is an ambiguous attempt, which it already resolves by
reading back and comparing bytes (`resolved_by_get`, `CasRequestControl.cpp:678-687`), now on a
one-request `readSnapshot`. For the raw callers — `claimMount`, `MountLeaseKeeper::claim` and
`terminate`, the probe — it is a new exception where today they would have received a possibly-foreign
token. That trade is right: the probe has proven the store does not do this, so the path is a genuine
anomaly, and an ambiguous outcome on an anomaly is the conservative answer.

Because the token is always present, **no consumer needs a read-back path**. Revision 1 gave
`MountLeaseKeeper::last_token` an optional and a read-before-renew, and told `Gc::acquireOrRenewLease`
to return `false` on a missing token. The second was wrong in the dangerous direction: the acquire
**commits** before the token is examined, so a `false` would have left a committed, renewing lease that
blocks every contender and never runs a round — a silent GC wedge. Both paths are unnecessary under this
contract, and neither exists.

The consumers of a write token, all unchanged in shape, `Token` type only: `MountLeaseKeeper::claim`
(two sites), `renew` and `terminate`; `putIfAbsentControlled`, `putOverwriteControlledImpl`,
`putIfAbsentControlledMutable`; `casGcMaintenanceState`; `CasRefCatalog::initializeEmptyForNewPool`;
`runCapabilityProbe` (three); `Gc::acquireOrRenewLease` (acquire, renew, steal), `Gc::run`'s round
commit and `rebuildBaseline`. `CasRefCatalog::Snapshot::token` stays `std::optional<Token>` meaning
*absent catalog*; under revision 1's `Committed{optional}` a present-but-unnamed catalog would have been
indistinguishable from an absent one and mutated with `expected = nullopt` — create-if-absent — forever.
That trap does not exist when the write token is not optional.

**Multipart, stated.** Above `max_single_part_upload_size` a non-GCS conditional write is
`CreateMultipartUpload` + `UploadPart`×n + `CompleteMultipartUpload`, with the condition on the
completion. "One call, one request" is a statement about control objects, which are far below the
threshold; the request-count test includes one body above it so the claim is scoped by a test rather
than by prose.

## Delete surface {#delete-surface}

`deleteExact(key, token)` is unchanged in shape. With no local refusal left — an invalid token cannot be
constructed and a persisted one is adopted or skipped before this call — `TokenMismatch` and `NotFound`
are **only ever the store's answer**, which is what `CasBackend.h:93-95` already says they are. GC's
redelete, which HEADs on `TokenMismatch` and labels the blob `Replaced`, is now acting on evidence.

## Assumptions {#assumptions}

**One.** A token names one incarnation, and the store never reuses a token for **different** content
(`CasBackend.h:226`). What no contract can promise is that identical content never yields the same
token: an S3 ETag is content-derived, so a restored copy of an old body may carry the old ETag, and a
design that recognises its own body by bytes also recognises a restored copy. This defeats
`proven_dead_token` observation just as much; it is stated here once.

Nothing else is assumed. In particular no ordering between two requests is assumed anywhere, because no
operation makes two and none continues across a reissue.

## Test seams, named {#seams}

- `CasProbe` proves enforcement with a deliberately wrong token (`CasProbe.cpp:180`). Under this
  contract it uses the token of the *other* probe object; that is a wrong token only because the two
  bodies differ (`probe-v1` versus `cas-s1`) and ETags are content-derived — the difference is the whole
  guarantee, and the probe comment says so.
- `InMemoryBackend::setEnforceTokens(false)` models a store that accepts every token. It stays; it is
  about the store, not about constructing tokens.
- `Token::forTest` is the one test minter.
- `EmulatedSingleProcess` mints under `emu_mutex`, which is an instance member though the header says
  "process-wide" (`CasObjectStorageBackend.h:39` vs `:210`); one backend per pool makes it latent, and
  the comment is corrected here. Its `emuMintToken` has a nonce fallback for an empty stat etag that
  mints from a counter — a token from no response. Under this contract that path throws like every other
  unmintable response.

## Effects on the designs this supersedes or revises {#effects}

**The exact-token entry guard is superseded entirely**: its three per-site checks and per-site
reactions are the private constructor; its finding that deletion must stay fail-loud is honoured because
no invalid token reaches `deleteExact`.

**Paired reads are `readSnapshot`.** **`[write-token-provenance-not-in-the-api]` is closed**: there is no
provenance to carry when the only source of a write token is the write response.

**The self-authored mount reclaim (revision 5)** loses its directional-read tripwire and its paired-read
prerequisite; its byte-recognition rule stands, and its remaining assumption is the one above. Its
prerequisites section is rewritten against this document once this lands.

## Verification {#verification}

**Compile time.** The two `static_assert`s. `grep` for `Token{` and `Token(` with arguments outside
`Backend/`, `Token::forTest` and the `PersistedToken` decoders returns nothing.

**One call, one request**, against a call-counting object storage, for `readSnapshot`, `openStream`,
`head`, `probeSentinelRaw`, `putIfAbsent`, `putOverwrite`, `casPut` and `deleteExact` — including one
`putOverwrite` body above the multipart threshold, so the scoping in [Write surface](#write-surface)
is asserted rather than asserted-about.

**One physical request under failure.** A fault-injecting object storage that fails the `GET` stream
after `N` bytes of a body larger than one buffer: `readSnapshot` throws; it does not return. Without the
`max_single_read_retries = 1` pin this test observes a second request and a token from it. This is the
test revision 1 could not have written and the one that matters most.

**Minting refuses what it must.** Empty, `*`, `" * "`, a quoted generation → `CORRUPTED_DATA` from
`head` and `readSnapshot`; nothing downstream can be handed a token, so no conditional request follows.

**Adopting refuses what it must.** A `cas_run` record and a GC outcome with an empty token and with a
foreign dialect → `adopt` returns `nullopt`; GC's redelete skips the entry, keeps it, logs it, and
issues no `deleteExact`; `CasInspect` renders both records unchanged.

**The write anomaly.** A store returning no ETag on a write: the probe refuses the pool; on a pool
already mounted, `putOverwrite` throws, the controller resolves by `readSnapshot`, and no call site ever
holds a token it did not receive from a write response.

**Fixtures.** Every former Native-over-Local fixture passes on `EmulatedSingleProcess`.

**Existing gates.** The CA-s3 lane and the `CAS*` gtest gate stay green; `Cas::Probe` passes on every
supported writable store.

## Acceptance {#acceptance}

1. Both `static_assert`s hold; no valued `Token` construction exists outside the minter, the test seam
   and the decoders' `PersistedToken`.
2. `get`, `GetResult`, `GetStreamResult::token`, `Range` on reads, `mintingTypeMatches`, the fallback
   `HEAD`, and `HeadResult::exists` no longer exist; `probeSentinelRaw` is one `GET`.
3. Every backend operation issues exactly one store request under the call-counting test, and
   `readSnapshot` throws rather than reissuing under the fault-injecting test.
4. The probe refuses a writable pool whose writes return no token; a runtime tokenless write throws.
5. `deleteExact` never returns `TokenMismatch` for any reason other than the store's own mismatch, and
   an unadoptable persisted token is skipped and logged, never deleted against or counted.
6. Read-buffer allocation on fold and point reads is unchanged from today (the `size_hint` test).
7. The guard spec is marked superseded and the reclaim design's prerequisites point here.
8. Existing lanes and gates green.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object **renews
a mount lease**. The collision is concrete, and it is not the comment's fault: CAS lives inside
`ReplicatedMergeTree`, whose commit path really does go through ClickHouse Keeper, so an honest
error-handling analysis in this code **must** mention the real Keeper — `CasRequestControl.h:250` does
exactly that when it rules out a collision with `Coordination::Exception` retriability. In a codebase
where "Keeper" sometimes has to mean ClickHouse Keeper, a local object may not borrow the word. The
precise name is `MountLeaseRenewer` (`installRenewer`, `startRenewer`, `admitRenewerCall`,
`MountLeaseRenewerState`).

Sized by the seven identifiers above: 101 occurrences in 6 non-test files, 98 in tests. Mechanical, and
its own commit.

## Out of scope {#out-of-scope}

**An entity-tag grammar for ETag values**, pending a compatibility survey across the S3-compatible
stores in the test matrix.

**Ranged reads through `Backend`.** No production caller needs one. If one appears, the size comes from
the caller and the API refuses to guess.

**`PoolMeta::admitOrValidate`'s unbounded conflict loop** (`CasPoolMeta.cpp:75`) and
**`resolved_by_get` under lockstep clones**: pre-existing, filed, unchanged here.

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

## What revision 1 got wrong {#what-revision-1-got-wrong}

Kept because each error is cheaper to read than to repeat.

It claimed `readSnapshot` pairs bytes and token "by construction" through
`readSmallObjectAndGetObjectMetadata`, when the S3 and Azure read buffers reissue a failed `GET` from the
current offset and report the last response's metadata — the exact mixed pair the reclaim design calls
uncontained. It made the write token optional and told `Gc::acquireOrRenewLease` to return `false`
without one, which would have left a committed, renewing lease that blocks every contender and runs no
round; and it gave the mount lease a read-back path one paragraph earlier, with no reason for treating
the two differently. It routed decoding through the backend, which `CasInspect`, the benchmark and the
format tests do not have, and which `Formats/` does not depend on. It said "loses its public
constructor" while leaving the members public and a default constructor in place, and sized the change
at two sites when about twenty-five production holders default-construct a `Token`. It kept `head`'s
`exists` flag, which has no `Token` to accompany it when false. It missed `probeSentinelRaw`, which is
three requests. It deleted `known_size` as `HEAD`-avoidance without noticing it also sized the read
buffer by two orders of magnitude. And it counted `MountLeaseKeeper::claim` as three requests (two or
four) and the rename as 159 occurrences (101 by the identifiers it named).

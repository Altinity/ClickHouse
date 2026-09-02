---
description: 'Design for a CAS backend API in which callers never hold a token: one opaque Incarnation minted only by the backend, seven operations that map one-to-one onto what object stores offer, one seam for persisted incarnations, and a landing order whose first step closes the clobber today.'
sidebar_label: 'Backend incarnation contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend incarnation contract'
doc_type: 'guide'
---

# CAS backend incarnation contract {#cas-backend-incarnation-contract}

Revision 3. Revisions 1 and 2 kept a `Token` in callers' hands and tried to lock it — a private
constructor, minting rules, a test minter, `adopt`, `static_assert`s — and each review found a new way
the string leaked: forty-odd rules to guard one value. This revision removes the value from the API
instead. [What earlier revisions got wrong](#what-earlier-revisions-got-wrong) keeps the record.

This document supersedes `2026-09-02-cas-empty-conditional-token-guard-design.md` and revises the
prerequisites of `2026-09-01-cas-self-authored-mount-reclaim-design.md`.

## Problem {#problem}

Callers hold a **string** and hand it back to the backend as proof of what they observed. Everything
else follows. Seven review rounds found nine ways to manufacture that string, every one verified in
code:

1. an empty value passes, and `WriteBufferFromS3` attaches `If-Match` only when the value is
   non-empty — so a fenced write becomes an unconditional overwrite;
2. `*` passes and means "any current representation";
3. a quoted generation (`"123"`) is unequal to `123` under `operator==` yet the GCS adapter strips the
   quotes and it commits;
4. a write's token may be the result of a later, unrelated `HEAD` (`tokenFromWriteResult`'s fallback),
   so it can belong to another writer's object — and `Gc::acquireOrRenewLease` stores it and the round
   commit CASes against it;
5. `get` assembles `{bytes, token}` from a `HEAD` and a `GET`, so the pair is consistent only under an
   ordering assumption a caching proxy breaks silently;
6. dialect is a runtime check at three entry points, not a property of the type;
7. the same invalid input fails differently at the write paths (header omitted) and the delete path
   (`SetIfMatch` unconditional, `400`);
8. decoders accept an empty persisted token in any dialect, and it reaches the destructive
   `deleteExact`;
9. `TokenMismatch` is documented as remote evidence that another incarnation is current, yet the
   existing dialect guard returns it for a local refusal — and GC labels the blob `Replaced` and counts
   the prefix drained.

Eight of the nine are ways to make a `Token`. The ninth — same-content replay — is a property of the
store and is the one assumption this contract states.

A second waste rides along. `get` always issues a `HEAD` and a `GET` though a `GET` returns everything
a `HEAD` does plus the body; `MountLeaseKeeper::claim` pays two requests on its mint path and four on
its adopt path; `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by
nobody; `probeSentinelRaw` is a `HEAD` then a `GET`.

## The idea {#the-idea}

**Callers never hold a token.** They hold what the store told them — an *incarnation* — and they can
only get one by performing an operation. There is nothing to construct, so nothing to construct wrongly.

The store offers exactly this: an object version (ETag on S3, generation on GCS), and conditional
operations on it — `GET`, `HEAD`, `PUT` with `If-Match` or `If-None-Match`, `DELETE` with `If-Match`.
The API is that, and not one word more.

## One type {#one-type}

```cpp
class Incarnation;   // opaque; operator==; created only by a Backend; no default constructor
```

The word already lives in the code ("incarnation token", the `MountLease` documentation). It is not
called `Token`, because it is not something the caller *sends*; it is something the caller *saw*.

Two assertions make the property checkable at compile time:

```cpp
static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
```

Backends mint through one protected path. Minting applies the rules a value must meet to name one
incarnation — and these are the backend parsing **its own responses**, not a contract with callers:

| dialect | a response value is an incarnation iff |
|---|---|
| Generation | canonical decimal: non-empty, digits only, no leading zero unless the value is `0` |
| ETag | non-empty; not `*` after trimming whitespace; no comma — a list matches any member, and no known store puts a comma in a single entity tag |
| Emulated | non-empty |

A response value that fails is not an incarnation; the operation that received it throws
`CORRUPTED_DATA` naming the key. No fallback, no second lookup.

"Not yet" is spelled `std::optional<Incarnation>`. Roughly twenty-five production sites that
default-construct a `Token` today become optionals, are constructed from an operation, or are
restructured; the implementation plan carries the list from the review of revision 1.

## Seven operations {#seven-operations}

```cpp
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; ObjectAttributes attributes; };

std::optional<Object>      read(key, size_t cap);       // one GET; bytes never from two incarnations
std::optional<Meta>        head(key);                   // one HEAD
std::unique_ptr<ReadBuffer> stream(key);                // bytes only, no incarnation

std::expected<Incarnation, Conflict>
                           write(key, bytes, std::optional<Incarnation> expected);
                                                        // nullopt = the key must be absent
enum class Removal { Removed, Gone, Mismatch };
Removal                    remove(key, Incarnation expected);

Incarnation                publish(key, BlobPublishRequest);   // unconditional; content-addressed keys only
ListPage                   list(prefix, cursor, limit);
```

**`read`** is the only source of an `Object`. **`head`** is for callers that do not want the body:
existence, size, and an incarnation to delete without reading — the one legitimate `HEAD`. **`stream`**
carries no incarnation because a lazily-read stream cannot promise one: it may reissue.

**`write`** is one operation with the precondition as a parameter, so the condition is visible at every
call site: `write(k, b, std::nullopt)` claims to be first; `write(k, b, seen)` changes what it saw.
`optional<Incarnation>` is today's `casPut` signature. The result is `std::expected` — used in
thirty-six files under `src/` already — so success yields the incarnation of what was written and
`Conflict` yields nothing: an incarnation cannot be extracted from a failed write. Today's
`PutResult{PreconditionFailed, {}}` was exactly that extraction, and it is how empty tokens entered
circulation. `Conflict` means one thing in both forms: the store said the precondition does not hold.
Ambiguity — the store did not answer, the write may have landed — is an exception, as today.

`putIfAbsent`, `putOverwrite` and `casPut` collapse into `write`; three outcome enums collapse into
`expected` and `Removal`.

**`remove`** is conditional only. There is no unconditional delete in the API because there are no
unconditional deletes in CAS — all fourteen `deleteExact` callers are conditional — and "delete whatever
is there" has no safe meaning when a republished blob under the same key is a legitimate live object.

**`publish`** is the one unconditional mutation, as `publishBlob` is today. It is a separate verb, not
a third state of `write`'s precondition, because its safety argument is different — a content-addressed
key has one possible content, so an unconditional `PUT` is idempotent — and a separate verb lets a
reviewer find every unconditional site by name. It returns the incarnation the store named, for the same
reason `write` does.

### Why not `Snapshot`, `Identity`, `Precondition` {#why-not-more-types}

An intermediate draft had five nominal types around one value. `Object` and `Meta` are plain results —
a struct with bytes has bytes to compare, a struct without does not, and no type is needed to say so.
`optional<Incarnation>` as precondition is the existing idiom. A `variant<Absent, Incarnation>` would
be a wrapper around the same two states.

## Three things that stay hard {#three-hard-things}

Each is irreducible, and the design's job is to give it one place.

### GC must remember which publication it condemned {#persisted-incarnations}

A blob is addressed by content hash, so a republished blob is **byte-identical** to the condemned one.
The only thing that distinguishes them is which publication — the incarnation. GC therefore persists
it in `cas_run` (`SourceEdgeRecord`) and GC outcomes.

Persisted, it is a `String` — not a `Token`, not a `PersistedIdentity`, a string. Decoders keep it a
string; `CasInspect` and the benchmark render strings and need no backend; `Formats/` keeps depending on
`Primitives/` only. There is exactly one way from a string to an `Incarnation`:

```cpp
std::optional<Incarnation> parse(String persisted);   // nullopt: this cannot fence anything
```

It is called in **one** place — the fold's intake, where a `CondemnedRow` becomes a `RetiredEntry` and
the backend is already in hand for the supersession `HEAD`. It must be there and not at `remove`,
because `foldDeltasIntoGeneration` moves a `delete_pending` entry into `redelete` and drops it from
`still_retired` **before** the delete phase; a refusal at `remove` is too late to keep the entry. An
unparseable row is carried as retained-unadoptable, republished into the successor run so it is not
lost, and logged once per round. Recovery is a follow-up
(`[gc-unadoptable-incarnation-recovery]`) — not `rebuildBaseline`, which condemns nothing by design.
`remove` never sees an invalid incarnation, so `Mismatch` means only what the store said.

Orphan nomination is not a persisted consumer: its incarnation comes from the same live `get` that
supplied the body. `CasFsck`'s compare of a `head` against a persisted string goes through `parse` as
well — the second and last call site.

### `read` cannot forbid a second request, but it can notice one {#read-drift}

`ReadBufferFromS3` reissues the `GET` from the current offset on a mid-body failure, without `If-Match`
and without comparing ETags, and `getObjectMetadataFromTheLastRequest` reports the **last** response —
so a failure at byte `k` yields `[0,k)` of one incarnation, `[k,end)` of another, and the second's
token. Per-request retry policy cannot be pushed through `readSmallObjectAndGetObjectMetadata`: the S3
request settings come from the disk, and `read_hint` is ignored. Azure's buffer has the same shape and
additionally reinitialises on credential refresh.

So `read` **detects** rather than prevents. It owns the small-object read itself — the same
`dynamic_cast` to the concrete buffer that `S3ObjectStorage::readSmallObjectAndGetObjectMetadata` does —
and asks the buffer for its response metadata **twice**: after the first chunk and after draining. A
different ETag means a reissue straddled a replacement, and `read` throws `CORRUPTED_DATA`. The same
ETag means, by the store's own token-implies-content contract (`CasBackend.h:226`), that every byte came
from one incarnation, however many requests the buffer made. The check lives in one function because
`Object` is born in one function.

The promise is stated as what is true: **`read` never returns bytes from two incarnations.** Not "one
request". `readSmallObjectAndGetObjectMetadata` violates its own "consistent metadata" contract under
retry for every caller, not only CAS; that is filed upstream and this design does not wait for it.

`cap` bounds **stored** bytes. `FormatTraits::object_cap` is a decompressed cap, and a zstd frame of an
incompressible body at the cap is slightly larger than the cap, so the bound is
`ZSTD_compressBound(object_cap)` for `CompressionPolicy::Always` formats and `object_cap` otherwise;
overflow is `CANNOT_READ_ALL_DATA`, and `readGcMaintenanceState` must classify it alongside
`CORRUPTED_DATA`. `read` allocates a fixed 64 KiB buffer: the buffer is the chunk size of one `GET`,
not a request count, and 64 KiB is sixteen times smaller than the 1 MiB default the current
`casSizedReadSettings` exists to avoid, without needing the exact size a `HEAD` used to supply.

### The store may not name what it wrote {#write-anomaly}

`write` and `publish` must return an `Incarnation`; a response without one has nothing to return.
`runCapabilityProbe` already depends on write tokens structurally — `t1` from step 3 is the
precondition of step 4 — so it refuses a writable pool whose writes carry none, and says so. S3 in the
ETag dialect always carries an ETag, GCS always a generation, `EmulatedSingleProcess` mints from a stat
under its mutex; Azure is read-only today.

At runtime, then, a nameless write response is a store anomaly — and the write **may have committed**.
It throws a dedicated code, `CAS_WRITE_UNATTRIBUTED`, which `classifyConditionalWriteResult` classifies
as **ambiguous**, so every controller method resolves it by reading back and comparing bytes
(`resolved_by_get`, now on `read`). Revision 2 used `CORRUPTED_DATA` here; `isDeterministicLocalFailure`
lists that code as deterministic, so three of the four controller methods would have rethrown it before
resolving. For raw callers outside the controller an exception after a possible commit is the same
class as a network failure after a commit — pre-existing, and handled where it is handled today; the
one place it can duplicate data, the read-then-freeze append in `ContentAddressedTransaction::writeFile`,
is filed as its own item because it is not new.

## What is promised about requests {#request-promises}

Only what the backend does itself. **No operation issues a `HEAD` it does not need**: `read`, `stream`
and `probeSentinelRaw` issue no `HEAD`; `head` issues exactly one; `probeSentinelRaw` becomes one `GET`,
whose 404 body distinguishes `NoSuchKey` from `NoSuchBucket` where a `HEAD` 404 cannot. That is
checkable at the `IObjectStorage` call boundary and is the whole of the claim.

Not promised, and stated so it is not inferred: multipart writes above `max_single_part_upload_size`
are several requests; `list` prefetches the next page; `stream` is lazy and may issue several; the S3
client has its own redirect and credential loops.

## Assumptions {#assumptions}

**One.** An incarnation names one version, and the store never reuses one for **different** content.
What no contract can promise is that identical content never yields the same incarnation: an S3 ETag is
content-derived, so a restored copy of an old body may carry the old ETag, and a design that recognises
its own body by bytes also recognises a restored copy. This defeats `proven_dead_token` observation just
as much; it is stated here once.

## Seams, named {#seams}

- **Tests** obtain incarnations by performing operations on `InMemoryBackend`. There is no test minter.
  A "wrong" incarnation for a key is that key's **previous** one after an overwrite — provably stale for
  that key — not another key's, since preconditions are key-scoped and generation values are not
  globally unique. `CasProbe` uses `t1` after overwriting to `t2`.
- `InMemoryBackend::setEnforceTokens(false)` models a store that accepts every precondition. It stays;
  it is about the store.
- `EmulatedSingleProcess` mints under `emu_mutex`, an instance member though its header says
  "process-wide" (`CasObjectStorageBackend.h:39` vs `:210`); the comment is corrected. Its
  `emuMintToken` nonce fallback for an empty stat etag — an incarnation from no response — is removed;
  that path throws.
- Test fixtures that run `Mode::Native` over `LocalObjectStorage` for convenience move to
  `EmulatedSingleProcess`. Fixtures that exist to exercise Native fakes — the fake generation store in
  `gtest_cas_s3_staging`, the S3 error-classification fakes in `gtest_cas_sentinel_probe` — stay
  Native with fakes that implement the small-object read.

## Landing order {#landing-order}

The diff is about 250 sites: 60 `get`, ~30 `head`, 29 writes, 14 deletes, ~25 default-constructed
holders, 3 backend implementations and ~30 test subclasses, 4 codecs, 55 test constructions in 14
files. That size is not a cost of the design; it is how the design works — an unconstructible type makes
the compiler find every place a string was passed off as an observation. It lands in four steps, each
green:

1. **Safety, inside the backend, small.** The two defects that produce a clobber today — an empty value
   reaching `If-Match`, and the fallback `HEAD` in `tokenFromWriteResult` — are fixed in
   `ObjectStorageBackend` with no caller change: refuse an empty or wildcard value at the wire on the
   write paths, throw on the delete path (which is fail-loud today and must stay so — a refusal outcome
   there is read by GC as evidence), and replace the fallback with `CAS_WRITE_UNATTRIBUTED`. About
   fifty lines. After this step the clobber is impossible, before any of the API exists.
2. **New operations beside the old.** `read`, `head`, `stream`, `write`, `remove`, `publish` are added
   to `Backend`; the old operations delegate. Nothing breaks.
3. **Migration, file by file**, each its own commit: the 60 `get` sites first — the largest and most
   mechanical block, with the `read`-or-`stream` decision already tabulated per site by the review of
   revision 1 — then writes, then deletes with `parse` at the fold's intake. Mechanical work, delegated
   and reviewed.
4. **The lock.** The old operations are deleted, `Incarnation` loses every public constructor, the
   `static_assert`s go in. The compiler lists what was missed.

Stated plainly: unconstructibility is a property of step 4. Until then the new API is additive and the
safety rests on step 1 — which is what is needed immediately; steps 2–4 are what makes the next such
defect impossible to write.

## Verification {#verification}

**Compile time.** The two `static_assert`s; `grep` for `Incarnation{` and `Incarnation(` with arguments
outside the backend minter returns nothing.

**No needless `HEAD`.** Against a call-counting `IObjectStorage`: `read`, `stream`, `probeSentinelRaw`
issue zero `getObjectMetadata` calls; `head` issues one.

**`read` never mixes.** A fake `IObjectStorage` whose read buffer yields `N` bytes of one body with one
ETag, then fails, then serves a different body with a different ETag on reissue: `read` throws. Same
body, same ETag on reissue: `read` returns it. This is the test that could not have been written under
revision 2's "one request" framing.

**Minting refuses.** Empty, `*`, a comma list, `00123` → `CORRUPTED_DATA` from `head` and `read`.

**`parse` refuses.** A `cas_run` row and a GC outcome with an empty or foreign-dialect string: the fold
carries the entry as unadoptable, republishes it, issues no `remove`, logs once; `CasInspect` renders
both unchanged.

**The write anomaly.** A store returning no ETag on write: the probe refuses the pool; on an already
mounted pool `write` throws `CAS_WRITE_UNATTRIBUTED`, and all four controller methods resolve it by
`read`.

**`expected` cannot leak.** A `Conflict` carries no incarnation — a compile-time property of the type.

**Step 1 in isolation.** Before any API exists: on a real S3-compatible store, a conditional overwrite
with an empty expected value leaves the object unchanged; a delete with one throws; a write whose
response lacks an ETag (fake) throws `CAS_WRITE_UNATTRIBUTED`.

**Existing gates.** The CA-s3 lane and the `CAS*` gtest gate stay green at every step.

## Acceptance {#acceptance}

1. Step 1 lands and its isolated tests pass before any API work.
2. After step 4: both `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`,
   `casPut`, `deleteExact`, `Token`, `mintingTypeMatches` and the fallback `HEAD` no longer exist.
3. No needless `HEAD`; `read` throws on drift; `parse` is called from exactly two sites.
4. `remove` never reports `Mismatch` for any reason other than the store's own mismatch.
5. The probe refuses a nameless-write store; a runtime nameless write is resolved by every controller
   method.
6. Existing lanes and gates green at every step.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object renews
a mount lease. The collision is real and not the comment's fault — CAS lives inside
`ReplicatedMergeTree`, whose commit path goes through ClickHouse Keeper, so `CasRequestControl.h:250`
must name the real Keeper when it rules out a `Coordination::Exception` collision. In a codebase where
"Keeper" sometimes has to mean ClickHouse Keeper, a local object may not borrow the word. The name is
`MountLeaseRenewer`. Sized by those seven identifiers: 101 occurrences in 6 non-test files, 98 in
tests. Mechanical, its own commit.

## Out of scope {#out-of-scope}

**Upstream:** `readSmallObjectAndGetObjectMetadata` consistent under retry; a per-request read retry
policy in `IObjectStorage`. Filed; not waited for.

**Pre-existing, filed:** `PoolMeta::admitOrValidate`'s unbounded conflict loop; the read-then-freeze
append's duplicate on post-commit exception; `resolved_by_get` under lockstep clones; recovery for an
unadoptable persisted incarnation.

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

**Revisions 1 and 2** kept `Token` in the API and locked it: private constructor, minting rules, a
`Token::forTest` minter, `PersistedToken` and `adopt`, `static_assert`s. Each review found a leak the
previous lock had not closed — public members, a default constructor, a public test minter, a decoder
without a backend — because a value that callers hold has to be defended everywhere it is held.
Revision 2 also promised "one physical request", which no `IObjectStorage` interface can deliver, and
told `Gc::acquireOrRenewLease` to step down on a missing token when its acquire had already committed —
a silent wedge. Its `CORRUPTED_DATA` on a nameless write was classified deterministic and rethrown by
three of four controller methods. Its `object_cap` bound measured decompressed bytes against stored
ones. Its adopt point sat after the fold had already dropped the entry.

**An intermediate draft** replaced `Token` with `Snapshot`, `Identity`, `PersistedIdentity`,
`Precondition` and `Absent` — five names around one value, and a `create` whose conditional nature was
in a comment. The user's objection — "a wrapper over a wrapper" — was correct.

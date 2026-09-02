---
description: 'Design for a CAS backend API in which callers never hold a token: one opaque, key-bound Incarnation minted only by the backend, seven operations that map one-to-one onto what object stores offer, no string-to-incarnation path at all, a twenty-line upstream slice that makes one-GET reads honest on S3 and GCS, and a landing order whose first step closes the clobber today.'
sidebar_label: 'Backend incarnation contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend incarnation contract'
doc_type: 'guide'
---

# CAS backend incarnation contract {#cas-backend-incarnation-contract}

Revision 4. Revision 3 was reviewed twice, independently, and rejected on the same three points by
both; [what earlier revisions got wrong](#what-earlier-revisions-got-wrong) keeps the record. This
document supersedes `2026-09-02-cas-empty-conditional-token-guard-design.md` and revises the
prerequisites of `2026-09-01-cas-self-authored-mount-reclaim-design.md`.

## Problem {#problem}

Callers hold a **string** and hand it back to the backend as proof of what they observed. Everything
else follows. Eight review rounds found nine ways to manufacture that string, every one verified in
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
8. decoders accept an empty persisted token in any dialect, and a `RetiredEntry::token` reaches the
   destructive `deleteExact`;
9. `TokenMismatch` is documented as remote evidence that another incarnation is current, yet the
   existing dialect guard returns it for a local refusal — and GC labels the blob `Replaced` and counts
   the prefix drained.

Two more, found while reviewing the fix: a genuine observation of key `a` can be applied to key `b` —
identical bytes give identical ETags on S3, and nothing binds the value to the object it came from;
and the persisted representation's dialect tag exists precisely because `"123"` is a valid ETag, a
valid emulated mtime and a valid generation at once, so a dialect-free string can never be classified.

Eight of the nine are ways to make a `Token`; the two additions are ways to misapply a real one. The
one remaining item — same-content replay — is a property of the store and is the assumption this
contract states.

A second waste rides along. `get` always issues a `HEAD` and a `GET` though a `GET` returns everything
a `HEAD` does plus the body; `MountLeaseKeeper::claim` pays two requests on its mint path and four on
its adopt path; `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by
nobody; `probeSentinelRaw` is a raw `HEAD`, then `get`'s `HEAD`, then a `GET`.

## The idea {#the-idea}

**Callers never hold a token.** They hold what the store told them about one object — an
*incarnation* — and they can only get one by performing an operation on that object. There is nothing
to construct, and nothing to apply to a different key.

The store offers exactly this: an object version (ETag on S3, generation on GCS) and conditional
operations on it — `GET`, `HEAD`, `PUT` with `If-Match` or `If-None-Match`, `DELETE` with `If-Match`.
The API is that, and not one word more.

## One type {#one-type}

```cpp
class Incarnation;   // opaque; operator==; created only by a Backend; no default constructor
                     // privately: {backend identity, key, dialect, value}; render() for logs and compares
```

The word already lives in the code ("incarnation token", the `MountLease` documentation). It is not
called `Token`, because it is not something the caller *sends*; it is something the caller *saw* — and
saw **of one key, from one backend**. A conditional operation given an incarnation of another key, or
minted by another backend, throws `LOGICAL_ERROR`: a programming error, not a store answer, so it does
not reintroduce problem 9. "Backend identity" is a per-instance id from a static counter at
construction — not the address, which is reused after destruction — and the check lives in the
**minting** backend's operations; `InstrumentedBackend` mints nothing and forwards without checking,
so an incarnation's identity is its inner backend's. One constraint follows: a cache that holds an
`Incarnation` (`CasManifestReader`'s, `readShardDecoded`'s) must not outlive the backend that minted
it; today both are owned by the `Pool` that owns the backend.

Two assertions make unconstructibility checkable at compile time; the key binding is checked by the
operations:

```cpp
static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
```

**Opaque means not constructible, not unreadable.** `render()` yields `dialect:value` for
`system.content_addressed_log`, `CasInspect`, and the one comparison in
[persisted incarnations](#persisted-incarnations). There is no inverse.

Backends mint through one protected path. Minting applies the rules a value must meet to name one
incarnation — the backend parsing **its own responses**, not a contract with callers:

| dialect | a response value is an incarnation iff |
|---|---|
| Generation | canonical positive decimal **after the SDK ETag-field quote strip** (`normalizeTokenValue` stays; the adapter installs the generation quoted): digits only, no leading zero, not `0` — zero is the dialect's own absence sentinel (`If-None-Match: *` maps to `x-goog-if-generation-match: 0`) |
| ETag | non-empty; not `*` after trimming whitespace; no comma — a list matches any member, and no known store puts a comma in a single entity tag |
| Emulated | non-empty |

A response value that fails is not an incarnation; the operation that received it throws
`CORRUPTED_DATA` naming the key. No fallback, no second lookup. `list` mints through the same path
where it surfaces per-key tokens (`tokenForList`, `supportsListTokens`), and is the fourth producer
after `read`, `head` and `write`.

Because the backend mints only its own dialect, `mintingTypeMatches` and its three call sites are
deleted. "Not yet" is spelled `std::optional<Incarnation>`; the twenty-six production sites that
default-construct a `Token` today become optionals, are constructed from an operation, or are
restructured — the reviews of revisions 1 and 3 list them.

## Seven operations {#seven-operations}

```cpp
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };

std::optional<Object>       read(key);            // one GET; bytes never from two incarnations
std::optional<Meta>         head(key);            // one HEAD
std::unique_ptr<ReadBuffer> stream(key);          // bytes only, no incarnation

std::expected<Incarnation, Conflict>
                            write(key, bytes, std::optional<Incarnation> expected);
                                                  // nullopt = the key must be absent
enum class Removal { Removed, Gone, Mismatch };
Removal                     remove(key, Incarnation expected);   // throws if the store archived instead

void                        publish(BlobPublishRequest);          // unconditional; content-addressed keys only
ListPage                    list(prefix, cursor, limit);
```

Plus `probeSentinelRaw`, the bootstrap classification probe, kept and reduced to one `GET`.

**`read`** is the only source of an `Object`. It is `IObjectStorage::readSmallObjectAndGetObjectMetadata`
under a native-conditional request mode, once the [upstream slice](#upstream-slice) makes that
function honest. It takes no per-call bound: one backend-wide **stored**-byte bound —
`ZSTD_compressBound` of the largest control cap, computed once beside the caps table — protects memory
against a rogue object and can be wrong in exactly one place, and the decoders already enforce each
format's `object_cap` on decode. The cap itself would be one place wrong: `object_cap` bounds
*decompressed* bytes, and an incompressible `PartManifest` or `GcOutcomes` at the cap is stored at up
to `ZSTD_compressBound(256 MiB)`, about 258 MiB, which a 256 MiB stored bound would refuse forever. Eighty call sites each computing
`ZSTD_compressBound(object_cap)` would be a surface where a bound one byte short is a permanent read
failure for one object. `read` allocates a fixed 64 KiB buffer: chunk size of one `GET`, not a
request count — eight times today's sized allocation for a 3.7 KiB fold read, sixteen times less than
the unsized 1 MiB default, and needing no `HEAD` to size.

**`head`** is for callers that do not want the body: existence, size, and an incarnation to delete
without reading — the one legitimate `HEAD`. `Meta` drops `attributes`: no production reader exists
outside the backends' own copying, no production write passes a non-empty `ObjectMeta`, and
`cas_owner` has no reader or writer as an object attribute anywhere under `ContentAddressed/` (the
string survives only as the `FormatId::Owner` text header). `ObjectMeta` on writes goes
with it.

**`stream`** carries no incarnation because a lazily-read stream cannot promise one: it may reissue.

**`write`** is one operation with the precondition as a parameter, so the condition is visible at every
call site: `write(k, b, std::nullopt)` claims to be first; `write(k, b, seen)` changes what it saw.
`optional<Incarnation>` is today's `casPut` signature. The result is `std::expected` — used in
thirty-six files under `src/` already — so success yields the incarnation of what was written and
`Conflict` yields nothing: an incarnation cannot be extracted from a failed write. Today's
`PutResult{PreconditionFailed, {}}` was exactly that extraction, and it is how empty tokens entered
circulation. `Conflict` means one thing in both forms: the store said the precondition does not hold.
Ambiguity — the store did not answer, the write may have landed — is an exception, as today.
`putIfAbsent`, `putOverwrite` and `casPut` collapse into `write`; three outcome enums into `expected`
and `Removal`.

**`remove`** is conditional only. There is no unconditional delete in the API because there is none in
CAS — all seventeen `deleteExact` call sites are conditional — and "delete whatever is there" has no safe
meaning when a republished blob under the same key is a legitimate live object. When the store
answers with a delete marker instead of a removal (versioning enabled — a mis-provisioned pool),
`remove` throws: the capability probe's step 8 refuses the pool on that throw, and GC's redelete
guard becomes unnecessary. No fourth `Removal` state.

**`publish`** is the one unconditional mutation **in this API**, as `publishBlob` is today. It is a
separate verb, not a third state of `write`'s precondition, because its safety argument is different
— a content-addressed key has one payload, so an unconditional `PUT` is safe whoever wins — and a
separate verb lets a reviewer find every unconditional site by name. It returns `void`: the staged
copy transport goes through `IObjectStorage::copyObject`, which returns nothing; the emulated transport
is a rename; and no consumer of a publication's incarnation exists. Three unconditional mutations
live **outside** this API by design — the S3 staging upload, the staging cleanup and
`sweepOwnMountStaging` — on mount-owned staging keys that carry no incarnation and are never read
through `read`; the contract does not cover them and says so.

### Why not `Snapshot`, `Identity`, `Precondition`, `parse` {#why-not-more}

An intermediate draft had five nominal types around one value. `Object` and `Meta` are plain results:
a struct with bytes has bytes to compare, a struct without does not, and no type is needed to say so.
`optional<Incarnation>` as precondition is the existing idiom. And revision 3's
`parse(String) -> optional<Incarnation>` — the one string-to-incarnation door — is gone entirely;
[persisted incarnations](#persisted-incarnations) says what replaced it.

## The upstream slice {#upstream-slice}

Two facts make a one-`GET` read dishonest today, and both are fixed in about thirty additive lines
across six files under `src/IO` and `src/Disks/DiskObjectStorage/ObjectStorages/S3` — a new five-line
header, the two settings headers, the S3 read buffer's header and source, and the S3 object storage. The change is
generic — it makes `readSmallObjectAndGetObjectMetadata` keep its own word for every caller — and
carries no CAS vocabulary, so it ports between branches on its own.

**Fact one: a plain `GET` never carries a GCS generation.** The generation reaches the SDK's ETag
field only through `applyGcsConditionalDialectToResponse`, which `PocoHTTPClient` invokes only
`if (isNativeConditionalRequest(request))`; `ReadBufferFromS3::sendRequest` builds a plain
`GetObjectRequest` and never marks it, and a test pins that behaviour
(`gtest_aws_s3_client.cpp:1364`). Today's `get` works on GCS only because its token comes from
`nativeHead`, which does mark its request. (That test pins the response-side gate with a hand-marked
`HEAD`; no existing test covers a marked `GET`, so the slice's test is new.) On the request side the
dialect adapter is a no-op for a conditionless `GET`; the OAuth client strips five AWS signing headers
from a marked request before setting its Bearer header, as it does for the marked `HEAD` today — no
header the store reads changes. The fix is the same one line `getObjectInfo.cpp:42` already
uses for `HEAD`:

- `ObjectStorageRequestMode` (today in `WriteSettings.h:35`) moves to a five-line
  `IO/ObjectStorageRequestMode.h` included by both settings headers;
- `ReadSettings` gains `object_storage_request_mode = Default` — one field, one default, no
  existing reader changes; `ReadBufferFromS3` already holds `read_settings` as a member, so no
  constructor changes;
- `ReadBufferFromS3::sendRequest` adds
  `req.setNativeConditional(read_settings.object_storage_request_mode == NativeConditional)`.

**Fact two: a reissued `GET` can straddle a replacement.** `ReadBufferFromS3::nextImpl` reissues from
the current offset on a mid-body failure, without `If-Match` and without comparing ETags, and
`getObjectMetadataFromTheLastRequest` reports the last response — so a failure at byte `k` can yield
`[0,k)` of one incarnation, `[k,end)` of another, and the second's ETag. `readSmallObjectAndGetObjectMetadata`
promises "consistent metadata" and cannot keep it. The fix:

- `ReadBufferFromS3::initialize` records the first response's ETag and, on **every** reissue, sets
  `response_identity_changed` if the new response's differs — per reissue, so an `A→B→A′` sequence is
  caught at `B`; six lines, two members, one getter, and no change to what any reader returns.
  `initialize` is the only response source on the `next()` path that `copyDataMaxBytes` drives
  (`seek` and the until-position setters re-enter it); `readBigAt` obtains responses separately and
  `read` never uses it;
- `S3ObjectStorage::readSmallObjectAndGetObjectMetadata` throws after draining if the flag is set —
  three lines.

That function is overridable per storage, which is also the seam the Native test fakes use: they
override it, and nothing in CAS casts a buffer. Azure's buffer has the same shape and its own override;
it is not a CAS store today and is out of this slice.

Two shapes that look simpler than this slice were checked and do not hold, recorded so they are not
re-derived. Marking every `GET` on GCS clients unconditionally — no `ReadSettings` field, no header
move — would put the generation into the ETag that the Iceberg version-hint writer (`Iceberg/Utils.cpp`)
copies into an **unmarked** `If-Match`, breaking Iceberg over GCS through the same S3 client; the mode
gate is necessary. Throwing inside `ReadBufferFromS3::initialize` on an ETag change — no flag, no
getter — would be caught by `nextImpl`'s own `catch (...)`, retried `max_single_read_retries` times
with backoff, and would need `processException` taught a new exception: more lines and worse
behaviour. The flag checked after the drain is the minimum.

Under the project's rule for shared surfaces this slice is consulted before it is written; the user
has approved the direction.

## Persisted incarnations {#persisted-incarnations}

GC must remember which publication it condemned. A republished blob is *payload*-identical to the
condemned one but not *byte*-identical — the envelope carries a fresh random `incarnation_tag` per
upload attempt — and that difference is exactly what makes a content-derived ETag differ between
publications. Either way, the only thing GC can hold across rounds is the incarnation it saw, so it
persists one in `cas_run` (`SourceEdgeRecord`) and GC outcomes, as the two strings the format already
carries: `token_type` and `token`.

**There is no way from those strings to an `Incarnation`.** Not `parse`, not `adopt`, nothing. The
persisted pair is compared, as text, against the rendering of a **live** observation of the same key:

```
auto meta = head(blob_key);                       // one HEAD
if (meta && meta->incarnation.render() == persisted)   // "dialect:value" == "dialect:value"
    remove(blob_key, meta->incarnation);
```

The incarnation handed to `remove` came from `head`, of that key, from this backend — every guarantee
of the type holds, and the persisted string never became one. A dialect-tagged string cannot be
misread across dialects, because the rendering carries the dialect too. A stale or garbage row simply
fails to match: GC records `Replaced` — justified this time by a live `HEAD`, remote evidence — keeps
the meta as today, and drops the row. A blob that was touched in the window is re-condemned by the
supersession path that already exists; a blob republished and then abandoned, with no edge, is
reclaimed on the next publication of that content, which the still-`Condemned` meta forces — the
same as today's `TokenMismatch` path, no better and no worse. Nothing is retained in a special
state, `CasInspect` renders strings as strings and needs no backend, and `Formats/` keeps depending
on `Primitives/` only.

This is the shape of the other nine `head`-then-`remove` sites in the tree. Its cost is one `HEAD` per
condemned-blob delete where today there is one only on a mismatch; GC's delete phase is background and
bounded by the round budget. The internal review of revision 3 recommended this trade; the codex review's minimum kept a
`parse(key, {dialect, value})` door plus a retained-unadoptable variant. This is the stricter of the
two, and it is taken.

Four sites compare a persisted token with a live one and all become render compares: GC's redelete
and `CasFsck`'s retirement check, which then `remove` with the head's incarnation; and the fold's
supersede compare in `closeBlob` and the meta writer's condemn-marker confirmation registry, which
compare and never remove. Orphan nomination is not one: its incarnation comes from the same live
`get` that supplied the body. No persisted token reaches `write` anywhere.

## The one hard thing left {#write-anomaly}

`write` must return an `Incarnation`; a response without one has nothing to return.
`runCapabilityProbe` already depends on write tokens — `t1` from step 1 is the precondition of step 4
— and refuses a nameless-write store today only by accident — when the `HEAD` is nameless too, `t2 == t1`
with both empty; a store that names `HEAD`s but not `PUT`s passes, because the fallback fills both from
`nativeHead`. It will refuse it by name. S3 in the ETag dialect always carries an ETag, GCS always a generation once the request is
marked, `EmulatedSingleProcess` mints from a stat under its mutex; Azure is read-only today.

At runtime a nameless write response is therefore a store anomaly — and the write **may have
committed**. It throws `CAS_WRITE_UNATTRIBUTED`, a code that `classifyConditionalWriteResult` treats
as ambiguous by default (every non-S3 exception is, provided the code is kept out of
`isDeterministicLocalFailure`), so all four controller methods resolve it by reading back — three by
comparing bytes, `slotOccupy` by returning `Occupied` for its caller to adjudicate. The fallback
`HEAD` in `tokenFromWriteResult` is deleted; nothing replaces it. For the thirty raw callers outside
the controller, both reviews traced every one: each either propagates to a caller whose next attempt
re-reads the key, or catches and resolves by read; none reads an exception as "not committed". It is
a new trigger on a path that today returns *something* — and what it returns today can be another
writer's incarnation, which is the defect. The one place a post-commit exception can duplicate data,
the read-then-freeze append in `ContentAddressedTransaction::writeFile`, is pre-existing and filed.

## What is promised about requests {#request-promises}

Only what the backend does itself. **No operation issues a `HEAD` it does not need**: `read`, `stream`
and `probeSentinelRaw` issue none; `head` issues one. Checkable at the `IObjectStorage` boundary.

**Reading an object's metadata is one `GET`, at the caller level too.** The two callers that do `head`
then `get` on one key today collapse: `MountLeaseKeeper::claim` from two and four requests to two and
two (`read`, then `write`); `CasManifestReader` to zero on a cache hit and one on a miss, which is its
own bounded change (`2026-09-02-cas-manifest-cache-by-id-design.md`) and lands first. Every other
`head`/`get` adjacency is two different keys, or a `head` before a bodiless `write`.

Not promised: multipart writes above `max_single_part_upload_size` are several requests; `list`
prefetches the next page; `stream` is lazy; the S3 client has its own redirect and credential loops.

## Assumptions {#assumptions}

**One, and it is assumed, not probed.** An incarnation names one version, and the store never reuses
one for **different** content. `CasBackend.h:234-236` says the capability probe does not test this;
it is a requirement on every backend. What no contract can promise is that identical content never
yields the same incarnation — a restored copy of an old body may carry the old ETag — and a design
that recognises its own body by bytes also recognises a restored copy. This defeats
`proven_dead_token` observation just as much; it is stated here once.

## Seams, named {#seams}

- **Tests** obtain incarnations by performing operations on `InMemoryBackend`; a "wrong" one for a key
  is that key's **previous** incarnation after an overwrite — provably stale, and bound to the same key,
  which is what the type now requires.
- **`runCapabilityProbe` is the one production site that constructs synthetic tokens today** — three
  numeric values "in the live dialect" for its wrong-token refusal checks — and it cannot under this
  contract. It is reordered so every wrong value is the same key's provably stale incarnation from
  this backend: overwrite with `t1` first (yielding `t2`), then the refusal check with `t1`; on the
  CAS key, create and commit with `ct1`, then the stale conflict with `ct1`; the wrong-token delete
  with `t1`. Bodies differ, so content-derived ETags differ, and the store answers exactly as it does
  today. This is the one production site the compiler lists at step 4 that is not a mechanical
  migration. The seventy-six test subclasses of a backend inherit the
  protected minter; that is a door for tests, guarded by review, and the document does not pretend the
  `static_assert`s close it.
- `InMemoryBackend::setEnforceTokens(false)` models a store that accepts every precondition. It stays.
- `EmulatedSingleProcess` mints under `emu_mutex`, an instance member though its header says
  "process-wide" (`CasObjectStorageBackend.h:39` vs `:211`); the comment is corrected. Its
  `emuMintToken` nonce fallback for an empty stat etag is removed; that path throws.
- Fixtures that run `Mode::Native` over `LocalObjectStorage` for convenience move to
  `EmulatedSingleProcess`. The two fake families that exist to exercise Native behaviour — the fake
  generation store in `gtest_cas_s3_staging` and the S3 error-classification fakes in
  `gtest_cas_sentinel_probe` — stay Native and **gain** an override of `readSmallObjectAndGetObjectMetadata`, which is the seam
  `read` uses — today both inherit the base implementation, which returns no metadata. The "read
  never mixes" test additionally needs the scripted S3 response to carry a body and to close mid-body;
  today it carries status and headers only. The sentinel-probe fakes that arm a `HEAD` fault are rewritten to arm a
  `GET` fault, since the probe issues no `HEAD` any more.
- `InstrumentedBackend` counts every operation into `ProfileEvents`; the new operations get counters
  and `CAS*Get` / `CAS*GetStream` are retired with the operations they counted.

## Landing order {#landing-order}

About 300 sites: 80 `get`, 24 `head`, 38 writes, 17 deletes, 26 default-constructed holders, 3 backend
implementations and 76 test subclasses, 4 codecs, 59 test constructions in 14 files. That size is how
the design works — an unconstructible, key-bound type makes the compiler find every place a string was
passed off as an observation. Four steps, each green:

1. **Safety, inside the backend, small.** In `ObjectStorageBackend`, with no caller change: the
   **full** minting grammar — not only empty and `*` — enforced at the legacy write entry points as
   `LOGICAL_ERROR` (a caller passed a non-token; `isDeterministicLocalFailure` already rethrows that
   code through the controller), the same on the delete path across all three backends (S3 is
   fail-loud there today, Emulated and `InMemoryBackend` return `TokenMismatch` and must throw
   instead), the same grammar applied where a `HEAD` or listing is minted into a token (as
   `CORRUPTED_DATA`: a store answer that cannot identify — so that step 1's `LOGICAL_ERROR` genuinely
   means a caller passed a non-token, not a store anomaly relabelled as a caller bug), and the
   fallback `HEAD` replaced by `CAS_WRITE_UNATTRIBUTED`. Under a hundred lines. After this step no
   clobber path is left: not empty, not `*`, not a quoted or zero-padded generation, not a list, not a
   token from a later unrelated `HEAD`. Not yet closed at step 1: a genuine incarnation of another
   key, which is what step 3's key binding is for. Two existing tests change with this step, and no
   others: `CASBackend.NullBackendShapeAndDefaults`, which expects `PreconditionFailed` and `NotFound`
   for a default `Token{}`, and — at step 4, not here —
   `CASObjectStorageBackend.NativeRejectsWrongDialectTokenBeforeTouchingTheWire`, which tests the
   dialect guard that the type replaces.
2. **The upstream slice, and the new operations beside the old.** The three-file slice lands as its
   own commit. Then `read`, `head`, `stream`, `write`, `remove`, `publish` are added to `Backend` with
   the old operations delegating where a delegation exists — and the document says where it does not:
   `getStream`'s token needs a `HEAD` the new `stream` will not issue, and `get`'s `attributes` and
   `Range` have no new source, so those old bodies stay until step 4 deletes them.
3. **Migration, file by file**, each its own commit: the 80 `get` sites first, with the `read`-or-`stream`
   decision already tabulated per site by the review of revision 1; then writes; then deletes, with GC's
   redelete and fsck taking the `head`-render-compare shape. Mechanical work, delegated and reviewed.
4. **The lock.** The old operations, `Token` and `mintingTypeMatches` are deleted, `Incarnation` loses
   every public constructor, the `static_assert`s go in. The compiler lists what was missed.

Unconstructibility is a property of step 4, and key binding of step 3. Until then the safety rests on
step 1 — which is what is needed immediately; the rest is what makes the next such defect impossible
to write.

## Verification {#verification}

**Compile time.** The two `static_assert`s; `grep` for `Incarnation{` and `Incarnation(` with
arguments outside the backend minter returns nothing.

**Key binding.** `head("a")` applied to `write("b", …)` and to `remove("b", …)` throws `LOGICAL_ERROR`
before any request; the same for an incarnation from a second backend instance.

**No needless `HEAD`.** Against a call-counting `IObjectStorage`: `read`, `stream` and
`probeSentinelRaw` issue zero `getObjectMetadata` calls; `head` issues one.

**`read` never mixes** — at the slice, where the fakes live: a scripted `GetObject` on the fake S3
client that serves `N` bytes of one body with one ETag, fails, then serves the rest with another ETag
→ `readSmallObjectAndGetObjectMetadata` throws; same ETag on reissue → it returns. On GCS: a marked
`GET` returns the generation in the ETag field, and `read` mints it.

**Minting refuses.** Empty, `*`, a comma list, `00123`, `0`, and a generation still quoted after the
strip → `CORRUPTED_DATA` from `head`, `read` and `list`. (`"123"` itself is a valid ETag-dialect value
and a valid pre-strip generation; it is not a refusal example.)

**Persisted compare.** A `cas_run` row whose `{type, value}` renders equal to `head`'s → `remove`
issued; unequal (stale, garbage, foreign dialect) → no `remove`, the round completes, and the
supersession path re-condemns a live blob on the next round. `CasInspect` renders both rows unchanged.

**The write anomaly.** A fake S3 client returning no ETag on `PutObject`: the probe refuses the pool by
name; on an already mounted pool `write` throws `CAS_WRITE_UNATTRIBUTED`, all four controller methods
resolve by `read`, and `isDeterministicLocalFailure` does not list the code.

**`expected` cannot leak.** A `Conflict` carries no incarnation — a compile-time property.

**Step 1 in isolation.** On a real S3-compatible store: a conditional overwrite with each value in the
grammar table's refusal set leaves the object unchanged and throws `LOGICAL_ERROR`; a delete with one
throws on all three backends; a write whose response lacks an ETag throws `CAS_WRITE_UNATTRIBUTED`.

**Existing gates.** The CA-s3 lane and the `CAS*` gtest gate stay green at every step; the upstream
slice's own tests (`gtest_aws_s3_client`, the S3 object-storage tests) stay green with the request mode
at `Default`.

## Acceptance {#acceptance}

1. Step 1 lands and its isolated tests pass before any API work.
2. The upstream slice lands as one commit, under a hundred lines, touching only `IO/` and
   `ObjectStorages/S3/`, with no CAS identifier in it.
3. After step 4: both `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`,
   `casPut`, `deleteExact`, `Token`, `mintingTypeMatches`, `ObjectMeta` on writes, `Meta::attributes`
   and the fallback `HEAD` no longer exist; `probeSentinelRaw` is one `GET`; `publish` returns `void`.
4. No needless `HEAD`; `read` throws on drift; no caller issues `head` and `read` on one key in one
   operation; no string becomes an `Incarnation` anywhere.
5. `remove` never reports `Mismatch` for any reason other than the store's own mismatch, and throws on
   a delete marker.
6. The probe refuses a nameless-write store by name; a runtime nameless write is resolved by every
   controller method.
7. Existing lanes and gates green at every step.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object renews
a mount lease. The collision is real and not the comment's fault — CAS lives inside
`ReplicatedMergeTree`, whose commit path goes through ClickHouse Keeper, so `CasRequestControl.h:250`
must name the real Keeper when it rules out a `Coordination::Exception` collision. In a codebase where
"Keeper" sometimes has to mean ClickHouse Keeper, a local object may not borrow the word. The name is
`MountLeaseRenewer`. Sized by those seven identifiers: 101 occurrences in 6 non-test files, 99 in
tests. Mechanical, its own commit.

## Out of scope {#out-of-scope}

**Azure**: the same slice shape applies to its read buffer and override, when Azure becomes a writable
CAS store.

**Pre-existing, filed:** `PoolMeta::admitOrValidate`'s unbounded conflict loop; the read-then-freeze
append's duplicate on post-commit exception; `resolved_by_get` under lockstep clones.

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

**Revisions 1 and 2** kept `Token` in the API and locked it; each review found a leak the previous
lock had not closed, because a value callers hold must be defended everywhere it is held. Revision 2
also promised "one physical request", which no `IObjectStorage` interface can deliver, told
`Gc::acquireOrRenewLease` to step down on a missing token when its acquire had already committed, used
`CORRUPTED_DATA` for a nameless write when the controller classifies that code as deterministic, and
put the persisted-token adoption after the fold had dropped the entry.

**An intermediate draft** had `Snapshot`, `Identity`, `PersistedIdentity`, `Precondition`, `Absent` —
five names around one value — and a `create` whose conditional nature was in a comment.

**Revision 3** removed the token from callers' hands but not from the program: its `parse(String)`
was a string-to-incarnation door that could not classify a dialect, because `"123"` is valid in all
three; its `Incarnation` was not bound to a key, so a genuine observation of `a` could clobber `b`; its
one-`GET` `read` could not mint on GCS at all, because a plain `GET` never carries a generation; its
two-point ETag check left an `A→B→A′` hole; its `dynamic_cast` inside CAS had no test seam; its
`publish` returned an incarnation three of four transports cannot produce; its `Removal` dropped the
delete-marker signal; its step 1 refused only empty and `*` and left quoted, zero-padded and listed
values as live clobber paths; and it accepted generation `0`.

---
description: 'Design for a CAS request contract that cannot be misused: one opaque key-bound Incarnation minted only by the backend, a transport layer that is private, one request layer whose every call names its retry policy, one retry engine for reads and writes alike, and a read-modify-write primitive that replaces nine hand-rolled loops.'
sidebar_label: 'Backend request contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend request contract'
doc_type: 'guide'
---

# CAS backend request contract {#cas-backend-request-contract}

Revision 5. Revision 4 settled the *identity* half of this contract — what an incarnation is and how
callers obtain one — and was reviewed as implementable with its corrections applied. This revision
folds in the *retry* half, from `2026-09-02-retry-coverage-by-construction.md` and the audit behind
it, because the two restructure the same interface and the same call sites, and the user decided they
land as one pass. [What earlier revisions got wrong](#what-earlier-revisions-got-wrong) keeps the
record. This document supersedes the retry-coverage note and
`2026-09-02-cas-empty-conditional-token-guard-design.md`, and revises the prerequisites of
`2026-09-01-cas-self-authored-mount-reclaim-design.md`.

## Problem {#problem}

Two audits of the same interface found two families of defect with one root.

**Identity.** Callers hold a **string** and hand it back to the backend as proof of what they
observed. Nine ways to manufacture that string were found and verified: an empty value that S3 and
Azure turn into an unconditional write; `*`; a quoted generation the GCS adapter unquotes; a write
token taken from a later, unrelated `HEAD`; a bytes/token pair assembled from two requests; dialect as
a runtime check; write and delete failing differently on the same bad input; decoders accepting an
empty persisted token that reaches the destructive delete; and a local refusal returned as remote
evidence. Two more were found reviewing the fix: a genuine observation of key `a` applied to key `b`,
and a dialect-free persisted string that `"123"` makes unclassifiable.

**Retry.** The three conditional-write primitives are pinned to a single HTTP attempt on purpose — a
transparently retried conditional write can cross a mount-lease boundary — and are safe only through
`CasRequestController`, which resolves an ambiguous attempt by an exact read before reissuing. They
are callable without it, and **twenty-three production sites do**: the pool-wide ref catalog written
twice per `CREATE TABLE` (the mechanism behind a reported ~40 % `CREATE TABLE` failure rate on GCS),
every write on the mount path, every GC-plane write, the loose table files. Nine of those sites are
hand-rolled read-modify-write loops, each with its own discipline: `for (;;)` with no bound in
`PoolMeta`, a hundred iterations with **no sleep** on `_ckpt`, two attempts on the GC lease. And the
reads that *are* retried are retried by the S3 SDK — up to 500 attempts at a 5 s cap with no jitter,
about 41 minutes — outside any CAS deadline, so a resolve `GET` inside a lease renewal can outlive
the lease it serves.

One root: **callers touch the transport directly, and the transport accepts what it is handed.** A
value anyone can construct, a primitive anyone can call. The fixes are the same shape: make the
value unconstructible and the primitive uncallable, and give callers one layer above with one door.

A second waste rides along: `get` issues a `HEAD` and a `GET` though a `GET` returns everything a
`HEAD` does plus the body; `MountLeaseKeeper::claim` pays two requests on its mint path and four on
adopt; `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by nobody;
`probeSentinelRaw` is a raw `HEAD`, then `get`'s `HEAD`, then a `GET`.

## The contract {#the-contract}

> **An `Incarnation` is minted only by the backend, from a store response, of one key, and names
> exactly one version.** Nothing else can produce one, and nothing can apply it to another key.
>
> **`Backend` is transport: one physical request per call, and every call is private to
> `CasRequests`.** No production code calls the store directly.
>
> **Every `CasRequests` call names its retry policy.** There is no default, so a call that has not
> thought about retry does not compile.
>
> **One retry engine serves reads and writes alike** — exponential backoff with full jitter, a
> deadline that binds, an ambiguous write resolved by an exact read before reissue, and the mount
> fence checked before every attempt and every sleep. Under a lease-bound policy no attempt or sleep
> that would end past the lease is started.

## One type {#one-type}

```cpp
class Incarnation;   // opaque; operator==; created only by a Backend; no default constructor
                     // privately {backend id, key, dialect, value}; render() -> "dialect:value"
```

The word already lives in the code. It is not `Token`, because it is not something the caller
*sends*; it is something the caller *saw* — of one key, from one backend. A `CasRequests` call given
an incarnation of another key or another backend throws `LOGICAL_ERROR`: a programming error, not a
store answer. "Backend identity" is a per-instance counter id, never the address, and the check lives
in the minting backend; `InstrumentedBackend` mints nothing and forwards. A cache holding an
`Incarnation` (`readShardDecoded`'s) must not outlive the backend that minted it; today it is owned by
the `Pool` that owns the backend.

```cpp
static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
```

`render()` yields `dialect:value` for `system.content_addressed_log`, `CasInspect` and the one
comparison in [persisted incarnations](#persisted-incarnations). There is no inverse. Minting applies
the rules a response value must meet — the backend parsing **its own responses**:

| dialect | a response value is an incarnation iff |
|---|---|
| Generation | canonical positive decimal **after the SDK ETag-field quote strip** (`normalizeTokenValue` stays; the adapter installs the generation quoted): digits only, no leading zero, not `0` — zero is the dialect's absence sentinel |
| ETag | non-empty; not `*` after trimming whitespace; no comma — a list matches any member, and no known store puts a comma in a single entity tag |
| Emulated | non-empty |

A value that fails is not an incarnation: `CORRUPTED_DATA` naming the key on a read; the anomaly in
[the write anomaly](#write-anomaly) on a write. `list` mints through the same path where it surfaces
per-key tokens. `mintingTypeMatches` and its three call sites are deleted. "Not yet" is spelled
`std::optional<Incarnation>`; the twenty-six production sites that default-construct a `Token` today
become optionals or are restructured — the reviews of revisions 1 and 3 list them.

## Two layers {#two-layers}

**`Backend` is transport.** It knows nothing of retries or fences. Each method is one physical
request, and every one that can be misused is private:

```cpp
class Backend
{
    friend class CasRequests;
    virtual std::optional<Object>  read(key)  = 0;   // one GET; bytes never from two incarnations
    virtual std::optional<Meta>    head(key)  = 0;   // one HEAD
    virtual ListPage               list(prefix, cursor, limit) = 0;
    virtual Removal                remove(key, Incarnation) = 0;         // If-Match
    virtual std::expected<Incarnation, Conflict> write(key, bytes, std::optional<Incarnation>) = 0;
    virtual SentinelProbe          probeSentinelRaw(key) = 0;   // one GET; NoSuchKey vs NoSuchBucket
public:
    std::unique_ptr<ReadBuffer>    stream(key);                 // data-plane
    void                           publish(BlobPublishRequest); // data-plane, unconditional
};
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal { Removed, Gone, Mismatch };                 // a delete marker throws
```

C++ applies access control to *calling*, not to *overriding*: the three production backends and the
seventy-six test subclasses override these as before and need no change. Only call sites move — and
a call site that cannot name a policy is precisely one that has not thought about retry.

`write` is one operation with the precondition as a parameter (`nullopt` = the key must be absent);
`putIfAbsent`, `putOverwrite` and `casPut` collapse into it. `std::expected` — in thirty-six files
under `src/` already — means a `Conflict` carries no incarnation: today's
`PutResult{PreconditionFailed, {}}` was exactly that extraction, and it is how empty tokens entered
circulation. `publish` returns `void` (no transport but one can name what it wrote, and no consumer
exists) and stays a separate verb so unconditional sites are greppable; three unconditional
mutations on mount-owned staging keys live outside this API by design.

**`CasRequests` is the only caller.** It is owned by the mount runtime because the **fence is a
property of the mount**, not of a call; it is the one thing a call does not name.

```cpp
class CasRequests
{
public:
    std::optional<Object>  read  (key,                         const Retry &);
    std::optional<Meta>    head  (key,                         const Retry &);
    ListPage               list  (prefix, cursor, limit,       const Retry &);
    Removal                remove(key, Incarnation seen,       const Retry &);

    std::expected<Incarnation, Conflict> create (key, bytes,                    const Retry &);
    std::expected<Incarnation, Conflict> replace(key, bytes, Incarnation seen,  const Retry &);
    std::expected<Settled, GaveUp>       readModifyWrite(key, Decide, const Retry &);
};
```

The three write verbs are the three shapes the audit found, named by what the caller holds:
`create` holds bytes and a claim to be first; `replace` holds bytes and an observation; `readModifyWrite`
holds only a decision. `slotOccupy` becomes `create(…, Retry::once())`; `resolveByExactGet` is the
engine's own resolve step, not an entry point. The sentinel probe is exposed as
`CasRequests::probeSentinel(key, const Retry &)`, the one read-class call whose 404 is an answer about
the bucket rather than the key. Today's five entry points and `CasRequestBudget` go.

## Retry {#retry}

```cpp
struct Retry
{
    uint64_t deadline_ms;      // absolute, boot clock; the one bound that binds
    uint32_t max_attempts;     // a ceiling, not the working bound

    static Retry standard();                                 // 90 s, 16 attempts
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms); // min(90 s, lease - margin)
    static Retry once();                                     // one attempt
};
```

**The deadline binds.** Attempts are a ceiling sized so the deadline arrives first (16 × ~3 s per
throttled attempt plus the backoff sum ≈ 104 s > 90 s; with jitter the mean is lower and the ceiling
still holds). A clear budget is one number that can be said aloud: ninety seconds, then `S3_ERROR`.
The user statement waits that long under throttling rather than failing faster; that is the decision,
and there is one class of wait, not several.

**Backoff is exponential with full jitter**: `sleep = uniform(0, min(cap, base · 2ⁿ))`, base 200 ms,
cap 5 s — the shape AWS and Google Cloud Storage both document. Today's engine has the exponent and no
jitter, so synchronized retries from many threads land in the same second under throttling; the
jitter is the one change to the algorithm. A `Retry-After` in the response is a lower bound on the
sleep.

**What retries**, one classification for reads and writes: 429, 408, 5xx (`SlowDown`,
`InternalError`, `ServiceUnavailable` included), connection loss, client timeout. **What does not**:
404 (an answer), 412 (`Conflict`, an answer), and the existing whitelist of definite failures
(malformed request, entity too large, access denied) in `classifyConditionalWriteResult`, which stays.

**An ambiguous write is resolved, not repeated**: an exact read under the same policy, byte
comparison, and only then a reissue — the engine's existing `resolved_by_get`, unchanged under the
new verbs. A read is idempotent and needs no resolve; a retried `read` is a fresh read with its own
first ETag, so the drift check is undisturbed.

**Three policies, because there are three real differences.** `standard`: everything that waits for
a result and can afford ninety seconds — catalog, `_ckpt`, table files, the GC plane, the mount at
open. `untilLeaseSafe`: only what must not outlive a lease — renew, farewell, and every read *inside*
them. `once`: where the retry belongs to the caller by meaning — each capability-probe step is its own
question to the store, and the `slotOccupy` callers drive their own `readModifyWrite`. A fourth shape
would be a fourth named constructor, never a parameter.

**The lease guarantee, mechanically.** `untilLeaseSafe` starts no attempt and no sleep that would end
past `lease_deadline − margin`: the check `now + backoff + attempt_timeout > deadline → give up now`
that `pauseBeforeReissue` already makes, plus the fence before every attempt and every sleep. What
improves: the renewal's resolve `GET` used to live in the SDK's own loop outside this deadline — the
amplifier in the audit's Gap 7 — and now lives under it.

## `readModifyWrite` {#read-modify-write}

```cpp
/// decide sees the current object (or absence) and returns the bytes to write, or nullopt: nothing to do.
using Decide = std::function<std::optional<String>(const std::optional<Object> & current)>;

struct Settled { std::optional<Incarnation> written; };            // nullopt: decide declined
struct GaveUp  { enum Why { Deadline, Attempts, FenceLost, Cancelled } why; std::optional<Object> last_seen; };
```

The loop, once for everyone: `read` under the policy → `decide` → `nullopt` is `Settled{nullopt}`;
otherwise `replace` against what was read, or `create` if nothing was; `Committed` is `Settled`;
`Conflict` sleeps with jitter and re-reads; an ambiguous write is resolved by exact read — our bytes
are `Settled`, another's are a conflict; deadline, ceiling, fence or cancellation are `GaveUp` with
the last object seen, so the caller can say what it saw. Conflicts spend the same budget as errors: a
hot key is also a failure, and it must end — GCS bounds mutations of one object at about one per
second, and today's `_ckpt` loop reissues without a sleep.

Anything the caller wants back it captures by reference; the primitive carries no template plumbing.
There is no separate conflict cap (that is `max_attempts`) and no between-attempts callback
(observability stays on the existing `observe` diagnostics).

The ref catalog — the site behind the `CREATE TABLE` failures — before and after:

```cpp
// today: CasRefCatalog::casUpdateImpl
for (;;)
{
    auto got = backend.get(key);                                // a 429 throws out; no retry
    auto next = apply(mutation, decode(got));
    auto res = backend.casPut(key, encode(next), got->token);   // one attempt; a 429 throws out
    if (res.outcome == CasOutcome::Committed) return …;
    /// Conflict: loop, no sleep
}
// after
auto settled = requests.readModifyWrite(key,
    [&](const std::optional<Object> & current) -> std::optional<String>
    {
        return encode(apply(mutation, decode(current)));
    },
    Retry::standard());
```

| site | today | after |
|---|---|---|
| `CasRefCatalog::casUpdateImpl` | `for (;;)`, no retry, no sleep | `readModifyWrite`, `standard` |
| `PoolMeta::admitOrValidate`, `createOrValidate` | `for (;;)`, unbounded (the livelock) | `readModifyWrite`, `standard` — bounded for free |
| `publishCkpt` | 100 × PUT+GET, **no sleep** | `readModifyWrite`, `standard` |
| `allocateWriterEpoch` | bounded loop | `readModifyWrite`, `standard`; decide = `next + 1` |
| `Gc::acquireOrRenewLease` | 2 attempts | `readModifyWrite`, `standard`; **decide returns `nullopt` when the leader is not us** — the loop ends by decision, not by a cap that stood in for "do not fight" |
| `Gc::pulseHeartbeat` | one attempt, conflict ignored | `readModifyWrite`, `standard`; decide = `++seq` |
| `casGcMaintenanceState` | one attempt | `readModifyWrite`, `standard` |
| `CasPlainObjects::casPutObject` | HEAD → put, 100 iterations on `PreconditionFailed` | `readModifyWrite`, `standard`; decide reads presence only |
| GC round commit | one attempt, conflict drops the round | **`replace`**, `standard` — on conflict it does not re-decide, it drops the round, and the verb says so |

`MountLeaseKeeper::renew` and `terminate` are `replace` under `untilLeaseSafe`; `claimMount` at open
and at remount is `readModifyWrite` under `standard` (there is no lease yet to bound it); the
capability probe is `create`/`replace`/`remove` under `once`.

## The upstream slice {#upstream-slice}

About thirty additive lines in six files under `src/IO` and `src/Disks/DiskObjectStorage/ObjectStorages/S3`,
generic, no CAS identifier, portable on its own. Three facts make it necessary.

**A plain `GET` never carries a GCS generation.** The generation reaches the SDK's ETag field only
through `applyGcsConditionalDialectToResponse`, which `PocoHTTPClient` invokes only
`if (isNativeConditionalRequest(request))`; `ReadBufferFromS3::sendRequest` never marks its
`GetObjectRequest`. Today's `get` works on GCS only because its token comes from `nativeHead`, which
marks. So: `ObjectStorageRequestMode` (today in `WriteSettings.h`) moves to a five-line
`IO/ObjectStorageRequestMode.h` included by both settings headers; `ReadSettings` gains
`object_storage_request_mode = Default`; `sendRequest` adds the one line `getObjectInfo.cpp:42`
already uses for `HEAD`. The request-side adapter is a no-op for a conditionless `GET`; the OAuth
client strips five AWS signing headers from a marked request as it does for the marked `HEAD` today.
Marking every `GET` unconditionally instead would put the generation into the ETag the Iceberg
version-hint writer copies into an **unmarked** `If-Match`; the mode gate is necessary.

**A reissued `GET` can straddle a replacement.** `ReadBufferFromS3::nextImpl` reissues from the
current offset on a mid-body failure without `If-Match`, and `getObjectMetadataFromTheLastRequest`
reports the last response, so `readSmallObjectAndGetObjectMetadata` cannot keep its own word
"consistent". `initialize` records the first response's ETag and sets `response_identity_changed` on
**every** reissue whose ETag differs (per reissue, so `A→B→A′` is caught at `B`); the S3 override of
`readSmallObjectAndGetObjectMetadata` throws after draining if the flag is set. That overridable
function is `read`'s implementation under the request mode, and it is the seam the Native test fakes
use — nothing in CAS casts a buffer. Throwing inside `initialize` instead would be caught by
`nextImpl`'s own retry loop; the flag checked after the drain is the minimum.

**The SDK retries reads outside any CAS deadline.** `S3ObjectStorage::readObject` takes the default
client, whose retry strategy is 500 attempts at a 5 s cap. `WriteSettings` already carries
`object_storage_retry_profile`, which `writeObject` honours by selecting the single-attempt client;
`ReadSettings` gains the same field, and `readObject` selects the same client when it is set. With
that, every control-plane request CAS makes is one physical attempt at the SDK, and the only retry
loop is `CasRequests`'. `readSmallObjectAndGetObjectMetadata` is then exactly one request per call.

Under the project's rule for shared surfaces this slice is consulted before it is written; the user
has approved the direction.

## Persisted incarnations {#persisted-incarnations}

GC must remember which publication it condemned — a republished blob is payload-identical to the
condemned one and byte-different only by its envelope's `incarnation_tag`, which is what makes a
content-derived ETag differ. It persists the two strings the `cas_run` format already carries,
`token_type` and `token`. **There is no way from those strings to an `Incarnation`.** The persisted
pair is compared, as text, against the rendering of a live observation of the same key:

```cpp
auto meta = requests.head(blob_key, Retry::standard());
if (meta && meta->incarnation.render() == persisted)
    requests.remove(blob_key, meta->incarnation, Retry::standard());
```

The incarnation handed to `remove` came from `head`, of that key, from this backend; the persisted
string never became one; a dialect-tagged string cannot be misread across dialects. A stale or garbage
row fails to match: GC records `Replaced` — justified by a live `HEAD`, remote evidence — keeps the
meta and drops the row; a blob touched in the window is re-condemned by the supersession path, and a
republished-then-abandoned one is reclaimed on the next publication of that content, as today. Four
sites compare a persisted token with a live one and all become render compares: GC's redelete and
`CasFsck`'s retirement check remove; the fold's supersede compare and the meta writer's confirmation
registry compare only. The cost is one `HEAD` per condemned-blob delete where today there is one only
on a mismatch; GC's delete phase is background and round-budgeted.

## The write anomaly {#write-anomaly}

`write` must return an `Incarnation`; a response without one has nothing to return.
`runCapabilityProbe` depends on write tokens structurally — `t1` from step 1 is step 4's
precondition — and refuses a nameless-write store today only by accident; it will refuse it by name.
At runtime a nameless response is a store anomaly and the write **may have committed**: it throws
`CAS_WRITE_UNATTRIBUTED`, which `classifyConditionalWriteResult` treats as ambiguous by default, so the
engine resolves it by reading back. The fallback `HEAD` in `tokenFromWriteResult` is deleted. Because
every write now goes through `CasRequests`, there are no raw callers for this exception to reach — the
thirty-caller trace of revision 4 is replaced by the type.

## What is promised about requests {#request-promises}

**No operation issues a `HEAD` it does not need**: `read`, `stream` and `probeSentinelRaw` issue none;
`head` issues one; `probeSentinelRaw` becomes one `GET`, whose 404 body distinguishes `NoSuchKey`
from `NoSuchBucket`. Reading an object's metadata is one `GET` at the caller level too:
`MountLeaseKeeper::claim` goes from two and four requests to two and two; the manifest reader's own
change (`2026-09-02-cas-manifest-cache-by-id-design.md`) lands first and independently.

**Control plane and data plane are two regimes, and the line is the nature of the request.**
Control-plane requests — everything through `CasRequests` — are small, deadline-bound, and retried by
one engine on a single-attempt client. Data-plane requests — the blob stream on the `MergeTree` read
path (`readBlobPayload`, below `Backend`) and `publish` (multipart with its own per-part retries) — are
large streams where the SDK's per-request retry is right, and stay on it. For them the audit's Gap 8
is closed by configuration: a CAS-specific `s3_retry_attempts` in place of the default 500.

Not promised: multipart writes above `max_single_part_upload_size` are several requests; `list`
prefetches the next page; `stream` is lazy.

## Assumptions {#assumptions}

**One, and it is assumed, not probed.** An incarnation names one version, and the store never reuses
one for **different** content; the capability probe does not test this, and it is a requirement on
every backend. What no contract can promise is that identical content never yields the same
incarnation — a restored copy of an old body may carry the old ETag — and a design that recognises its
own body by bytes also recognises a restored copy. This defeats `proven_dead_token` observation just as
much; it is stated here once.

## Seams, named {#seams}

- **Tests** obtain incarnations by performing operations on `InMemoryBackend`; a "wrong" one for a key
  is that key's **previous** incarnation after an overwrite. The seventy-six test subclasses inherit
  the protected minter; that is a door for tests, guarded by review.
- **`runCapabilityProbe`** constructs three synthetic tokens today and cannot under this contract. It
  is reordered so every wrong value is the same key's stale incarnation: overwrite with `t1` first,
  then the refusal check with `t1`; on the CAS key, create and commit with `ct1`, then the stale
  conflict; the wrong-token delete with `t1`. Bodies differ, so ETags differ, and the store answers as
  today. The one production site the compiler lists at the lock that is not a mechanical migration.
- **`ThrottlingBackend`**, a sibling of `InstrumentedBackend`: answers a chosen retryable status on every
  *n*-th request through `CasRequests`. The gate under it is in [Verification](#verification). The fake
  GCS service of the integration suite gains the same mode on its control surface.
- `InMemoryBackend::setEnforceTokens(false)` stays; it models the store. `EmulatedSingleProcess`'s
  `emu_mutex` comment ("process-wide") is corrected; its nonce fallback for an empty stat etag throws.
  Native-over-Local convenience fixtures move to `EmulatedSingleProcess`; the two Native fake families
  gain an override of `readSmallObjectAndGetObjectMetadata`.
- `InstrumentedBackend` counts the new operations; `CAS*Get` / `CAS*GetStream` events are retired.

## Landing order {#landing-order}

About 300 sites: 80 `get`, 24 `head`, 38 writes, 17 deletes, 26 default-constructed holders, 3
backend implementations and 76 test subclasses, 4 codecs, 59 test constructions in 14 files — the
same sites the identity contract already had to touch; the retry half adds a policy token to each and
shrinks the nine loops. Five steps, each green:

1. **Safety, inside the backend, small.** In `ObjectStorageBackend`, no caller change: the full
   minting grammar enforced at the legacy write entry points as `LOGICAL_ERROR`, the same on the
   delete path across all three backends, the grammar applied where `HEAD` and `list` mint as
   `CORRUPTED_DATA`, and the fallback `HEAD` replaced by `CAS_WRITE_UNATTRIBUTED`. Under a hundred
   lines; after it no clobber path is left. Two existing tests change (`CASBackend.NullBackendShapeAndDefaults`
   here; `NativeRejectsWrongDialectTokenBeforeTouchingTheWire` at step 5).
2. **The upstream slice**, one commit, the three facts above; `IO/` and `ObjectStorages/S3/` only.
3. **`CasRequests` and `Retry` beside the old.** The engine is today's controller with two changes —
   jitter, and reads — under the new verbs; the old `putXxxControlled` delegate. `ThrottlingBackend`
   and its gate land here, **before** the migration, so the migration runs under it.
4. **Migration, file by file, every site naming its policy**: 80 `get` → `requests.read(…, Retry::…)`,
   24 `head`, 38 writes → `create`/`replace`/`readModifyWrite` (the nine loops collapse), 17 deletes.
   The `read`-or-`stream` decision per site is already tabulated by the review of revision 1. Mechanical,
   delegated, reviewed per file, gate green after each.
5. **The lock.** `Backend`'s methods go private, the old controller entry points, `CasRequestBudget`,
   `Token`, `mintingTypeMatches` and the fallback `HEAD` are deleted, the `static_assert`s go in. The
   compiler lists what was missed.

Unconstructibility and uncallability are properties of step 5; key binding of step 4. Until then the
safety rests on step 1 — what is needed immediately — and the retries on step 3.

## Verification {#verification}

**Compile time.** The two `static_assert`s; no valued `Incarnation` construction outside the minter;
a `Backend` method called from outside `CasRequests` does not compile; a `CasRequests` call without a
`Retry` does not compile.

**No site without a retry, executably.** Under `ThrottlingBackend` answering 429 on every *n*-th
request, the pool-level scenario tests — `CREATE TABLE`, `INSERT`, `DROP`, `RENAME`, a writable mount
at open, a GC round — all succeed, and every key touched shows at least one reissue. This is the audit
as a test: a call under `Retry::once()` where `standard` was meant turns the gate red in seconds.

**The lease, on fake clocks.** Under sustained 429, `untilLeaseSafe` starts its last attempt before
`lease_deadline − margin − attempt_timeout` and issues **zero** requests after it; a renewal whose
resolve `GET` is throttled terminates by the deadline rather than hanging.

**Backoff as a distribution.** Over a thousand series on fake clocks, `sleep ∈ [0, min(cap, base·2ⁿ)]`
with a mean near half the ceiling; `Retry-After` is honoured as a lower bound; without jitter the test
is red.

**`readModifyWrite` under contention.** Two threads incrementing one counter through it on
`InMemoryBackend`: the total is the sum, every conflict is followed by a sleep, and a key in perpetual
conflict ends in `GaveUp{Deadline}` — the bound `PoolMeta` lacks today.

**Identity.** Key binding: `head("a")` applied to `write("b", …)` throws before any request. Minting
refuses empty, `*`, a comma list, `00123`, `0`, and a generation still quoted after the strip.
Persisted compare: a matching row removes; a stale, garbage or foreign-dialect row does not, the round
completes, `CasInspect` renders both. The write anomaly: a fake S3 client returning no ETag on
`PutObject` makes the probe refuse the pool by name and a mounted pool throw
`CAS_WRITE_UNATTRIBUTED`, resolved by read. `Conflict` carries no incarnation — a compile-time
property.

**Reads.** `read`, `stream`, `probeSentinelRaw` issue zero `getObjectMetadata` calls; `head` one. `read`
never mixes — a scripted `GetObject` on the fake S3 client serving one body then failing then serving
another with a different ETag makes `readSmallObjectAndGetObjectMetadata` throw. On GCS a marked `GET`
returns the generation and `read` mints it.

**Existing gates.** The CA-s3 lane, the `CAS*` gtest gate and the upstream slice's own tests green at
every step; `Cas::Probe` passes on every supported writable store.

## Acceptance {#acceptance}

1. Step 1 lands first with its isolated tests.
2. The upstream slice lands as one commit, under a hundred lines, `IO/` and `ObjectStorages/S3/` only,
   no CAS identifier.
3. After step 5: both `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`, `casPut`,
   `deleteExact`, `Token`, `mintingTypeMatches`, `CasRequestBudget`, the five old controller entry
   points and the fallback `HEAD` no longer exist; `Backend` has no public mutation; every `CasRequests`
   call names a policy.
4. The throttling gate is green with a reissue observed on every touched key; the audit's twenty-three
   sites are all under `CasRequests`.
5. `untilLeaseSafe` issues no request past `lease_deadline − margin`; the jitter test is red without
   jitter; `readModifyWrite` loses no increment under contention and bounds a hot key.
6. No needless `HEAD`; `read` throws on drift; no string becomes an `Incarnation` anywhere; `remove`
   never reports `Mismatch` for any reason other than the store's own.
7. Existing lanes and gates green at every step.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object renews
a mount lease. The collision is real and not the comment's fault — CAS lives inside
`ReplicatedMergeTree`, whose commit path goes through ClickHouse Keeper, so `CasRequestControl.h:250`
must name the real Keeper when it rules out a `Coordination::Exception` collision. The name is
`MountLeaseRenewer`; 101 occurrences in 6 non-test files, 99 in tests; mechanical, its own commit.

## Out of scope {#out-of-scope}

**Azure**, when it becomes a writable CAS store: the same slice shape for its read buffer and override.

**Pre-existing, filed:** `resolved_by_get` under lockstep clones; the read-then-freeze append's
duplicate on a post-commit exception. (`PoolMeta::admitOrValidate`'s unbounded loop is closed by this
design, not filed.)

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

**Revisions 1 and 2** kept `Token` in the API and locked it; each review found a leak the previous lock
had not closed, because a value callers hold must be defended everywhere it is held. **Revision 3**
removed the token from callers' hands but not from the program (`parse(String)`, unbound to a key, a
one-`GET` read that could not mint on GCS, a two-point ETag check with an `A→B→A′` hole, a `publish`
that returned what three transports cannot produce). **Revision 4** fixed those and was found
implementable with corrections — the stored-byte bound, the probe's synthetic tokens, the quoted
generation, the backend-identity definition — and is the identity half of this document unchanged in
substance.

**What revision 4 did not have** is retry: it left the transport publicly callable, so twenty-three
sites could keep writing without one, and it left reads on the SDK's forty-minute loop outside every
CAS deadline. Revision 5 adds the second half of the same idea — the primitive uncallable, as the value
was unconstructible — and one engine above both.

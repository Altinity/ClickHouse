---
description: 'Design for a CAS request contract that cannot be misused: a transport that deals in strings and is callable only with a key that one class can make, one façade that mints key-bound Incarnations and whose every call names its retry policy, one deadline-bound engine for reads and writes, one three-outcome write result, and a read-modify-write primitive that replaces the hand-rolled loops.'
sidebar_label: 'Backend request contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend request contract'
doc_type: 'guide'
---

# CAS backend request contract {#cas-backend-request-contract}

Revision 6. Revision 5 folded the retry half into the identity contract and was reviewed twice, by two
independent models; both found the same structural holes — an access scheme that private virtuals
cannot deliver, a two-outcome write result for a three-outcome protocol, a retry ceiling that binds
before the deadline it was meant to back, an upstream claim the slice could not keep, and a
migration step that needed a conversion the contract forbids. This revision closes them. [What earlier
revisions got wrong](#what-earlier-revisions-got-wrong) keeps the record. This document supersedes
`2026-09-02-cas-retry-coverage-by-construction.md` and
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
every write on the mount path, every GC-plane write, the loose table files. A dozen of those sites
are hand-rolled read-modify-write loops, each with its own discipline: `for (;;)` with no bound in
`PoolMeta::admitOrValidate`, a hundred iterations with **no sleep** on `_ckpt` and on the catalog, two
attempts on the GC lease, four on the heartbeat-floor fence-out. And the reads that *are* retried are
retried by the S3 SDK — up to 500 attempts at a 5 s cap with no jitter, about 41 minutes — outside any
CAS deadline, so a resolve `GET` inside a lease renewal can outlive the lease it serves.

One root: **callers touch the transport directly, and the transport accepts what it is handed.** A
value anyone can construct, a primitive anyone can call. The fixes are the same shape: make the
value unconstructible and the primitive uncallable, and give callers one layer above with one door.

A second waste rides along: `get` issues a `HEAD` and a `GET` though a `GET` returns everything a
`HEAD` does plus the body; `MountLeaseKeeper::claim` pays two requests on its mint path and four on
adopt; `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by nobody;
`probeSentinelRaw` is a raw `HEAD`, then `get`'s `HEAD`, then a `GET`.

## The contract {#the-contract}

> **An `Incarnation` is minted only by `CasRequests`, from a transport response, of one key, and names
> exactly one version.** Nothing else can produce one, and nothing can apply it to another key.
>
> **`Backend` is transport: one physical request per call, strings in and strings out, and no call can
> be made without a `TransportAccess` that only `CasRequests` can construct.** No production code and
> no test calls the store directly.
>
> **Every `CasRequests` call names its retry policy.** There is no default, so a call that has not
> thought about retry does not compile.
>
> **One engine serves reads and writes alike** — exponential backoff with full jitter, a deadline that
> is the only bound, an ambiguous write resolved by an exact read before reissue, and the fence
> generation captured at call entry and re-checked before every attempt and every sleep. Under a
> lease-bound policy no attempt or sleep that would end past the lease is started.
>
> **A write has three outcomes and reports all three.** `Committed`, `Conflict`, `GaveUp` — and
> `GaveUp` says whether anything was sent, because the ref lane's wedge decision hangs on that bit.

## One type {#one-type}

```cpp
class Incarnation;   // opaque; operator==; created only by CasRequests; no default constructor
                     // privately {backend id, key, dialect, value}; render() -> "dialect:value"
```

The word already lives in the code. It is not `Token`, because it is not something the caller
*sends*; it is something the caller *saw* — of one key, from one backend. A `CasRequests` call given
an incarnation of another key or another `CasRequests` throws `LOGICAL_ERROR`: a programming error,
not a store answer. "Backend identity" is a per-instance counter id, never the address. A cache
holding an `Incarnation` (`readShardDecoded`'s) must not outlive the `CasRequests` that minted it;
today it is owned by the `Pool` that owns the backend.

```cpp
static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
static_assert(!std::is_default_constructible_v<TransportAccess>);
```

`render()` yields `dialect:value` for `system.content_addressed_log`, `CasInspect` and the one
comparison in [persisted incarnations](#persisted-incarnations). There is no inverse. Minting applies
the rules a response value must meet — the façade parsing **the transport's responses**, with the
dialect it knows from the backend it wraps:

| dialect | a response value is an incarnation iff |
|---|---|
| Generation | canonical positive decimal **after the SDK ETag-field quote strip** (`normalizeTokenValue` stays; the adapter installs the generation quoted): digits only, no leading zero, not `0` — zero is the dialect's absence sentinel |
| ETag | non-empty; not `*` after trimming whitespace; no comma — a list matches any member, and no known store puts a comma in a single entity tag |
| Emulated | non-empty |

A value that fails is not an incarnation: `CORRUPTED_DATA` naming the key on a read; the anomaly in
[the write anomaly](#write-anomaly) on a write. `list` mints through the same path where it surfaces
per-key values. `mintingTypeMatches` and its three call sites are deleted. "Not yet" is spelled
`std::optional<Incarnation>`; the twenty-six production sites that default-construct a `Token` today
become optionals or are restructured — the reviews of revisions 1 and 3 list them.

## Two layers {#two-layers}

**`Backend` is transport, in strings, under a key.** It knows nothing of retries, fences or
incarnations. Each method is one physical request; each takes a `TransportAccess`, an uncopyable
token whose only constructor is private and befriended to `CasRequests`:

```cpp
class TransportAccess { friend class CasRequests; TransportAccess() = default; /* not copyable */ };

class Backend
{
public:
    struct Raw    { String bytes; String value; };            // value: the store's own ETag/generation text
    struct RawMeta{ uint64_t size; String value; };
    virtual std::optional<Raw>     read  (key, TransportAccess &) = 0;   // one GET; bytes never from two responses
    virtual std::optional<RawMeta> head  (key, TransportAccess &) = 0;   // one HEAD
    virtual RawListPage            list  (prefix, cursor, limit, TransportAccess &) = 0;
    virtual RawRemoval             remove(key, String expected_value, TransportAccess &) = 0;      // If-Match
    virtual std::expected<String, RawConflict> write(key, bytes, std::optional<String> expected_value, TransportAccess &) = 0;
    virtual RawSentinel            probeSentinelRaw(key, TransportAccess &) = 0;                    // one GET
    virtual std::unique_ptr<ReadBuffer> stream(key, TransportAccess &) = 0;                        // data-plane body
    virtual void                   publish(BlobPublishRequest) = 0;     // data-plane, unconditional, no key
};
```

Why a key argument and not private virtuals: C++ applies access control to the *call* through the
*static type*, so a private virtual in `Backend` leaves every public override on `InMemoryBackend` and
`ObjectStorageBackend` callable by anything holding the concrete type, and it leaves no room for a
decorator — `InstrumentedBackend`, the throttling seam, and the two test decorators that forward
`inner->get(...)` today would not compile. A key closes both: no static type can call without one,
and a decorator forwards the key it was handed, so decorators keep working with no second friend.
Overrides may sit under any access specifier; the three production backends and the test subclasses
change **signatures** (`Token` → `String`, `GetResult` → `Raw`), not access, and the migration counts
them.

Why strings: the backend must consume the expected value on `write` and `remove` and produce the
observed one on every read, and if it did that in `Incarnation`s it would need to mint — a protected
minter every subclass inherits, which is the string-to-incarnation door under another name. So the
transport never sees the type. `CasRequests` extracts the value from an `Incarnation` it has verified
belongs to this key and this backend, and mints the response's value under the grammar above. The
type exists on one side of the key.

`write` is one operation with the precondition as a parameter (`nullopt` = the key must be absent);
`putIfAbsent`, `putOverwrite` and `casPut` collapse into it. `publish` returns `void` (no transport
but one can name what it wrote, and no consumer exists) and takes no key so unconditional sites stay
greppable; the three unconditional mutations on mount-owned staging keys live outside this API by
design. `stream` and `publish` are the data-plane exceptions to "one physical request": the SDK
retries inside a body stream and inside a multipart upload, and that is right for them.

**`CasRequests` is the only caller.** It is constructed with a **fence**, because the fence is a
property of whoever owns the lease, not of a call: the mount plane passes the mount fence, the GC
plane an open fence (its writes are guarded by the `gc/state` incarnation, not by a mount lease, as
today), the tools an open fence (`CasDecommission` says so explicitly today). The engine captures the
fence **generation** at call entry and calls `checkFenceOrThrow(admitted)` before every attempt and
every sleep — the generation is bumped by every loss *and every re-arm*, so a caller admitted under a
prior lease incarnation aborts even when the fence is currently open, which is the invariant
`casPutObject`, `publishCkpt` and the ref lane hold by hand today and would otherwise lose.

```cpp
class CasRequests
{
public:
    CasRequests(BackendPtr, Fence);                        // Fence: generation(), checkOrThrow(admitted)

    std::optional<Object>       read  (key, const Retry &);
    std::optional<Meta>         head  (key, const Retry &);
    ListPage                    list  (prefix, cursor, limit, const Retry &);
    void                        forEachListedKey(prefix, Fn, const Retry &);
    Removal                     remove(key, Incarnation seen, const Retry &);
    SentinelProbe               probeSentinel(key, const Retry &);
    std::unique_ptr<ReadBuffer> stream(key, const Retry &);          // the OPEN is under policy; the body is the SDK's

    WriteResult create (key, bytes,                    const Retry &);
    WriteResult replace(key, bytes, Incarnation seen,  const Retry &);
    WriteResult readModifyWrite(key, Decide,           const Retry &);
};
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal { Removed, Gone, Mismatch };                       // a delete marker throws
```

The three write verbs are the three shapes the audit found, named by what the caller holds:
`create` holds bytes and a claim to be first; `replace` holds bytes and an observation;
`readModifyWrite` holds only a decision. Today's five controller entry points go; `resolveByExactGet`
is the engine's own resolve step, not an entry point.

## One write result {#write-result}

```cpp
struct Committed { Incarnation incarnation; };
struct Conflict  { std::optional<Object> occupant; };   // filled ONLY from the engine's own resolve read
struct GaveUp    { enum Why { Deadline, FenceLost, Cancelled } why; bool sent_any; std::optional<Object> last_seen; };
using WriteResult = std::variant<Committed, Conflict, GaveUp>;     // .orThrow() for callers that want S3_ERROR
```

A controlled write has three outcomes today, and the third is consumed as a **protocol fact**, not a
diagnostic: `CasRefLedger::commitRefChunk` releases the transaction id only when the unresolved
reason proves nothing was sent and wedges otherwise; the recovery walk and the wedge retry act on the
same bit; `MountLeaseKeeper::renew` classifies `Committed` / `Conflict` / `NotAttempted` / `Vanished`
from the controller's fields. `std::expected<Incarnation, Conflict>` could express none of it and
would have collapsed "gave up" into an exception — precisely the bare-`Unresolved` collapse the ref
lane guards against. One variant carries all three; `sent_any` is the bit the wedge decision needs
(today's `unresolvedProvesNothingWasSent`, whose header says adding a member is a protocol decision —
it stays a protocol decision, now a field).

`Conflict::occupant` bends a rule revision 4 stated: "a `Conflict` carries no incarnation". That
rule is right for the **write's own response** — today's `PutResult{PreconditionFailed, {}}` was
exactly how empty tokens entered circulation — and does not apply to an object the engine *read*.
`slotOccupy`'s `Occupied{bytes, incarnation}` is `Conflict{occupant}` filled from the resolve read,
and only from it. `create`'s content-addressed corruption verdict (a different occupant at a
content-addressed key is `CORRUPTED_DATA` in `putIfAbsentControlled`, a plain `Conflict` in the
mutable variant) moves to the two callers that own the key's meaning — the ref lane and
`stageManifest` — which compare the occupant and throw.

## Retry {#retry}

```cpp
struct Retry
{
    uint64_t deadline_ms;         // absolute, boot clock; the one bound
    bool     single_attempt;      // once(): no reissue, no sleep

    static Retry standard();                                    // now + 90 s
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms);    // min(now + 90 s, lease - safety margin)
    static Retry once();
};
```

**The deadline is the only bound.** Revision 5 kept a `max_attempts` ceiling "as a safety net" and
argued the deadline arrives first; under sustained 429s — the `CREATE TABLE` case — a throttled attempt
returns in tens of milliseconds, sixteen attempts exhaust after about 56 s of backoff without jitter
and about 28 s with it, and the ceiling binds at half a minute while the document promised ninety
seconds. The ceiling is gone; `once()` is a flag; there is no `GaveUp::Attempts`. Ninety seconds is
one number that can be said aloud, and the user statement waits that long under throttling rather
than failing faster. That is the decision, and there is one class of wait.

**An attempt is reserved before it is started.** The engine owns one constant, `attempt_reservation_ms`
— the request timeout the single-attempt client is built with (today the cloned client keeps the
disk's 30 s transport timeout while the controller budgets 5 s "as a scheduling estimate only", so a
single hung attempt can already outlive `lease − margin`; the clone gets `request_timeout =
attempt_reservation` in the same commit) — and starts no attempt and no sleep unless
`now + sleep + attempt_reservation ≤ deadline`. `untilLeaseSafe` takes the safety margin from mount
config. The two numbers survive as mount configuration under today's names
(`attempt_timeout_ms`, `lease_safety_margin_ms`); `CasRequestBudget` loses `operation_deadline_ms`
and `max_attempts`, keeps those two, and keeps its consumers — the mount fence's own margin in
`refAppendFenceOk`, the startup inequality `attempt_timeout + margin < TTL` in
`validateCasRequestBudget` on which the successor's observation threshold rests, the detached-drain
deadlines, the recovery walk's bounds — unchanged.

**Backoff is exponential with full jitter**: `sleep = uniform(0, min(cap, base · 2ⁿ))`, base 200 ms,
cap 5 s — the shape AWS and Google Cloud Storage both document. Today's engine has the exponent and no
jitter, so synchronized retries from many threads land in the same second under throttling; the
jitter is the one change to the algorithm. `Retry-After` is not honoured: neither `S3Exception` nor
`PocoHTTPClient` surfaces the header, and reading it would widen the upstream slice for a value the
jittered cap already approximates.

**What retries**, one classification for reads and writes: 429, 408, 5xx (`SlowDown`,
`InternalError`, `ServiceUnavailable` included), connection loss, client timeout, and `ExpiredToken`
once after the credential refresh the read buffer already performs. **What does not**: 404 (an
answer), 412 (`Conflict`, an answer), and the existing whitelist of definite failures (malformed
request, entity too large, access denied) in `classifyConditionalWriteResult`, which stays.

**The verdict is the call's, not the last attempt's.** A definite rejection or a 404 that follows an
earlier *ambiguous* attempt of the same call proves nothing about that earlier attempt — it may have
landed — so the engine resolves by exact read before it reports `Conflict`, exactly as the controller
rules today. A 404 on an `If-Match` write inside `readModifyWrite` means the key vanished: re-read,
and `decide` sees absence.

**An ambiguous write is resolved, not repeated**: an exact read under the same policy and the same
deadline, byte comparison, and only then a reissue — the engine's existing `resolved_by_get`,
unchanged under the new verbs. A read is idempotent and needs no resolve; a retried `read` is a fresh
read with its own first ETag, so the drift check is undisturbed.

**Three policies, because there are three real differences.** `standard`: everything that waits for
a result and can afford ninety seconds — catalog, `_ckpt`, table files, the GC plane, the mount at
open, and **the capability probe**: a 429 is not an answer to "does the store enforce the
precondition", a 412 is, so the probe's steps retry like any other request and a writable mount at
open succeeds under throttling. `untilLeaseSafe`: renew, and every read *inside* it — nothing else.
`once`: the `slotOccupy` callers, which drive their own outer loop, and the **farewell** at
shutdown — it is a certificate of convenience (the clean marker lets `claimMount` reclaim without
the ~36.5 s observation), harmless past the lease, and not worth ninety seconds of a shutdown; a
lease-bound policy would *skip* it after a failed renewal, which is the case where it matters most. A
fourth shape would be a fourth named constructor, never a parameter.

**The lease guarantee, mechanically.** `untilLeaseSafe` starts no attempt and no sleep that would end
past `lease_deadline − margin`, with the reservation above as the premise — the check
`pauseBeforeReissue` already makes, now with a reservation the transport honours. The renewal's
resolve `GET` used to live in the SDK's own loop outside this deadline — the amplifier in the
audit's Gap 7 — and now lives under it, sharing the write's deadline. `mountObservationThresholdMs`
is unaffected: its argument rests on `attempt_timeout + margin < TTL` and on each attempt being
bounded, and the reservation is what makes the second premise true.

## `readModifyWrite` {#read-modify-write}

```cpp
/// decide sees the current object (or absence) and returns the bytes to write, or nullopt: nothing to do.
/// It may consult the caller's state and may issue reads of its own through the same CasRequests.
using Decide = std::function<std::optional<String>(const std::optional<Object> & current)>;
```

The loop, once for everyone: `read` under the policy → `decide` → `nullopt` is `Committed` with the
incarnation read (nothing to write, the observation stands) — or, when nothing was read, a `Conflict`
with no occupant; otherwise `replace` against what was read, or `create` if nothing was; `Committed`
returns; `Conflict` sleeps with jitter and re-reads; an ambiguous write is resolved by exact read — our
bytes are `Committed`, another's are a conflict; deadline, fence or cancellation are `GaveUp` with
`sent_any` and the last object seen. Conflicts spend the same budget as errors: a hot key is also a
failure, and it must end — GCS bounds mutations of one object at about one per second, and today's
`_ckpt` and catalog loops reissue without a sleep.

**The decision encodes the site's terminal conditions.** `readModifyWrite` is not "retry until
landed"; it is "re-decide until the decision is `nullopt` or lands". A site whose conflict is
terminal today keeps it terminal by returning `nullopt` — or is not a `readModifyWrite` at all but a
`replace`. The inventory below says which, for every loop and every read-then-write shape the two
audits and the two reviews found:

| site | today | after |
|---|---|---|
| `CasRefCatalog::casUpdateImpl` | 100 attempts, no retry on throw, **no sleep** | `readModifyWrite`, `standard` |
| `CasRefCatalog` completed-removal deletion | 100 attempts, same shape | `readModifyWrite`, `standard` |
| `PoolMeta::admitOrValidate` | `for (;;)`, unbounded (the livelock) | `readModifyWrite`, `standard` — bounded for free |
| `PoolMeta::createOrValidate` | one `create`, then `admitOrValidate` | `create`, then the row above |
| `publishCkpt` | 100 attempts + deadline, **no sleep** | `readModifyWrite`, `standard` |
| `allocateWriterEpoch` | 100 attempts | `readModifyWrite`, `standard`; decide issues the subtree `LIST` and the sentinel probe on the absent branch, as today |
| `computeHeartbeatFloor` fence-out | 4 reclassify attempts | `readModifyWrite`, `standard`; decide = the stability classification |
| `Gc::acquireOrRenewLease` | 2 attempts, observation/steal machine | `readModifyWrite`, `standard`; decide **is** the machine: renew when ours, steal when the foreign heartbeat is frozen past the threshold the GC already tracks, `nullopt` otherwise; `CORRUPTED_DATA` on vanish-after-observe as today |
| `Gc::pulseHeartbeat` | 1 attempt, conflict ignored | `readModifyWrite`, `standard`; decide = `++seq` **only while the current owner is us**, else `nullopt` — a deposed leader must not fight the new one on a key GCS bounds to one write per second |
| `casGcMaintenanceState` | 1 attempt against a caller-supplied expected | **`replace`**, `standard` — "write if unchanged, else skip"; a concurrently advanced cursor wins, as today |
| `CasPlainObjects::casPutObject` | `HEAD` → put, 100 iterations | `readModifyWrite`, `standard`; decide reads presence only (the single-appender invariant) |
| GC round commit; GC rebuild commit | 1 attempt, conflict drops the round | **`replace`**, `standard` — no re-decide, and the verb says so |
| `MountLeaseKeeper::claim` | `head` → `putIfAbsent`, or `get` → `putOverwrite` (+ diagnostic re-`get`) | `readModifyWrite`, `standard`; two requests on both paths |
| `claimOwnerOrThrow` | read → `putIfAbsent` → re-read with corruption arms | `create`, then `read` on `Conflict` — two calls, because `Conflict` carries bytes only from the engine's resolve |
| `putDeterministicArtifact`; the outcomes log | `putIfAbsent` then `get`-and-compare-adopt | `create`, then `read` on `Conflict` — same |
| `PartWriteTxn::ensureBlobPresent` | `head` → data-plane publish → `putMetaIfAbsent` via controller | `head` under `standard`; `publish`; `create` under `standard` |
| `MountLeaseKeeper::renew` | controlled overwrite under the lease deadline | `replace`, `untilLeaseSafe` |
| `MountLeaseKeeper::terminate` (farewell) | unconditional-by-intent `If-Match` write | `replace`, `once` |
| `runCapabilityProbe` | raw calls, one attempt each | `create`/`replace`/`remove`, `standard` |

The ref catalog — the site behind the `CREATE TABLE` failures — before and after:

```cpp
// today: CasRefCatalog::casUpdateImpl
for (size_t attempt = 0; attempt < kMaxCatalogCasAttempts; ++attempt)
{
    auto got = backend.get(key);                                // a 429 throws out; no retry
    auto next = apply(mutation, decode(got));
    auto res = backend.casPut(key, encode(next), got->token);   // one attempt; a 429 throws out
    if (res.outcome == CasOutcome::Committed) return …;
    /// Conflict: loop, no sleep
}
// after
auto result = requests.readModifyWrite(key,
    [&](const std::optional<Object> & current) -> std::optional<String>
    {
        return encode(apply(mutation, decode(current)));
    },
    Retry::standard()).orThrow();
```

Anything the caller wants back it captures by reference; the primitive carries no template plumbing.
There is no separate conflict cap and no between-attempts callback (observability stays on the
existing `observe` diagnostics).

## The upstream slice {#upstream-slice}

About forty additive lines in six files under `src/IO` and `src/Disks/DiskObjectStorage/ObjectStorages/S3`,
generic, no CAS identifier, portable on its own. Three facts make it necessary, and one honest limit
bounds it.

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

**The SDK retries reads outside any CAS deadline, twice over.** `S3ObjectStorage::readObject` takes
the default client, whose retry strategy is 500 attempts at a 5 s cap; and beneath the client,
`ReadBufferFromS3::nextImpl` itself reissues up to `max_single_read_retries` (default 4) with its own
backoff, for every retryable S3 error including 429. `WriteSettings` already carries
`object_storage_retry_profile`, which `writeObject` honours by selecting the single-attempt client;
`ReadSettings` gains the same field, and `readObject`, when it is set, selects the same client **and
pins `max_single_read_retries` to one** in the request settings it hands the buffer. With that, `read`
and `write` are one physical attempt each, `readSmallObjectAndGetObjectMetadata` is exactly one
request per call, and the single-attempt client is built with `request_timeout = attempt_reservation`
so an attempt is bounded by the number the engine reserves.

**What the slice does not do, by choice.** `HEAD`, `LIST`, conditional `DELETE` and the sentinel
`GET` go through `IObjectStorage` methods that take no settings and use the default client; threading
a profile through three more interface signatures would triple the slice. They stay SDK-retried, for
three reasons that hold: none of them is lease-bound (renew uses `read` and `write` only); a
transparently retried `If-Match` `DELETE` is safe, since the precondition guarantees no newer version
is ever removed and a 404 on the reissue is the same answer as a 204 on the first; and their retry
count is bounded by configuration — the CAS disk sets `s3_retry_attempts` (a handful, not 500), which
is also what bounds the data plane. The engine's own retry stacks above the SDK's for these four; the
cost is a longer worst case on `head`/`list`/`remove`, not a safety property. The promise is stated
as it is kept: **`read` and `write` are single-attempt; `head`, `list`, `remove`, `probeSentinel` and
the data plane are SDK-retried and bounded.**

**The read bound.** `read` refuses a body larger than the compress bound of the largest control-plane
cap — `ZSTD_compressBound(max over the caps table)`, computed once beside that table — through
`readSmallObjectAndGetObjectMetadata`'s `max_size_bytes`; a manifest that would decode to 256 MiB may
legitimately be stored larger than 256 MiB, and the bound is on stored bytes.

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

The incarnation handed to `remove` came from `head`, of that key, from this `CasRequests`; the
persisted string never became one; a dialect-tagged string cannot be misread across dialects. A stale
or garbage row fails to match: GC records `Replaced` — justified by a live `HEAD`, remote evidence —
keeps the meta and drops the row; a blob touched in the window is re-condemned by the supersession
path, and a republished-then-abandoned one is reclaimed on the next publication of that content, as
today. Four sites compare a persisted token with a live one and all become render compares: GC's
redelete and `CasFsck`'s retirement check remove; the fold's supersede compare and the meta writer's
confirmation registry compare only. The cost is one `HEAD` per condemned-blob delete where today
there is one only on a mismatch; GC's delete phase is background and round-budgeted.

## The write anomaly {#write-anomaly}

`write` must return a value the grammar accepts; a response without one has nothing to return.
`runCapabilityProbe` depends on write values structurally — `t1` from step 1 is step 4's
precondition — and refuses a nameless-write store today only by accident; it will refuse it by name.
At runtime a nameless response is a store anomaly and the write **may have committed**: the façade
throws `CAS_WRITE_UNATTRIBUTED`, which `classifyConditionalWriteResult` treats as ambiguous by
default, so the engine resolves it by reading back. The fallback `HEAD` in `tokenFromWriteResult` is
deleted. Because every write now goes through `CasRequests`, there are no raw callers for this
exception to reach.

## What is promised about requests {#request-promises}

**No operation issues a `HEAD` it does not need**: `read`, `stream` and `probeSentinel` issue none;
`head` issues one; `probeSentinel` becomes one `GET`, whose 404 body distinguishes `NoSuchKey`
from `NoSuchBucket`. Reading an object's metadata is one `GET` at the caller level too:
`MountLeaseKeeper::claim` goes from two and four requests to two and two; the manifest reader's own
change (`2026-09-02-cas-manifest-cache-by-id-design.md`) lands first and independently.

**Control plane and data plane are two regimes, and the line is the nature of the request.**
Control-plane requests — everything through `CasRequests` — are small and deadline-bound, retried by
one engine; `read` and `write` on a single-attempt client, the rest SDK-retried and bounded as the
slice section states. Data-plane requests — the blob stream on the `MergeTree` read path (the body
behind `stream`) and `publish` (multipart with its own per-part retries) — are large streams where the
SDK's per-request retry is right, and stay on it. For everything on the SDK, the audit's Gap 8 is
closed by configuration: a CAS-specific `s3_retry_attempts` in place of the default 500. `stream`'s
*open* is under the caller's policy because `CasBlobInDegree` streams GC blob-target runs — a
control object — through it.

Not promised: multipart writes above `max_single_part_upload_size` are several requests; `list`
prefetches the next page.

## Assumptions {#assumptions}

**One, and it is assumed, not probed.** An incarnation names one version, and the store never reuses
one for **different** content; the capability probe does not test this, and it is a requirement on
every backend. What no contract can promise is that identical content never yields the same
incarnation — a restored copy of an old body may carry the old ETag — and a design that recognises its
own body by bytes also recognises a restored copy. This defeats `proven_dead_token` observation just as
much; it is stated here once.

## Seams, named {#seams}

- **Tests have one door: a `CasRequests` over an open fence, `Retry::once()`.** No test calls a
  `Backend` method — none can, without the key. Tests obtain incarnations by performing operations
  on `InMemoryBackend` through it; a "wrong" one for a key is that key's **previous** incarnation after
  an overwrite. Today about 1,600 direct transport calls in the CAS gtest files (`putIfAbsent` 315,
  `head` 472, `get` ≈535, `deleteExact` 99, `putOverwrite` 95, `list` 79, `casPut` 65, `getStream` 19)
  and 73 `InMemoryBackend` subclasses change with the signatures; they are counted in
  [Landing order](#landing-order) and migrate with the production sites, mostly by pattern.
- **Decorators keep working.** `InstrumentedBackend`, the two test decorators, and the new
  `ThrottlingBackend` forward the `TransportAccess` they receive. `ThrottlingBackend` answers a chosen
  retryable status on every *n*-th request of any kind; the gate under it is in
  [Verification](#verification). The fake GCS service of the integration suite gains the same mode on
  its control surface.
- **`runCapabilityProbe`** constructs three synthetic tokens today and cannot under this contract. It
  is reordered so every wrong value is the same key's stale incarnation: overwrite with `t1` first,
  then the refusal check with `t1`; on the CAS key, create and commit with `ct1`, then the stale
  conflict; the wrong-token delete with `t1`. Bodies differ, so ETags differ, and the store answers as
  today. The one production site the compiler lists at the lock that is not a mechanical migration.
- `InMemoryBackend::setEnforceTokens(false)` stays; it models the store. `EmulatedSingleProcess`'s
  `emu_mutex` comment ("process-wide") is corrected; its nonce fallback for an empty stat etag throws.
  Native-over-Local convenience fixtures move to `EmulatedSingleProcess`; the two Native fake families
  gain an override of `readSmallObjectAndGetObjectMetadata`.
- `InstrumentedBackend` counts the new operations; `CAS*Get` / `CAS*GetStream` events are retired.

## Landing order {#landing-order}

About 300 production sites — 80 `get`, 24 `head`, 38 writes, 17 deletes, 11 `forEachListedKey`, 26
default-constructed holders, 3 backend implementations, 4 codecs — plus about 1,600 test transport
calls in 57 files, 73 `InMemoryBackend` subclasses and 59 test constructions in 14 files. Five steps,
each green:

1. **Safety, inside the backend, small.** In `ObjectStorageBackend`, no caller change: the full
   minting grammar enforced at the legacy write entry points as `LOGICAL_ERROR`, the same on the
   delete path across all three backends, the grammar applied where `HEAD` and `list` mint as
   `CORRUPTED_DATA`, and the fallback `HEAD` replaced by `CAS_WRITE_UNATTRIBUTED`. Under a hundred
   lines; after it no clobber path is left. Two existing tests change (`CASBackend.NullBackendShapeAndDefaults`
   here; `NativeRejectsWrongDialectTokenBeforeTouchingTheWire` at step 5).
2. **The upstream slice**, one commit, the three facts and the pins above; `IO/` and `ObjectStorages/S3/` only.
3. **`CasRequests`, `Retry`, `WriteResult` and `TransportAccess` beside the old, independent of it.**
   The new engine is today's controller's algorithm with three changes — jitter, reads, the deadline
   as the only bound — under the new verbs, minting from the transport's strings. The old
   controller is **not** rewired onto it: delegation would need a `Token`→`Incarnation` conversion,
   which is the door, so two engines coexist for the length of the migration and the old one is
   deleted whole at step 5. The `Backend` interface gains the new string-and-key signatures here, and
   the three production backends implement both sets until step 5 (the raw methods are thin; the
   duplication is a few dozen lines for one migration window). `ThrottlingBackend` and its gate land
   here, **before** the migration, so the migration runs under it.
4. **Migration, file by file, every site naming its policy**, tests with their production file:
   80 `get` → `requests.read(…, Retry::…)`, 24 `head`, 38 writes → `create`/`replace`/`readModifyWrite`
   (the inventory's loops collapse), 17 deletes, 11 `forEachListedKey`; the test files by pattern
   (`backend.putIfAbsent(k, v)` → `requests.create(k, v, Retry::once())`). The `read`-or-`stream`
   decision per site is already tabulated by the review of revision 1. Mechanical, delegated,
   reviewed per file, gate green after each.
5. **The lock.** The old `Backend` signatures, the old controller and its five entry points, `Token`,
   `mintingTypeMatches`, the fallback `HEAD` and `CasRequestBudget`'s two dead fields are deleted;
   the `static_assert`s go in. The compiler lists what was missed.

Unconstructibility and uncallability are properties of step 5; key binding and the three-outcome
result of step 3 for every migrated site. Until then the safety rests on step 1 — what is needed
immediately — and the retries on step 3.

## Verification {#verification}

**Compile time.** The three `static_assert`s; no valued `Incarnation` construction outside
`CasRequests`; a `Backend` method called from anywhere without a `TransportAccess` does not compile,
including from a test and from code holding the concrete backend type; a `CasRequests` call without
a `Retry` does not compile.

**No site without a retry, executably.** Under `ThrottlingBackend` answering 429 on every *n*-th
request, the pool-level scenario tests — `CREATE TABLE`, `INSERT`, `DROP`, `RENAME`, a writable mount
at open (probe included), a GC round — all succeed, and every key touched shows at least one reissue.
This is the audit as a test: a call under `Retry::once()` where `standard` was meant turns the gate
red in seconds.

**The lease, on fake clocks.** Under sustained 429, `untilLeaseSafe` starts its last attempt before
`lease_deadline − margin − attempt_reservation` and issues **zero** requests after it; a renewal whose
resolve `GET` is throttled terminates by the deadline rather than hanging; the single-attempt client
is constructed with `request_timeout == attempt_reservation`.

**The deadline is the only bound.** Under zero-latency 429s, `standard` keeps reissuing until the
ninety-second deadline and gives up with `GaveUp{Deadline}`, never earlier; `once` sends one request
and no sleep.

**Backoff as a distribution.** Over a thousand series on fake clocks, `sleep ∈ [0, min(cap, base·2ⁿ)]`
with a mean near half the ceiling; without jitter the test is red.

**The write result.** A write whose only attempt was refused by the fence before sending reports
`GaveUp{sent_any = false}`; one that went ambiguous and then hit the deadline reports
`GaveUp{sent_any = true}`; `commitRefChunk` releases the transaction id on the first and wedges on the
second — the existing ref-lane tests, re-pointed at the field. `Conflict::occupant` is set only after
a resolve read: a 412 without a resolve carries `nullopt`.

**The fence generation.** A call admitted under generation *g*, with the fence re-armed to *g+1* and
open before its second attempt, ends in `GaveUp{FenceLost}` without sending — the invariant
`casPutObject` holds by hand today, now on every site.

**`readModifyWrite` under contention.** Two threads incrementing one counter through it on
`InMemoryBackend`: the total is the sum, every conflict is followed by a sleep, and a key in perpetual
conflict ends in `GaveUp{Deadline}` — the bound `PoolMeta` lacks today. The heartbeat decide declines
when the owner has changed: a deposed leader issues no second write.

**Identity.** Key binding: `head("a")` applied to `replace("b", …)` throws before any request. Minting
refuses empty, `*`, a comma list, `00123`, `0`, and a generation still quoted after the strip.
Persisted compare: a matching row removes; a stale, garbage or foreign-dialect row does not, the round
completes, `CasInspect` renders both. The write anomaly: a fake S3 client returning no ETag on
`PutObject` makes the probe refuse the pool by name and a mounted pool throw
`CAS_WRITE_UNATTRIBUTED`, resolved by read.

**Reads.** `read`, `stream`, `probeSentinel` issue zero `getObjectMetadata` calls; `head` one. `read`
never mixes — a scripted `GetObject` on the fake S3 client serving one body then failing then serving
another with a different ETag makes `readSmallObjectAndGetObjectMetadata` throw. On GCS a marked `GET`
returns the generation and `read` mints it. Under the single-attempt profile a throttled `GetObject`
produces exactly one request at the fake client — `max_single_read_retries` pinned. `read` refuses a
body above the bound.

**Existing gates.** The CA-s3 lane, the `CAS*` gtest gate and the upstream slice's own tests green at
every step; `Cas::Probe` passes on every supported writable store.

## Acceptance {#acceptance}

1. Step 1 lands first with its isolated tests.
2. The upstream slice lands as one commit, under a hundred lines, `IO/` and `ObjectStorages/S3/` only,
   no CAS identifier.
3. After step 5: the three `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`,
   `casPut`, `deleteExact`, `Token`, `mintingTypeMatches`, the old controller and its five entry points,
   the fallback `HEAD` no longer exist; no `Backend` method is callable without a `TransportAccess`;
   every `CasRequests` call names a policy; no test calls a `Backend` method.
4. The throttling gate is green with a reissue observed on every touched key, the probe included; the
   audit's twenty-three sites are all under `CasRequests`.
5. `untilLeaseSafe` issues no request past `lease_deadline − margin`; `standard` gives up only at its
   deadline; the jitter test is red without jitter; `readModifyWrite` loses no increment under
   contention and bounds a hot key; the fence-generation test passes on every write verb.
6. `GaveUp::sent_any` drives the ref lane's wedge decision; `Conflict::occupant` is set only from a
   resolve read.
7. `read` and `write` are one physical attempt each; no needless `HEAD`; `read` throws on drift and on
   the bound; no string becomes an `Incarnation` anywhere; `remove` never reports `Mismatch` for any
   reason other than the store's own.
8. Existing lanes and gates green at every step.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object renews
a mount lease. The collision is real and not the comment's fault — CAS lives inside
`ReplicatedMergeTree`, whose commit path goes through ClickHouse Keeper, so `CasRequestControl.h:250`
must name the real Keeper when it rules out a `Coordination::Exception` collision. The name is
`MountLeaseRenewer`; 101 occurrences in 6 non-test files, 99 in tests; mechanical, its own commit.

## Out of scope {#out-of-scope}

**Azure**, when it becomes a writable CAS store: the same slice shape for its read buffer and override.

**A retry profile for `HEAD`, `LIST` and `DELETE` at the `IObjectStorage` level** — three more
interface signatures — if the bounded SDK retry on those four ever proves insufficient. The
throttling gate would say so.

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
generation, the backend-identity definition — and is the identity half of this document in substance,
with one change below.

**Revision 5** added the retry half and was reviewed twice. It locked the transport with private
virtuals, which do not bind a public override and leave no decorator compilable; it let every
subclass mint; it returned `std::expected<Incarnation, Conflict>` from a write whose third outcome the
ref lane and the renewer consume as protocol; it kept a sixteen-attempt ceiling that under real
throttling binds at half a minute and called the deadline the bound; it claimed every control-plane
request became one physical attempt when `ReadBufferFromS3` retries four times beneath the client and
`HEAD`/`LIST`/`DELETE` never leave the default one; it dropped revision 4's read bound; it put the
probe and the farewell under policies that defeat their purpose; it counted 300 sites and missed
1,600 in tests; and it had the old controller delegate to the new engine across a conversion the
contract forbids. Revision 6 keeps the shape — one minted key-bound incarnation, a transport nobody
else can call, one façade whose every call names a policy, one engine, one primitive — and changes
the mechanisms: a key instead of access specifiers, strings on the transport side, one three-outcome
result, a deadline-only policy with a reserved attempt, a fence generation pinned at entry, a slice
that promises what it keeps, and two engines side by side until the lock.

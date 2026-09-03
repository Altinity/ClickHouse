---
description: 'Design for a CAS request contract that cannot be misused: a transport that deals in strings and is callable only with a key that one class can make, one façade that mints key-bound Incarnations and whose every call names its retry policy, one deadline-bound engine for reads and writes, one three-outcome write result, and a read-modify-write primitive that replaces the hand-rolled loops.'
sidebar_label: 'Backend request contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend request contract'
doc_type: 'guide'
---

# CAS backend request contract {#cas-backend-request-contract}

Revision 8. Revision 5 folded the retry half into the identity contract; revisions 6 and 7 answered
three reviews of it; this revision answers the review of revision 7 (nineteen findings, two that
changed the design: a write result with no alternative for a definite refusal, and an admitted fence
generation the engine read for itself instead of taking from the operation that was admitted). [What earlier
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
`HEAD` does plus the body; `MountLeaseKeeper::claim` pays two requests on its mint path and three on a
successful adopt; `GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by nobody;
`probeSentinelRaw` is a raw `HEAD`, then `get`'s `HEAD`, then a `GET`.

## The contract {#the-contract}

> **An `Incarnation` is minted only by `CasRequests`, from a transport response, of one key, and names
> exactly one version.** Nothing else can produce one, and nothing can apply it to another key.
>
> **`Backend` is transport: one physical request per call, strings in and strings out, and no call can
> be made without a `TransportAccess` that only `CasRequests` can construct.** No production code and
> no test calls the store directly. Four capability predicates on `Backend` are not transport and are
> named as such.
>
> **Every `CasRequests` call names its retry policy.** There is no default, so a call that has not
> thought about retry does not compile.
>
> **Every request belongs to an admitted operation.** The caller is admitted once — under the fence
> generation of that moment, or under a generation it recorded earlier — and every request of the
> operation carries that admission; the engine re-checks it, and the remaining budget, before every
> attempt and every sleep. A re-arm between admission and request is a refusal, never a pass.
>
> **One engine serves reads and writes alike** — exponential backoff with full jitter, a deadline that
> is the only bound, an ambiguous write resolved by an exact read before reissue. Under a lease-bound
> policy no attempt or sleep that would end past the lease is started.
>
> **A write reports what happened and what was seen, and never throws for a store answer.**
> `Committed`, `Declined`, `Conflict`, `Refused`, `GaveUp`; `Conflict` and `GaveUp` carry an
> `Observation` that distinguishes "nothing observed" from "proved absent" from "someone else's
> object"; `Refused` is the store's proof that the write never applied; `GaveUp` says whether anything
> was sent — because the ref lane's three arms and the renewer's "vanished" verdict hang on exactly
> those bits.

## One type {#one-type}

```cpp
class Incarnation;   // opaque; operator==; created only by CasRequests; no default constructor
                     // privately {backend id, key, dialect, value}; render() -> "dialect:value"
```

The word already lives in the code. It is not `Token`, because it is not something the caller
*sends*; it is something the caller *saw* — of one key, from one backend. A `CasRequests` call given
an incarnation of another key or another `CasRequests` throws `LOGICAL_ERROR`: a programming error,
not a store answer. "Backend identity" is a per-instance counter id, never the address. A holder
that keeps an `Incarnation` across calls — `MountLeaseKeeper::last_token`, the mount observation map,
GC's manifest-cleanup map, `CasBlobInDegree`'s condemned rows — must not outlive the `CasRequests`
that minted it; all of them are owned by the `Pool` that owns the backend.

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
    virtual void                   publish(BlobPublishRequest, TransportAccess &) = 0;             // data-plane, unconditional

    // capability predicates, not transport: answered from configuration or one preflight request;
    // defaults kept as today (four test classes derive from Backend directly and rely on them)
    virtual bool supportsListTokens() const = 0;
    virtual void checkPoolPreconditions();                // the one that issues a request (bucket versioning)
    virtual void checkSkipAccessCheckSupport();
    virtual void checkConditionalWriteSingleAttemptSupport();
};
struct RawConflict {};   // the store refused the precondition; 404 and 412 on an If-Match write collapse here,
                         // and the façade's resolve read is what tells them apart
```

`Raw` carries bytes and the store's value and nothing else: `ObjectMeta` (the owner triple no
production caller supplies or reads) and the `Range` argument of `get`/`getStream` (every production
caller uses the default window) are retired with their contract tests. The four predicates are
outside the transport contract: three answer from configuration; `checkPoolPreconditions` is
`runCapabilityProbe`'s step 0 and issues one bucket-versioning request on the SDK path, bounded like
the rest of that path by the CAS disk's `s3_retry_attempts`.

Why a key argument and not private virtuals: C++ applies access control to the *call* through the
*static type*, so a private virtual in `Backend` leaves every public override on `InMemoryBackend` and
`ObjectStorageBackend` callable by anything holding the concrete type, and it leaves no room for a
decorator — `InstrumentedBackend`, the throttling seam, and the two test decorators that forward
`inner->get(...)` today would not compile. A key closes both: no static type can call without one,
and a decorator forwards the key it was handed, so decorators keep working with no second friend.
Stated honestly: the guarantee is that **no code outside a transport implementation** can call a
transport method. An override or decorator is handed the key by reference and could store its address
for later; nothing detects that, and it is the same trust a transport implementation is given today.
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
but one can name what it wrote, and no consumer exists) and takes the key like every other method;
the three unconditional mutations on mount-owned staging keys live outside this API by design.
`stream` and `publish` are the data-plane exceptions to "one physical request": the SDK retries
inside a body stream and inside a multipart upload, and that is right for them; what the façade's
policy governs is the *initiation*.

**`CasRequests` is the only caller.** It is constructed with a **fence**, because the fence is a
property of whoever owns the lease, not of a call: the mount plane passes the mount fence, the GC
plane an open fence (its writes are guarded by the `gc/state` incarnation, not by a mount lease, as
today), the tools an open fence (`CasDecommission` says so explicitly today). The fence has one
non-throwing admission predicate:

```cpp
struct Fence
{
    uint64_t generation() const;
    enum class Admit { Ok, LostOrRearmed, NoBudget };
    Admit admit(uint64_t admitted_generation, uint64_t needed_ms) const;   // never throws
    void  checkOrThrow(uint64_t admitted_generation) const;                // for the sites that want the exception
};
```

Today the two predicates are disjoint and neither is the one an engine needs: `checkFenceOrThrow`
knows the generation and can only throw; `refAppendFenceOk` knows the budget and never compares the
generation. `admit` is both.

**The admitted generation is the operation's, not the engine's.** Three sites say so in their own
words today: `ensureBlobPresent` captures the generation once and carries it across a `HEAD`, a
publish and a create ("this generation belongs to the operation, not to one attempt; an outer retry
after ambiguous I/O must not adopt a re-armed incarnation"); `publishCkpt` takes it as a parameter
that its callers read from **persisted runtime records** (`append_attempt->admitted_fence_generation`,
the wedge's); `resolveWedgeOnce` states the negative ("never the CURRENT generation: a retry that
passes because the mount was re-armed is a write from an incarnation that never admitted this
transaction"). An engine that read `generation()` when a verb was entered would read the current one
and pass exactly the check those sites exist to fail. So the verbs are not on `CasRequests`; they are
on an **operation handle** the caller obtains at admission and keeps for the operation's length:

```cpp
class CasRequests
{
public:
    CasRequests(BackendPtr, Fence);
    CasOperation admit();                               // admitted now, under generation()
    CasOperation resume(uint64_t admitted_generation);  // admitted earlier; the generation came from a runtime record
};

class CasOperation                                      // move-only; every request re-checks its admission
{
public:
    std::optional<Object>       read  (key, const Retry &);
    std::optional<Meta>         head  (key, const Retry &);
    ListPage                    list  (prefix, cursor, limit, const Retry &);
    void                        forEachListedKey(prefix, Fn, const Retry &);
    Removal                     remove(key, Incarnation seen, const Retry &);
    Removal                     removeCurrent(key, const Retry &);           // head → remove(seen), re-head on Mismatch
    SentinelProbe               probeSentinel(key, const Retry &);
    std::unique_ptr<ReadBuffer> stream(key, const Retry &);                  // the OPEN is under policy; the body is the SDK's
    void                        publish(BlobPublishRequest, const Retry &);  // the INITIATION is under policy

    WriteResult create (key, bytes,                   const Retry &);
    WriteResult replace(key, bytes, Incarnation seen, const Retry &);
    WriteResult readModifyWrite          (key, DecideOnObject, const Retry &);   // reads the body
    WriteResult readModifyWriteOnPresence(key, DecideOnMeta,   const Retry &);   // HEAD only; one site
};
```

Before every attempt and every sleep the engine calls `fence.admit(op.generation, sleep +
reservation)`: `LostOrRearmed` is `GaveUp{FenceLost}`, `NoBudget` is `GaveUp{Deadline}` with the lease
as its source. The generation is bumped by every loss *and every re-arm*, so an operation admitted
under a prior lease incarnation gives up even when the fence is currently open — and now
`casPutObject`, `publishCkpt`, `ensureBlobPresent` and the wedge retry all get it from the handle
instead of from four hand-written `check_fence_or_throw(admitted)` calls. The GC plane and the tools
`admit()` against an open fence, whose generation never moves. Sites that want the typed transient
exception with the operator message call `orThrow()`, which maps `FenceLost` back onto it.

```cpp
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal { Removed, Gone, Mismatch };                       // a delete marker throws
```

The three write verbs are the three shapes the audit found, named by what the caller holds:
`create` holds bytes and a claim to be first; `replace` holds bytes and an observation;
`readModifyWrite` holds only a decision. `removeCurrent` is the delete-shaped sibling of
`readModifyWriteOnPresence`, for `casRemoveObject`'s `head` → `deleteExact` loop — the one hand-rolled
no-sleep loop that would otherwise survive. Today's five controller entry points go;
`resolveByExactGet` is the engine's own resolve step, not an entry point.

## One write result {#write-result}

```cpp
struct NotObserved {};                 // no resolve read was made (a 412 without a resolve, a refused admission)
struct ProvenAbsent {};                // the resolve read completed and found no object
using Observation = std::variant<NotObserved, ProvenAbsent, Meta, Object>;   // Meta from a presence-only resolve, Object from a body read

struct Committed { Incarnation incarnation; uint32_t attempts_sent; bool resolved_by_read; };
struct Declined  { Observation seen; };                      // readModifyWrite only: decide returned nullopt
struct Conflict  { Observation seen; };
struct Refused   { int store_error; String message; };      // the store proved this write never applied (the definite-failure whitelist)
struct GaveUp
{
    enum Why { Deadline, FenceLost, Cancelled, Unresolved } why;
    enum Source { Policy, Lease } deadline_source;           // which bound of untilLeaseSafe produced Deadline
    bool sent_any;
    Observation last_seen;
};
using WriteResult = std::variant<Committed, Declined, Conflict, Refused, GaveUp>;
std::optional<Incarnation> orThrow(WriteResult &&);          // Committed → its incarnation; Declined → nullopt; the rest throw, table below
```

`orThrow` throws, per alternative: `Conflict` → `ABORTED` with the observation rendered;
`Refused` → the store's own error code and message (today's `S3_ERROR` text); `GaveUp{Deadline}` and
`GaveUp{Unresolved}` → `throwCasWriteRetryLater` (`NETWORK_ERROR`, the retry-later class today's
budget exhaustion uses); `GaveUp{FenceLost}` → `throwCasTransientUnavailable`;
`GaveUp{Cancelled}` → `ABORTED`. Sites whose non-convergence is a corruption verdict today
(`allocateWriterEpoch`) keep their own `switch`.

A controlled write has three outcomes today, and the third is consumed as a **protocol fact**, not a
diagnostic: `CasRefLedger::commitRefChunk` releases the transaction id only when the unresolved
reason proves nothing was sent and wedges otherwise; the recovery walk and the wedge retry act on the
same bit; `MountLeaseKeeper::renew` classifies `Committed` / `Conflict` / `NotAttempted` / `Vanished`
from the controller's fields. `std::expected<Incarnation, Conflict>` could express none of it and
would have collapsed "gave up" into an exception — precisely the bare-`Unresolved` collapse the ref
lane guards against. `sent_any` is the bit the wedge decision needs (today's
`unresolvedProvesNothingWasSent`, whose header says adding a member is a protocol decision — it stays
a protocol decision, now a field).

**The observation is a tri-state, not an optional.** `MountLeaseKeeper::renew` consumes
`resolve_observation_completed` on two paths: a conflict *without* an authoritative resolve is a
retry-later transient (`NETWORK_ERROR`), a conflict *with* a resolve that found nothing is
`Vanished`, terminal, fail closed. Two optionals would make "no observation" and "observed absent"
the same value, and getting them backwards either fails a healthy mount closed or keeps a mount
writing after a successor deleted its slot — the two errors this branch exists to keep apart. The
same tri-state is `slotOccupy`'s `Occupied` (an `Object` from the resolve read, and only from it),
`readModifyWrite`'s "404 on `replace` means the key vanished", and the `Why::Unresolved` case below.
It bends a rule revision 4 stated — "a `Conflict` carries no incarnation" — which is right for the
**write's own response** (today's `PutResult{PreconditionFailed, {}}` was exactly how empty tokens
entered circulation) and does not apply to an object the engine *read*.

**`Refused` is the third arm the ref lane already has.** `classifyConditionalWriteResult`'s
definite-failure whitelist (malformed request, entity too large, access denied) is returned as a
value by `putIfAbsentControlled`, and `commitRefChunk` switches on it: `DefiniteFailure` returns the
append attempt to `Ready` and reports the transaction id never used — distinct from the
`sent_any = false` arm and from the wedge. `stageManifest` compares against it too. Folding it into
`GaveUp{Unresolved}` would set `sent_any = true` and **wedge the lane** on an access-denied; folding
it into `sent_any = false` would be the lie `DefiniteFailureAfterAmbiguity` exists to prevent
(a definite refusal after an earlier ambiguous attempt of the same call proves nothing about that
attempt, and is resolved by read — the engine's rule, unchanged); throwing would turn a `switch` into
a `catch` at two sites. So it is an alternative, and the write surface stays exception-free.

**`Why::Unresolved`** is the outcome `slotOccupy` reports as `AttemptsExhausted` today: the create
was ambiguous or admission was lost after it, and the resolve read found nothing — no deadline
passed, no fence was lost, nothing was cancelled, and the only honest label is that it is
unresolved, with `sent_any = true` and `last_seen = ProvenAbsent`. **`Committed`** carries
`attempts_sent` and `resolved_by_read`, and **`GaveUp`** its `deadline_source`, because the mount
runtime's two counters (`CASMountRenewalRecovered`, `CASMountRenewalDeadlineExceeded`) are computed
from exactly those, and an operator reads the second to learn whether the lease or the policy bound
ended the renewal.

`create`'s content-addressed corruption verdict (a different occupant at a content-addressed key is
`CORRUPTED_DATA` in `putIfAbsentControlled`, a plain `Conflict` in the mutable variant) moves to the
two callers that own the key's meaning — the ref lane and `stageManifest` — which compare the
occupant and throw.

## Retry {#retry}

```cpp
struct Retry
{
    uint64_t deadline_ms;         // absolute, boot clock; the one bound
    bool     single_attempt;      // once(): one write attempt, no reissue, no sleep; the resolve read is not an attempt

    static Retry within(uint64_t ms);                           // the primitive: now + ms
    static Retry standard();                                    // within(90 s)
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms);    // min(standard, lease - safety margin)
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

**`once` means one write attempt.** The resolve read that follows an ambiguous or refused write is
part of settling that one attempt, not a second attempt: `slotOccupy`'s contract — `Created` costs
one request, `Occupied` costs two (the create, then the resolve `GET`) — is exactly `create` under
`once`, and `Conflict{Object}` is only reachable because the resolve read is made.

**An attempt is reserved before it is started, and the transport honours the reservation.** The
engine starts no attempt and no sleep unless `now + sleep + attempt_reservation ≤ deadline`, and the
reservation is the request timeout the single-attempt client is built with. Today neither half
holds: the controller budgets 5 s "as a scheduling estimate only" while the cloned client keeps the
disk's 30 s transport timeout, so a single hung attempt can outlive `lease − margin`. The carrier is
the same door the profile already opened: `object_storage_attempt_timeout_ms` beside
`object_storage_retry_profile` in both settings headers, and `getSingleAttemptClient(request_timeout_ms)`
keyed on `(base client, timeout)` — generic, no CAS identifier, in the slice. On the CAS side
`attempt_timeout_ms` and `lease_safety_margin_ms` **become parsed disk settings** with today's values
as defaults; today they are compiled-in struct defaults that production never assigns.
`CasRequestBudget` keeps those two and the recovery walk's three (`recovery_retry_budget_ms`,
`recovery_retry_initial_backoff_ms`, `recovery_retry_max_backoff_ms`) and loses
`operation_deadline_ms` and `max_attempts`; its three non-controller consumers of the deadline — the
two `CkptDeadline` constructions in `CasRefLedger` and `renew`'s `request_deadline` — become
`Retry::standard()`. `validateCasRequestBudget` keeps `attempt_timeout + margin < TTL`, on which the
successor's observation threshold rests, and loses its `attempt_timeout < operation_deadline` clause
with the field. `refAppendFenceOk` and the detached-drain deadlines read the survivors as before.

**Backoff is exponential with full jitter**: `sleep = uniform(0, min(cap, base · 2ⁿ))`, base 200 ms,
cap 5 s — the shape AWS and Google Cloud Storage both document. Today's engine has the exponent and no
jitter, so synchronized retries from many threads land in the same second under throttling; the
jitter is the one change to the algorithm. `Retry-After` is not honoured: neither `S3Exception` nor
`PocoHTTPClient` surfaces the header, and reading it would widen the upstream slice for a value the
jittered cap already approximates.

**What retries**, one classification for reads and writes: 429, 408, 5xx (`SlowDown`,
`InternalError`, `ServiceUnavailable` included), connection loss, client timeout, and `ExpiredToken`
once after the credential refresh the slice makes effective on the read path (below). **What does not**: 404 (an
answer), 412 (`Conflict`, an answer), the existing whitelist of definite failures (malformed request,
entity too large, access denied) in `classifyConditionalWriteResult`, which stays and becomes
`Refused`, and the deterministic local set `isDeterministicLocalFailure` names today —
`LOGICAL_ERROR`, `NOT_IMPLEMENTED`, `BAD_ARGUMENTS`, `CORRUPTED_DATA` — which is rethrown at once,
because reissuing replays the same failure and buries the root cause; this design adds two producers
of that set (a minting failure is `CORRUPTED_DATA` naming the key, an incarnation of the wrong key is
`LOGICAL_ERROR`), so the rule matters more, not less.

**The verdict is the call's, not the last attempt's.** A definite rejection or a 404 that follows an
earlier *ambiguous* attempt of the same call proves nothing about that earlier attempt — it may have
landed — so the engine resolves by exact read before it reports `Conflict`, exactly as the controller
rules today. A 404 on an `If-Match` write inside `readModifyWrite` means the key vanished: re-read,
and `decide` sees absence.

**An ambiguous write is resolved, not repeated**: an exact read under the same policy and the same
deadline, byte comparison, and only then a reissue — the engine's existing `resolved_by_get`,
unchanged under the new verbs. A read is idempotent and needs no resolve; a retried `read` is a fresh
read with its own first ETag, so the drift check is undisturbed.

**Four policies, because there are four real differences.** (A policy names a call; a site with a read
and a write names two, as the inventory does.) `standard`: everything that waits for
a result and can afford ninety seconds — catalog, `_ckpt`, table files, the GC plane, the mount at
open, and **the capability probe**: a 429 is not an answer to "does the store enforce the
precondition", a 412 is, so the probe's steps retry like any other request and a writable mount at
open succeeds under throttling. `untilLeaseSafe`: renew, and every read *inside* it — nothing else.
`once`: the `slotOccupy` callers, which drive their own outer loop, and the GC heartbeat pulse,
which is periodic by nature and re-pulses on cadence. `within(10 s)`: the **farewell** at shutdown
— a certificate of convenience (the clean marker lets `claimMount` reclaim without the ~36.5 s
observation), harmless past the lease, worth a few seconds of a shutdown and not ninety; a
lease-bound policy would *skip* it after a failed renewal, which is the case where it matters most,
and a single attempt would forfeit it to one 429 on exactly the throttled store where the successor's
wait is expensive. A fifth shape would be a fifth named constructor, never a parameter.

**The lease guarantee, mechanically.** `untilLeaseSafe` starts no attempt and no sleep that would end
past `lease_deadline − margin`, with the reservation above as the premise — the check
`pauseBeforeReissue` already makes, now with a reservation the transport honours and with the fence's
`admit` carrying the same arithmetic for the mount's own margin. The renewal's
resolve `GET` used to live in the SDK's own loop outside this deadline — the amplifier in the
audit's Gap 7 — and now lives under it, sharing the write's deadline. `mountObservationThresholdMs`
is unaffected: its argument rests on `attempt_timeout + margin < TTL` and on each `read`/`write`
attempt being bounded, and the reservation is what makes the second premise true for the two verbs
renewal uses. The SDK-retried verbs (`head`, `list`, `remove`, `probeSentinel`) have their own stated
envelope — `s3_retry_attempts × request_timeout + their backoff sum` — and it is **that** number the
engine reserves for them in `fence.admit`, so a fence-gated `HEAD` in `casPutObject` or a `remove` in
`casRemoveObject` is bounded by arithmetic, not excused.

## `readModifyWrite` {#read-modify-write}

```cpp
/// decide sees the current object (or absence) and returns the bytes to write, or nullopt: nothing to do.
/// It may consult the caller's state and may issue reads of its own through the same CasRequests.
using DecideOnObject = std::function<std::optional<String>(const std::optional<Object> & current)>;
using DecideOnMeta   = std::function<std::optional<String>(const std::optional<Meta>   & current)>;   // presence only
```

Two verbs, one loop. `readModifyWriteOnPresence` reads by `HEAD`, never reports an `Object` in its
observations (a `Meta` instead), and has one site — `casPutObject`, whose bytes the caller froze before
the loop — so that "no operation issues a request it does not need" holds in this direction too. The loop, once for everyone: read under the policy → `decide` → `nullopt` is
`Declined{seen}` (nothing to write; the observation is returned so the caller can say what it saw);
otherwise `replace` against what was read, or `create` if nothing was; `Committed` returns;
`Conflict` sleeps with jitter and re-reads; an ambiguous write is resolved by exact read — our bytes
are `Committed`, another's are a conflict; a 404 on the `replace` is a vanish: re-read, and `decide`
sees absence; deadline, fence or cancellation are `GaveUp` with `sent_any` and the last observation. Conflicts spend the same budget as errors: a hot key is also a
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
| `CasRefCatalog::deleteCompletedRemoving` | 100 attempts; a failed `casPut` is stored, a **mandatory complete catalog read** follows, and the exception is rethrown unless that read proves the row gone or changed; a `Committed` whose read still shows the old row is retry-later; the read also produces the caller's two result values | stays hand-written over `read` + `replace` under `standard`: its authority is the post-write read, not the write response, which `readModifyWrite` returns on. Three exits: `EntryChanged` from the read, proven-gone from the read, retry-later |
| `PoolMeta::admitOrValidate` | `for (;;)`, unbounded (the livelock) | `readModifyWrite`, `standard` — bounded for free |
| `PoolMeta::createOrValidate` | one `create`, then `admitOrValidate` | `create`, then the row above |
| `publishCkpt` | 100 attempts + deadline, **no sleep** | `readModifyWrite`, `standard` |
| `allocateWriterEpoch` | 100 attempts | `readModifyWrite`, `standard`; decide issues the subtree `LIST` and the sentinel probe on the absent branch, as today |
| `computeHeartbeatFloor` fence-out | 4 reclassify attempts | `readModifyWrite`, `standard`; decide = the stability classification |
| `Gc::acquireOrRenewLease` | 2 attempts, observation/steal machine | `readModifyWrite`, `standard`; decide **is** the machine: renew when ours, steal when the foreign heartbeat is frozen past the threshold the GC already tracks, `nullopt` otherwise; `CORRUPTED_DATA` on vanish-after-observe as today |
| `Gc::pulseHeartbeat` | `get` + 1 `casPut`, `owner = gc_id`, conflict ignored | `read` under `standard`, then `replace` (or `create` when absent) under **`once`**, `owner = gc_id` as today; conflict terminal, the next pulse comes on cadence. The discriminator against a deposed leader fighting is the **lease**, which `heartbeatLoop` already checks (`i_am_leader`) before every pulse — not the heartbeat's owner field, which a new leader must be able to take over |
| `casGcMaintenanceState` | 1 attempt against a caller-supplied expected, `nullopt` when absent | `create` on absence, else **`replace`**, `standard` — "write if unchanged, else skip"; a concurrently advanced cursor wins, as today. The one write made from inside a `catch (...)` before rethrow is `once` |
| `CasPlainObjects::casPutObject` | `HEAD` → put, 100 iterations, **no sleep** | `readModifyWriteOnPresence`, `standard` (the single-appender invariant) — still a `HEAD`, never a body |
| `CasPlainObjects::casRemoveObject` | `HEAD` → `deleteExact`, 100 iterations, **no sleep**, `ABORTED` on exhaustion | `removeCurrent`, `standard` |
| GC round commit; GC rebuild commit | 1 attempt, conflict drops the round | **`replace`**, `standard` — no re-decide, and the verb says so |
| `claimMount` / `claimMountAwaitingExpiry`; `MountLeaseKeeper::claim`; `claimOwnerOrThrow` | one shape, three sites: read → conditional write, **conflict terminal** (`LiveDoubleStart` "never overwrite a slot that appeared under us"; `claim`'s refusal re-reads once then throws `MountFencedException` or `ABORTED`; `claimOwnerOrThrow`'s re-read has corruption arms) | `read`, then `create` or `replace`, `standard`; never `readModifyWrite` — the conflict is the verdict, and its `Observation` is the re-read the sites make today. `claim`'s successful adopt goes from three requests (`head` + `get` + `putOverwrite`) to two |
| `putDeterministicArtifact`; the outcomes log | `putIfAbsent` then `get`-and-compare-adopt | `create`, `standard`; compare-adopt on the `Conflict`'s `Object` |
| `PartWriteTxn::ensureBlobPresent` | `head` → data-plane publish → `putMetaIfAbsent` via controller | `head`, `standard`; `publish`, `standard`; `create`, `standard` |
| `MountLeaseKeeper::renew` | controlled overwrite under the lease deadline | `replace`, `untilLeaseSafe` |
| `MountLeaseKeeper::terminate` (farewell) | `If-Match` write; on refusal a re-`get`, silent when `gc_fenced`, else `CASMountExclusivityViolation` + `ABORTED` | `replace`, `within(10 s)`; the refusal's `Observation` is the re-read |
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
auto op = requests.admit();
auto result = op.readModifyWrite(key,
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

Under a hundred additive lines in nine files — `IO/ObjectStorageRequestMode.h` (new),
`IO/WriteSettings.h`, `IO/ReadSettings.h`, `IO/ReadBufferFromS3.{h,cpp}`, `S3ObjectStorage.{h,cpp}`,
and the two includers that name the moved enum — under `src/IO` and
`src/Disks/DiskObjectStorage/ObjectStorages/S3`, generic, no CAS identifier, portable on its own. Four
facts make it necessary, and one honest limit bounds it.

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
pins `max_single_read_retries` to one** in the request settings it hands the buffer. Both settings
headers also gain `object_storage_attempt_timeout_ms`, and `getSingleAttemptClient(request_timeout_ms)`
builds and caches the clone per `(base client, timeout)`. With that, `read` and `write` are one
physical attempt each, `readSmallObjectAndGetObjectMetadata` is exactly one request per call, and an
attempt is bounded by the number the engine reserves.

**Pinning the read to one attempt breaks credential rotation unless the storage learns of it.**
`ReadBufferFromS3::processException` refreshes an expired token into the **buffer's own** client and
returns "retry"; with one attempt the buffer rethrows and the refreshed client dies with it, and
`readObject` builds the next buffer from the storage's unchanged client — so the engine's one
permitted `ExpiredToken` reissue would hit the same stale credentials until the deadline. (No CAS `head` recovers it
either: `tryGetObjectMetadataImpl` has no catch.) `readObject` has returned by the time the buffer
refreshes, so the carrier is the `credentials_refresh_callback` the storage already hands the buffer:
under the single-attempt profile it installs the refreshed client into the storage, so the engine's
reissue sees it. This is the fourth fact, and the one the previous revision's claim of "one attempt"
would have regressed.

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
auto op = requests.admit();          // the GC plane: an open fence
auto meta = op.head(blob_key, Retry::standard());
if (meta && meta->incarnation.render() == persisted)
    op.remove(blob_key, meta->incarnation, Retry::standard());
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
`head` issues one; on the Native path `probeSentinel` becomes one `GET`, whose 404 body
distinguishes `NoSuchKey` from `NoSuchBucket` (the emulated path keeps its container stat, because a
bare local `GET` cannot tell "key absent" from "pool directory gone"). Reading an object's metadata is one `GET` at the caller level too:
`MountLeaseKeeper::claim` goes from two and three requests to two and two; the manifest reader's own
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
  an overwrite. Today about 1,720 direct transport calls in the CAS gtest files (`putIfAbsent` 315,
  `head` 472, `get` ≈535, `deleteExact` 99, `putOverwrite` 95, `list` 79, `casPut` 65, `publishBlob` 30,
  `getStream` 19), 73 `InMemoryBackend` subclasses and four classes deriving from `Backend` directly
  (`NullBackend`, `DialectGatedCountingBackend`, `FailDeletesUnderPrefixBackend`,
  `LocalCountingBackend`) change with the signatures; they are counted in
  [Landing order](#landing-order) and migrate with the production sites, mostly by pattern.
- **Decorators keep working.** `InstrumentedBackend`, the two test decorators, and the new
  `ThrottlingBackend` forward the `TransportAccess` they receive. `ThrottlingBackend` has two modes:
  **first-per-key** — refuse the first request on every key once with a chosen retryable status,
  which is the deterministic seam the coverage gate needs — and **every-*n*-th**, for the
  distribution tests. The gate under it is in [Verification](#verification). The fake GCS service of
  the integration suite gains the first-per-key mode on its control surface.
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

About 300 production call sites — 80 `get`, 24 `head`, 38 writes, 17 deletes, 11 `forEachListedKey`,
26 default-constructed holders, 3 backend implementations, 4 codecs — plus 53 production functions
declared over `Backend &` that become `CasRequests &` (`CasRefCatalog::read`, `publishCkpt`,
`allocateWriterEpoch`, `computeHeartbeatFloor`, `loadMeta`, `openSourceEdgeRun`, `listMounts`, …),
`Pool::backend()` itself, about 1,720 test transport calls in 57 files, 73 `InMemoryBackend`
subclasses and four direct `Backend` subclasses, 59 test constructions in 14 files, the `ObjectMeta` and ranged-read contract tests that
go with their seams, and `gtest_cas_request_control.cpp`, the old controller's own file, deleted
whole at the lock. Five steps, each green:

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
4. **Migration by connected component, every site naming its policy**, tests with their production
   files. `Token` crosses files in returned and out-parameter positions, so the unit is the
   token-flow component, not the file: `Pool::stagingPutIfAbsent`'s out-token (declared in `CasPool.h`,
   implemented in `CasPool.cpp`, consumed in `CasPartWriteTxn.cpp`), `CasRefCatalog::Snapshot::token`
   compared in `CasGc.cpp`, `acquireOrRenewLease`'s `Token &`. The token-carrying structs go with
   their component: `GetResult`, `HeadResult`, `ListedKey`, `CasRefCatalog::Snapshot`,
   `SlotOccupyResult`, `CasOverwriteResult`, `MountRenewResult`. Within a component:
   80 `get` → `op.read(…, Retry::…)`, 24 `head`, 38 writes → `create`/`replace`/`readModifyWrite`
   (the inventory's loops collapse), 17 deletes, 11 `forEachListedKey`; the test files by pattern
   (`backend.putIfAbsent(k, v)` → `op.create(k, v, Retry::once())`). The `read`-or-`stream`
   decision per site is already tabulated by the review of revision 1. Mechanical, delegated,
   reviewed per file, gate green after each.
5. **The lock.** The old `Backend` signatures, the old controller and its five entry points and test
   file, `Pool::backend()`, `Token`, `mintingTypeMatches`, the fallback `HEAD` and
   `CasRequestBudget`'s two dead fields are deleted; the `static_assert`s go in. The compiler lists
   what was missed.

Unconstructibility and uncallability are properties of step 5; key binding and the three-outcome
result of step 3 for every migrated site. Until then the safety rests on step 1 — what is needed
immediately — and the retries on step 3.

## Verification {#verification}

**Compile time.** The three `static_assert`s; no valued `Incarnation` construction outside
`CasRequests`; a `Backend` method called from anywhere without a `TransportAccess` does not compile,
including from a test and from code holding the concrete backend type; a `CasRequests` call without
a `Retry` does not compile.

**No site without a retry, executably.** Under `ThrottlingBackend` in first-per-key mode (every
key's first request refused once with 429), the pool-level scenario tests — `CREATE TABLE`, `INSERT`,
`DROP`, `RENAME`, a writable mount at open (probe included), a GC round — all succeed, and every key
touched shows exactly one refusal and at least one reissue. Every key: the first request on `gc/hb` is
the pulse's `read` under `standard`, which reissues; the `once` writes (the pulse, `slotOccupy`) are
never a key's first request, so no key is excluded. This is the audit as a test: a call under
`Retry::once()` where `standard` was meant turns the gate red in seconds.

**The lease, on fake clocks.** Under sustained 429, `untilLeaseSafe` starts its last attempt before
`lease_deadline − margin − attempt_reservation` and issues **zero** requests after it; a renewal whose
resolve `GET` is throttled terminates by the deadline rather than hanging, with
`GaveUp{deadline_source = Lease}`; the single-attempt client the fake S3 storage hands out is
constructed with `request_timeout == object_storage_attempt_timeout_ms`.

**The deadline is the only bound.** Under zero-latency 429s, `standard` keeps reissuing until the
ninety-second deadline and gives up with `GaveUp{Deadline}`, never earlier; `once` sends one request
and no sleep.

**Backoff as a distribution.** Over a thousand series on fake clocks, `sleep ∈ [0, min(cap, base·2ⁿ)]`
with a mean near half the ceiling; without jitter the test is red.

**The write result.** A write whose only attempt was refused by the fence before sending reports
`GaveUp{sent_any = false}`; one that went ambiguous and then hit the deadline reports
`GaveUp{sent_any = true}`; `commitRefChunk` releases the transaction id on the first and wedges on the
second — the existing ref-lane tests, re-pointed at the field. The observation is honest: a 412
without a resolve carries `NotObserved`; a resolve that finds nothing carries `ProvenAbsent` and
`renew` reports `Vanished` on it and retry-later on `NotObserved` — the two existing renewer tests,
re-pointed; `create` under `once` costs one request when it commits and two when it conflicts, and
the conflict carries the occupant. `Committed::attempts_sent` and `GaveUp::deadline_source` feed the
two mount counters as today.

**The fence.** An operation admitted under generation *g*, with the fence re-armed to *g+1* and open
before its next request, ends in `GaveUp{FenceLost}` without sending — on a second attempt of one
verb, and on the *next verb* of the same handle (the `ensureBlobPresent` shape: `head`, publish,
create), and on a handle `resume`d from a recorded generation (the wedge shape); a fresh `admit()`
after the re-arm passes; `admit` with `needed_ms` larger than the mount's remaining
`deadline − margin` returns `NoBudget` and the call ends in `GaveUp{Deadline, Lease}`; `orThrow()` on
`FenceLost` throws the same transient-unavailable exception `checkFenceOrThrow` throws today.

**`readModifyWrite` under contention.** Two threads incrementing one counter through it on
`InMemoryBackend`: the total is the sum, every conflict is followed by a sleep, and a key in perpetual
conflict ends in `GaveUp{Deadline}` — the bound `PoolMeta` lacks today. `readModifyWriteOnPresence` issues
`HEAD`s and no `GET`, and its observations are `Meta`, never `Object`. A decide that declines returns
`Declined`, and `orThrow()` returns `nullopt` on it. `Refused` from an access-denied fake: the ref
lane returns the attempt to `Ready` and does not wedge; `stageManifest` throws its definite-failure
message. The heartbeat under `once` issues exactly one write per pulse, conflict or not.
`removeCurrent` on a key replaced between its `HEAD` and its `DELETE` re-heads and removes the new
incarnation, never the old.

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
2. The upstream slice lands as one commit, under a hundred additive lines in the nine files named,
   `IO/` and `ObjectStorages/S3/` only, no CAS identifier.
3. After step 5: the three `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`,
   `casPut`, `deleteExact`, `publishBlob`, `Token`, `ObjectMeta`, `Range`, `mintingTypeMatches`,
   `Pool::backend()`, the old controller and its five entry points, the fallback `HEAD` no longer
   exist; no transport method of `Backend` is callable without a `TransportAccess` (the four
   capability predicates are the named exceptions); every `CasRequests` call names a policy; no test
   calls a transport method.
4. The throttling gate is green with a reissue observed on every touched key, the probe included; the
   audit's twenty-three sites are all under `CasRequests`.
5. `untilLeaseSafe` issues no request past `lease_deadline − margin`; `standard` gives up only at its
   deadline; the jitter test is red without jitter; `readModifyWrite` loses no increment under
   contention and bounds a hot key; the fence-generation test passes on every write verb.
6. `GaveUp::sent_any` drives the ref lane's wedge decision; the `Observation` tri-state drives the
   renewer's `Vanished`-versus-retry-later verdict; an `Object` in an observation comes only from a
   resolve read; the two mount renewal counters keep their inputs.
7. `read` and `write` are one physical attempt each and credential rotation still recovers on the
   read path; no needless `HEAD` and no needless `GET`; `read` throws on drift and on the bound; no
   string becomes an `Incarnation` anywhere; `remove` never reports `Mismatch` for any reason other
   than the store's own; `attempt_timeout_ms` and `lease_safety_margin_ms` are parsed disk settings.
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
contract forbids. Revision 6 kept the shape — one minted key-bound incarnation, a transport nobody
else can call, one façade whose every call names a policy, one engine, one primitive — and changed
the mechanisms: a key instead of access specifiers, strings on the transport side, one three-outcome
result, a deadline-only policy with a reserved attempt, a fence generation pinned at entry, a slice
that promises what it keeps, and two engines side by side until the lock.

**Revision 6** was reviewed once more and still had seven design holes, each a place where the
document described a mechanism the code could not supply: `publish` had no key and no façade verb;
two optionals stood where the renewer needs a tri-state ("nothing observed" and "proved absent" were
one value); `once` could not both be one request and return `slotOccupy`'s occupant; the read pin
silently broke credential rotation; the heartbeat rule "only while the owner is us" meant a new leader
could never pulse; the `Fence` interface could only throw where the result type demanded a value and
had no budget arithmetic; and the attempt reservation had no carrier into the single-attempt client
and the two budget numbers were not configuration. Revision 7 supplied the mechanisms: a keyed
`publish` and its verb, the `Observation` tri-state and `Why::Unresolved`, "`once` is one write
attempt, the resolve read is not an attempt", a refreshed client the reissue can see, the heartbeat
as a one-attempt write discriminated on the lease, `Fence::admit`, and
`object_storage_attempt_timeout_ms` with the two CAS numbers as parsed settings.

**Revision 7** had two design holes left. Its write result had no alternative for the store's
definite refusal, which the ref lane consumes as a third arm distinct from both "nothing sent" and
the wedge — mapping it onto either would have wedged a lane on an access-denied or lied about what
was sent. And its engine read the fence generation for itself at call entry, which is the current
generation, and so would have passed exactly the re-arm check that `ensureBlobPresent`, `publishCkpt`
and the wedge retry exist to fail. Revision 8 adds `Refused` and moves the verbs onto an operation
handle admitted by the caller — `admit()` now, or `resume(generation)` from a runtime record — which
also deletes the four hand-written admitted-generation checks instead of preserving some of them.

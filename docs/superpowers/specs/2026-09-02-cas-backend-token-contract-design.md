---
description: 'Design for a CAS request contract that cannot be misused: a transport that deals in strings and is callable only with a key one class can make; one façade whose operations are admitted under a fence and whose every call names its retry policy; one deadline-bound engine for reads and writes; one five-alternative write result; and a read-modify-write primitive that replaces the hand-rolled loops.'
sidebar_label: 'Backend request contract'
sidebar_position: 43
slug: /superpowers/specs/cas-backend-token-contract
title: 'CAS backend request contract'
doc_type: 'guide'
---

# CAS backend request contract {#cas-backend-request-contract}

Revision 12, the revision the implementation plan is written from. The contract has been stable
since revision 6; six review rounds since found mechanisms the code could not supply, and the last of
them found two: a write's reservation must cover its resolve read for the "every conflict is settled
by one read" invariant to hold at the lease edge, and the definite-refusal predicate must be the
complement of the credential-refresh predicate rather than a list of two names. Both are folded in
here; what remains for the plan is checklist, not contract. [What earlier revisions got
wrong](#what-earlier-revisions-got-wrong) keeps the record. This document supersedes
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
are hand-rolled read-modify-write loops, each with its own discipline — unbounded, or a hundred
iterations with no sleep, or two attempts — and the reads that *are* retried are retried by the S3
SDK, up to 500 attempts at a 5 s cap with no jitter, about 41 minutes, outside any CAS deadline, so a
resolve `GET` inside a lease renewal can outlive the lease it serves.

One root: **callers touch the transport directly, and the transport accepts what it is handed.** A
value anyone can construct, a primitive anyone can call. The fixes are the same shape: make the
value unconstructible and the primitive uncallable, and give callers one layer above with one door.

A second waste rides along: `get` issues a `HEAD` and a `GET` though a `GET` returns everything a
`HEAD` does plus the body; `MountLeaseKeeper::claim` pays three requests on a successful adopt;
`GetStreamResult::token` is produced by a `HEAD` on every stream open and consumed by nobody;
`probeSentinelRaw` is a raw `HEAD`, then `get`'s `HEAD`, then a `GET`.

## The contract {#the-contract}

> **An `Incarnation` is minted only by `CasRequests`, from a transport response, of one key, and names
> exactly one version.** Nothing else can produce one, and nothing can apply it to another key.
>
> **`Backend` is transport: one physical request per call, strings in and strings out, and no call can
> be made without a `TransportAccess` that only `CasRequests` can construct.** No code outside a
> transport implementation calls the store. Four capability predicates on `Backend` are not transport
> and are named as such.
>
> **Every request belongs to an admitted operation.** An operation is admitted under a fence
> generation — `admit()` takes the current one, `resume(g)` one that a runtime record carries — and may
> carry one caller-supplied liveness predicate, a non-throwing `bool()` over facts the fence cannot
> see: a retired ref-table runtime, a replaced wedge, a stopping detached task. The engine consults the
> fence's generation and budget, then the predicate, at three points: before every attempt, before
> every sleep, and after a proven commit. A refusal before sending is `GaveUp{FenceLost, sent_any =
> false}`; a refusal after a proven commit is `GaveUp{FenceLost, sent_any = true}`, never `Committed`,
> because the write may be durable — the ref lane wedges on it, as it wedges on `FenceLostPostWrite`
> today. `admitted()` on the handle answers the same question where no request is made.
>
> **Every call names its retry policy.** There is no default, so a call that has not thought about
> retry does not compile.
>
> **One engine serves reads and writes alike, and every control-plane request is one physical
> attempt** — exponential backoff with full jitter, a deadline that is the only bound, an ambiguous
> write resolved by an exact read before reissue. Under a lease-bound policy no attempt or sleep that
> would end past the lease is started.
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
*sends*; it is something the caller *saw* — of one key, from one backend. A call given an
incarnation of another key or of another backend throws `LOGICAL_ERROR`: a programming error, not a
store answer. "Backend identity" is a per-instance counter id, never the address; the mount plane, the
GC plane and the tools each hold a `CasRequests` over the same `Pool`-owned backend, and their
incarnations are interchangeable by design (the mount observation map and GC's manifest-cleanup map
cross planes). A holder that keeps an `Incarnation` across calls — `MountLeaseKeeper::last_token`, the
mount observation map, GC's manifest-cleanup map, `CasBlobInDegree`'s condemned rows — must not
outlive the backend; all of them are owned by the `Pool` that owns it.

```cpp
static_assert(!std::is_default_constructible_v<Incarnation>);
static_assert(!std::is_constructible_v<Incarnation, String>);
static_assert(!std::is_default_constructible_v<TransportAccess>);
static_assert(!std::is_copy_constructible_v<TransportAccess>);
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
per-key values. `mintingTypeMatches` is deleted. "Not yet" is spelled `std::optional<Incarnation>`;
the sites that default-construct a `Token` today become optionals or are restructured.

## Two layers {#two-layers}

**`Backend` is transport, in strings, under a key.** It knows nothing of retries, fences or
incarnations. Each method is one physical request; each takes a `TransportAccess`, an uncopyable
token whose only constructor is private and befriended to `CasRequests`:

```cpp
class TransportAccess
{
    friend class CasRequests;
        friend class Backend;                    // migration only: the legacy forwarders need a key; deleted at the lock
    TransportAccess() = default;
    TransportAccess(const TransportAccess &) = delete;
    TransportAccess & operator=(const TransportAccess &) = delete;
};

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
    virtual RawSentinel            probeSentinelRaw(key, TransportAccess &);   // default body kept; Native overrides
    virtual std::unique_ptr<ReadBuffer> stream(key, TransportAccess &) = 0;                        // data-plane body
    virtual void                   publish(BlobPublishRequest, TransportAccess &) = 0;             // data-plane, unconditional

    // capability predicates, not transport: answered from configuration or one preflight request; defaults kept
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
`runCapabilityProbe`'s step 0 and issues one bucket-versioning preflight.

Why a key argument and not private virtuals: C++ applies access control to the *call* through the
*static type*, so a private virtual in `Backend` leaves every public override on `InMemoryBackend` and
`ObjectStorageBackend` callable by anything holding the concrete type, and it leaves no room for a
decorator — `InstrumentedBackend`, the throttling seam, and the test decorators that forward
`inner->get(...)` today would not compile. A key closes both: no static type can call without one,
and a decorator forwards the key it was handed, so decorators keep working with no second friend.
Stated honestly: the guarantee is that **no code outside a transport implementation** can call a
transport method. An override or decorator is handed the key by reference and could store its address
for later; nothing detects that, and it is the same trust a transport implementation is given today.
Overrides may sit under any access specifier; the production backends and the test subclasses change
**signatures** (`Token` → `String`, `GetResult` → `Raw`), not access.

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

**`CasRequests` is the only caller, and an operation is its unit.** It is constructed with a
**fence**, because the fence is a property of whoever owns the lease, not of a call: the mount plane
passes the mount fence, the GC plane an open fence (its writes are guarded by the `gc/state`
incarnation, not by a mount lease, as today), the tools an open fence (`CasDecommission` and `CasFsck`
consult no fence today — by omission, not by a stated choice; this makes the choice stated). The
fence has one non-throwing admission predicate over the two things it owns, the generation and the
remaining budget:

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

**The admitted generation is the operation's, not the engine's, and the fence is not the only
fact.** `ensureBlobPresent` captures the generation once and carries it across a `HEAD`, a publish and
a create ("this generation belongs to the operation, not to one attempt"); `publishCkpt` takes it as a
parameter its callers read from **persisted runtime records**; `resolveWedgeOnce` says "never the
CURRENT generation: a retry that passes because the mount was re-armed is a write from an incarnation
that never admitted this transaction". And five ref-lane sites pass admission predicates that are
conjunctions of the generation with facts the fence cannot see — `catalog_life_invalidated`,
`superseded_by_remount`, `same_wedge_under_lock` taken under `state_mutex`, a stopping detached task —
"both are checked; neither implies the other". So the verbs are on an **operation handle**, admitted
by the caller, that carries the generation and, when the caller has them, the extra facts:

```cpp
using Liveness = std::function<bool()>;                       // non-throwing; facts the fence cannot see

class CasRequests
{
public:
    CasRequests(BackendPtr, Fence);
    CasOperation admit(Liveness = {});                             // admitted now, under generation()
    CasOperation resume(uint64_t admitted_generation, Liveness = {});   // admitted earlier; the generation came from a runtime record
};

class CasOperation                                             // move-only; every request re-checks its admission
{
public:
    bool admitted() const;                                     // fence and predicate; for verdict points

    std::optional<Object>       read  (key, const Retry &);
    std::optional<Meta>         head  (key, const Retry &);
    ListPage                    list  (prefix, cursor, limit, const Retry &);
    void                        forEachListedKey(prefix, Fn, const Retry & per_page, page_limit = 1000, on_page_fetched = {});   // Fn returns bool: false stops the walk
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
struct Object { String bytes;  Incarnation incarnation; };
struct Meta   { uint64_t size; Incarnation incarnation; };
enum class Removal { Removed, Gone, Mismatch };                       // a delete marker throws CAS_DELETE_MARKER, carrying the store's answer
```

At three points — before every attempt, before every sleep, after a proven commit — the engine calls
`fence.admit(op.generation, sleep + reservation)` and then the liveness predicate. `LostOrRearmed` or
a false predicate before sending is `GaveUp{FenceLost, sent_any = false}`; after a proven commit it
is `GaveUp{FenceLost, sent_any = true}`, never `Committed` — the check `putIfAbsentControlled` makes
today, reported as `FenceLostPostWrite`, on which the ref lane wedges because the write may be
durable. `NoBudget` is `GaveUp{Deadline}` with the lease as its source. The generation is bumped by
every loss *and every re-arm*, so an operation admitted under a prior lease incarnation gives up even
when the fence is currently open. The GC plane and the tools `admit()` against an open fence, whose
generation never moves. The predicate returns `bool` because the engine does not need to know why. `renew` distinguishes
"not attempted because we are stopping" from a fence or deadline refusal today, from the stop cause
*as sampled by the gate that refused*; under this contract it samples its own cancellation flag
**before** issuing the operation and carries that sample, so a `GaveUp{FenceLost, sent_any = false}`
is classified by a value taken before the race, not after it. One verdict changes: a stop requested
*during* the operation is classified terminal instead of `NotAttempted` (today the refusing gate
sampled the flag after it flipped), so `consumeRenewResult`'s `Terminal` arm rethrows a shutdown-race renewal that its `NotAttempted` arm
used to report and swallow — noisier, not blinder, and the `CASMountRenewalDeadlineExceeded` counter is
unaffected. There is no `Why::Cancelled`: a false
predicate is `FenceLost`, and the only party that knows about cancellation is the one that sampled
it.

About forty-four hand-written admitted-generation checks exist today in seven files. The handle
replaces the ones that immediately precede a request. The ones that guard a verdict —
`ensureBlobPresent`'s dependency proof, `publishCkpt`'s `IdenticalSkip` and epoch-decrease arms, the
wedge's post-I/O recheck under `state_mutex`, `deleteCompletedRemovingAtSnapshot`'s post-read status —
become `op.admitted()` at the same place. The staging write buffer, outside this API by design, holds
a handle of its own. The five composite predicates the ref lane passes today lose their generation
term and keep the rest as the operation's liveness predicate; `publishCkpt`'s two callbacks and the
catalog's `LeaderFenceStatus` parameter are deleted with them. Sites that want the typed transient
exception with the operator message call `orThrow()`, which maps `FenceLost` back onto it.

The three write verbs are the three shapes the audit found, named by what the caller holds:
`create` holds bytes and a claim to be first; `replace` holds bytes and an observation;
`readModifyWrite` holds only a decision. `removeCurrent` is the delete-shaped sibling of
`readModifyWriteOnPresence`, for `casRemoveObject`'s `head` → `deleteExact` loop — the one hand-rolled
no-sleep loop that would otherwise survive. `forEachListedKey` is the one verb whose single call is an
unbounded number of requests — `Gc::fold` over the whole refs prefix, the orphan-manifest sweep,
`CasFsck::listAll` — so its policy governs **each page**, not the walk: a walk of a thousand pages under
`standard` is a thousand ninety-second budgets, never a truncated enumeration, and a partial fold is
exactly the error GC's coverage record exists to prevent. It keeps `page_limit` and `on_page_fetched`,
which `Gc::fold` and `CasFsck` set independently today, and its callback returns `bool` — `false`
stops the walk — so `probePoolBootstrapResidual`'s first-page short-circuit is a `forEachListedKey`
and not a twelfth hand-written pagination loop. Today's five controller entry points go;
`resolveByExactGet` is the engine's own resolve step, not an entry point.

## One write result {#write-result}

```cpp
struct NotObserved {};                 // no read happened: admission was refused before sending, or the resolve read itself failed
struct ProvenAbsent {};                // the resolve read completed and found no object
using Observation = std::variant<NotObserved, ProvenAbsent, Meta, Object>;   // Meta from a presence-only resolve, Object from a body read

struct Committed { Incarnation incarnation; uint32_t attempts_sent; bool resolved_by_read; };
struct Declined  { Observation seen; };                      // readModifyWrite only: decide returned nullopt
struct Conflict  { Observation seen; };
struct Refused   { int store_error; String message; };      // the store proved this write never applied (the definite-failure whitelist)
struct GaveUp
{
    enum Why { Deadline, FenceLost, Unresolved } why;         // every member has a producer; there is no Cancelled
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
budget exhaustion uses); `GaveUp{FenceLost}` → `throwCasTransientUnavailable`. Sites whose non-convergence is a corruption verdict today
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
`sent_any = false` arm and from the wedge. `stageManifest` compares against it too, and `isDeterministicBlobPublicationFailure` — the
publication loop's rethrow-versus-retry decision around `publish` in `ensureBlobPresent` — is its
third consumer; it stays with that loop, over `isDefinitelyRefusedWrite`, and moves out of the old
controller's file before the lock deletes it. Folding it into
`GaveUp{Unresolved}` would set `sent_any = true` and **wedge the lane** on an access-denied; folding
it into `sent_any = false` would be the lie `DefiniteFailureAfterAmbiguity` exists to prevent
(a definite refusal after an earlier ambiguous attempt of the same call proves nothing about that
attempt, and is resolved by read — the engine's rule, unchanged); throwing would turn a `switch` into
a `catch` at two sites. So it is an alternative, and the write surface stays exception-free.

**`Why::Unresolved`** is the outcome `slotOccupy` reports as `AttemptsExhausted` today: the create
was ambiguous and the resolve read either found nothing or itself failed — no deadline passed, no
fence was lost, and the only honest label is that it is unresolved, with `sent_any = true` and
`last_seen` either `ProvenAbsent` or `NotObserved`. (Admission lost *after* a send is not this; it
is `GaveUp{FenceLost, sent_any = true}`.) **`Committed`** carries
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

            static uint64_t backoff(uint32_t attempt);              // the engine's jittered sleep, for the two hand-written loops
    static Retry within(uint64_t ms);                           // the primitive: now + ms
    static Retry standard();                                    // within(90 s)
    static Retry untilLeaseSafe(uint64_t lease_deadline_ms);    // min(standard, lease - safety margin)
    static Retry once();
};
```

**The deadline is the only bound.** An attempts ceiling "as a safety net" binds first under the
throttling this design exists for: a 429 returns in tens of milliseconds, and sixteen attempts
exhaust in about half a minute of jittered backoff while the document promises ninety seconds. There
is no ceiling; `once()` is a flag; there is no `GaveUp::Attempts`. Ninety seconds is one number that
can be said aloud, and the user statement waits that long under throttling rather than failing
faster. That is the decision, and there is one class of wait.

**`once` means one write attempt.** The resolve read that follows an ambiguous or refused write is
part of settling that one attempt, not a second attempt: `slotOccupy`'s contract — `Created` costs
one request, `Occupied` costs two (the create, then the resolve `GET`) — is exactly `create` under
`once`, and `Conflict{Object}` is only reachable because the resolve read is made.

**An attempt is reserved before it is started, and the guarantee is on the start side.** The engine
starts no attempt and no sleep unless `now + sleep + attempt_reservation ≤ deadline`, and the
reservation is the request timeout the single-attempt client is built with. What that timeout bounds
must be said exactly: Poco applies it as connect, send and receive inactivity windows per socket
operation, set three times inside one attempt, so it bounds an attempt that **stalls** and not one
that **trickles** — a peer that keeps sending a byte inside every window is not bounded by it. So the
promise is: no attempt that could not finish inside the bound is *started*; an attempt already in
flight against a trickling peer can outlive the bound. A read verb reserves one attempt timeout; **a write verb reserves two** — the write and the resolve
read that settles it — so the invariant below ("every conflict is settled by one read") holds at the
deadline edge and "zero requests after the boundary" stays true; that is the only second envelope, and
it costs one attempt timeout of head-room at the edge. Today the two controlled entry points differ:
`putIfAbsentControlled` resolves unconditionally, `putOverwriteControlledImpl` gates its resolve on
one attempt timeout and can refuse it — which is the renewer's path, so today `Vanished` is
unreachable at the lease edge; under the reservation it is reachable everywhere. Today
neither half holds: the controller budgets 5 s "as a scheduling estimate only" while the cloned
client keeps the disk's 30 s transport timeout. The carrier is the same door the profile already
opened: `object_storage_attempt_timeout_ms` beside `object_storage_retry_profile` in both settings
headers. On the CAS side `attempt_timeout_ms` and `lease_safety_margin_ms` **become parsed disk
settings** with today's values as defaults; today they are compiled-in struct defaults that
production never assigns. `CasRequestBudget` keeps those two and the recovery walk's three
(`recovery_retry_budget_ms`, `recovery_retry_initial_backoff_ms`, `recovery_retry_max_backoff_ms`) and
loses `operation_deadline_ms`, `max_attempts`, `retry_initial_backoff_ms` and `retry_max_backoff_ms`
(the engine's base and cap are its own constants); its three non-controller consumers of the deadline
— the two `CkptDeadline` constructions in `CasRefLedger` and `renew`'s `request_deadline` — become
`Retry::standard()`. `validateCasRequestBudget` keeps `attempt_timeout + margin < TTL`, on which the
successor's observation threshold rests, and loses its other three clauses with their fields.
`refAppendFenceOk` and the detached-drain deadlines read the survivors as before.

**Backoff is exponential with full jitter**: `sleep = uniform(0, min(cap, base · 2ⁿ))`, base 200 ms,
cap 5 s — the shape AWS and Google Cloud Storage both document. Today's engine has the exponent and no
jitter, so synchronized retries from many threads land in the same second under throttling; the
jitter is the one change to the algorithm. `Retry-After` is not honoured: neither `S3Exception` nor
`PocoHTTPClient` surfaces the header, and reading it would widen the upstream slice for a value the
jittered cap already approximates.

**What retries**, one classification for reads and writes: 429, 408, 5xx (`SlowDown`,
`InternalError`, `ServiceUnavailable` included), connection loss, client timeout, and the credential-refresh class (`isAccessTokenExpiredError`) after a refresh that returned a new
client. **What does not**: 404
(an answer), 412 (`Conflict`, an answer — settled by the resolve read below), the definite-failure
whitelist, and the deterministic local set. **The whitelist becomes CAS-local and narrower**: today
`classifyConditionalWriteResult` uses the general `isAccessDeniedError`, which names `ExpiredToken`
and `InvalidToken` — so an expired credential is a definite failure on the write path today, which
returns the append attempt to `Ready` and cannot recover. The design defines a CAS-local `isDefinitelyRefusedWrite(e) = isMalformedRequestError(e) ||
isEntityTooLargeError(e) || (isAccessDeniedError(e) && !e.isAccessTokenExpiredError())` — the
carve-out is the **complement of the refresh predicate** by construction, not a list of names, so
`InvalidAccessKeyId` and `SignatureDoesNotMatch`, which the refresh predicate also fires on, are
carved out with `ExpiredToken` and `InvalidToken` — and leaves the shared helpers untouched. The rule
on that class: refresh once through the callback; if a new client came back, the error is ambiguous
and the call reissues under its deadline; if no refresh was available, it is `Refused`. There is no
"exactly once" — the deadline is the bound, as everywhere. **Anything unlisted is the third class, ambiguous**: resolved by read and
reissued until the deadline, never `Refused` — `NoSuchBucket`, `InvalidObjectState`, an unmodeled
name; that fall-through is what today's classifier does on purpose, because an unclassified error may
have landed, and a `Refused` on it would un-wedge the ref lane wrongly. The deterministic local set `isDeterministicLocalFailure` names today —
`LOGICAL_ERROR`, `NOT_IMPLEMENTED`, `BAD_ARGUMENTS`, `CORRUPTED_DATA` — which is rethrown at once,
because reissuing replays the same failure and buries the root cause. Three of today's four entry
points do this; `putIfAbsentControlled` — the ref lane's own — does not, and classifies any non-S3
exception as ambiguous. Unifying the rule means a `LOGICAL_ERROR` or `CORRUPTED_DATA` on the append
path becomes an exception that escapes `commitRefChunk` instead of a value it switches on; that is
the intended behaviour, and the ref-lane tests that pin it change. This design adds two producers of
that set (a minting failure is `CORRUPTED_DATA` naming the key, an incarnation of the wrong key is
`LOGICAL_ERROR`), so the rule matters more, not less.

**The verdict is the call's, not the last attempt's.** A definite rejection or a 404 that follows an
earlier *ambiguous* attempt of the same call proves nothing about that earlier attempt — it may have
landed — so the engine resolves by exact read before it reports `Conflict` or `Refused`, exactly as
the controller rules today. A 404 on an `If-Match` write inside `readModifyWrite` means the key
vanished: re-read, and `decide` sees absence.

**Every conflict and every ambiguity is settled by one exact read before the call reports
anything** — an engine invariant, for every policy, `once` included. A `RawConflict` on `create`,
`replace` or the inner write of `readModifyWrite` is not an answer about *who* holds the key, and a
404 and a 412 collapse onto it below the façade; the resolve read is what tells "someone else's
object" from "vanished" — `renew`'s fail-closed `Vanished` and every "conflict terminal, its `Observation` the re-read" row
depend on it; today only `putIfAbsentControlled` does it unconditionally, and the reservation above is
what lets the overwrite path do it too. An
ambiguous write is resolved the same way: an exact read under the same policy and the same deadline,
byte comparison, and only then a reissue — the engine's existing `resolved_by_get`, unchanged under
the new verbs. `NotObserved` therefore has two producers only: an admission refused before sending,
and a resolve read that itself failed. A read is idempotent and needs no resolve; a retried `read` is a fresh
read with its own first ETag, so the drift check is undisturbed.

**Four policies, because there are four real differences.** (A policy names a call; a site with a
read and a write names two.) `standard`: everything that waits for a result and can afford ninety
seconds — catalog, `_ckpt`, table files, the GC plane, the mount at open, and **the capability
probe**: a 429 is not an answer to "does the store enforce the precondition", a 412 is, so the
probe's steps retry like any other request and a writable mount at open succeeds under throttling.
`untilLeaseSafe`: renew, and every read *inside* it — nothing else. `once`: the `slotOccupy` callers,
which drive their own outer loop, and the GC heartbeat pulse, which is periodic by nature and
re-pulses on cadence. `within(10 s)`: the **farewell** at shutdown — a certificate of convenience
(the clean marker lets `claimMount` reclaim without the ~36.5 s observation), harmless past the lease,
worth a few seconds of a shutdown and not ninety; a lease-bound policy would *skip* it after a failed
renewal, which is the case where it matters most, and a single attempt would forfeit it to one 429 on
exactly the throttled store where the successor's wait is expensive. For the same reason the farewell
is admitted against an **open fence**, not the mount fence: a terminal renewal trips the mount fence
(`tripMountLost`) before the farewell runs, and an operation admitted under it would give up without
sending. The mount plane therefore constructs two `CasRequests` over the one backend — one over the
mount fence for everything, one over an open fence used only by the farewell — and the farewell's
safety is `last_token`, the `If-Match` on the slot, not the lease it is retiring. A fifth shape would be a fifth
named constructor, never a parameter.

**The lease guarantee, mechanically.** `untilLeaseSafe` starts no attempt and no sleep that would end
past `lease_deadline − margin`, with the reservation above as the premise — the check
`pauseBeforeReissue` already makes, now with a reservation the transport is built to and with the
fence's `admit` carrying the same arithmetic for the mount's own margin. The renewal's resolve `GET`
used to live in the SDK's own loop outside this deadline — the amplifier in the audit's Gap 7 — and
now lives under it, sharing the write's deadline. `mountObservationThresholdMs` is unaffected: its
argument rests on `attempt_timeout + margin < TTL` and on each attempt being bounded, and the
reservation makes the second premise true for an attempt that stalls; for an attempt that trickles it
does not, today or after this design, and that is stated here rather than claimed away.

## `readModifyWrite` {#read-modify-write}

```cpp
/// decide sees the current object (or absence) and returns the bytes to write, or nullopt: nothing to do.
/// It may consult the caller's state and may issue reads of its own through the same operation.
using DecideOnObject = std::function<std::optional<String>(const std::optional<Object> & current)>;
using DecideOnMeta   = std::function<std::optional<String>(const std::optional<Meta>   & current)>;   // presence only
```

Two verbs, one loop. `readModifyWriteOnPresence` reads by `HEAD`, never reports an `Object` in its
observations (a `Meta` instead), and has one site — `casPutObject`, whose bytes the caller froze
before the loop — so that "no operation issues a request it does not need" holds in this direction
too. The loop, once for everyone: read under the policy → `decide` → `nullopt` is `Declined{seen}`
(nothing to write; the observation is returned so the caller can say what it saw); otherwise
`replace` against what was read, or `create` if nothing was; `Committed` returns; on `Conflict` the engine's resolve read **is** the next iteration's read — the
loop sleeps with jitter and re-decides on it, never reading twice per conflict on the hot keys; an
ambiguous write is resolved by the same read — our bytes are `Committed`, another's are a conflict; a 404 on the `replace` is a vanish: re-read, and `decide` sees absence;
deadline or admission are `GaveUp` with `sent_any` and the last observation. Conflicts spend
the same budget as errors: a hot key is also a failure, and it must end — GCS bounds mutations of one
object at about one per second, and today's `_ckpt` and catalog loops reissue without a sleep.

**The decision encodes the site's terminal conditions.** `readModifyWrite` is not "retry until
landed"; it is "re-decide until the decision is `nullopt` or lands". A site whose conflict is
terminal today keeps it terminal by returning `nullopt` — or is not a `readModifyWrite` at all but a
`replace`. **`decide` may throw, and an exception from it propagates unchanged — never classified,
never reissued.** It is the caller's control flow, not a store answer: `casUpdateImpl`'s callers throw
four typed markers from inside `mutate`, each caught by exact type and mapped to a distinct outcome,
and `admitOrValidate`'s `LOGICAL_ERROR` on "vanished mid-admission" is the same shape; `Declined`
therefore needs no reason of its own. Anything the caller wants back it captures by reference; the
primitive carries no template plumbing. There is no separate conflict cap and no between-attempts
callback (observability stays on the existing `observe` diagnostics).

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
auto result = orThrow(op.readModifyWrite(key,
    [&](const std::optional<Object> & current) -> std::optional<String>
    {
        return encode(apply(mutation, decode(current)));
    },
    Retry::standard()));
```

## Where each verb goes {#inventory}

The design is the verb and the policy per site; what each site does today the migration re-reads
from the code, and the plan's checklist carries it. Rules first, then the sites.

- A loop that re-decides on conflict is `readModifyWrite`; its decide encodes the site's terminal
  conditions (declines, corruption throws, captured state such as "my previous attempt was an
  absent-create that lost") and may issue reads under a named policy.
- A site whose conflict is terminal today is `read` then `create`/`replace`, never
  `readModifyWrite`: the conflict is the verdict, and its `Observation` is the re-read the site makes
  today.
- A site whose authority is a **post-write read** rather than the write response stays hand-written
  over the verbs (`deleteCompletedRemoving`: a `Committed` whose resolution read still shows the old
  row is retry-later).
- A data-plane publish is not a conditional write and cannot be inside a `readModifyWrite`; the loop
  around it stays, its requests named.
- **A hand-written loop captures one `Retry` before it starts, shares it across every call it makes,
  and sleeps with the engine's jitter between iterations** (`Retry::backoff(attempt)` is the engine's
  own helper, exposed for exactly these two sites). The loop ends when that deadline does — never a
  hundred unslept iterations against the hot catalog key, and never a hundred ninety-second budgets.
  This makes "conflicts spend the same budget as errors" true everywhere it is stated.

| site | after |
|---|---|
| `CasRefCatalog::casUpdateImpl` | `readModifyWrite`, `standard` |
| `CasRefCatalog::deleteCompletedRemoving` | hand-written over `read` + `replace` + post-write `read` under one shared `standard`, jittered sleep between iterations; outcomes `Deleted` / `ProofRefused` / `EntryChanged` / `FencedOut` (via `op.admitted()`) and the retry-later throw stay |
| `PoolMeta::admitOrValidate` | `readModifyWrite`, `standard` — bounded for free (the livelock closes) |
| `PoolMeta::createOrValidate` | `read`, `standard`; on absence `create`; on conflict the row above — the steady-state open stays one `GET` |
| `publishCkpt` | `readModifyWrite`, `standard`; its decline-time verdicts (`IdenticalSkip`, the epoch-decrease arm) read `op.admitted()` |
| `allocateWriterEpoch` | `readModifyWrite`, `standard`; the decide's subtree `LIST` and sentinel probe under `standard`; the post-conflict subtree re-check and its `CORRUPTED_DATA` live in captured state |
| `computeHeartbeatFloor` fence-out | `readModifyWrite`, `standard`; decide = the stability classification |
| `Gc::acquireOrRenewLease` | `readModifyWrite`, `standard`; decide **is** the machine: renew when ours; steal only when the lease tuple is unchanged across two observations **and** the heartbeat pair is unchanged **and** `allow_steal` (false on the manual path); `nullopt` otherwise; its `gc/hb` read under `standard`; `CORRUPTED_DATA` on vanish-after-observe |
| `Gc::pulseHeartbeat` | `read` under `standard`, then `replace` (or `create` when absent) under **`once`**, `owner = gc_id` as today; conflict terminal, the next pulse comes on cadence. `heartbeatLoop` gates each pulse on `i_am_leader`, an in-process flag reset when the deposed leader's next round completes, so a deposed leader may pulse a few more times under its own name — and the lease's liveness test is observation-based (an unchanged `(owner, hb_seq)` pair) precisely so that such stray pulses are tolerated, as today; what `once` removes is a deposed leader *fighting* for ninety seconds |
| `casGcMaintenanceState` | `create` on absence, else `replace`, `standard` — "write if unchanged, else skip"; the one write made from inside a `catch (...)` is `once` |
| `CasPlainObjects::casPutObject` | `readModifyWriteOnPresence`, `standard` — a `HEAD`, never a body |
| `CasPlainObjects::casRemoveObject` | `removeCurrent`, `standard` |
| GC round commit; GC rebuild commit | `replace`, `standard` — no re-decide, and the verb says so |
| `claimMount` / `claimMountAwaitingExpiry`; `MountLeaseKeeper::claim`; `claimOwnerOrThrow` | one shape, three sites: `read`, then `create` or `replace`, `standard`; conflict terminal, its `Observation` the re-read; `claim`'s successful adopt goes from three requests to two |
| `putDeterministicArtifact`; the outcomes log | `create`, `standard`; compare-adopt on the `Conflict`'s `Object` |
| `PartWriteTxn::ensureBlobPresent` | the outer publication loop stays under one shared `standard` with the engine's sleep between iterations, its `head` / `publish` / `create` on that policy, its dependency-proof returns behind `op.admitted()`; `reconcileMetaClean` becomes `readModifyWrite`, `standard` |
| `MountLeaseKeeper::renew` | `replace`, `untilLeaseSafe` |
| `MountLeaseKeeper::terminate` (farewell) | `replace`, `within(10 s)`, on an operation admitted against an open fence; the refusal's `Observation` is the re-read |
| `runCapabilityProbe` | every call `standard`; its delete-marker refusal keeps its operator message (below) |
| `probePoolBootstrapResidual` | `forEachListedKey` with an early stop on the first page, then one `read`, both `standard`; the whole-function fail-closed `catch` stays |
| the ref lane (`commitRefChunk`, the recovery walk, `resolveWedgeOnce`) | `create` under `standard` on a `resume(g, liveness)` handle; four arms: `Committed`, `Refused`, `GaveUp{sent_any}`, and `Conflict{Object}` — a different occupant at the content-addressed key, which the lane compares and reports as `CORRUPTED_DATA` (today the controller's resolve throws it) |

## The upstream slice {#upstream-slice}

One commit under `src/IO` and `src/Disks/DiskObjectStorage/ObjectStorages/S3` (and `IObjectStorage.h`
for three overloads), generic, no CAS identifier, portable on its own. Four facts make it necessary;
its promise is stated below; its mechanism and its line count are the commit's own.

**A plain `GET` never carries a GCS generation.** The generation reaches the SDK's ETag field only
through `applyGcsConditionalDialectToResponse`, which `PocoHTTPClient` invokes only
`if (isNativeConditionalRequest(request))`; `ReadBufferFromS3::sendRequest` never marks its
`GetObjectRequest`. Today's `get` works on GCS only because its token comes from `nativeHead`, which
marks. So `ObjectStorageRequestMode` moves out of `WriteSettings.h` into its own header, `ReadSettings`
gains it, and the read buffer marks its `GET` under it. Marking every `GET` unconditionally instead
would put the generation into the ETag the Iceberg version-hint writer copies into an **unmarked**
`If-Match`; the mode gate is necessary.

**A reissued `GET` can straddle a replacement.** `ReadBufferFromS3::nextImpl` reissues from the
current offset on a mid-body failure without `If-Match`, and `getObjectMetadataFromTheLastRequest`
reports the last response, so `readSmallObjectAndGetObjectMetadata` cannot keep its own word
"consistent". The buffer records whether any reissue answered with a different ETag, and the S3
override of `readSmallObjectAndGetObjectMetadata` throws after draining if one did. That overridable
function is `read`'s implementation under the request mode, and it is the seam the Native test fakes
use — nothing in CAS casts a buffer.

**The SDK retries control-plane requests outside any CAS deadline, twice over.** `readObject` takes the
default client (500 attempts at a 5 s cap), and beneath it `ReadBufferFromS3` reissues up to
`max_single_read_retries` (default 4) with its own backoff, for every retryable S3 error including
429; `HEAD`, `LIST` and conditional `DELETE` reach `IObjectStorage` through methods that take no
settings and use the default client. `WriteSettings` already carries `object_storage_retry_profile`,
which `writeObject` honours by selecting the single-attempt client. `ReadSettings` gains the same
field, and `readObject` under it selects that client and pins `max_single_read_retries` to one; both
settings gain `object_storage_attempt_timeout_ms`, which the single-attempt clone is built with; and
`tryGetObjectMetadataWithNativeToken`, `iterate` and `removeObjectIfTokenMatches` gain overloads that
take the retry profile — default bodies forward for `Default` and refuse `SingleAttempt` with
`NOT_IMPLEMENTED` (the capability `supportsRetryProfile` already gates the writable mount), S3 bodies
select the client exactly as `writeObject` does. `S3ObjectStorage` overrides all three today, `AzureObjectStorage` only `iterate`, and no decorator
overrides any — which is what makes a writable Azure open refuse at the base default. The capability check that gates `SingleAttempt` runs
today only for a writable Native open; a **read-only** pool over a storage without the overloads keeps
its control plane on `Default` — the one regime where the SDK still retries a control-plane request,
and a regime with no lease, so no lease guarantee is at stake; a writable open over such a storage
refuses as today. The default client, with the disk's `retry_attempts`, serves the data plane and
that read-only regime only. No operator is asked to lower
`retry_attempts`; the data plane keeps the SDK's retries (a soak note records a merge that needed
them under `SlowDown`) and the control plane never uses them.

**Pinning the read to one attempt breaks credential rotation unless the storage learns of it.**
`ReadBufferFromS3::processException` refreshes an expired token into the **buffer's own** client and
returns "retry"; with one attempt the buffer rethrows and the refreshed client dies with it, and the
storage's next buffer is built from its unchanged client. (No CAS `head` recovers it either:
`tryGetObjectMetadataImpl` has no catch.) The carrier is the `credentials_refresh_callback` the storage hands the buffer — today it only
*returns* a client, which the buffer keeps for itself; under the single-attempt profile `readObject`
hands the buffer a **wrapping** callback that installs the new client into the storage before
returning it. With one attempt the buffer still throws after refreshing (`last_attempt` is true on
attempt one), so `read` recovers on the engine's next reissue, which then sees the installed client.
The buffer's refresh predicate is the access-error class `isAccessTokenExpiredError`
(`INVALID_ACCESS_KEY_ID | ACCESS_DENIED | INVALID_SIGNATURE | UNKNOWN`), and it fires on that class. The other verbs get the refresh **where their
request is actually issued**: `head` and `remove` issue theirs inline in `S3ObjectStorage`, and a
single `refreshAndRetryOnExpiredCredentials(fn)` helper wraps both (the same in-place shape
`getObjectMetadata` has — catch, refresh through the callback, `client.set`, reissue once); `write`
issues its request from `WriteBufferFromS3::finalize` after `writeObject` has returned, and `list`
from `S3IteratorAsync` on the list pool against the client captured at construction, so neither can
recover in place — for those two the CAS backend, on the access-error class, calls
`IObjectStorage::tryRefreshCredentialsViaCallback` so the storage installs the refreshed client, and
the engine's next reissue (`write` again; `Backend::list` rebuilds its iterator) sees it. The credential story, per verb:

| verb | request issued in | who refreshes | recovers |
|---|---|---|---|
| `read` | `ReadBufferFromS3` | the buffer, through the wrapping callback that installs into the storage | on the next engine reissue |
| `head`, `remove` | `S3ObjectStorage`, inline | `refreshAndRetryOnExpiredCredentials` around the request | in place |
| `write` | `WriteBufferFromS3::finalize`, after `writeObject` returned | the CAS backend, via `tryRefreshCredentialsViaCallback` | on the next engine reissue |
| `list` | `S3IteratorAsync`, on the list pool | the CAS backend, via `tryRefreshCredentialsViaCallback` | on the next engine reissue (`Backend::list` rebuilds its iterator) |

**The promise.** `read` is one request and never mixes two bodies; `head`, `remove` and `write` are
one request each; `list` is one page at a time but a page may be several `ListObjectsV2` calls,
prefetched on the list pool — so its single reservation under-reserves it, a precision gap and not a
soundness one, since `list` never runs under a lease-bound policy; the emulated sentinel keeps its container stat, because a bare local
`GET` cannot tell "key absent" from "pool directory gone", and the Native sentinel is one `GET`;
credential rotation recovers on every verb within one engine reissue, per the table above; every
attempt is bounded by the reservation as stated; `read`
refuses a body larger than the compress bound of the largest control-plane cap
(`ZSTD_compressBound` over the caps table, computed once beside it, through
`readSmallObjectAndGetObjectMetadata`'s `max_size_bytes`) — a manifest that decodes to 256 MiB may
legitimately be stored larger, and the bound is on stored bytes.

Under the project's rule for shared surfaces this slice is consulted before it is written; the user
has approved the direction.

## Persisted incarnations {#persisted-incarnations}

GC must remember which publication it condemned — a republished blob is payload-identical to the
condemned one and byte-different only by its envelope's `incarnation_tag`, which is what makes a
content-derived ETag differ. It persists the two strings the `cas_run` format already carries,
`token_type` and `token`, and the type those fields have today is `Token`, which this design deletes.
Their type becomes `PersistedIncarnation { String dialect; String value; }` — the target for
`TokenFields::build` and `writeTokenFields` in the wire vocabulary, the record-stream and outcomes
formats, and `CasBlobInDegree`'s condemned rows —
with one operation, comparison against a `render()`, and **no path to an `Incarnation`**; the
"no inverse" rule lives in that type. The persisted pair is compared, as text, against the rendering
of a live observation of the same key:

```cpp
auto op = requests.admit();          // the GC plane: an open fence
auto meta = op.head(blob_key, Retry::standard());
if (meta && meta->incarnation.render() == persisted)
    op.remove(blob_key, meta->incarnation, Retry::standard());
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

`write` must return a value the grammar accepts; a response without one has nothing to return.
`runCapabilityProbe` depends on write values structurally — `t1` from step 1 is step 4's
precondition — and refuses a nameless-write store today only by accident; it will refuse it by name.
At runtime a nameless response is a store anomaly and the write **may have committed**: the façade
throws `CAS_WRITE_UNATTRIBUTED`, which `classifyConditionalWriteResult` treats as ambiguous by
default, so the engine resolves it by reading back. The fallback `HEAD` in `tokenFromWriteResult` is
deleted. Because every write now goes through an operation, there are no raw callers for this
exception to reach.

## What is promised about requests {#request-promises}

**No operation issues a request it does not need**: `read`, `stream` and the Native `probeSentinel`
issue no `HEAD`; `head` issues one; `readModifyWriteOnPresence` issues no `GET`. Reading an object's
metadata is one `GET` at the caller level: `MountLeaseKeeper::claim` goes from two and three requests
to two and two; the manifest reader's own change (`2026-09-02-cas-manifest-cache-by-id-design.md`)
landed first and independently.

**Control plane and data plane are two regimes, and the line is the nature of the request.**
Control-plane requests — everything through an operation — are small, deadline-bound, single-attempt
at the transport and retried by one engine. Data-plane requests — the blob stream on the `MergeTree`
read path (the body behind `stream`) and `publish` (multipart with its own per-part retries) — are
large streams where the SDK's per-request retry is right, and stay on it. `stream`'s *open* is under
the caller's policy because `CasBlobInDegree` streams GC blob-target runs — a control object —
through it. Retiring `stream`'s `HEAD` also retires the size it fed to the read buffer's allocation
hint; a small control object streams through a default-sized buffer, which is the cost of not
issuing the request, and a caller that knows the size may pass it.

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

- **Tests have one door: a `CasRequests` over an open fence, `admit()`, `Retry::once()`.** No test
  calls a `Backend` method — none can, without the key. Tests obtain incarnations by performing
  operations on `InMemoryBackend` through it; a "wrong" one for a key is that key's **previous**
  incarnation after an overwrite. The direct transport calls in the CAS gtest files, the
  `InMemoryBackend` subclasses and the four classes deriving from `Backend` directly change with the
  signatures; the plan's checklist counts them.
- **Decorators keep working.** `InstrumentedBackend`, the test decorators, and the new
  `ThrottlingBackend` forward the `TransportAccess` they receive. `ThrottlingBackend` has two modes:
  **first-per-key** — refuse the first request on every key once with a chosen retryable status,
  which is the deterministic seam the coverage gate needs — and **every-*n*-th**, for the
  distribution tests. The fake GCS service of the integration suite gains the first-per-key mode on
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
- **A delete marker** is a named exception, `CAS_DELETE_MARKER`, thrown by `remove`; the two consumers
  keep their verdicts by catching it — the probe rethrows `NOT_IMPLEMENTED` with its operator message
  about bucket versioning, GC's redelete rethrows `LOGICAL_ERROR` — so the message an operator sees
  today is the message they see after.

## Landing order {#landing-order}

Five steps, each green. The plan carries the site checklists; the rules that make the migration
safe are these.

1. **Safety, inside the backend, small.** In `ObjectStorageBackend`, no caller change: the full
   minting grammar enforced at the legacy write entry points as `LOGICAL_ERROR`, the same on the
   delete path across all three backends, the grammar applied where `HEAD` and `list` mint as
   `CORRUPTED_DATA`, and the fallback `HEAD` replaced by `CAS_WRITE_UNATTRIBUTED`. After it no clobber
   path is left.
2. **The upstream slice**, one commit, the four facts above; `IO/`, `ObjectStorages/S3/` and the
   three `IObjectStorage` overloads; no CAS identifier.
3. **`CasRequests`, `CasOperation`, `Retry`, `WriteResult` and `TransportAccess` beside the old,
   independent of it.** The new engine is today's controller's algorithm with three changes — jitter,
   reads, the deadline as the only bound — under the new verbs, minting from the transport's strings.
   The old controller is **not** rewired onto it: delegation would need a `Token`→`Incarnation`
   conversion, which is the door, so two engines coexist for the length of the migration and the old
   one is deleted whole at step 5. The `Backend` interface gains the new string-and-key signatures
   here, and the production backends implement both sets until step 5. **The new methods are the
   primitives; the legacy methods forward to them through the virtual**, obtaining the key from a
   `protected: static TransportAccess Backend::migrationAccess()` that exists only for the window and
   goes with the legacy signatures at step 5. This is not a style choice: the CAS gtest files
   carry hundreds of *overrides* of transport methods — counting backends, scripted `putIfAbsent`s,
   `HeadThenDeleteOnceBackend`, `FailDeletesUnderPrefixBackend` — and they are the fault-injection
   seams. If the legacy methods were the primitives, a production site migrated to `op.create` would
   reach `Backend::write` and an override written against `putIfAbsent` would stop firing with the
   test still green, the worst failure a migration can have. So each test override moves to the new
   signature **in the same commit as the production site it instruments**, and the commit shows the
   fault still fires. `ThrottlingBackend` and its gate land here, **before** the migration, so the
   migration runs under it. Because the legacy methods now forward, `Range`, `ObjectMeta` and
   `GetStreamResult::token` — which a forwarding `get`/`getStream` cannot preserve — are retired
   **here**, with their contract tests, not at the lock.
4. **Migration by connected component, every call naming its policy**, tests with their production
   files. `Token` crosses files in returned and out-parameter positions, so the unit is the token-flow
   component, not the file (`Pool::stagingPutIfAbsent`'s out-token declared in one file, implemented
   in another, consumed in a third; `CasRefCatalog::Snapshot::token` compared in GC;
   `acquireOrRenewLease`'s `Token &`), and the token-carrying structs go with their component
   (`GetResult`, `HeadResult`, `ListedKey`, `CasRefCatalog::Snapshot`, `SlotOccupyResult`,
   `CasOverwriteResult`, `MountRenewResult`). Within a component the work is mechanical: `get` →
   `op.read(…, Retry::…)`, `head`, writes → `create`/`replace`/`readModifyWrite` (the loops
   collapse), deletes, `forEachListedKey`; the `read`-or-`stream` decision per site is already
   tabulated by the review of revision 1. Delegated, reviewed per component, gate green after each.
5. **The lock.** The old `Backend` signatures, the old controller and its five entry points and test
   file, `Pool::backend()`, `Token`, `mintingTypeMatches`, the fallback `HEAD` and
   `CasRequestBudget`'s dead fields, `Backend::migrationAccess` and the `friend class Backend` line
   are deleted; the four `static_assert`s go in. The compiler lists what was missed.

Unconstructibility and uncallability are properties of step 5; key binding, admission and the
write result of step 3 for every migrated site. Until then the safety rests on step 1 — what is
needed immediately — and the retries on step 3.

## Verification {#verification}

**Compile time.** The four `static_assert`s; no valued `Incarnation` construction outside
`CasRequests`; a `Backend` transport method called from anywhere without a `TransportAccess` does
not compile, including from a test and from code holding the concrete backend type; a verb without a
`Retry` does not compile; a verb outside an operation does not exist.

**No site without a retry, executably.** Under `ThrottlingBackend` in first-per-key mode (every
key's first request refused once with 429), the pool-level scenario tests — `CREATE TABLE`, `INSERT`,
`DROP`, `RENAME`, a writable mount at open (probe included), a GC round — all succeed, and every key
touched shows exactly one refusal and at least one reissue. Every key the gate's scenarios touch: the
first request on `gc/hb` is the pulse's `read` under `standard`, which reissues, and the `once` writes
of the pulse and the wedge retry are never a key's first request. One `once` write *is* a key's
first request — the recovery walk's epoch seal at `T+1`, a key nothing has requested — and it is
reached only by a dead-epoch recovery, which none of the gate's scenarios run; it is the gate's one
stated exclusion. This is the audit as a test: a call under
`Retry::once()` where `standard` was meant turns the gate red in seconds.

**The lease, on fake clocks.** Under sustained 429, `untilLeaseSafe` starts its last attempt before
`lease_deadline − margin − attempt_reservation` and issues **zero** requests after it; a renewal whose
resolve `GET` is throttled terminates by the deadline rather than hanging, with
`GaveUp{deadline_source = Lease}`; the single-attempt client the fake S3 storage hands out is
constructed with `request_timeout == object_storage_attempt_timeout_ms`.

**The deadline is the only bound.** Under zero-latency 429s, `standard` keeps reissuing until the
ninety-second deadline and gives up with `GaveUp{Deadline}`, never earlier; `once` sends one write
and no sleep, and its conflict costs exactly one resolve read.

**Backoff as a distribution.** Over a thousand series on fake clocks, `sleep ∈ [0, min(cap, base·2ⁿ)]`
with a mean near half the ceiling; without jitter the test is red.

**The write result.** A write whose only attempt was refused by admission before sending reports
`GaveUp{FenceLost, sent_any = false}`; one that went ambiguous and then hit the deadline reports
`GaveUp{Deadline, sent_any = true}`; one that committed and then failed admission reports
`GaveUp{FenceLost, sent_any = true}`, never `Committed`; `commitRefChunk` releases the transaction id
on the first and wedges on the other two — the existing ref-lane tests, re-pointed at the fields.
`Refused` from an access-denied fake: the ref lane returns the attempt to `Ready` and does not
wedge; `stageManifest` throws its definite-failure message. The observation is honest: every
412 is followed by one resolve read before the call returns; a resolve that finds nothing carries
`ProvenAbsent` and `renew` reports `Vanished` on it; a resolve read that fails carries `NotObserved`
and `renew` reports retry-later — the two existing renewer tests, re-pointed.
`Committed::attempts_sent` and `GaveUp::deadline_source` feed the two mount counters as today.

**Admission.** An operation admitted under generation *g*, with the fence re-armed to *g+1* and open
before its next request, ends in `GaveUp{FenceLost}` without sending — on a second attempt of one
verb, on the *next verb* of the same handle (the `ensureBlobPresent` shape: `head`, publish, create),
and on a handle `resume`d from a recorded generation (the wedge shape); a fresh `admit()` after the
re-arm passes. A liveness predicate that turns false between two attempts ends the operation the same
way; `op.admitted()` reports false at a verdict point without a request. `admit` with `needed_ms`
larger than the mount's remaining `deadline − margin` returns `NoBudget` and the call ends in
`GaveUp{Deadline, Lease}`. `orThrow()` on `FenceLost` throws the same transient-unavailable exception
`checkFenceOrThrow` throws today.

**`readModifyWrite` under contention.** Two threads incrementing one counter through it on
`InMemoryBackend`: the total is the sum, every conflict is followed by a sleep, and a key in perpetual
conflict ends in `GaveUp{Deadline}` — the bound `PoolMeta` lacks today. `readModifyWriteOnPresence`
issues `HEAD`s and no `GET`, and its observations are `Meta`, never `Object`. A decide that declines
returns `Declined`, and `orThrow()` returns `nullopt` on it; a decide that throws propagates its
exception unchanged with no reissue. The heartbeat under `once` issues exactly one write per pulse,
conflict or not. `removeCurrent` on a key replaced between its `HEAD` and its `DELETE` re-heads and
removes the new incarnation, never the old. `forEachListedKey` over more pages than one policy's
budget could serve completes the walk.

**Identity.** Key binding: `head("a")` applied to `replace("b", …)` throws before any request. Minting
refuses empty, `*`, a comma list, `00123`, `0`, and a generation still quoted after the strip.
Persisted compare: a matching row removes; a stale, garbage or foreign-dialect row does not, the round
completes, `CasInspect` renders both. The write anomaly: a fake S3 client returning no ETag on
`PutObject` makes the probe refuse the pool by name and a mounted pool throw
`CAS_WRITE_UNATTRIBUTED`, resolved by read.

**Requests.** `read`, `stream`, the Native `probeSentinel` issue zero `getObjectMetadata` calls; `head`
one. `read` never mixes — a scripted `GetObject` on the fake S3 client serving one body then failing
then serving another with a different ETag makes `readSmallObjectAndGetObjectMetadata` throw. On GCS a
marked `GET` returns the generation and `read` mints it. Under the single-attempt profile a throttled
`GetObject`, `HeadObject`, `ListObjectsV2` and `DeleteObject` each produce exactly one request at the
fake client; an `ExpiredToken` on a single-attempt `GET` is followed by one reissue on a refreshed
client. `read` refuses a body above the bound. A writable open over a storage without the overloads refuses
`SingleAttempt` loudly; a read-only open over it runs under `Default` and succeeds.

**Existing gates.** The CA-s3 lane, the `CAS*` gtest gate and the upstream slice's own tests green at
every step; `Cas::Probe` passes on every supported writable store.

## Acceptance {#acceptance}

1. Step 1 lands first with its isolated tests.
2. The upstream slice lands as one commit, `IO/`, `ObjectStorages/S3/` and the three `IObjectStorage`
   overloads only, no CAS identifier.
3. After step 3: `Range`, `ObjectMeta` and `GetStreamResult::token` no longer exist. After step 5: the four `static_assert`s hold; `get`, `getStream`, `putIfAbsent`, `putOverwrite`,
   `casPut`, `deleteExact`, `publishBlob`, `Token`, `mintingTypeMatches`,
   `Pool::backend()`, the old controller and its five entry points, the fallback `HEAD` no longer
   exist; no transport method of `Backend` is callable without a `TransportAccess`, and `CasRequests` is
   the only friend that can make one; every verb names a policy and belongs to an operation; no test calls a transport method; no production
   `check_fence_or_throw(admitted)` remains outside the staging write buffer.
4. The throttling gate is green with a reissue observed on every touched key, the probe included; the
   audit's twenty-three sites are all under operations.
5. `untilLeaseSafe` issues no request past `lease_deadline − margin`; `standard` gives up only at its
   deadline; the jitter test is red without jitter; `readModifyWrite` loses no increment under
   contention and bounds a hot key; the admission tests pass on every verb and on `resume`d handles.
6. `GaveUp::sent_any` and `Refused` drive the ref lane's three arms; the `Observation` tri-state
   drives the renewer's `Vanished`-versus-retry-later verdict; an `Object` in an observation comes only
   from a resolve read; the two mount renewal counters keep their inputs.
7. Every control-plane request is one physical attempt (a `list` verb may be several
   `ListObjectsV2` pages, prefetched off-thread) and credential rotation recovers on every verb within
   one engine reissue; no needless `HEAD` and no needless `GET`; `read` throws on drift and on the bound; no
   string becomes an `Incarnation` anywhere; `remove` never reports `Mismatch` for any reason other
   than the store's own; `attempt_timeout_ms` and `lease_safety_margin_ms` are parsed disk settings.
8. Existing lanes and gates green at every step.

## Companion change: the `Keeper` name {#companion-keeper-rename}

`MountLeaseKeeper`, `installKeeper`, `startKeeper`, `admitKeeperCall`, `MountLeaseKeeperState`,
`keeper_state` and `mount_keeper` read as calls into ClickHouse Keeper. They are not: the object renews
a mount lease. The collision is real and not the comment's fault — CAS lives inside
`ReplicatedMergeTree`, whose commit path goes through ClickHouse Keeper, so `CasRequestControl.h`
must name the real Keeper when it rules out a `Coordination::Exception` collision. The name is
`MountLeaseRenewer`; mechanical, its own commit.

## Out of scope {#out-of-scope}

**Azure**, when it becomes a writable CAS store: the same slice shape for its read buffer, its
override and its three overloads.

**Pre-existing, filed:** `resolved_by_get` under lockstep clones; the read-then-freeze append's
duplicate on a post-commit exception. (`PoolMeta::admitOrValidate`'s unbounded loop is closed by this
design, not filed.)

**The remount driver's ownership capability**, which the reclaim design needs and this contract does
not touch.

## What earlier revisions got wrong {#what-earlier-revisions-got-wrong}

**Revisions 1 and 2** kept `Token` in the API and locked it; each review found a leak the previous lock
had not closed, because a value callers hold must be defended everywhere it is held. **Revision 3**
removed the token from callers' hands but not from the program. **Revision 4** fixed those and was
found implementable with corrections, and is the identity half of this document in substance.
**Revision 5** added the retry half with private virtuals that bind no public override, a
two-outcome write result, an attempts ceiling that bound before the deadline, an upstream claim the
slice could not keep, and a migration step across a forbidden conversion. **Revision 6** replaced the
mechanisms — a key, strings on the transport side, a three-outcome result, a deadline-only policy —
and still had a keyless `publish`, two optionals where the renewer needs a tri-state, a `once` that
contradicted `slotOccupy`, a read pin that broke credential rotation, a heartbeat rule under which a
new leader could never pulse, a fence that could only throw, and a reservation with no carrier.
**Revision 7** supplied those and lacked `Refused` and an admitted generation that belonged to the
operation. **Revision 8** supplied those two and was found to have described admission as a single
per-plane fact when five sites conjoin it with runtime liveness, to have promised a bound on the
SDK-retried lane that no configuration provided and no arithmetic could reserve, and — across three
rounds — to have accumulated a "today" column and a census that each review found wrong in places
and that no part of the design depended on. Revision 9 made admission a fence per plane plus one
liveness predicate per operation checked at three points, widened the slice by three overloads so
every control-plane request is single-attempt, and deleted the description the migration will re-read
from the code. **Revision 9** still claimed the reservation over-estimates when Poco's timeouts are
per socket operation and a trickling peer is unbounded; promised credential rotation on verbs whose
slice bodies had no refresh; admitted the farewell under the mount fence that a terminal renewal
trips; left the two hand-written loops with no shared bound; carried a `Cancelled` nothing produced;
and deleted `Token` without naming the persisted pair's new type. Revision 10 stated the lease
guarantee on the start side, admitted the farewell against an open fence, gave the hand-written loops
one `Retry` and the engine's sleep, dropped `Cancelled`, named `PersistedIncarnation`, and let a list
walk stop. **Revision 10** still had an expired credential on both sides of the classification
(today's shared access-denied helper names it, so the write path treats it as definite), stated
"every conflict is resolved" only for `once` while the renewer's `Vanished` needs it everywhere,
placed the refresh in `writeObject` and `iterate`, which issue no request, and had legacy forwarders
call keyed virtuals with no way to make a key. Revision 11 made the resolve an engine invariant, narrowed the whitelist into a CAS-local predicate,
put the refresh where each request is issued, and gave the migration window a friendship the lock
deletes. **Revision 11** stated the invariant without reserving the resolve read, so at the lease edge
it contradicted "zero requests after the boundary", and drew the carve-out as two names while the
refresh predicate fires on four codes. Revision 12 reserves two attempt timeouts for a write and
defines the carve-out as the refresh predicate's complement; with that, six review rounds have found
nothing left in the contract, and the residue — a table of who refreshes which verb, a resolve read
that feeds the next decide, a deleted copy constructor — is the kind the implementation plan carries.

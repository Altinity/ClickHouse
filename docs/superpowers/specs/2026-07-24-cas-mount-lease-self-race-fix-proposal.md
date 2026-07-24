# CAS mount-lease renewal self-race SIGABRT — fix proposal

> **⚠️ SUPERSEDED (2026-07-24, same day).** The first codex gpt-5.6-sol adversarial review
> (SAFE WITH CHANGES, 7 CONFIRMED findings — transcript
> `tmp/codex_review_mountlease_selfrace_result.txt`) refuted this proposal's central claim
> ("same uuid + same epoch + unfenced is provably our own prior write" — false: the
> `allocateWriterEpoch` empty-root hole lets a second process hold the same pair) and mandated a
> broader fix. Replaced by `2026-07-24-cas-mount-lease-self-race-fix-v2-design.md` (three layers:
> resolve-on-ambiguity in `renewOnce`, non-aborting classification incl. `superseded`, epoch
> re-mint guard). Kept for history.

**Status:** SUPERSEDED by v2 (was: PROPOSAL 2026-07-24, pre-implementation, pending review).
**Branch:** `cas-gc-rebuild` (PR #2073).
**Origin:** CI triage of Altinity PR #2073 (run 30019911967), `Stateless tests (amd_asan_ubsan,
content_addressed s3 storage, parallel)` job. Third variant of STID 3982-3b48 (parts 1a `8742d746d4e`,
1b `cafb64652d0` already landed, both ancestors of the report SHA `0ff1cbf`).

## Problem

The server aborts (`SIGABRT` under ASan — a `LOGICAL_ERROR` constructed, which calls
`abortOnFailedAssertion` at construction time, before any `catch` can run) during a routine
mount-lease background renewal. Server log (`clickhouse-server.log`), same background thread,
~10 seconds apart:

```
23:29:53.575 <Error> CasMountLeaseKeeper: CAS mount-lease: background renewal failed transiently,
             retrying while the lease is still valid: Code: 499. DB::Exception: Message:
             Poco::Exception. Code: 1000, e.code() = 0, Timeout
23:30:03.613 <Fatal>: Logical error: 'CAS mount-lease: key
             '.../gc/server-roots/stateless-ca-s3/mount' was touched by a foreign writer —
             failing closed, never re-minting'.
```

Stack (from the crash report):
```
CasServerRoot.cpp:1011  DB::Cas::SingleWriterSlot::onRenewMismatch(String const&)
CasServerRoot.cpp:879   DB::Cas::MountLeaseKeeper::onRenewMismatch(String const&)
CasServerRoot.cpp:1003  DB::Cas::SingleWriterSlot::renewOnce()
CasServerRoot.cpp:1082  DB::Cas::SingleWriterSlot::backgroundLoop(...)
```

No other server crash, no data loss anywhere else in that run; the mount-lease machinery itself
(self-remount) is proven to work correctly elsewhere in the same run (a *different*, unrelated
lease-loss event at 21:20:23 recovered cleanly via self-remount: `CasPool: CAS self-remount
'stateless-ca-s3': recovered as writer_epoch 2` at 21:21:03). The abort is purely a
misclassification of one specific, self-caused case as an unrecoverable "should never happen" bug.

## Root cause

### The transient-vs-confirmed split (already correct, working as designed)

`SingleWriterSlot::backgroundLoop` (`CasServerRoot.cpp:1065-1125`) wraps every `renewOnce()` call in
a generic `catch (...)`. It distinguishes:
- a **transient** exception (the `putOverwrite` itself threw before observing any outcome — a
  timeout, 5xx, or connection reset) — logged as "failed transiently, retrying", the loop
  continues, *as long as* `shouldFenceOnTransientRenewFailure()` says the last confirmed lease
  hasn't neared its safety-margin deadline (`MountLeaseKeeper` overrides this to ride out exactly
  this case; see the doc comment at `CasServerRoot.h:107-121`);
- a **confirmed mismatch** — `renewOnce` observed a definite `PreconditionFailed` outcome and called
  `onRenewMismatch`, which sets `last_renew_failure_was_confirmed_mismatch = true` before it always
  throws (`CasServerRoot.h:126-127`: *"MUST throw — a renew mismatch never continues"*) — the loop
  stops for good and hands off to `onRenewFailed()` → the mount-lease keeper's `on_lost` callback →
  the pool's self-remount recovery loop (`CasPool.cpp`, `CasPool::selfRemountLoop`-style code around
  lines 950-1050), which re-claims with a fresh `writer_epoch`.

This split is sound and is exactly what happened here: 23:29:53 was the transient case (client-side
timeout, ridden out); the retry ~10s later got a **confirmed** `PreconditionFailed`, so
`onRenewMismatch` ran.

### The classifier gap

`MountLeaseKeeper::onRenewMismatch` (`CasServerRoot.cpp:822-880`) re-reads the current mount-lease
body and classifies the mismatch into exactly three cases before falling through to the base
class's generic throw:

```cpp
void MountLeaseKeeper::onRenewMismatch(const String & mismatched_key)
{
    const auto got = backend->get(mismatched_key);
    if (got)
    {
        const MountLease current = decodeMountLease(got->bytes);

        if (current.server_uuid == server_uuid && current.gc_fenced)
            { ... throw MountFencedException(...); }               // recoverable, no abort

        if (current.server_uuid == server_uuid && current.writer_epoch != writer_epoch)
            { ... throw Exception(ErrorCodes::LOGICAL_ERROR, "...superseded by a newer incarnation..."); }

        if (current.server_uuid != server_uuid)
            { ... throw Exception(ErrorCodes::LOGICAL_ERROR, "...foreign server..."); }
    }
    else
        { ... throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "...vanished..."); }  // recoverable, no abort

    SingleWriterSlot::onRenewMismatch(mismatched_key);   // <-- base class: unconditional LOGICAL_ERROR
}
```

The **only** condition that reaches the base-class fallthrough (line 879 → base at line 1011) is:
**same `server_uuid`, same `writer_epoch`, NOT `gc_fenced`** — i.e. the re-read body is, by every
field the classifier checks, *provably our own currently-active, unfenced claim*, yet the
conditional renew still mismatched.

This is not "structurally impossible" (the comment at line 827-828 calls it "no plausible
classification"): it is the expected shape of the **ambiguous-timeout self-race** just described.
The 23:29:53 attempt's `putOverwrite` threw a client-side timeout with the outcome unobserved — the
PUT may have landed server-side anyway (bumping the lease body's `seq`/token) while the client never
saw the ack. The 23:30:03 retry then presents the *stale, pre-timeout* expected-token, which
mismatches against the (self-)bumped body. Since this is a **single-writer** protocol
(`SingleWriterSlot`), a body with our own `server_uuid` **and** our own `writer_epoch` cannot belong
to any other writer — under this protocol's invariants there is no other process that could hold
both simultaneously. It can only be:
1. our own prior write (the ambiguous-timeout case above), or
2. a genuine, currently-unmodeled bug.

Case 1 is real and reachable (confirmed by the log timeline); nothing in the existing code
distinguishes it from a hypothetical case 2, so today **both** fall into `LOGICAL_ERROR`, which
aborts the process at *construction time* — before `backgroundLoop`'s `catch (...)` ever runs (the
abort happens inside `Exception::Exception` → `handle_error_code` → `abortOnFailedAssertion`, per
the crash stack; the catch block genuinely never gets a chance).

### Relationship to STID 3982-3b48 parts 1a/1b

This is the **same class of bug**, already fixed twice in this exact function for two other
conditions:
- **Part 1a** (`8742d746d4e`, 2026-07-21): the *vanished* case (absent body) used to fall through to
  the base class's `LOGICAL_ERROR`. Fixed by adding the explicit `else` branch above, throwing
  `FILE_DOESNT_EXIST` instead — "stop renewing... WITHOUT aborting the server -- LOGICAL_ERROR here
  aborts debug/ASan builds at exception construction" (verbatim comment, lines 869-870).
- **Part 1b** (`cafb64652d0`, 2026-07-21): the *absent-at-clean-release* case (in `terminate()`,
  a sibling function) got the same treatment — "used to construct an unconditional `LOGICAL_ERROR`
  -- which aborts debug/ASan builds at exception construction... now a no-op release."

Both prior fixes: identify a condition that reaches `LOGICAL_ERROR` today, prove it is an ordinary
(not "should never happen") operational condition, and replace the abort with a non-aborting,
already-handled-by-the-outer-machinery exception. This proposal is the third instance of exactly
that pattern, for the one remaining gap in the same classifier.

**Note:** the `superseded` branch (`writer_epoch` differs, same `uuid`) is a *different* condition
— it fires when another incarnation of *us* (a fresh remount with a new epoch) has already taken
over — and is **not** touched by this proposal; it still throws `LOGICAL_ERROR` today and is out of
scope here (no evidence it is reachable by the same self-race, since epoch mismatch implies a
distinct remount event, not a token/seq drift within the same epoch).

## Proposed fix

Add a fourth classified branch to `MountLeaseKeeper::onRenewMismatch`, between the `fenced_by_gc`
check and the `writer_epoch != writer_epoch` (superseded) check, for exactly the gap condition
(`server_uuid == server_uuid && writer_epoch == writer_epoch && !gc_fenced`):

```cpp
if (current.server_uuid == server_uuid && current.writer_epoch == writer_epoch && !current.gc_fenced)
{
    /// Same uuid, same epoch, unfenced: under the single-writer protocol this can only be OUR OWN
    /// prior write -- most likely an ambiguous client-side timeout on an earlier renewal PUT that
    /// landed server-side without us observing the ack (bumping seq/token past what we hold). Not a
    /// foreign writer, not GC-fenced: recoverable, never a LOGICAL_ERROR abort.
    ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
    emitMountEvent(event_sink, CasEventType::MountConflict, srid, "self_raced_renewal", &current,
        "own mount slot advanced past our held token by an ambiguous prior renewal (client-side "
        "timeout) -- not a foreign writer; recoverable via self-remount");
    throw Exception(ErrorCodes::NETWORK_ERROR,
        "CAS mount-lease: key '{}' advanced past our held token by our own ambiguous prior renewal "
        "-- recoverable: re-open via self-remount", mismatched_key);
}
```

This mirrors the `vanished` precedent exactly: a plain, non-`LOGICAL_ERROR` `Exception` (no new
exception class needed), propagated through the *already-generic* `catch (...)` in
`backgroundLoop` (confirmed by reading it: recovery there is dispatched purely by
`last_renew_failure_was_confirmed_mismatch` plus calling `onRenewFailed()`/`on_lost`, never by
catching a specific exception *type* — the one place that *does* catch a specific type,
`catch (const MountFencedException &)` in `CasPool.cpp:625`, is in the separate initial-claim
`keeperStart()` path, not the background-renewal path this bug lives in, so reusing
`MountFencedException` here is not necessary and would conflate two different recovery reasons).

`ErrorCodes::NETWORK_ERROR` is chosen to match the vocabulary already used for this family of
mount-lease-lost conditions elsewhere in the codebase (e.g. the writer-side "CAS write could not be
committed (CAS mount lost / lease expired...)" messages are also `NETWORK_ERROR`); any other
non-abort code would work equally well as far as `backgroundLoop` is concerned.

### Alternative considered and rejected (for now): adopt-and-continue without throwing

A lighter fix would recognize the same condition but *adopt* the observed `(seq, token)` as the new
baseline and `return` normally (no throw, no self-remount, no fresh epoch — nothing was actually
lost). This is rejected as the primary proposal because:
- it would violate the stated, load-bearing contract "`onRenewMismatch` MUST throw — a renew
  mismatch never continues" (`CasServerRoot.h:126-127`), which every other branch (including the two
  already-landed non-aborting ones) honors;
- it requires an extra proof this proposal doesn't need: that "adopt a body we did not
  witness via our own successful CAS" is safe in every case that reaches this branch, not just the
  timeout-retry one motivating this proposal;
- the self-remount path this proposal reuses is already implemented, already exercised in
  production (the 21:20:23 event in the same run recovered via it cleanly), and orthogonal to this
  change — reusing it is the lowest-risk option.

It is recorded here in case the reviewer judges the throw-and-remount round-trip too expensive and
wants a cheaper path considered instead.

## Verification plan (not yet executed)

1. **TDD, per project convention:** write a failing test first. `MountLeaseKeeper::onRenewMismatch`
   is unit-testable directly (see `gtest_cas_mount.cpp`'s existing coverage of `claimOwnerOrThrow`
   for the pattern) — construct a keeper, seed the backend with a mount-lease body carrying the
   keeper's own `server_uuid`/`writer_epoch`/`gc_fenced=false` but a token/seq the keeper does not
   hold, and call `onRenewMismatch` directly. Confirm it currently throws `LOGICAL_ERROR` (the bug,
   reproduced deterministically, no live rustfs/network needed).
2. Implement the fix above.
3. Confirm the same test now throws a non-`LOGICAL_ERROR` exception instead.
4. Run the full existing mount-lease/self-remount suites (`gtest_cas_mount.cpp`,
   `gtest_cas_operation_gate.cpp`, and any other `*CasMountLease*`/`*MountLeaseKeeper*` tests) to
   confirm no regression in the three already-classified branches.
5. If a scenario/soak test exists that injects a client-side PUT timeout on the mount-lease renewal
   specifically, consider adding or extending one to exercise this exact sequence end-to-end
   (transient timeout → confirmed same-uuid/epoch mismatch → self-remount, no abort). Not required
   to land the fix, but the most direct evidence against the live production issue.

## Open questions for review

1. Is the single-writer invariant ("same `server_uuid` + same `writer_epoch` implies it's us") as
   airtight as assumed, or is there a narrow window (e.g. `writer_epoch` reuse across an
   out-of-band scenario, or a stale cached `server_uuid` read) where this could misclassify a
   genuine third-party condition as self-raced?
2. Does `ProfileEvents::CasMountLeaseLost` (already incremented by `superseded`/`foreign_writer`/
   `vanished`) correctly describe this new case semantically, or does it warrant its own more
   specific counter (mirroring the `CasGcRetiredSparedByReref` split done for a similarly-shaped GC
   false-positive in the same triage — see `docs/superpowers/cas/BACKLOG.md`
   `RECOVERED-INDEGREE-ATTRIBUTION`)?
3. Is `ErrorCodes::NETWORK_ERROR` the right code, or would a distinct code better signal "self-raced,
   not actually a network problem" to an operator reading the log?
4. Should this also cover the `superseded` (`writer_epoch != writer_epoch`) branch, or is that
   genuinely out of scope (no evidence of reachability by this same mechanism)?

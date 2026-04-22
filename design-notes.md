# Iceberg REST Catalog Commit Safety — Design Notes

**Branch:** `fix/iceberg-rest-catalog-commit-safety`
**Base:** `origin/antalya-26.1` @ `f3b77fd2296`
**Original ticket:** #1609 (storage leak on TRUNCATE catalog commit failure)
**Status:** Investigation complete. Design pending lead approval for expanded scope.

## Summary

What started as a scoped follow-up to PR #1529 (add cleanup to `TRUNCATE`
mirroring the pattern in `Mutations.cpp`) revealed that the `Mutations.cpp`
pattern itself is unsafe: under a specific network-failure sequence during
catalog commit, it causes table corruption, not just a storage leak.

The original #1609 fix as written would have propagated the same bug into
`TRUNCATE`. This branch is therefore repurposed from "port the cleanup
pattern" to "fix the underlying commit-safety issue in the REST catalog
client, then add cleanup to both `Mutations` and `TRUNCATE` on top of the
fixed foundation."

## The bug

The Iceberg REST catalog client has two issues that combine into a
corruption path during `updateMetadata`:

1. **HTTP client classifies 409 as retriable.**
   `src/IO/ReadWriteBufferFromHTTP.cpp:22-34`. `isRetriableError`'s
   non-retriable list is `{400, 401, 403, 404, 405, 501}`. 409 (Conflict)
   is absent and therefore retriable.

2. **Retries re-send the full POST body.**
   `src/IO/ReadWriteBufferFromHTTP.cpp:314-403, 464-522`. The `on_retry`
   callback does `impl.reset()`, forcing `initialize()` on every retry,
   which re-invokes `out_stream_callback` and re-sends the request body.
   This happens regardless of whether the original failure was during the
   POST or during response-body reading.

3. **`updateMetadata` collapses all `HTTPException` variants to `return false`.**
   `src/Databases/DataLake/RestCatalog.cpp:1347-1355`. A single `catch`
   for `const DB::HTTPException &` with no status-code inspection. A clean
   pre-commit 409 and a post-commit-retry-exhaustion 409 are
   indistinguishable to the caller.

4. **No idempotency mechanism.**
   No `If-Match`, `ETag`, `Idempotency-Key`, or request-ID headers are sent.
   Iceberg REST spec §3.5 does not define an idempotency key for commits;
   it expects clients to treat 409 as a non-retriable conflict requiring
   metadata refresh. The client violates this on both points.

### Resulting corruption sequence

1. Client POSTs `updateMetadata` with `assert-ref-snapshot-id = N`.
2. Server commits successfully, advances to snapshot `N+1`, returns 200.
3. TCP drops before client reads response body.
4. Client's HTTP layer treats the drop as retriable, re-sends the POST.
5. Server sees `assert-ref-snapshot-id = N` but is at `N+1` → responds 409.
6. Client retries (409 is classified retriable) until `http_max_tries`
   exhausted; rethrows the final 409 as `HTTPException`.
7. `updateMetadata` catches it, returns `false`.
8. Caller (`Mutations::writeMetadataFiles` or proposed `TRUNCATE` cleanup)
   interprets `false` as pre-commit rejection and runs `cleanup()`.
9. Files the catalog now references are deleted. Table corrupted.

### Production exposure

`Mutations.cpp` ships merged to `antalya-26.1` today. Any customer using
the Iceberg engine with REST catalog and performing mutations is exposed
to this under transient network failure during catalog commit.

`TRUNCATE` (#1529) is also merged; the original #1609 storage leak is
real but benign relative to the corruption bug. Proceeding with #1609 as
originally scoped would put the corruption bug into `TRUNCATE` as well.

## Investigation evidence

Full Phase 1 investigation and Claim 1–4 verification are available in
the branch's agent-session transcripts (attached to the escalation).
Every claim is cited to `file:line` and confirmed against code, not
speculation.

Key citations:
- `src/IO/ReadWriteBufferFromHTTP.cpp:22-34` — retriable classification
- `src/IO/ReadWriteBufferFromHTTP.cpp:519-522` — `impl.reset()` forces body re-send
- `src/Databases/DataLake/RestCatalog.cpp:1347-1355` — undistinguished catch
- `src/Storages/ObjectStorage/DataLakes/Iceberg/Mutations.cpp:522,530` — unconditional cleanup

## Proposed fix (three layers)

### Layer 1 — HTTP client: 409 is non-retriable

Add `HTTPStatus::HTTP_CONFLICT` to the `non_retriable_errors` array in
`isRetriableError`. This closes the window where a post-commit TCP drop
is converted into a retry loop that masks success as failure.

After Layer 1, the post-commit network drop surfaces as a
`Poco::Net::NetException` (not caught by `updateMetadata`'s `HTTPException`
catch) and propagates up to the caller as a genuinely ambiguous error —
which is the correct signal.

**Blast radius:** every REST-catalog HTTP retry. To be validated by Task 2
enumeration before implementation. Mitigation if too broad: add a
commit-specific retry classifier instead of changing the global one.

### Layer 2 — `updateMetadata`: typed result

Change return type from `bool` to:

```cpp
enum class CatalogCommitResult {
    Committed,         // 200 OK
    RejectedCleanly,   // 4xx before any retry, catalog state unchanged
    Unknown,           // anything else: retries exhausted, transport
                       // errors, ambiguous outcomes. Catalog state
                       // cannot be determined without a fresh read.
};
```

The `catch` block inspects `e.getHTTPStatus()` to distinguish `RejectedCleanly`
from `Unknown`. Uncaught exception types (e.g., `Poco::Net::NetException`)
either propagate or are caught and converted to `Unknown` — design decision,
see Open Questions.

### Layer 3 — Callers: gate cleanup on `RejectedCleanly`

`Mutations::writeMetadataFiles` and the new `TRUNCATE` cleanup both:

- Run `cleanup()` only on `CommitResult::RejectedCleanly`.
- On `Unknown`: log at WARNING with all written paths and the exception
  message, leave files in place, do not modify local state. The resulting
  leak is tracked in metrics; operators can reconcile manually if needed.
- On `Committed`: success, no cleanup.

### Failpoint strategy

`Mutations.cpp` already has `iceberg_writes_cleanup` (`FailPoint.cpp:121`).
Add a matching `iceberg_truncate_cleanup` for the new path. Both should
be triggerable in a way that exercises the `Unknown` branch too, not just
`RejectedCleanly`, so tests can verify the gating works.

## Open questions (for lead review)

1. **Layer 1 blast radius.** Is it safe to make 409 globally non-retriable
   for all REST catalog HTTP traffic? Task 2 enumeration will answer.
2. **Upstream status.** Has ClickHouse upstream already fixed any of this?
   Task 3 enumeration will answer. If yes, we backport instead of invent.
3. **`NetException` handling in Layer 2.** Should `updateMetadata` catch
   `Poco::Net::NetException` and convert to `CommitResult::Unknown`, or
   let it propagate? Propagation is simpler but less structured; catch +
   convert is more work but gives callers one uniform result type.
4. **Mutations.cpp backport policy.** Fixing this changes the semantics
   of the existing merged mutations path. Standard backport, or does
   this need a customer comms note?
5. **Scope for this ticket.** Is this one branch / one PR covering all
   three layers, or split: (a) client fix in one PR, (b) caller
   adaptation + #1609 truncate fix in a follow-up? Split is safer for
   review; unified is faster end-to-end.

## Non-goals

- Fixing the Iceberg REST spec's lack of idempotency keys. That's an
  upstream spec conversation, not a ClickHouse fix.
- Broader HTTP retry policy redesign beyond the 409 classification.
- Any changes to non-REST catalogs (Hive, Glue, etc.) — out of scope.

## Status

Awaiting lead decision on:
- Approval to expand scope from #1609 to three-layer fix.
- Priority classification of the mutations corruption path.
- Whether to file a separate tracking ticket for the corruption bug
  (recommended) or subsume under #1609 (not recommended — different
  severity classes).

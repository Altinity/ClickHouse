# CAS unconditional resurrect — design

**Status:** approved, not implemented.
**Supersedes:** `2026-08-04-cas-streaming-conditional-overwrite-design.md`, and reverts the two
commits that implemented its first two tasks (`531adeebd6b`, `784308ddc76`).

## 1. The defect, and why the first design was the wrong shape

One CAS upload branch materializes a whole blob body in memory: resurrecting a condemned incarnation
from a LOCAL staging source. `putOverwrite` takes a whole `String`, so the branch re-reads the staged
temp file, builds `[fresh_header][payload]` in RAM, and writes that. Blob bodies have no size cap, so
a large enough blob makes this an out-of-memory crash rather than a slow path.

The first design added a CONDITIONAL streaming overwrite (`putOverwriteStream` with `If-Match`). It
worked, but it carried the condition's whole cost for a benefit that turns out to be small:

- a generation-token store (GCS) honours no precondition on multipart completion, so conditional
  writes there are forced single-part under a cap — meaning that design could not fix GCS at all, and
  needed a guard, a `declared_size` parameter, and a documented ceiling to say so;
- it needed a new seam method with a refusal path, a `RefusingSink`, and overrides across twelve
  `Backend` implementations.

**The decisive observation is in our own code.** `resurrectStaged` — the S3-native staging arm of the
SAME branch — already writes unconditionally, and its comment states the reasoning we re-derived
independently: *"An `If-Match` on the condemned token would only save a redundant re-upload on a lost
race, never prevent data loss."* The three reasons given there hold for the local arm verbatim:

1. the payload bytes of two racing resurrections are identical — only the envelope and therefore the
   token differ;
2. no consumer reads a dep token's VALUE; only `has_value()` gates anything;
3. INV-NO-RETURN comes from minting a FRESH `incarnation_tag`, which changes the bytes and so the
   ETag, making every queued exact-token delete of the condemned incarnation miss. The condition is
   not what protects the resurrection — the fresh tag is.

And durable references cannot be harmed by an overwrite, because they do not name incarnations:
`ManifestEntry` carries a `BlobRef` (the content hash) and the part-manifest format contains no token
at all.

## 2. What the local arm was missing

Not a condition — a streaming form. `putOverwrite(String)` was the only overwrite available, so the
local arm was both conditional and materializing. The S3-staging arm was neither, because it had
`resurrectStaged`.

Worth stating plainly, because the method's name misleads: `resurrectStaged` performs NO server-side
copy. In Native mode it does `readObject(staging_key)` → `ignore(payload_offset)` →
`writeObject(blob_key, …, WriteSettings{})` → write fresh header → `copyData`. The bytes cross the
client exactly as they would from a local file. The server-side copy lives in `promoteStaged` (the
ordinary write-once create), not here.

So the two arms differ in ONE thing: where the reader comes from.

| | reader | write |
|---|---|---|
| S3-staging | `readObject(staging_key)`, header skipped | unconditional, streaming, `WriteSettings{}` |
| local | `write_payload` callback over the staged temp file | conditional, whole body in RAM |

## 3. The design

**One resurrect operation, with the reader as a parameter.**

```cpp
/// Unconditionally writes `[fresh_header][payload]` to `blob_key`, streaming `payload` from
/// `reader`, and returns the token of the incarnation it created.
virtual Token resurrect(ReadBuffer & payload, const String & blob_key, const String & fresh_header) = 0;
```

`staging_payload_offset` disappears from the signature: whoever opens the reader skips the header,
which is where that knowledge belongs.

The write uses default `WriteSettings{}` — no `object_storage_write_if_match`, therefore no
`s3_force_single_part_upload`, therefore multipart on every backend **including GCS**.

**`BlobSource` supplies a reader, not a callback.**

```cpp
struct BlobSource
{
    uint64_t size = 0;
    std::function<std::unique_ptr<ReadBuffer>()> open;   /// opens a fresh reader over exactly `size` bytes
    std::optional<String> server_side_copy_from;
    static BlobSource fromString(String bytes);
};
```

`open` is a factory rather than a single buffer because this path retries: it re-uploads after the
object vanishes, and it re-decides after a race. Each attempt opens its own reader. There are only
two producers in the tree — the production one already does `ReadBufferFromFile(staging_key)` inside
its callback, so it hands that back directly, and `fromString` becomes a `ReadBufferFromString`.

`server_side_copy_from` stays: it still selects `promoteStaged` for the ordinary create, where the
server-side copy is real and worth having. For the RESURRECT it now only decides which reader to open
— `readObject` instead of a file.

**Every attempt re-reads HEAD and the meta.**

The decision is made per attempt, never carried:

- object absent → ordinary write-once `putIfAbsentStream` (this branch already exists);
- meta `Clean` → someone else resurrected it, or it was never condemned → adopt the current
  incarnation, write nothing;
- meta `Condemned` → open a reader and resurrect unconditionally.

The meta is the signal, not a token comparison: `BlobMeta` carries `state`, `condemn_round` and
`size`, and deliberately does not record which incarnation was condemned — so there is nothing to
compare a HEAD against. A successful resurrect ends by flipping the meta back to `Clean`
(`writeResurrectMetaClean`), which is exactly what a later attempt observes.

HEAD is still needed, for the token and size that go into the dep record — not as a condition.

**What a lost race now costs.** Two writers that both observe `Condemned` both write; the last wins.
The loser spent the upload and changed nothing else: bodies are payload-identical, durable references
address content, and its own token matters only inside its transaction. Previously the loser paid one
HEAD instead — that saving is the one thing this design gives up, and it is bounded by how often two
writers race the same condemned blob.

## 4. What this removes

From the branch, by reverting `531adeebd6b` and `784308ddc76`:

- `Backend::putOverwriteStream` and its overrides in twelve implementations (three real backends, nine
  test doubles)
- `declared_size` as a seam concept
- the GCS single-PUT guard, its three tests, and the ceiling it enforced **on this path**
- `RefusingSink`, `EmulatedOverwriteBufferedSink`, `InMemoryOverwriteSink`
- the six `CASBackendContract` streaming-overwrite tests

From the existing code:

- `resurrectStaged`'s `staging_key`/`staging_payload_offset` parameters, replaced by a reader
- `BlobSource::write_payload`, replaced by `open`

`putOverwrite(String)` stays — its callers are small mutable control objects (root manifests,
`gc/state`, mount records) that read-modify-write in memory anyway.

**Note on the explicit-abort change.** Reverting `531adeebd6b` also removes the explicit
`write_buf->cancel()` on a refused conditional write in `NativeStreamingSink::finalize`. That
correction is worth keeping on its own merits — `putIfAbsentStream` still loses conditions, and the
destructor path logs a warning each time — so it must be re-applied as a separate commit rather than
lost with the revert.

## 5. GCS stops being a special case here

The single-part cap applies only to CONDITIONAL writes, because that is where GCS drops the
precondition. An unconditional resurrect needs no precondition, so it takes the multipart path like
any other backend and has no size ceiling.

This does NOT mean GCS is generally unbounded: the ordinary write-once create still uses
`If-None-Match`, so it remains single-part and capped. That is a separate limit on a separate path,
and `[gcs-conditional-overwrite-rethink]` still owns it. What changes is that the RESURRECT path stops
contributing to it, and the user-facing documentation must not claim a ceiling that this path no
longer has.

## 6. Testing

1. **No materialization.** Resurrect from a reader that generates bytes on the fly and declares a size
   far above any buffer; assert peak memory does not scale with it. State the measurement method in
   the test.
2. **Round trip.** The resurrected object is `[fresh_header][payload]`, its token differs from the
   condemned one, and its fresh `incarnation_tag` differs from the source header's — INV-NO-RETURN.
3. **Per-attempt re-decision.** With the meta flipped to `Clean` between attempts, the next attempt
   adopts instead of writing. With the object deleted between attempts, it falls into the write-once
   branch.
4. **Lost race is harmless.** Two resurrects of the same ref, serialized by the test; the second
   overwrites, both parts read back correctly, and GC's queued exact-token delete of the ORIGINAL
   condemned incarnation misses both.
5. **GCS.** On a generation-dialect backend, a resurrect above the old single-PUT cap now succeeds —
   the regression test for the removed ceiling, so it cannot creep back.
6. **Gate.** Full CA gate in release AND ASan; ASan carries 18 `*DeathTest` suites the release build
   does not compile.

## 7. Order of work

The revert comes first and lands green on its own, so the branch never carries two designs at once:

1. Revert `784308ddc76` and `531adeebd6b`; re-apply the `NativeStreamingSink` explicit-abort fix as
   its own commit; gate.
2. `BlobSource::write_payload` → `open`, both producers and the tests.
3. `resurrectStaged` → `resurrect(ReadBuffer &, …)`, all backends; staging caller opens `readObject`
   and skips the header itself.
4. The local arm switches to `resurrect`; per-attempt HEAD+meta re-decision.
5. Documentation: the GCS pages must stop implying this path has a ceiling.

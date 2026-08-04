# CAS streaming conditional overwrite — design

**Status:** approved, not implemented.
**Supersedes:** the condemned-upload memory admission (`initializeCondemnedUploadAdmission`), which
this design deletes outright.

## 1. The defect

One CAS upload branch materializes a whole blob body in memory: resurrecting a condemned incarnation
from a LOCAL staging source. `putOverwrite` takes a whole `String`, so the branch re-reads the staged
temp file, builds `[fresh_header][payload]` in RAM, and writes that
(`CasPartWriteTxn.cpp`, the `else` arm of the `source.server_side_copy_from` branch).

Blob bodies have no size cap — a single blob can be tens of gigabytes, far larger than server memory.
So this branch can be made to allocate an unbounded amount of memory, and the only reason it is not a
routine crash is that it is rare: it runs only when the per-hash meta point-read observed `Condemned`
just before.

The current mitigation, `ByteWeightedSemaphore` behind `initializeCondemnedUploadAdmission`, does not
address this. It bounds the AGGREGATE of concurrent admissions, which is a real problem the fan-out
introduced — N bodies where the serial path held one — but a body heavier than the whole budget is
admitted anyway, **exclusively**, because refusing it would mean refusing to write data and waiting
for it to "fit" would wait forever. Its guarantee is therefore
`peak = max(capacity, largest single body)`, and the second term is unbounded. It removes the
multiplication, never the magnitude.

The bytes are already on disk. Nothing about this path needs them in memory; the memory is an
artifact of the seam's shape.

## 2. Goal and non-goals

**Goal.** Give the local-source resurrection branch the same size-independence the S3-native staging
branch already has, and delete the admission semaphore and its setting with it.

**Non-goals.**
- GCS is NOT fixed here (§6). It needs a different mechanism and an upstream change; this design only
  makes its existing limit explicit instead of silent.
- The S3-native staging branch (`resurrectStaged`) is unchanged. It is strictly better where
  available — the bytes never traverse the server at all — and stays the preferred path.
- No protocol step changes. The write remains one conditional write against the exact expected token.
- `cas_blob_upload_pool_size` stays. It keeps bounding upload fan-out; it only loses its second role
  as the divisor that derived the memory budget.

## 3. Why a conditional write can stream at all

`putIfAbsentStream` already streams a conditional write: it builds `WriteSettings`, sets
`object_storage_write_if_none_match = "*"`, and hands back a sink over
`IObjectStorage::writeObject`. The condition rides on the write buffer and is evaluated when the
object completes.

`WriteBufferFromS3` applies both conditions in BOTH completion paths — `PutObject` and
`CompleteMultipartUpload` (`WriteBufferFromS3.cpp`, the `SetIfMatch`/`SetIfNoneMatch` calls in each).
So an `If-Match` conditional write is already correct through a multipart completion on S3.

And today's `putOverwrite` is not a different mechanism: `nativeConditionalPut` builds the same
`WriteSettings` with `object_storage_write_if_match`, calls the same `writeObject`, writes the whole
`String` into the returned buffer, and finalizes. The difference between the two methods is the SHAPE
of the caller's input, not the capability of the storage.

## 4. The seam

One new method on `Backend`, symmetric to the existing streaming create:

```cpp
/// Streaming variant of putOverwrite. Same contract: replaces the current object only when its
/// token equals `expected`; a mismatch leaves the object unchanged and finalize() reports
/// PreconditionFailed. Large bodies (blob bodies have no size cap) MUST use this rather than the
/// whole-String form.
///
/// `declared_size` is the total byte count the caller will write, header included. It is REQUIRED,
/// not a hint: a generation-token backend (GCS) cannot perform a conditional write above its
/// single-part cap at all, and must refuse BEFORE the first byte goes out rather than after the
/// body has been sent (§6). The caller always knows it here -- the header encodes to the pool's
/// fixed `blob_header_len` and the payload length is the verified `source.size`. Backends that do
/// not need it ignore it; the sink does not enforce it against what is actually written, because
/// the storage's own size check already covers a caller that lies.
virtual WriteSinkPtr putOverwriteStream(const String & key, const Token & expected,
                                        uint64_t declared_size, const ObjectMeta & meta) = 0;
WriteSinkPtr putOverwriteStream(const String & key, const Token & expected, uint64_t declared_size)
{
    return putOverwriteStream(key, expected, declared_size, {});
}
```

It returns the existing `WriteSink` (`buffer()`, `finalize() -> PutResult`, `cancel() noexcept`) and
inherits its documented misuse/lifetime contract unchanged: single-caller, not thread-safe, dead
after finalize or cancel, must not outlive its `Backend`.

The whole-`String` `putOverwrite` REMAINS. Its callers are mutable control objects — root manifests,
`gc/state`, mount records — that are small by construction and read-modify-write in memory anyway.
Only the blob-body caller moves.

### Per-mode implementations

| Mode | Implementation |
|---|---|
| `Native` | `conditionalWriteSettings()` with `object_storage_write_if_match = expected.value`, then `writeObject`, wrapped in the same `NativeStreamingSink` that serves `putIfAbsentStream`. The token of the written incarnation comes from the write-time ETag exactly as `nativeConditionalPut` already does, falling back to a HEAD when the storage returns none. |
| `EmulatedSingleProcess` | The sink writes to a temp file under the pool's scratch directory. `finalize()` takes `emu_mutex`, re-checks existence and token equality, and installs the file; mismatch returns `PreconditionFailed` and discards the temp file. The token comparison stays under the same mutex as today — the emulation's serialization point does not move. |
| `InMemoryBackend` | Accumulates and applies under its own lock; test-only, no size concern. |
| `InstrumentedBackend` | Delegates and counts, as it does for every other operation. |

`mintingTypeMatches(expected.type)` is checked before anything is written, exactly as `putOverwrite`
does — a wrong-dialect token must never reach the wire.

## 5. The call site

The local-source arm of the condemned displacement becomes:

```cpp
const uint64_t declared_size = meta.blob_header_len + source.size;
auto sink = store->backend().putOverwriteStream(key, condemned_token, declared_size);
writeString(buildHeader(), sink->buffer());
source.write_payload(sink->buffer());
const PutResult res = sink->finalize();
```

`declared_size` is exactly the expression that used to be the semaphore weight. The quantity does not
disappear with the semaphore — it changes purpose, from rationing memory to letting a
generation-token backend refuse early.

Everything else about the arm is preserved:

- `buildHeader()` still mints a FRESH `incarnation_tag`, so the resurrected body differs from the
  condemned incarnation and the queued exact-token delete of the condemned one cannot kill it
  (INV-NO-RETURN). This is the invariant the whole arm exists to uphold and it is untouched.
- The fence generation captured at the displacement decision is still re-checked immediately before
  the write, and `mayMutate()` with it. The write is still a raw, controller-uncoupled backend call.
- The arm is still reached ONLY after the per-hash meta point-read observed `Condemned`, so it still
  overwrites a condemned body and never a live one.
- The payload is still re-read from OUR OWN staging object, never GET-ed from the condemned key.

## 6. GCS: unchanged behaviour, named limit

GCS does not honour preconditions on multipart completion. This is documented by Google —
"Preconditions are not supported in the requests" on the XML API multipart upload page — and was
measured independently on 2026-07-03 (commit `0a3bc2f1fc6`). A lost precondition there does not fail:
it **silently overwrites**, which for CAS is the worst possible outcome.

That is why `conditionalWriteSettings()` sets `s3_force_single_part_upload = true` for
generation-token stores, with `s3_single_part_upload_max_bytes_override = gcs_max_conditional_put_bytes`
(default 1 GiB) as its necessary companion: if multipart is forbidden, the body must go in one part,
and one part is buffered whole in RAM.

Consequences, stated rather than worked around:

- On GCS, streaming does NOT reduce memory. The body lands in a single RAM-buffered part either way.
- On GCS, a conditional overwrite of a body larger than the cap is IMPOSSIBLE, before and after this
  change.

So this design adds one thing for GCS: on a generation-token backend, `putOverwriteStream` checks the
declared body size against the cap BEFORE writing anything and throws if it exceeds it. The message
must name the cause (GCS honours no preconditions on multipart completion), the setting
(`gcs_max_conditional_put_bytes`), and the intended fix (§9). Today the same case fails as a raw
storage error after gigabytes have already been sent.

This makes GCS fail EARLY and LEGIBLY. It does not make it work.

## 7. Failure handling

`finalize()` returns `PutResult`; `PreconditionFailed` means the expected token no longer matches and
the object was left untouched — the existing contract, unchanged.

On that path the sink MUST abort the multipart upload explicitly, before returning. Relying on
`WriteBufferFromS3`'s destructor is not acceptable here: the destructor path logs a warning
("was neither finished nor aborted") and this call site can fail its precondition as a NORMAL
outcome — a racing writer displacing the condemned token first is an expected race the caller already
handles. Left to the destructor it would produce routine warning noise and, worse, leave uploaded
parts billable until a lifecycle rule reaps them.

`cancel()` (caller abandons mid-write, e.g. the fence check throws) aborts the same way, and is
`noexcept` per the existing sink contract.

## 8. What is deleted

- `initializeCondemnedUploadAdmission`, `condemnedUploadAdmission`, `shutdownCondemnedUploadAdmission`
- `class ByteWeightedSemaphore` and `ByteWeightedSemaphoreLock`, including `StatsForTest`,
  `setHeldHookForTest`, `setWaitHookForTest`, `resetStatsForTest`
- the server setting `cas_condemned_upload_memory_bytes` and its documentation row
- the call from the server startup path

No compatibility shim, no deprecation alias, no accepted-and-ignored setting: this is pre-release with
no persisted data, and an ignored setting is worse than an absent one because it reads as still doing
something.

**Tests that go away with it, and why that is not a coverage loss.** The suites that drive the
semaphore directly (aggregate cap respected; overweight body admitted exclusively; overweight never
starves; no co-holder during an exclusive grant) test properties OF THE SEMAPHORE. When the semaphore
is gone those properties no longer exist to be violated — there is no admission to be unfair, and no
aggregate to exceed, because nothing accumulates in memory. They are deleted, not ported. A reviewer
looking for them should find this paragraph.

## 9. Follow-up, explicitly out of scope

**GCS large conditional overwrite.** The mechanism named in `0a3bc2f1fc6` is: unconditional multipart
upload to a temporary key, then a CONDITIONAL `Compose` onto the target. ClickHouse already carries
`S3::ComposeObjectRequest` and `Client::ComposeObject` (added for GCS copy), but the request exposes
no precondition — it would need `x-goog-if-generation-match` via its `GetRequestSpecificHeaders`,
i.e. a change to shared upstream code in `src/IO/S3/`. It also introduces a new debris class
(temporary keys orphaned by a crash between upload and compose) that GC or fsck must reclaim. That is
its own design, with its own risks, and does not belong in this one.

## 10. Testing

The point to prove is the ABSENCE of materialization, not the ability to write a large object. A test
that streams tens of gigabytes through CI proves the same thing far more expensively and becomes the
first test everyone disables.

1. **No materialization.** Drive the resurrection with a source whose `write_payload` generates bytes
   on the fly (never holding them) and declares a size far above any plausible buffer. Assert peak
   process memory does not scale with the declared size. This is the test the whole design exists for.
2. **Streaming overwrite succeeds.** A condemned incarnation is displaced through the streaming path;
   the resulting body is `[fresh_header][payload]`, its token differs from the condemned one, and the
   fresh `incarnation_tag` differs from the staging header's — INV-NO-RETURN still holds.
3. **Wrong token.** `finalize()` reports `PreconditionFailed` and the stored object is byte-identical
   to what it was before, with its token unchanged.
4. **No orphaned upload.** After a `PreconditionFailed` and after an explicit `cancel()`, the backend
   reports no in-progress multipart upload for that key.
5. **Emulated parity.** Cases 2-4 pass identically in `EmulatedSingleProcess`, and the temp file is
   gone in every outcome.
6. **GCS guard.** On a generation-dialect backend, a declared size above the cap throws before any
   byte is written, and the message names the setting.
7. **Gate.** Full CA gate green in release AND ASan — ASan carries 18 `*DeathTest` suites that do not
   exist in release, so release alone does not cover the abort paths.

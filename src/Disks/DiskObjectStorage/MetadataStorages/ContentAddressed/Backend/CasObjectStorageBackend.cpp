#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Core/Defines.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/copyData.h>
#include <IO/WriteSettings.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include <base/defines.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
}
}

namespace DB::Cas
{

#if USE_AWS_S3
/// `ShouldRetry` is the AWS SDK's polymorphic retry decision. It must refuse a second HTTP attempt;
/// changing the POD `retry_strategy.max_retries` alone would only bound the ClickHouse-side wrapper,
/// not this SDK loop. Every consultation is counted because it proves that the SDK considered the
/// first attempt inconclusive or failed; otherwise the retry-consultation metric would remain zero
/// even when the SDK reached this decision point.
bool detail::SingleAttemptRetryStrategy::ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const
{
    recordConditionalWriteSdkRetryConsidered();
    return false;
}

long detail::SingleAttemptRetryStrategy::CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const
{
    /// AWSClient prepares the delay before calling `ShouldRetry`, so this method is reached even
    /// though no retry will be made. Returning zero avoids needless backoff computation; it is never
    /// used as a sleep because `ShouldRetry` immediately refuses the attempt.
    return 0;
}
#endif

ObjectStorageBackend::ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_, uint64_t conditional_single_put_cap_)
    : object_storage(std::move(object_storage_))
    , mode(mode_)
    , conditional_single_put_cap(conditional_single_put_cap_)
    , emu_root(object_storage->getCommonKeyPrefix())
{
    if (mode == Mode::Native && object_storage->conditionalOpsUseGenerationTokens())
        native_token_type = TokenType::Generation;

#if USE_AWS_S3
    /// Build the single-attempt conditional-write client once. `tryGetS3StorageClient`
    /// returns null for non-S3 storage used by local tests; in that case `conditionalWriteSettings`
    /// leaves the client override unset so the object-storage adapter remains constructible for
    /// isolated tests. Writable Native mounts reject that configuration in the explicit capability
    /// check instead of silently relying on transparent retries.
    ///
    /// Both retry knobs must be overridden: `retry_strategy.max_retries` (the POD struct) only bounds
    /// this Client's OWN network-exception wrapper loop (Client::doRequestWithRetryNetworkErrors); the
    /// AWS SDK's internal per-request retry (5xx, throttling, ...) is driven entirely through the
    /// polymorphic `retryStrategy` object, which `getClientConfiguration` copies BY POINTER — so
    /// leaving it in place would still consult the DISK'S retry strategy (max_retries=500, fixed at
    /// its OWN construction time, unaffected by mutating the POD copy here) for that path.
    if (mode == Mode::Native)
    {
        if (auto base_client = object_storage->tryGetS3StorageClient())
        {
            auto cfg = base_client->getClientConfiguration();
            cfg.retry_strategy.max_retries = 0;
            cfg.retryStrategy = std::make_shared<detail::SingleAttemptRetryStrategy>();

            /// Enable `Expect: 100-continue` only on this CAS client. A server can reject an
            /// `If-Match` or `If-None-Match` request before accepting its body, so waiting for the
            /// 100 response avoids uploading a large body that cannot commit; keeping the override
            /// request-scoped leaves unrelated S3 traffic unchanged. Respect the disk's configured
            /// `expect_continue_min_bytes`; if it is unset, retain the established 1 MiB threshold.
            static constexpr uint64_t kCasFallbackExpectContinueMinBytes = 1024 * 1024;
            if (cfg.expect_continue_min_bytes == 0)
                cfg.expect_continue_min_bytes = kCasFallbackExpectContinueMinBytes;

            single_attempt_s3_client = base_client->cloneWithConfigurationOverride(cfg);
        }
    }
#endif
}

/// See Backend::checkPoolPreconditions. Only the Native, generation-dialect (GCS) combination has
/// anything to check: a token-exact DELETE on a versioned bucket archives a noncurrent generation
/// instead of reclaiming storage, so GC "reclaim" would silently stop reclaiming.
void ObjectStorageBackend::checkPoolPreconditions()
{
    if (mode != Mode::Native || native_token_type != TokenType::Generation)
        return;

    const auto versioned = object_storage->isBucketVersioningEnabled();
    if (!versioned.has_value())
    {
        /// The check itself could not be verified — either the GetBucketVersioning-equivalent call
        /// failed (e.g. permissions) or the storage does not support answering it. We proceed on the
        /// ASSUMPTION that versioning is off rather than fail-closing the mount on an unknown: a
        /// confirmed Enabled below is what actually breaks reclaim, and an outright refusal to mount
        /// whenever the check is inconclusive would be too aggressive. This is intentionally logged
        /// (not silent) so an operator can confirm the bucket's real state.
        LOG_WARNING(getLogger("CasObjectStorageBackend"),
            "CAS on GCS: could not VERIFY the bucket-versioning precondition (the versioning check "
            "request failed or is not supported by this backend) — proceeding on the assumption that "
            "bucket versioning is OFF. If versioning is actually enabled, token-exact DELETEs will "
            "archive noncurrent generations instead of reclaiming storage and GC will silently stop "
            "reclaiming space. Please verify the bucket's versioning setting manually.");
        return;
    }

    if (*versioned)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS on GCS: the bucket has object VERSIONING enabled. A token-exact DELETE on a "
            "versioned bucket archives a noncurrent generation instead of reclaiming storage — GC "
            "would silently stop reclaiming space. Disable versioning on the bucket (and prefer "
            "soft-delete duration 0 for CAS pools) and retry the mount.");
}

/// See Backend::checkConditionalWriteSingleAttemptSupport. This is a MOUNT-TIME gate, deliberately
/// separate from the ctor: the ctor stays silent (single_attempt_s3_client best-effort null) so
/// narrow, targeted unit tests can keep constructing a raw Native-mode backend over a non-S3
/// IObjectStorage (LocalObjectStorage) to exercise OTHER behaviors in isolation — the established
/// convention throughout this test suite (see e.g. gtest_cas_backend_generation.cpp). A REAL writable
/// mount, by contrast, always reaches this check: runCapabilityProbe (CasProbe.cpp) calls it for
/// every non-read-only Pool::open, so production never silently runs Native-mode conditional writes
/// under the disk's default (~500-attempt) transparent retry policy.
void ObjectStorageBackend::checkConditionalWriteSingleAttemptSupport()
{
    if (mode != Mode::Native)
        return;

    /// Without AWS S3 support at all, Native mode inherently has no single-attempt machinery — always
    /// fail closed. With it, the ctor's best-effort client build (see single_attempt_s3_client) is
    /// what must have actually succeeded.
#if USE_AWS_S3
    const bool has_single_attempt_client = static_cast<bool>(single_attempt_s3_client);
#else
    const bool has_single_attempt_client = false;
#endif
    if (!has_single_attempt_client)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS Native-mode conditional writes require a single-attempt S3 client (RFC "
            "cas-s3-timeout-retry-control) but none could be built for this object storage "
            "(IObjectStorage::tryGetS3StorageClient returned null, or this build has no AWS S3 "
            "support) — refusing to mount writable. Native mode is designed for an S3-like "
            "conditional dialect only; a non-S3 object storage should use EmulatedSingleProcess.");
}

/// =========================================================================================
/// Native helpers
/// =========================================================================================

std::optional<HeadResult> ObjectStorageBackend::nativeHead(const String & key)
{
    auto metadata = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
    if (!metadata)
        return std::nullopt;

    HeadResult hr;
    hr.exists = true;
    hr.size = metadata->size_bytes;
    hr.token = tokenForHead(metadata->etag);
    hr.attributes = ObjectMeta(metadata->attributes.begin(), metadata->attributes.end());
    return hr;
}

/// Finalize a conditional write (the condition rode on the buffer's WriteSettings) and map a
/// precondition loss to an OUTCOME — anything else propagates.
///
/// A backend reports a lost condition as an `S3Exception` carrying the canonical S3 error code string
/// from the response XML `<Code>` (`S3Exception::getExceptionName`); a conditional-write 412 is
/// UNMODELED for the AWS SDK (its enum value is UNKNOWN), so `S3Exception::isPreconditionFailed` is the
/// typed signal — the `PreconditionFailed` name, or that token in the raw body for S3-compatible stores
/// (RustFS) whose non-AWS body the SDK cannot parse into a name. A `404 NoSuchKey` on an `If-Match` PUT
/// (the key was deleted out from under us) is treated identically: protocol callers handle 'mismatch'
/// and 'gone' the same way (re-validate), so both collapse onto `PreconditionFailed`. `NoSuchKey` IS
/// modeled by the SDK, and `WriteBufferFromS3` retries it internally surfacing the exhaustion with the
/// typed enum code (and no name), so the enum is matched as well as the name. The mapping is fail-safe in
/// direction: a misread error becomes a retryable PreconditionFailed/Conflict, never a false success.
///
/// Native conditional writes require an S3-compatible integration environment for end-to-end
/// coverage. Unit tests cover the emulated semantics, the typed exception path, and this classifier
/// through the test-only `detail` declaration.
#if USE_AWS_S3
PutOutcome detail::finalizeConditionalWrite(WriteBuffer & buf)
{
    try
    {
        buf.finalize();
    }
    catch (const S3Exception & e)
    {
        if (e.isPreconditionFailed()
            || e.getExceptionName() == "NoSuchKey"
            || e.getS3ErrorCode() == Aws::S3::S3Errors::NO_SUCH_KEY)
            return PutOutcome::PreconditionFailed;
        throw;
    }
    return PutOutcome::Done;
}
#endif

/// Build-dispatching shim for the write paths below: without the AWS SDK there is no S3Exception
/// to classify, so the errors of finalize simply propagate.
static PutOutcome finalizeConditionalWrite(WriteBuffer & buf)
{
#if USE_AWS_S3
    return detail::finalizeConditionalWrite(buf);
#else
    buf.finalize();
    return PutOutcome::Done;
#endif
}

/// Instrument the same single `finalize` call used by both Native write paths without changing their
/// `Done`/`PreconditionFailed`-or-rethrow contract. A classified precondition loss is `Unresolved`,
/// not `Committed` or a definite exception, because the response does not prove who created or
/// replaced the object; the higher-level request controller may then resolve it with exact-key state.
static PutOutcome finalizeConditionalWriteInstrumented(WriteBuffer & buf)
{
    recordConditionalWriteAttemptStarted();
    try
    {
        const PutOutcome legacy = finalizeConditionalWrite(buf);
        recordConditionalWriteOutcome(
            legacy == PutOutcome::Done ? classifyConditionalWriteResult() : CasWriteOutcome::Unresolved);
        return legacy;
    }
    catch (const std::exception & e)
    {
        recordConditionalWriteOutcome(classifyConditionalWriteResult(e));
        throw;
    }
}

/// Issue a conditional PUT (the condition rides on `ws`) and map a precondition loss — see
/// finalizeConditionalWrite. The condition is checked by the backend when the object is completed,
/// so the precondition loss always surfaces from the buffer's finalize, never from write.
PutResult ObjectStorageBackend::nativeConditionalPut(const String & key, const String & bytes, const WriteSettings & ws, const ObjectMeta & meta)
{
    std::optional<ObjectAttributes> attrs;
    if (!meta.empty())
        attrs.emplace(meta.begin(), meta.end());   /// ObjectMeta is the same map type as ObjectAttributes
    auto buf = object_storage->writeObject(
        StoredObject(key), WriteMode::Rewrite, attrs, DBMS_DEFAULT_BUFFER_SIZE, ws);
    buf->write(bytes.data(), bytes.size());
    if (finalizeConditionalWriteInstrumented(*buf) == PutOutcome::PreconditionFailed)
        return {PutOutcome::PreconditionFailed, {}};

    /// Record the token of the incarnation WE just wrote (model WCreate). The S3 write returns
    /// its object ETag in the PutObject/CompleteMultipartUpload response, so no follow-up HEAD
    /// is needed — this is ~73% of the CA backend's HEADs. A backend with no write-time ETag
    /// (local files) returns nullopt and we fall back to the HEAD (a cheap local stat there).
    Token token;
    if (auto etag = buf->getResultObjectETag(); etag && !etag->empty())
        token = tokenForHead(*etag);
    else
    {
        /// No write-time ETag (local files) or an (anomalous) empty one: fall back to the HEAD —
        /// the pre-existing behavior, so an empty-ETag server is never worse than before.
        auto hr = nativeHead(key);
        token = hr ? hr->token : Token{};
    }
    return {PutOutcome::Done, token};
}

namespace
{

/// True-streaming WriteSink for Native mode: the underlying object-storage write buffer was opened
/// with `If-None-Match: *` riding on its WriteSettings, so bytes stream through it directly and the
/// condition is checked when finalize completes the object — see finalizeConditionalWrite for the
/// outcome mapping. Nothing is ever published on cancel/destruction.
class NativeStreamingSink final : public WriteSink
{
public:
    NativeStreamingSink(ObjectStorageBackend & backend_, String key_, std::unique_ptr<WriteBufferFromFileBase> write_buf_)
        : backend(backend_)
        , key(std::move(key_))
        , write_buf(std::move(write_buf_))
    {
    }

    WriteBuffer & buffer() override { return *write_buf; }

    PutResult finalize() override
    {
        chassert(!done);   /// finalize after finalize/cancel is a misuse — see the WriteSink contract
        done = true;
        if (finalizeConditionalWriteInstrumented(*write_buf) == PutOutcome::PreconditionFailed)
            return {PutOutcome::PreconditionFailed, {}};

        /// Record the token of the incarnation we just wrote (model WCreate). The S3 write
        /// returns its object ETag in the response, so no follow-up HEAD is needed (the bulk of
        /// the CA backend's HEADs). Backends with no write-time ETag (local) return nullopt and
        /// we fall back to the HEAD (a cheap local stat there).
        Token token;
        if (auto etag = write_buf->getResultObjectETag(); etag && !etag->empty())
            token = backend.tokenForHead(*etag);
        else
        {
            /// No write-time ETag (local) or an (anomalous) empty one: fall back to the HEAD.
            auto hr = backend.head(key);
            token = hr.exists ? hr.token : Token{};
        }
        return {PutOutcome::Done, token};
    }

    void cancel() noexcept override
    {
        done = true;
        write_buf->cancel();
    }

    ~NativeStreamingSink() override
    {
        if (!done)
            cancel();
    }

private:
    ObjectStorageBackend & backend;
    const String key;
    std::unique_ptr<WriteBufferFromFileBase> write_buf;
    bool done = false;
};

/// Memory-buffered WriteSink for EmulatedSingleProcess mode (unit tests only — buffering the whole
/// body is acceptable and documented): accumulates into a WriteBufferFromOwnString and delegates the
/// conditional publish to putIfAbsent at finalize, which provides atomicity under emu_mutex. Nothing
/// is ever published on cancel/destruction.
class EmulatedBufferedSink final : public WriteSink
{
public:
    EmulatedBufferedSink(Backend & backend_, String key_, ObjectMeta meta_)
        : backend(backend_)
        , key(std::move(key_))
        , meta(std::move(meta_))
    {
    }

    WriteBuffer & buffer() override { return buf; }

    PutResult finalize() override
    {
        chassert(!done);   /// finalize after finalize/cancel is a misuse — see the WriteSink contract
        done = true;
        return backend.putIfAbsent(key, buf.str(), meta);
    }

    void cancel() noexcept override
    {
        done = true;
        buf.cancel();
    }

    ~EmulatedBufferedSink() override
    {
        if (!done)
            cancel();
    }

private:
    Backend & backend;
    const String key;
    const ObjectMeta meta;
    WriteBufferFromOwnString buf;
    bool done = false;
};

}

/// True when an exception from `IObjectStorage::readObject` means "the object is simply not there".
/// Two surfaces:
///   1. S3/RustFS:        `S3Exception` with `S3Errors::NO_SUCH_KEY` (the modeled enum — the primary
///      signal) or `getExceptionName() == "NoSuchKey"` (the canonical XML `<Code>` string, present
///      when the SDK was able to parse it; mirrors `finalizeConditionalWrite`'s detection).
///   2. Local / emulated: `DB::Exception` with `ErrorCodes::FILE_DOESNT_EXIST` (from
///      `ReadBufferFromFile` when `open(2)` returns ENOENT).
///
/// Any other error (network, auth, throttle, corruption) propagates unchanged — fail-closed.
static bool isObjectNotFound(const std::exception & e)
{
#if USE_AWS_S3
    if (const auto * s3e = dynamic_cast<const S3Exception *>(&e))
        return s3e->getS3ErrorCode() == Aws::S3::S3Errors::NO_SUCH_KEY
            || s3e->getExceptionName() == "NoSuchKey";
#endif
    if (const auto * dbe = dynamic_cast<const Exception *>(&e))
        return dbe->code() == ErrorCodes::FILE_DOESNT_EXIST;
    return false;
}

/// Read `range` of the object at `path` as a TRUE ranged read: seek to the offset and bound the
/// read window. Seek the storage buffer to the requested offset and bound the returned bytes instead
/// of reading a whole snapshot run and slicing it afterward; snapshot runs can be gigabytes at scale,
/// while the caller's memory budget is O(block).
static String readObjectRanged(IObjectStorage & object_storage, const String & path, Range range,
                               uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
    String content;
    if (range.whole())
    {
        readStringUntilEOF(content, *buf);
        return content;
    }

    /// An offset at or past EOF yields an empty result, matching the range contract of the previous
    /// whole-read implementation.
    /// `seek` past the object size may throw depending on the storage, so fail-close the window
    /// against the known size before touching the buffer position.
    /// Native callers already HEAD the key, so passing its size avoids another metadata round trip.
    /// A zero size means the caller does not know it and metadata must be fetched here.
    const uint64_t object_size = known_size != 0 ? known_size
        : static_cast<uint64_t>(object_storage.getObjectMetadata(path, /*with_tags=*/false).size_bytes);
    if (range.offset >= object_size)
        return {};

    /// The readable window, clamped to EOF. `setReadUntilPosition` is only a hint (not every object
    /// storage honors it — LocalObjectStorage does not), so the exact byte count below is what bounds
    /// the read; the hint lets storages that DO honor it avoid over-fetching.
    const uint64_t available = object_size - range.offset;
    const uint64_t to_read = range.length.has_value() ? std::min(*range.length, available) : available;

    if (range.length.has_value())
        buf->setReadUntilPosition(range.offset + *range.length);
    buf->seek(static_cast<off_t>(range.offset), SEEK_SET);

    content.resize(to_read);
    const size_t got = buf->read(content.data(), to_read);
    content.resize(got);
    return content;
}

/// Open a forward-only stream over `range` of the object at `path`, positioned at the window's first
/// byte and bounded to its last. Mirrors
/// `readObjectRanged`'s seek + bound, but RETURNS the buffer instead of draining it — the caller reads
/// at its own pace, so nothing is materialized whole. Returns nullptr when the offset is at or past EOF
/// (the empty-window clamp), matching the ranged-get contract.
static std::unique_ptr<ReadBuffer> openObjectRangedStream(IObjectStorage & object_storage, const String & path, Range range,
                                                          uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
    if (range.whole())
        return buf;

    /// Clamp exactly like `readObjectRanged`: an offset at or past EOF yields an empty stream, and
    /// `seek` past the object size may throw depending on the storage, so fail-close against the known
    /// size before touching the buffer position.
    /// As in `readObjectRanged`, a caller-supplied size avoids another metadata round trip; zero means
    /// that the size is unknown and must be fetched.
    const uint64_t object_size = known_size != 0 ? known_size
        : static_cast<uint64_t>(object_storage.getObjectMetadata(path, /*with_tags=*/false).size_bytes);
    if (range.offset >= object_size)
        return std::make_unique<ReadBufferFromString>(std::string_view{});

    /// `setReadUntilPosition` is only a hint (LocalObjectStorage does not honor it), but for a returned
    /// stream it is the only bound available — the caller drains to EOF, so a storage that DOES honor
    /// the hint stops at the window end, and one that does not over-reads only the trailing bytes.
    if (range.length.has_value())
        buf->setReadUntilPosition(range.offset + *range.length);
    buf->seek(static_cast<off_t>(range.offset), SEEK_SET);
    return buf;
}

ReadSettings casSizedReadSettings(const ReadSettings & base, uint64_t known_size)
{
    if (known_size == 0)
        return base;
    return base.adjustBufferSize(known_size + CAS_FOLD_READ_SLACK_BYTES);
}

/// =========================================================================================
/// Emulated helpers (caller holds emu_mutex)
/// =========================================================================================

String ObjectStorageBackend::emuPath(const String & key) const
{
    if (emu_root.empty())
        return key;
    if (!emu_root.empty() && emu_root.back() == '/')
        return emu_root + key;
    return emu_root + "/" + key;
}

bool ObjectStorageBackend::emuExists(const String & key) const
{
    return object_storage->exists(StoredObject(emuPath(key)));
}

String ObjectStorageBackend::emuRead(const String & key, Range range) const
{
    return readObjectRanged(*object_storage, emuPath(key), range);
}

void ObjectStorageBackend::emuWrite(const String & key, const String & bytes, const ObjectMeta & meta)
{
    std::optional<ObjectAttributes> attrs;
    if (!meta.empty())
        attrs.emplace(meta.begin(), meta.end());   /// ObjectMeta is the same map type as ObjectAttributes
    auto buf = object_storage->writeObject(StoredObject(emuPath(key)), WriteMode::Rewrite, attrs);
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

Token ObjectStorageBackend::emuObserveToken(const String & key)
{
    auto it = emu_tokens.find(key);
    if (it != emu_tokens.end())
        return Token{std::to_string(it->second), TokenType::Emulated};

    /// First time we see a key that already exists on disk: seed a fresh emulated token for it so that
    /// pre-existing objects participate in token-exact semantics within this process.
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    return Token{std::to_string(seq), TokenType::Emulated};
}

/// =========================================================================================
/// Backend interface
/// =========================================================================================

std::optional<GetResult> ObjectStorageBackend::get(const String & key, Range range)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        if (!hr)
            return std::nullopt;

        /// The object may be deleted between the HEAD above and the GET below (a GC or concurrent
        /// writer racing the read window). Catch the not-found signal and honor the `optional`
        /// contract — callers such as `Pool::loadShardDecoded` already handle a nullopt return and
        /// treat it as "raced a deletion, absent". Any other error (network, auth, corruption)
        /// propagates unchanged — fail-closed by construction.
        GetResult gr;
        try
        {
            gr.bytes = readObjectRanged(*object_storage, key, range, hr->size);
        }
        catch (const std::exception & e)
        {
            if (isObjectNotFound(e))
                return std::nullopt;
            throw;
        }
        gr.token = hr->token;
        return gr;
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    /// The emulated path holds emu_mutex across the exists-check and the read, so no concurrent
    /// caller in this process can delete the file in between. External deletion (e.g. a test teardown
    /// racing a read) is still handled: convert FILE_DOESNT_EXIST to nullopt rather than letting it
    /// escape as an unexplained exception.
    GetResult gr;
    try
    {
        gr.bytes = emuRead(key, range);
    }
    catch (const std::exception & e)
    {
        if (isObjectNotFound(e))
            return std::nullopt;
        throw;
    }
    gr.token = emuObserveToken(key);
    return gr;
}

std::optional<GetStreamResult> ObjectStorageBackend::getStream(const String & key, Range range)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        if (!hr)
            return std::nullopt;

        /// Same HEAD-then-read race as `get`: the object may be deleted between the HEAD above and the
        /// stream open below. Honor the `optional` contract on a not-found signal; any other error
        /// (network, auth, corruption) propagates unchanged — fail-closed by construction.
        GetStreamResult sr;
        try
        {
            sr.stream = openObjectRangedStream(*object_storage, key, range, hr->size);
        }
        catch (const std::exception & e)
        {
            if (isObjectNotFound(e))
                return std::nullopt;
            throw;
        }
        sr.token = hr->token;
        return sr;
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    /// The emulated path holds emu_mutex across the exists-check and the stream open, matching `get`.
    /// External deletion still converts to nullopt rather than escaping as an unexplained exception.
    GetStreamResult sr;
    try
    {
        sr.stream = openObjectRangedStream(*object_storage, emuPath(key), range);
    }
    catch (const std::exception & e)
    {
        if (isObjectNotFound(e))
            return std::nullopt;
        throw;
    }
    sr.token = emuObserveToken(key);
    return sr;
}

HeadResult ObjectStorageBackend::head(const String & key)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        return hr ? *hr : HeadResult{};
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return HeadResult{};

    auto metadata = object_storage->tryGetObjectMetadata(emuPath(key), /*with_tags=*/false);
    /// A path that exists on the Local filesystem but yields no object metadata is a directory, not
    /// an object (`tryGetObjectMetadata` returns nullopt for a directory). HEAD must report it as
    /// not-an-object (exists=false) — otherwise existsFile/getStorageObjects treat a pool sub-dir (e.g.
    /// `store`, traversed by system.remote_data_paths) as a file and a later body read throws EISDIR.
    if (!metadata)
        return HeadResult{};
    HeadResult hr;
    hr.exists = true;
    hr.size = metadata->size_bytes;
    hr.attributes = ObjectMeta(metadata->attributes.begin(), metadata->attributes.end());
    hr.token = emuObserveToken(key);
    return hr;
}

/// Base WriteSettings for every Native conditional write. The post-upload existence/size HEAD
/// (check_objects_after_upload) is SKIPPED: CAS-mutable keys (shard manifests, gc/state, the
/// registry) are legitimately replaced by a concurrent conditional PUT between our upload and the
/// check's HEAD - the size comparison false-positives as "a bug in S3" under perfectly normal
/// contention (observed live against RustFS: a publish's manifest CAS raced the GC fence and the
/// mismatch terminated the server from the upload worker). Integrity for these keys is
/// the conditional PUT outcome + the observed token - a recheck adds nothing and races by design.
///
/// On a generation-token store (GCS), a conditional write must ALSO never take the multipart path:
/// GCS enforces no preconditions on `CompleteMultipartUpload` (measured), so a lost
/// precondition on a multipart write would silently overwrite instead of failing. Force single-PUT
/// and raise the single-part cap to conditional_single_put_cap (RAM-buffered) to keep the fast path
/// available for bodies up to that size; a bigger body throws NOT_IMPLEMENTED from
/// WriteBufferFromS3::createMultipartUpload.
WriteSettings ObjectStorageBackend::conditionalWriteSettings() const
{
    WriteSettings ws;
    ws.s3_skip_check_objects_after_upload = true;
    if (native_token_type == TokenType::Generation)
    {
        ws.s3_force_single_part_upload = true;
        ws.s3_single_part_upload_max_bytes_override = conditional_single_put_cap;
    }
    /// Exactly one attempt at the WriteBufferFromS3 layer too: makeSinglepartUpload/
    /// completeMultipartUpload run their OWN retry loop above the S3 client, reissuing the identical
    /// (conditional!) request on NO_SUCH_KEY — a client-level override alone does not bound it. Plain
    /// size_t field, harmless (ignored) for a non-S3 write path.
    ws.s3_max_unexpected_write_error_retries_override = 1;
#if USE_AWS_S3
    /// Exactly one HTTP attempt for every conditional write when the request-scoped client exists;
    /// a null client (non-S3 object storage) leaves the disk's own client in place and is rejected for
    /// writable Native mounts by `checkConditionalWriteSingleAttemptSupport`.
    if (single_attempt_s3_client)
        ws.s3_client_override = single_attempt_s3_client;
#endif
    return ws;
}

PutResult ObjectStorageBackend::putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws = conditionalWriteSettings();
        ws.object_storage_write_if_none_match = "*";
        return nativeConditionalPut(key, bytes, ws, meta);
    }

    std::lock_guard lock(emu_mutex);
    if (emuExists(key))
        return {PutOutcome::PreconditionFailed, {}};

    emuWrite(key, bytes, meta);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    return {PutOutcome::Done, Token{std::to_string(seq), TokenType::Emulated}};
}

WriteSinkPtr ObjectStorageBackend::putIfAbsentStream(const String & key, const ObjectMeta & meta)
{
    if (mode == Mode::Native)
    {
        /// Same WriteSettings construction as putIfAbsent — the condition rides on the write buffer
        /// and is checked when finalize completes the object.
        WriteSettings ws = conditionalWriteSettings();
        ws.object_storage_write_if_none_match = "*";
        std::optional<ObjectAttributes> attrs;
        if (!meta.empty())
            attrs.emplace(meta.begin(), meta.end());   /// ObjectMeta is the same map type as ObjectAttributes
        auto buf = object_storage->writeObject(
            StoredObject(key), WriteMode::Rewrite, attrs, DBMS_DEFAULT_BUFFER_SIZE, ws);
        return std::make_unique<NativeStreamingSink>(*this, key, std::move(buf));
    }

    return std::make_unique<EmulatedBufferedSink>(*this, key, meta);
}

PutResult ObjectStorageBackend::putOverwrite(const String & key, const String & bytes, const Token & expected, const ObjectMeta & meta)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws = conditionalWriteSettings();
        ws.object_storage_write_if_match = expected.value;
        return nativeConditionalPut(key, bytes, ws, meta);
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return {PutOutcome::PreconditionFailed, {}};
    if (!tokenMatches(emuObserveToken(key), expected))
        return {PutOutcome::PreconditionFailed, {}};

    emuWrite(key, bytes, meta);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    return {PutOutcome::Done, Token{std::to_string(seq), TokenType::Emulated}};
}

CasResult ObjectStorageBackend::casPut(const String & key, const String & bytes, const std::optional<Token> & expected, const ObjectMeta & meta)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws = conditionalWriteSettings();
        if (expected.has_value())
            ws.object_storage_write_if_match = expected->value;
        else
            ws.object_storage_write_if_none_match = "*";

        /// The PUT-side outcomes (Done / PreconditionFailed) collapse onto CAS outcomes 1:1: a lost
        /// condition — whether a mismatched If-Match or a 404 on an If-Match PUT — is a Conflict.
        PutResult put = nativeConditionalPut(key, bytes, ws, meta);
        return put.outcome == PutOutcome::Done
            ? CasResult{CasOutcome::Committed, put.token}
            : CasResult{CasOutcome::Conflict, {}};
    }

    std::lock_guard lock(emu_mutex);
    const bool exists = emuExists(key);

    if (!expected.has_value())
    {
        if (exists)
            return {CasOutcome::Conflict, {}};
    }
    else
    {
        if (!exists)
            return {CasOutcome::Conflict, {}};
        if (!tokenMatches(emuObserveToken(key), *expected))
            return {CasOutcome::Conflict, {}};
    }

    emuWrite(key, bytes, meta);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    return {CasOutcome::Committed, Token{std::to_string(seq), TokenType::Emulated}};
}

DeleteOutcome ObjectStorageBackend::deleteExact(const String & key, const Token & token)
{
    if (mode == Mode::Native)
    {
        /// `removeObjectIfTokenMatches` maps onto `DeleteOutcome` one-to-one. `NOT_IMPLEMENTED` from a
        /// backend that does not enforce conditional removal propagates — fail-closed by construction.
        auto result = object_storage->removeObjectIfTokenMatches(StoredObject(key), token.value);
        DeleteOutcome d;
        d.created_delete_marker = result.created_delete_marker;
        switch (result.outcome)
        {
            case ConditionalRemoveOutcome::Removed:
                d.kind = DeleteOutcome::Kind::Deleted;
                break;
            case ConditionalRemoveOutcome::TokenMismatch:
                d.kind = DeleteOutcome::Kind::TokenMismatch;
                break;
            case ConditionalRemoveOutcome::NotFound:
                d.kind = DeleteOutcome::Kind::NotFound;
                break;
        }
        return d;
    }

    std::lock_guard lock(emu_mutex);
    DeleteOutcome d;
    if (!emuExists(key))
    {
        d.kind = DeleteOutcome::Kind::NotFound;
        return d;
    }
    if (!tokenMatches(emuObserveToken(key), token))
    {
        d.kind = DeleteOutcome::Kind::TokenMismatch;
        return d;
    }

    object_storage->removeObjectIfExists(StoredObject(emuPath(key)));
    emu_tokens.erase(key);
    d.kind = DeleteOutcome::Kind::Deleted;
    return d;
}

PutResult ObjectStorageBackend::promoteStaged(const String & staging_key, const String & blob_key)
{
    if (mode != Mode::Native)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::promoteStaged is Native-mode only (EmulatedSingleProcess has no "
            "server-side conditional copy and is never selected for S3 staging)");

    /// WRITE-ONCE conditional server-side copy staging -> blob (`If-None-Match:*` on the destination),
    /// via `IObjectStorage::copyObjectConditional`. `created` ⇒ the destination ETag is the new
    /// incarnation token; `!created` ⇒ the destination already existed = the "lost the race" 412 signal.
    /// Counted with the same attempt/outcome counters as every other conditional write
    /// (`finalizeConditionalWriteInstrumented`'s contract): the copy is a conditional
    /// create attempt too, and it is initiated by the controlled content-addressed upload path — an
    /// uncounted attempt would hide SDK-versus-controller retry accounting.
    /// A resolved `!created` is counted `Unresolved` (the 412 does not prove who created the occupant),
    /// mirroring the PUT paths.
    recordConditionalWriteAttemptStarted();
    ConditionalCopyResult res;
    try
    {
        res = object_storage->copyObjectConditional(
            StoredObject(staging_key), StoredObject(blob_key), getReadSettings(), WriteSettings{});
    }
    catch (const std::exception & e)
    {
        recordConditionalWriteOutcome(classifyConditionalWriteResult(e));
        throw;
    }
    recordConditionalWriteOutcome(res.created ? classifyConditionalWriteResult() : CasWriteOutcome::Unresolved);
    if (!res.created)
        return {PutOutcome::PreconditionFailed, {}};
    return {PutOutcome::Done, Token{res.dest_etag, native_token_type}};
}

Token ObjectStorageBackend::resurrectStaged(const String & staging_key, const String & blob_key,
                                            const String & fresh_header, uint64_t staging_payload_offset)
{
    if (mode != Mode::Native)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::resurrectStaged is Native-mode only (EmulatedSingleProcess has no "
            "server-side copy and is never selected for S3 staging)");

    /// A condemned-resurrect overwrite is not a verbatim server-side copy: that would reproduce the
    /// condemned incarnation's exact bytes and therefore its identical ETag. The queued exact-token
    /// delete of the condemned incarnation could then delete the live resurrection. Instead re-upload
    /// the writer's own staging payload (skipping the staging object's envelope header) under a
    /// fresh-tagged `fresh_header`, so the resurrected body and token differ from the condemned one
    /// (`INV-NO-RETURN`).
    /// The source is always the writer's staging object, never the condemned `blob_key`; the caller
    /// reaches this method only after observing `Condemned` metadata, so the overwrite targets no live blob.
    auto in = object_storage->readObject(StoredObject(staging_key), getReadSettings(), /*read_hint=*/std::nullopt);
    if (staging_payload_offset)
        in->ignore(staging_payload_offset);

    /// Unconditional overwrite of the condemned body (plain WriteSettings — no If-Match/If-None-Match).
    auto out = object_storage->writeObject(
        StoredObject(blob_key), WriteMode::Rewrite, /*attributes=*/std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, WriteSettings{});
    out->write(fresh_header.data(), fresh_header.size());
    copyData(*in, *out);
    out->finalize();

    /// The plain write does not reliably surface the destination ETag across dialects, so HEAD the
    /// fresh incarnation to learn its token.
    const auto hr = nativeHead(blob_key);
    if (!hr)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ObjectStorageBackend::resurrectStaged: blob {} is absent immediately after the resurrect "
            "re-upload — failing closed", blob_key);
    return hr->token;
}

ListPage ObjectStorageBackend::list(const String & prefix, const String & cursor, size_t limit)
{
    /// Use the lazy object-storage iterator instead of `listObjects(..., max_keys=0)`: the latter
    /// materialized the whole prefix, then sliced client-side, so a paginated walk re-fetched the full
    /// subtree for every page. The backend cursor is "last key returned" (exclusive on resume).
    ///
    /// Some backends ignore `start_after`; filtering `key <= cursor` keeps the contract correct there,
    /// only losing the resume optimization. S3 honors `start_after` and avoids the hot-path re-scan.
    if (limit == 0)
        return {};

    const String physical_prefix = (mode == Mode::EmulatedSingleProcess) ? emuPath(prefix) : prefix;
    const String strip = (mode == Mode::EmulatedSingleProcess) ? emuPath("") : String{};
    if (mode == Mode::EmulatedSingleProcess)
    {
        RelativePathsWithMetadata children;
        object_storage->listObjects(physical_prefix, children, /*max_keys=*/0);

        std::vector<ListedKey> all;
        all.reserve(children.size());
        for (const auto & child : children)
        {
            if (child->relative_path.substr(0, physical_prefix.size()) != physical_prefix)
                continue;
            ListedKey lk;
            lk.key = child->relative_path.substr(strip.size());
            lk.size = child->metadata ? child->metadata->size_bytes : 0;
            if (child->metadata)
                lk.token = tokenForList(child->metadata->etag);
            all.push_back(std::move(lk));
        }
        std::sort(all.begin(), all.end(), [](const ListedKey & a, const ListedKey & b) { return a.key < b.key; });

        ListPage page;
        auto all_it = cursor.empty()
            ? std::lower_bound(all.begin(), all.end(), prefix, [](const ListedKey & a, const String & s) { return a.key < s; })
            : std::upper_bound(all.begin(), all.end(), cursor, [](const String & s, const ListedKey & a) { return s < a.key; });
        while (all_it != all.end() && page.keys.size() < limit)
        {
            page.keys.push_back(*all_it);
            ++all_it;
        }
        if (!page.keys.empty() && all_it != all.end())
            page.next_cursor = page.keys.back().key;
        return page;
    }

    const std::optional<String> start_after = cursor.empty()
        ? std::nullopt
        : std::optional<String>(cursor);

    ListPage page;
    auto it = object_storage->iterate(physical_prefix, /*max_keys=*/0, /*with_tags=*/false, start_after);
    for (; it->isValid(); it->next())
    {
        const auto child = it->current();
        if (child->relative_path.substr(0, physical_prefix.size()) != physical_prefix)
            continue;

        ListedKey lk;
        lk.key = child->relative_path.substr(strip.size());
        if (!cursor.empty() && lk.key <= cursor)
            continue;

        lk.size = child->metadata ? child->metadata->size_bytes : 0;
        /// Surface the per-key incarnation token (matching what `head` would return, see above) so the
        /// `supportsListTokens() == true` capability is honest. A listing without an etag leaves the
        /// token unset, which GC discover treats as Read (fail closed). The supportsListTokens()+
        /// empty-etag gate now lives in tokenForList.
        if (child->metadata)
            lk.token = tokenForList(child->metadata->etag);

        if (page.keys.size() == limit)
        {
            page.next_cursor = page.keys.back().key;
            break;
        }
        page.keys.push_back(std::move(lk));
    }

    return page;
}

}

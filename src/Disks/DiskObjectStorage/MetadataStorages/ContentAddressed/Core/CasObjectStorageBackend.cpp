#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>

#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Core/Defines.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
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

ObjectStorageBackend::ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_, uint64_t conditional_single_put_cap_)
    : object_storage(std::move(object_storage_))
    , mode(mode_)
    , conditional_single_put_cap(conditional_single_put_cap_)
    , emu_root(object_storage->getCommonKeyPrefix())
{
    if (mode == Mode::Native && object_storage->conditionalOpsUseGenerationTokens())
        native_token_type = TokenType::Generation;
}

/// See Backend::checkStorePreconditions. Only the Native, generation-dialect (GCS) combination has
/// anything to check: a token-exact DELETE on a versioned bucket archives a noncurrent generation
/// instead of reclaiming storage, so GC "reclaim" would silently stop reclaiming.
void ObjectStorageBackend::checkStorePreconditions()
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
    hr.token = Token{metadata->etag, native_token_type};
    hr.attributes = ObjectMeta(metadata->attributes.begin(), metadata->attributes.end());
    return hr;
}

/// Finalize a conditional write (the condition rode on the buffer's WriteSettings) and map a
/// precondition loss to an OUTCOME — anything else propagates.
///
/// A backend reports a lost condition as an `S3Exception` carrying the canonical S3 error code string
/// from the response XML `<Code>` (`S3Exception::getExceptionName`); a conditional-write 412 is
/// UNMODELED for the AWS SDK (its enum value is UNKNOWN), so the name — matched EXACTLY, never as a
/// substring of free text — is the typed signal. A `404 NoSuchKey` on an `If-Match` PUT (the key was
/// deleted out from under us) is treated identically: protocol callers handle 'mismatch' and 'gone'
/// the same way (re-validate), so both collapse onto `PreconditionFailed`. `NoSuchKey` IS modeled by
/// the SDK, and `WriteBufferFromS3` retries it internally surfacing the exhaustion with the typed enum
/// code (and no name), so the enum is matched as well as the name. The mapping is fail-safe in
/// direction: a misread error becomes a retryable PreconditionFailed/Conflict, never a false success.
///
/// HONEST NOTE: the Native conditional-write paths are exercised end-to-end only at M-W against
/// RustFS; unit coverage is the Emulated mode, the typed-catch compile path, and the classifier
/// itself (CasS3Signal in gtest_cas_backend.cpp — hence the detail:: exposure in the header).
#if USE_AWS_S3
PutOutcome detail::finalizeConditionalWrite(WriteBuffer & buf)
{
    try
    {
        buf.finalize();
    }
    catch (const S3Exception & e)
    {
        if (e.getExceptionName() == "PreconditionFailed"
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
    if (finalizeConditionalWrite(*buf) == PutOutcome::PreconditionFailed)
        return {PutOutcome::PreconditionFailed, {}};

    /// Record the token of the incarnation WE just wrote (model WCreate). The S3 write returns
    /// its object ETag in the PutObject/CompleteMultipartUpload response, so no follow-up HEAD
    /// is needed — this is ~73% of the CA backend's HEADs. A backend with no write-time ETag
    /// (local files) returns nullopt and we fall back to the HEAD (a cheap local stat there).
    Token token;
    if (auto etag = buf->getResultObjectETag(); etag && !etag->empty())
        token = Token{*etag, native_token_type};
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
        if (finalizeConditionalWrite(*write_buf) == PutOutcome::PreconditionFailed)
            return {PutOutcome::PreconditionFailed, {}};

        /// Record the token of the incarnation we just wrote (model WCreate). The S3 write
        /// returns its object ETag in the response, so no follow-up HEAD is needed (the bulk of
        /// the CA backend's HEADs). Backends with no write-time ETag (local) return nullopt and
        /// we fall back to the HEAD (a cheap local stat there).
        Token token;
        if (auto etag = write_buf->getResultObjectETag(); etag && !etag->empty())
            token = Token{*etag, backend.nativeTokenType()};
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
/// read window (spec 2026-07-02 snapshot-streaming §Backend seam). Never read-whole-then-substr —
/// the snapshot runs this serves are GBs at scale and the caller's memory budget is O(block).
static String readObjectRanged(IObjectStorage & object_storage, const String & path, Range range,
                               uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(StoredObject(path), getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    if (range.whole())
    {
        readStringUntilEOF(content, *buf);
        return content;
    }

    /// Clamp exactly like the old substr path: an offset at or past EOF yields an empty result.
    /// `seek` past the object size may throw depending on the storage, so fail-close the window
    /// against the known size before touching the buffer position.
    /// M1 (final review): callers on the Native path already HEAD'ed the key — threading that size
    /// here saves one metadata round-trip per ranged read against real S3. 0 = unknown, fetch.
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
/// byte and bounded to its last (spec 2026-07-02 snapshot-streaming §Backend seam). Mirrors
/// `readObjectRanged`'s seek + bound, but RETURNS the buffer instead of draining it — the caller reads
/// at its own pace, so nothing is materialized whole. Returns nullptr when the offset is at or past EOF
/// (the empty-window clamp), matching the ranged-get contract.
static std::unique_ptr<ReadBuffer> openObjectRangedStream(IObjectStorage & object_storage, const String & path, Range range,
                                                          uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(StoredObject(path), getReadSettings(), /*read_hint=*/std::nullopt);
    if (range.whole())
        return buf;

    /// Clamp exactly like `readObjectRanged`: an offset at or past EOF yields an empty stream, and
    /// `seek` past the object size may throw depending on the storage, so fail-close against the known
    /// size before touching the buffer position.
    /// M1 (final review): callers on the Native path already HEAD'ed the key — threading that size
    /// here saves one metadata round-trip per ranged read against real S3. 0 = unknown, fetch.
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
        /// contract — callers such as `Store::loadShardDecoded` already handle a nullopt return and
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
    /// B38: a path that exists on the Local filesystem but yields NO object metadata is a DIRECTORY, not
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
/// mismatch TERMINATED the server from the upload worker, M-W T13). Integrity for these keys is
/// the conditional PUT outcome + the observed token - a recheck adds nothing and races by design.
///
/// On a generation-token store (GCS), a conditional write must ALSO never take the multipart path:
/// GCS enforces no preconditions on CompleteMultipartUpload (measured 2026-07-03), so a lost
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
    if (emuObserveToken(key) != expected)
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
        if (emuObserveToken(key) != *expected)
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
    if (emuObserveToken(key) != token)
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
    const ConditionalCopyResult res = object_storage->copyObjectConditional(
        StoredObject(staging_key), StoredObject(blob_key), getReadSettings(), WriteSettings{});
    if (!res.created)
        return {PutOutcome::PreconditionFailed, {}};
    return {PutOutcome::Done, Token{res.dest_etag, native_token_type}};
}

Token ObjectStorageBackend::resurrectStaged(const String & staging_key, const String & blob_key)
{
    if (mode != Mode::Native)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::resurrectStaged is Native-mode only (EmulatedSingleProcess has no "
            "server-side copy and is never selected for S3 staging)");

    /// UNCONDITIONAL server-side copy staging -> blob — the sanctioned condemned-resurrect overwrite
    /// (spec §5/§9). The source is ALWAYS the writer's own staging object, NEVER a read of the
    /// condemned `blob_key` (`feedback_ca_resurrect_invariant`). `copyObject` does not surface the
    /// destination ETag, so HEAD the fresh incarnation to learn its token.
    object_storage->copyObject(
        StoredObject(staging_key), StoredObject(blob_key), getReadSettings(), WriteSettings{});
    const auto hr = nativeHead(blob_key);
    if (!hr)
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
            "ObjectStorageBackend::resurrectStaged: blob {} is absent immediately after the resurrect "
            "copy — failing closed", blob_key);
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
            if (child->metadata && !child->metadata->etag.empty())
                lk.token = Token{child->metadata->etag, native_token_type};
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
        /// token unset, which GC discover treats as Read (fail closed).
        if (supportsListTokens() && child->metadata && !child->metadata->etag.empty())
            lk.token = Token{child->metadata->etag, native_token_type};

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

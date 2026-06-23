#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>

#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Core/Defines.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteSettings.h>

#include <Common/Exception.h>

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
}
}

namespace DB::Cas
{

ObjectStorageBackend::ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_)
    : object_storage(std::move(object_storage_))
    , mode(mode_)
    , emu_root(object_storage->getCommonKeyPrefix())
{
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
    hr.token = Token{metadata->etag, TokenType::ETag};
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
        token = Token{*etag, TokenType::ETag};
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
    NativeStreamingSink(Backend & backend_, String key_, std::unique_ptr<WriteBufferFromFileBase> write_buf_)
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
            token = Token{*etag, TokenType::ETag};
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
    Backend & backend;
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

/// Read the whole object at `path` and honor `range` by substr. Objects on this path are small (root
/// manifests, tree/blob bodies in tests), so read-whole + substr is the correct and simplest way to
/// serve a sub-range — no seek/limit machinery needed.
static String readObjectRanged(IObjectStorage & object_storage, const String & path, Range range)
{
    auto buf = object_storage.readObject(StoredObject(path), getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);

    if (range.whole())
        return content;

    const size_t offset = static_cast<size_t>(range.offset);
    if (offset >= content.size())
        return {};
    if (range.length.has_value())
        return content.substr(offset, static_cast<size_t>(*range.length));
    return content.substr(offset);
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
            gr.bytes = readObjectRanged(*object_storage, key, range);
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
    HeadResult hr;
    hr.exists = true;
    hr.size = metadata ? metadata->size_bytes : 0;
    if (metadata)
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
static WriteSettings casWriteSettings()
{
    WriteSettings ws;
    ws.s3_skip_check_objects_after_upload = true;
    return ws;
}

PutResult ObjectStorageBackend::putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws = casWriteSettings();
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
        WriteSettings ws = casWriteSettings();
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
        WriteSettings ws = casWriteSettings();
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
        WriteSettings ws = casWriteSettings();
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

ListPage ObjectStorageBackend::list(const String & prefix, const String & cursor, size_t limit)
{
    /// Enumerate the prefix, then apply cursor/limit over the SORTED keys exactly like the in-memory
    /// backend (the object storage listing order is unspecified, so we sort to make pagination stable).
    /// In EmulatedSingleProcess mode physical paths are scoped under the storage root, so we list under
    /// the resolved physical prefix and strip the root back to the logical key the caller passed in.
    const String physical_prefix = (mode == Mode::EmulatedSingleProcess) ? emuPath(prefix) : prefix;
    const String strip = (mode == Mode::EmulatedSingleProcess) ? emuPath("") : String{};

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
        all.push_back(std::move(lk));
    }
    std::sort(all.begin(), all.end(), [](const ListedKey & a, const ListedKey & b) { return a.key < b.key; });

    ListPage page;
    const String & start = (cursor > prefix) ? cursor : prefix;
    auto it = std::lower_bound(all.begin(), all.end(), start,
        [](const ListedKey & a, const String & s) { return a.key < s; });

    while (it != all.end() && page.keys.size() < limit)
    {
        page.keys.push_back(*it);
        ++it;
    }

    if (it != all.end())
        page.next_cursor = it->key;

    return page;
}

}

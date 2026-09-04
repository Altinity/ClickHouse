#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <Poco/Exception.h>
#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
}
}

namespace DB::Cas
{

namespace
{

/// The windowed slice of `data` for `range`, with the clamping `getStream` documents: an offset at or
/// past EOF yields an empty result; an open-ended length runs to EOF.
String sliceWindow(const String & data, Range range)
{
    const size_t offset = static_cast<size_t>(range.offset);
    if (offset >= data.size())
        return {};
    if (range.length.has_value())
        return data.substr(offset, static_cast<size_t>(*range.length));
    return data.substr(offset);
}

/// A CALLER bug, refused before it ever reaches the store: an empty, wildcard or list value would
/// turn a conditional mutation into an unconditional one. Stricter than
/// `isIncarnationValue(Dialect::Emulated, ...)` (non-empty only): this backend is a test double
/// reused across the whole CAS gtest suite, so it also refuses `*` and a comma even though no value
/// it currently mints can contain either.
bool isValidEmulatedTokenValue(const String & value)
{
    return !value.empty() && value != "*" && value.find(',') == String::npos;
}

/// Every conditional mutation refuses a malformed expected value unconditionally, matching the
/// production backend's own guard: this is the test backend for that contract, and must not accept
/// anything production refuses. A caller holding only a HEAD-derived value for a key it has not
/// confirmed exists must gate on presence itself before calling, exactly as every production call
/// site already does.
void checkExpectedValue(const String & key, const String & value)
{
    if (!isValidEmulatedTokenValue(value))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "InMemoryBackend: refusing a conditional mutation of '{}' with a malformed token '{}': "
            "an empty, wildcard or list token would turn the precondition into an unconditional write",
            key, value);
}

}

Token InMemoryBackend::mintToken()
{
    Token t;
    t.value = std::to_string(++token_seq_);
    t.type = TokenType::Emulated;
    return t;
}

std::exception_ptr InMemoryBackend::takeArmedFailure(ArmedFailures & armed, const String & key)
{
    std::lock_guard lock(mutex_);
    const auto it = armed.find(key);
    if (it == armed.end() || it->second.empty())
        return nullptr;
    std::exception_ptr error = it->second.front();
    it->second.erase(it->second.begin());
    return error;
}

std::function<void()> InMemoryBackend::hookFor(const Hooks & hooks, const String & key) const
{
    std::lock_guard lock(mutex_);
    const auto it = hooks.find(key);
    return it == hooks.end() ? std::function<void()>{} : it->second;
}

std::optional<Backend::Raw> InMemoryBackend::read(const String & key, TransportAccess &)
{
    if (auto armed = takeArmedFailure(read_failures_, key))
        std::rethrow_exception(armed);

    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return std::nullopt;

    return Raw{it->second.bytes, it->second.token.value};
}

std::optional<GetStreamResult> InMemoryBackend::getStream(const String & key, Range range)
{
    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return std::nullopt;

    /// Copy the windowed bytes into an owning buffer — the in-memory backend has no separate storage
    /// to stream from, so the "stream" reads from a private copy of exactly the requested window.
    GetStreamResult sr;
    sr.stream = std::make_unique<ReadBufferFromOwnString>(sliceWindow(it->second.bytes, range));
    sr.token = it->second.token;
    return sr;
}

std::unique_ptr<ReadBuffer> InMemoryBackend::stream(const String & key, TransportAccess &)
{
    auto sr = getStream(key, Range{});
    if (!sr)
        return nullptr;
    return std::move(sr->stream);
}

std::optional<Backend::RawMeta> InMemoryBackend::head(const String & key, TransportAccess &)
{
    if (auto armed = takeArmedFailure(head_failures_, key))
        std::rethrow_exception(armed);

    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return std::nullopt;

    return RawMeta{static_cast<uint64_t>(it->second.bytes.size()), it->second.token.value};
}

std::expected<String, Backend::RawConflict> InMemoryBackend::write(
    const String & key, const String & bytes, const std::optional<String> & expected_value, TransportAccess &)
{
    return applyWrite(key, bytes, expected_value);
}

std::expected<String, Backend::RawConflict> InMemoryBackend::applyWrite(
    const String & key, const String & bytes, const std::optional<String> & expected_value)
{
    if (expected_value)
        checkExpectedValue(key, *expected_value);

    if (auto armed = takeArmedFailure(write_failures_, key))
        std::rethrow_exception(armed);

    /// Both hooks run with NO lock held: a hook exists to read and write this backend from inside a
    /// write, and `mutex_` is not recursive.
    if (auto hook = hookFor(before_write_hooks_, key))
        hook();

    auto result = writeUnderLock(key, bytes, expected_value);
    if (!result.has_value())
        return result;

    if (auto hook = hookFor(write_committed_hooks_, key))
        hook();

    /// Last, so the object is durable and every observer has run before the response goes missing.
    if (takeAmbiguousLandedWrite(key))
        throw Poco::TimeoutException("InMemoryBackend: the write of '" + key + "' landed and its response was lost");

    return result;
}

std::expected<String, Backend::RawConflict> InMemoryBackend::writeUnderLock(
    const String & key, const String & bytes, const std::optional<String> & expected_value)
{
    std::lock_guard lock(mutex_);

    // One-shot injected ambiguous outcome: throw WITHOUT touching the store, modeling a request
    // whose own attempt outcome never reached the caller. Poco::TimeoutException, not
    // DB::Exception, is deliberate: a client-side timeout is the real shape of this failure, and
    // its class is what every caller classifies by -- ambiguous, in both build configurations.
    auto ambiguous_it = ambiguous_write_keys_.find(key);
    if (ambiguous_it != ambiguous_write_keys_.end())
    {
        ambiguous_write_keys_.erase(ambiguous_it);
        throw Poco::TimeoutException("InMemoryBackend: injected ambiguous write outcome for '" + key + "'");
    }

    auto refuse_it = refuse_next_write_keys_.find(key);
    if (refuse_it != refuse_next_write_keys_.end())
    {
        refuse_next_write_keys_.erase(refuse_it);
        return std::unexpected(RawConflict{});
    }

    if (!expected_value)
    {
        if (store_.contains(key))
            return std::unexpected(RawConflict{});

        Token t = mintToken();
        Object obj;
        obj.bytes = bytes;
        obj.token = t;
        store_[key] = std::move(obj);
        return t.value;
    }

    auto it = store_.find(key);
    if (it == store_.end())
        return std::unexpected(RawConflict{});
    if (enforce_tokens_ && it->second.token.value != *expected_value)
        return std::unexpected(RawConflict{});

    Token t = mintToken();
    it->second.bytes = bytes;
    it->second.token = t;
    return t.value;
}

void InMemoryBackend::publish(const BlobPublishRequest & request, TransportAccess &)
{
    if (const auto * streaming = std::get_if<StreamingBlobPublication>(&request.publication))
    {
        std::unique_ptr<ReadBuffer> payload = streaming->open_payload();
        if (!payload)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "InMemoryBackend::publish: payload source for {} returned no reader",
                request.destination_key);

        /// Drain before taking the store lock: the source may itself read another object from this
        /// backend. The complete body remains private until its size has been validated.
        String body = streaming->fresh_envelope;
        blob_publication_detail::BlobPayloadCopyResult copy_result;
        {
            WriteBufferFromString out(body, AppendModeTag{});
            copy_result = blob_publication_detail::copyBlobPayloadBounded(*payload, out, streaming->payload_size);
            if (copy_result.exact(streaming->payload_size))
                out.finalize();
            else
                out.cancel();
        }

        if (!copy_result.exact(streaming->payload_size))
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "InMemoryBackend::publish: source yielded {}{} payload bytes for {}, declared {} -- nothing was published",
                copy_result.has_excess ? "more than " : "",
                copy_result.copied,
                request.destination_key,
                streaming->payload_size);

        std::lock_guard lock(mutex_);
        Object object;
        object.bytes = std::move(body);
        object.token = mintToken();
        store_[request.destination_key] = std::move(object);
        return;
    }

    const auto & staged = std::get<VerbatimStagedBlobPublication>(request.publication);
    std::lock_guard lock(mutex_);
    const auto source = store_.find(staged.object_key);
    if (source == store_.end())
        throw Exception(
            ErrorCodes::FILE_DOESNT_EXIST,
            "InMemoryBackend::publish: staging object {} is absent",
            staged.object_key);

    Object object;
    object.bytes = source->second.bytes;
    object.token = mintToken();
    store_[request.destination_key] = std::move(object);
}

DeleteOutcome InMemoryBackend::applyDelete(const String & key, const Token & token)
{
    // Caller holds the mutex.
    auto it = store_.find(key);
    if (it == store_.end())
    {
        DeleteOutcome d;
        d.kind = DeleteOutcome::Kind::NotFound;
        return d;
    }

    if (enforce_tokens_ && it->second.token != token)
    {
        DeleteOutcome d;
        d.kind = DeleteOutcome::Kind::TokenMismatch;
        return d;
    }

    store_.erase(it);
    DeleteOutcome d;
    d.kind = DeleteOutcome::Kind::Deleted;
    d.created_delete_marker = simulate_delete_markers_;
    return d;
}

Backend::RawRemoval InMemoryBackend::remove(const String & key, const String & expected_value, TransportAccess &)
{
    /// See `checkExpectedValue`: a malformed value is refused as a caller bug, unconditionally --
    /// covering both the immediate delete below and the hold_deletes_ enqueue path, so a queued
    /// PendingDelete can never carry a malformed value either.
    checkExpectedValue(key, expected_value);

    const Token expected{expected_value, TokenType::Emulated};

    std::lock_guard lock(mutex_);

    if (hold_deletes_)
    {
        // Validate the key exists (and the value matches if enforcing) before queuing,
        // but don't remove yet — just enqueue.
        auto it = store_.find(key);
        if (it == store_.end())
            return RawRemoval::Gone;
        if (enforce_tokens_ && it->second.token != expected)
            return RawRemoval::Mismatch;
        PendingDelete pd;
        pd.key = key;
        pd.token = expected;
        pending_deletes_.push_back(std::move(pd));
        return simulate_delete_markers_ ? RawRemoval::DeleteMarker : RawRemoval::Removed;
    }

    const DeleteOutcome d = applyDelete(key, expected);
    switch (d.kind)
    {
        case DeleteOutcome::Kind::Deleted:
            return d.created_delete_marker ? RawRemoval::DeleteMarker : RawRemoval::Removed;
        case DeleteOutcome::Kind::NotFound:
            return RawRemoval::Gone;
        case DeleteOutcome::Kind::TokenMismatch:
            return RawRemoval::Mismatch;
    }
    UNREACHABLE();
}

Backend::RawListPage InMemoryBackend::list(const String & prefix, const String & cursor, size_t limit, TransportAccess &)
{
    if (limit == 0)
        return {};

    std::lock_guard lock(mutex_);
    RawListPage page;

    // Cursor is the last key returned by the previous page.
    auto it = cursor.empty() ? store_.lower_bound(prefix) : store_.upper_bound(cursor);

    size_t count = 0;
    while (it != store_.end() && count < limit)
    {
        if (!it->first.starts_with(prefix))
            break;

        RawListedKey lk;
        lk.key = it->first;
        lk.size = static_cast<uint64_t>(it->second.bytes.size());
        lk.value = it->second.token.value;   /// in-memory backend always surfaces it (supportsListTokens == true)
        page.keys.push_back(std::move(lk));
        ++count;
        ++it;
    }

    // Set next_cursor if there are more keys in this prefix
    if (!page.keys.empty() && it != store_.end() && it->first.starts_with(prefix))
        page.next_cursor = page.keys.back().key;

    return page;
}

bool InMemoryBackend::refreshCredentials()
{
    std::lock_guard lock(mutex_);
    ++refresh_credentials_calls_;
    return refresh_credentials_result_;
}

size_t InMemoryBackend::refreshCredentialsCalls() const
{
    std::lock_guard lock(mutex_);
    return refresh_credentials_calls_;
}

void InMemoryBackend::failNextWriteWith(const String & key, std::exception_ptr error)
{
    std::lock_guard lock(mutex_);
    write_failures_[key].push_back(std::move(error));
}

void InMemoryBackend::failNextReadWith(const String & key, std::exception_ptr error)
{
    std::lock_guard lock(mutex_);
    read_failures_[key].push_back(std::move(error));
}

void InMemoryBackend::failNextHeadWith(const String & key, std::exception_ptr error)
{
    std::lock_guard lock(mutex_);
    head_failures_[key].push_back(std::move(error));
}

void InMemoryBackend::onBeforeWrite(const String & key, std::function<void()> hook)
{
    std::lock_guard lock(mutex_);
    before_write_hooks_[key] = std::move(hook);
}

void InMemoryBackend::onWriteCommitted(const String & key, std::function<void()> hook)
{
    std::lock_guard lock(mutex_);
    write_committed_hooks_[key] = std::move(hook);
}

void InMemoryBackend::setHoldDeletes(bool hold)
{
    std::lock_guard lock(mutex_);
    hold_deletes_ = hold;
}

size_t InMemoryBackend::pendingDeletes() const
{
    std::lock_guard lock(mutex_);
    return pending_deletes_.size();
}

DeleteOutcome InMemoryBackend::landPendingDelete(size_t i)
{
    std::lock_guard lock(mutex_);
    if (i >= pending_deletes_.size())
    {
        DeleteOutcome d;
        d.kind = DeleteOutcome::Kind::NotFound;
        return d;
    }

    PendingDelete pd = pending_deletes_[i];
    pending_deletes_.erase(pending_deletes_.begin() + static_cast<ptrdiff_t>(i));

    // Apply the token check at LAND time — the object may have been modified since the delete was enqueued.
    return applyDelete(pd.key, pd.token);
}

void InMemoryBackend::refuseNextWrite(const String & key)
{
    std::lock_guard lock(mutex_);
    refuse_next_write_keys_.insert(key);
}

void InMemoryBackend::injectAmbiguousWrite(const String & key)
{
    std::lock_guard lock(mutex_);
    ambiguous_write_keys_.insert(key);
}

void InMemoryBackend::injectAmbiguousLandedWrite(const String & key)
{
    std::lock_guard lock(mutex_);
    ambiguous_landed_keys_.insert(key);
}

bool InMemoryBackend::takeAmbiguousLandedWrite(const String & key)
{
    std::lock_guard lock(mutex_);
    return ambiguous_landed_keys_.erase(key) != 0;
}

void InMemoryBackend::setEnforceTokens(bool enforce)
{
    std::lock_guard lock(mutex_);
    enforce_tokens_ = enforce;
}

void InMemoryBackend::setSimulateDeleteMarkers(bool simulate)
{
    std::lock_guard lock(mutex_);
    simulate_delete_markers_ = simulate;
}

void InMemoryBackend::setRefreshCredentialsResult(bool result)
{
    std::lock_guard lock(mutex_);
    refresh_credentials_result_ = result;
}

}

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <IO/WriteBufferFromString.h>
#include <base/defines.h>
#include <algorithm>

namespace DB::Cas
{

namespace
{

/// Memory-buffered WriteSink: accumulates the body in a WriteBufferFromOwnString and delegates the
/// conditional publish to InMemoryBackend::putIfAbsent at finalize — the single mutex acquisition
/// inside putIfAbsent gives atomicity for free. Nothing is ever published on cancel/destruction.
class InMemoryWriteSink final : public WriteSink
{
public:
    InMemoryWriteSink(InMemoryBackend & backend, String key, ObjectMeta meta)
        : backend_(backend)
        , key_(std::move(key))
        , meta_(std::move(meta))
    {
    }

    WriteBuffer & buffer() override { return buf_; }

    PutOutcome finalize(Token * out_token) override
    {
        chassert(!done_);   /// finalize after finalize/cancel is a misuse — see the WriteSink contract
        done_ = true;
        return backend_.putIfAbsent(key_, buf_.str(), out_token, meta_);
    }

    void cancel() noexcept override
    {
        done_ = true;
        buf_.cancel();
    }

    ~InMemoryWriteSink() override
    {
        if (!done_)
            cancel();
    }

private:
    InMemoryBackend & backend_;
    String key_;
    ObjectMeta meta_;
    WriteBufferFromOwnString buf_;
    bool done_ = false;
};

}

Token InMemoryBackend::mintToken()
{
    Token t;
    t.value = std::to_string(++token_seq_);
    t.type = TokenType::Emulated;
    return t;
}

std::optional<GetResult> InMemoryBackend::get(const String & key, Range range)
{
    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return std::nullopt;

    const String & data = it->second.bytes;
    String result;
    size_t offset = static_cast<size_t>(range.offset);
    if (offset >= data.size())
    {
        result = "";
    }
    else if (range.length.has_value())
    {
        size_t len = static_cast<size_t>(*range.length);
        result = data.substr(offset, len);
    }
    else
    {
        result = data.substr(offset);
    }

    GetResult gr;
    gr.bytes = std::move(result);
    gr.token = it->second.token;
    gr.attributes = it->second.meta;
    return gr;
}

HeadResult InMemoryBackend::head(const String & key)
{
    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return HeadResult{};

    HeadResult hr;
    hr.exists = true;
    hr.size = static_cast<uint64_t>(it->second.bytes.size());
    hr.token = it->second.token;
    hr.attributes = it->second.meta;
    return hr;
}

PutOutcome InMemoryBackend::putIfAbsent(const String & key, const String & bytes, Token * out_token, const ObjectMeta & meta)
{
    std::lock_guard lock(mutex_);
    if (store_.count(key))
        return PutOutcome::PreconditionFailed;

    Token t = mintToken();
    Object obj;
    obj.bytes = bytes;
    obj.token = t;
    obj.meta = meta;
    store_[key] = std::move(obj);
    if (out_token)
        *out_token = t;
    return PutOutcome::Done;
}

WriteSinkPtr InMemoryBackend::putIfAbsentStream(const String & key, const ObjectMeta & meta)
{
    return std::make_unique<InMemoryWriteSink>(*this, key, meta);
}

PutOutcome InMemoryBackend::putOverwrite(const String & key, const String & bytes, const Token & expected, Token * out_token, const ObjectMeta & meta)
{
    std::lock_guard lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end())
        return PutOutcome::PreconditionFailed;

    if (enforce_tokens_ && it->second.token != expected)
        return PutOutcome::PreconditionFailed;

    Token t = mintToken();
    it->second.bytes = bytes;
    it->second.token = t;
    it->second.meta = meta;
    if (out_token)
        *out_token = t;
    return PutOutcome::Done;
}

CasOutcome InMemoryBackend::casPut(const String & key, const String & bytes, const std::optional<Token> & expected, Token * out_token, const ObjectMeta & meta)
{
    std::lock_guard lock(mutex_);

    // One-shot injected conflict
    auto fail_it = fail_next_cas_.find(key);
    if (fail_it != fail_next_cas_.end())
    {
        fail_next_cas_.erase(fail_it);
        return CasOutcome::Conflict;
    }

    auto it = store_.find(key);
    bool exists = (it != store_.end());

    if (!expected.has_value())
    {
        // create-if-absent CAS
        if (exists)
            return CasOutcome::Conflict;
        Token t = mintToken();
        Object obj;
        obj.bytes = bytes;
        obj.token = t;
        obj.meta = meta;
        store_[key] = std::move(obj);
        if (out_token)
            *out_token = t;
        return CasOutcome::Committed;
    }
    else
    {
        // swap-if-current CAS
        if (!exists)
            return CasOutcome::Conflict;
        if (enforce_tokens_ && it->second.token != *expected)
            return CasOutcome::Conflict;
        Token t = mintToken();
        it->second.bytes = bytes;
        it->second.token = t;
        it->second.meta = meta;
        if (out_token)
            *out_token = t;
        return CasOutcome::Committed;
    }
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

DeleteOutcome InMemoryBackend::deleteExact(const String & key, const Token & token)
{
    std::lock_guard lock(mutex_);

    if (hold_deletes_)
    {
        // Validate the key exists (and token matches if enforcing) before queuing,
        // but don't remove yet — just enqueue.
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
        PendingDelete pd;
        pd.key = key;
        pd.token = token;
        pending_deletes_.push_back(std::move(pd));
        DeleteOutcome d;
        d.kind = DeleteOutcome::Kind::Deleted;
        d.created_delete_marker = simulate_delete_markers_;
        return d;
    }

    return applyDelete(key, token);
}

ListPage InMemoryBackend::list(const String & prefix, const String & cursor, size_t limit)
{
    std::lock_guard lock(mutex_);
    ListPage page;

    // Start from max(prefix, cursor) — whichever sorts later
    const String & start = (cursor > prefix) ? cursor : prefix;

    auto it = store_.lower_bound(start);

    size_t count = 0;
    while (it != store_.end() && count < limit)
    {
        if (it->first.substr(0, prefix.size()) != prefix)
            break;

        ListedKey lk;
        lk.key = it->first;
        lk.size = static_cast<uint64_t>(it->second.bytes.size());
        page.keys.push_back(std::move(lk));
        ++count;
        ++it;
    }

    // Set next_cursor if there are more keys in this prefix
    if (it != store_.end() && it->first.substr(0, prefix.size()) == prefix)
        page.next_cursor = it->first;

    return page;
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

void InMemoryBackend::failNextCasPut(const String & key)
{
    std::lock_guard lock(mutex_);
    fail_next_cas_.insert(key);
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

}

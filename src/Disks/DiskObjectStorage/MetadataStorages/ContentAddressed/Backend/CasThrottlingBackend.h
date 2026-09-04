#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>

#include "config.h"

#if USE_AWS_S3

#include <IO/S3Common.h>

#include <mutex>
#include <set>

namespace DB::ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

namespace DB::Cas
{

/// A `Backend` decorator that refuses a chosen share of the requests passing through it, so a test
/// can drive the request contract's reissue, resolve and give-up paths against a real backend with
/// no network in the picture.
///
/// The refusal is an `S3Exception` carrying `SLOW_DOWN` (HTTP 429) or `SERVICE_UNAVAILABLE` (503).
/// Both are RETRYABLE by `S3Exception::isRetryableError` -- its unretryable set holds neither -- and
/// that is the property being modelled: a retryable store refusal leaves the attempt AMBIGUOUS, so
/// the caller must resolve it by reading rather than treat it as a definite failure.
class ThrottlingBackend final : public Backend
{
public:
    /// `FirstPerKey` refuses the first request naming each key and forwards every later one;
    /// `EveryNth` refuses every n-th request across all keys.
    enum class Mode : uint8_t { FirstPerKey, EveryNth };

    ThrottlingBackend(BackendPtr inner_, Mode mode_, size_t n_, int status)
        : inner(std::move(inner_))
        , mode(mode_)
        , every_nth(n_)
        , error(status == 429 ? Aws::S3::S3Errors::SLOW_DOWN : Aws::S3::S3Errors::SERVICE_UNAVAILABLE)
    {
        if (status != 429 && status != 503)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ThrottlingBackend: refusal status must be 429 or 503, got {}", status);
        if (mode == Mode::EveryNth && every_nth == 0)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ThrottlingBackend: EveryNth needs a period of at least 1");
    }

    /// How many requests naming `key` this backend has refused. A `list` counts under its prefix and
    /// a `publish` under its destination key.
    size_t refusals(const String & key) const
    {
        std::lock_guard lock(mutex);
        const auto it = refusal_counts.find(key);
        return it == refusal_counts.end() ? 0 : it->second;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->read(key, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->head(key, access);
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        refuseOrPass(prefix);
        return inner->list(prefix, cursor, limit, access);
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->remove(key, expected_value, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->write(key, bytes, expected_value, access);
    }

    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->stream(key, access);
    }

    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        refuseOrPass(request.destination_key);
        inner->publish(request, access);
    }

    SentinelProbeResult probeSentinelRaw(const String & key, TransportAccess & access) override
    {
        refuseOrPass(key);
        return inner->probeSentinelRaw(key, access);
    }

    /// ---- The legacy surface, refused and forwarded AS legacy ----
    ///
    /// Not inherited from `Backend`: its forwarder would call the primitive on THIS object, so the
    /// inner backend would receive a primitive and any legacy override it carries would never run.
    /// Each request is refused (or not) exactly once, on whichever surface its caller used.
    std::optional<GetStreamResult> getStream(const String & key, Range range) override
    {
        refuseOrPass(key);
        return inner->getStream(key, range);
    }

    std::optional<GetResult> get(const String & key, Range range) override
    {
        refuseOrPass(key);
        return inner->get(key, range);
    }

    HeadResult head(const String & key) override
    {
        refuseOrPass(key);
        return inner->head(key);
    }

    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta) override
    {
        refuseOrPass(key);
        return inner->putIfAbsent(key, bytes, meta);
    }

    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                           const ObjectMeta & meta) override
    {
        refuseOrPass(key);
        return inner->putOverwrite(key, bytes, expected, meta);
    }

    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                     const ObjectMeta & meta) override
    {
        refuseOrPass(key);
        return inner->casPut(key, bytes, expected, meta);
    }

    DeleteOutcome deleteExact(const String & key, const Token & token) override
    {
        refuseOrPass(key);
        return inner->deleteExact(key, token);
    }

    ListPage list(const String & prefix, const String & cursor, size_t limit) override
    {
        refuseOrPass(prefix);
        return inner->list(prefix, cursor, limit);
    }

    void publishBlob(const BlobPublishRequest & request) override
    {
        refuseOrPass(request.destination_key);
        inner->publishBlob(request);
    }

    SentinelProbeResult probeSentinelRaw(const String & key) override
    {
        refuseOrPass(key);
        return inner->probeSentinelRaw(key);
    }

    /// Unhide the base overloads this class's own declarations would otherwise shadow: the
    /// convenience forms that omit Range/ObjectMeta/expected-token.
    using Backend::get;
    using Backend::getStream;
    using Backend::head;
    using Backend::list;
    using Backend::probeSentinelRaw;
    using Backend::putIfAbsent;
    using Backend::putOverwrite;
    using Backend::casPut;

    /// Facts about the wrapped backend, not requests to refuse.
    Dialect dialect() const override { return inner->dialect(); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
    uint64_t attemptTimeoutMs() const override { return inner->attemptTimeoutMs(); }
    bool refreshCredentials() override { return inner->refreshCredentials(); }
    void checkPoolPreconditions() override { inner->checkPoolPreconditions(); }
    void checkSkipAccessCheckSupport() override { inner->checkSkipAccessCheckSupport(); }
    void checkConditionalWriteSingleAttemptSupport() override { inner->checkConditionalWriteSingleAttemptSupport(); }

private:
    /// Decide this request and record a refusal, then throw outside the lock.
    void refuseOrPass(const String & key)
    {
        bool refusing = false;
        {
            std::lock_guard lock(mutex);
            refusing = mode == Mode::FirstPerKey ? refused_keys.insert(key).second : (++requests % every_nth) == 0;
            if (refusing)
                ++refusal_counts[key];
        }
        if (refusing)
            throw S3Exception(error, "throttled by ThrottlingBackend: {}", key);
    }

    const BackendPtr inner;
    const Mode mode;
    const size_t every_nth;
    const Aws::S3::S3Errors error;

    mutable std::mutex mutex;
    std::set<String> refused_keys;
    std::map<String, size_t> refusal_counts;
    size_t requests = 0;
};

}

#endif

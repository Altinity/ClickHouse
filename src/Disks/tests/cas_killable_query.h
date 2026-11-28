#pragma once

#include "config.h"

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>
#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/ThreadStatus.h>
#include <Interpreters/Context_fwd.h>
#include <Interpreters/ProcessList.h>
#include <Common/tests/gtest_global_context.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace DB::ErrorCodes
{
extern const int ABORTED;
}

/// Skips a test that enables a fail point in a build without libfiu, where enabling one throws `SUPPORT_IS_DISABLED`.
#define CAS_SKIP_WITHOUT_FAILPOINTS() \
    do \
    { \
        if constexpr (!::DB::Cas::tests::kFailPointsAvailable) \
            GTEST_SKIP() << "built without libfiu, so fail points are unavailable"; \
    } while (false)

namespace DB::Cas::tests
{

inline constexpr bool kFailPointsAvailable = USE_LIBFIU;

/// A query registered in the process list, so a kill reaches CAS code through the thread group as it
/// does in the server. Construct it on a thread that has a `ThreadStatus`; the constructing
/// thread stays attached. Destroy it after every worker thread joined.
struct CasKillableQuery
{
    explicit CasKillableQuery(ContextPtr global);
    ~CasKillableQuery();

    CasKillableQuery(const CasKillableQuery &) = delete;
    CasKillableQuery & operator=(const CasKillableQuery &) = delete;

    /// For a worker thread with its own `ThreadStatus`, as a query's thread pool attaches its workers.
    void attachCurrentThread();

    /// `KILL QUERY`; with `cause`, the query throws that exception instead of `QUERY_WAS_CANCELLED`.
    void kill(std::exception_ptr cause = nullptr);

    ContextMutablePtr context;
    ThreadGroupPtr group;
    ProcessList::EntryPtr entry;
    QueryStatusPtr status;
};

/// Runs `body` on a fresh thread, with a `ThreadStatus` when `with_thread_status`, joins it, and rethrows on the calling
/// thread an exception that left `body`, which would otherwise terminate the test process. Thread-local owner
/// state and the gtest main thread's missing `ThreadStatus` make a raw thread the only clean starting point.
template <typename Body>
void onThread(bool with_thread_status, Body && body)
{
    std::exception_ptr error;
    std::thread worker([&]
    {
        std::optional<ThreadStatus> thread_status;
        if (with_thread_status)
            thread_status.emplace();
        try
        {
            body();
        }
        catch (...)
        {
            error = std::current_exception();
        }
    });
    worker.join();
    if (error)
        std::rethrow_exception(error);
}

/// Runs `body(query)` on a fresh thread with a `ThreadStatus`, attached to a fresh query registered in the process
/// list as a SQL query's thread is, and joins it.
template <typename Body>
void onKillableQueryThread(Body && body)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        body(query);
    });
}

enum class StoppedBy : uint8_t { Background, Query };

inline const char * nameOf(StoppedBy owner)
{
    return owner == StoppedBy::Background ? "background owner" : "killed query";
}

/// A background owner on the calling thread whose check throws `ABORTED` once `*stopped` is set.
inline OwnerScope backgroundOwnerStoppedBy(const std::shared_ptr<std::atomic<bool>> & stopped)
{
    return OwnerScope::background([stopped]
    {
        if (stopped->load())
            throw Exception(ErrorCodes::ABORTED, "background owner stopped");
    });
}

/// The stop of `owner`: kills `query`, or sets `*background_stopped`.
inline std::function<void()> stopperOf(StoppedBy owner, CasKillableQuery & query,
                                       const std::shared_ptr<std::atomic<bool>> & background_stopped)
{
    if (owner == StoppedBy::Query)
        return [&query] { query.kill(); };
    return [background_stopped] { *background_stopped = true; };
}

namespace client_like_detail
{

template <typename Call>
auto checkedOnThrow(Call && call)
{
    try
    {
        return call();
    }
    catch (...)
    {
        CurrentThread::checkIfNotCancelled();
        throw;
    }
}

template <typename Call, typename Succeeded>
auto checkedUnless(Call && call, Succeeded && succeeded)
{
    auto answer = checkedOnThrow(std::forward<Call>(call));
    if (!succeeded(answer))
        CurrentThread::checkIfNotCancelled();
    return answer;
}

}

/// Forwards to `inner` and, as the S3 client does after every unsuccessful response, checks the current
/// thread's query on each miss, conflict or failure before answering. Success is never checked. The
/// sentinel probe stays the base default, which goes through the decorated `head` and `read`.
class CasClientLikeBackend final : public Backend
{
public:
    explicit CasClientLikeBackend(BackendPtr inner_) : inner(std::move(inner_)) {}

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        return client_like_detail::checkedUnless([&] { return inner->read(key, access); }, [](const auto & r) { return r.has_value(); });
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        return client_like_detail::checkedUnless([&] { return inner->head(key, access); }, [](const auto & r) { return r.has_value(); });
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        return client_like_detail::checkedOnThrow([&] { return inner->list(prefix, cursor, limit, access); });
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        return client_like_detail::checkedUnless(
            [&] { return inner->remove(key, expected_value, access); }, [](RawRemoval r) { return r == RawRemoval::Removed; });
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        return client_like_detail::checkedUnless(
            [&] { return inner->write(key, bytes, expected_value, access); }, [](const auto & r) { return r.has_value(); });
    }

    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override
    {
        return client_like_detail::checkedUnless([&] { return inner->stream(key, access); }, [](const auto & r) { return r != nullptr; });
    }

    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        client_like_detail::checkedOnThrow([&] { inner->publish(request, access); });
    }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        client_like_detail::checkedOnThrow([&] { inner->removeManyWriteOnce(keys, access); });
    }

    Dialect dialect() const override { return inner->dialect(); }
    bool supportsListTokens() const override { return inner->supportsListTokens(); }
    uint64_t attemptTimeoutMs() const override { return inner->attemptTimeoutMs(); }
    uint64_t attemptEnvelopeMs() const override { return inner->attemptEnvelopeMs(); }
    size_t bulkDeleteKeyLimit() const override { return inner->bulkDeleteKeyLimit(); }
    bool refreshCredentials() override { return inner->refreshCredentials(); }
    void checkPoolPreconditions() override { inner->checkPoolPreconditions(); }
    void checkSkipAccessCheckSupport() override { inner->checkSkipAccessCheckSupport(); }
    void checkConditionalWriteSingleAttemptSupport() override { inner->checkConditionalWriteSingleAttemptSupport(); }

private:
    BackendPtr inner;
};

}

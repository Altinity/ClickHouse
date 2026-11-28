#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasHotKeys.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefCatalogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/WriteMode.h>
#include <Disks/tests/cas_killable_query.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Disks/tests/cas_time_taking_backend.h>

#include <Common/CurrentMemoryTracker.h>
#include <Common/CurrentThread.h>
#include <Common/MemoryTracker.h>
#include <Common/Exception.h>
#include <Common/ThreadStatus.h>
#include <Common/tests/gtest_global_context.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromFileBase.h>
#include <IO/WriteHelpers.h>
#include <base/scope_guard.h>

#include "config.h"

#include <Poco/Exception.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unistd.h>

#include <fmt/format.h>

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

namespace DB::ErrorCodes
{
    extern const int ABORTED;
    extern const int BAD_ARGUMENTS;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int MEMORY_LIMIT_EXCEEDED;
    extern const int NETWORK_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int QUERY_WAS_CANCELLED;
    extern const int READONLY;
    extern const int S3_ERROR;
    extern const int TIMEOUT_EXCEEDED;
}

using namespace DB;
using namespace DB::Cas;
using DB::Cas::tests::CasClientLikeBackend;
using DB::Cas::tests::CasKillableQuery;
using DB::Cas::tests::StoppedBy;
using DB::Cas::tests::nameOf;
using DB::Cas::tests::onThread;
using DB::Cas::tests::CountingBackend;
using DB::Cas::tests::ManualClock;
using DB::Cas::tests::TimeTakingBackend;
using DB::Cas::tests::openRequestsForTest;

namespace
{

constexpr auto WATCHDOG = std::chrono::seconds(30);

/// 0 for no exception.
int codeOf(const std::exception_ptr & e)
{
    if (!e)
        return 0;
    try
    {
        std::rethrow_exception(e);
    }
    catch (const Exception & ex)
    {
        return ex.code();
    }
    catch (...)
    {
        return -1;
    }
}

/// Empty for no exception.
String messageOf(const std::exception_ptr & e)
{
    if (!e)
        return {};
    try
    {
        std::rethrow_exception(e);
    }
    catch (const std::exception & ex)
    {
        return ex.what();
    }
    catch (...)
    {
        return {};
    }
}

/// The query's verdict, then the background owners': both checks, in the order a scan head runs them.
std::exception_ptr checkOnce()
{
    try
    {
        throwIfQueryStopped();
        throwIfBackgroundStopped();
    }
    catch (...)
    {
        return std::current_exception();
    }
    return nullptr;
}

/// A background owner that throws a fresh `ABORTED` once `stop` is set.
OwnerScope backgroundStoppedBy(const std::atomic<bool> & stop)
{
    return OwnerScope::background([&stop]
    {
        if (stop.load())
            throw Exception(ErrorCodes::ABORTED, "background owner stopped");
    });
}

/// Wakes `cv` once, after its waiter released `mutex` to wait. Start it while the waiter holds the lock.
std::thread wakeOnceWaiting(std::mutex & mutex, std::condition_variable & cv)
{
    return std::thread([&mutex, &cv]
    {
        std::lock_guard lock(mutex);
        cv.notify_all();
    });
}

}

static_assert(!std::is_copy_constructible_v<OwnerScope>);
static_assert(!std::is_copy_assignable_v<OwnerScope>);
static_assert(std::is_move_constructible_v<OwnerScope>);

/// An exception that leaves the body of `onThread` fails the calling test instead of terminating the test process.
TEST(CASOwnerScope, OnThreadRethrowsTheBodysExceptionOnTheCallingThread)
{
    EXPECT_THROW(onThread(false, [] { throw Exception(ErrorCodes::ABORTED, "thrown on the raw thread"); }), Exception);
}

TEST(CASOwnerScope, NoScopeNoQueryIsANoOp)
{
    for (bool with_thread_status : {false, true})
    {
        onThread(with_thread_status, [&]
        {
            EXPECT_EQ(ownerStopRequested(), nullptr);
            EXPECT_NO_THROW(throwIfQueryStopped());
            EXPECT_NO_THROW(throwIfBackgroundStopped());
            EXPECT_FALSE(insideOwed());
            EXPECT_FALSE(OwnerScope::effectiveCheck());
        });
    }
}

TEST(CASOwnerScope, BackgroundStopIsThrown)
{
    onThread(false, [&]
    {
        std::atomic<bool> stop{false};
        {
            auto scope = backgroundStoppedBy(stop);
            EXPECT_EQ(checkOnce(), nullptr);
            stop = true;
            auto verdict = checkOnce();
            ASSERT_NE(verdict, nullptr);
            EXPECT_EQ(codeOf(verdict), ErrorCodes::ABORTED);
            EXPECT_EQ(codeOf(ownerStopRequested()), ErrorCodes::ABORTED);
        }
        EXPECT_EQ(checkOnce(), nullptr);
        EXPECT_EQ(ownerStopRequested(), nullptr);
    });
}

TEST(CASOwnerScope, OwedSuppressesTheThrowButNotTheVerdict)
{
    onThread(false, [&]
    {
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        {
            auto owed = OwnerScope::owed();
            EXPECT_TRUE(insideOwed());
            EXPECT_EQ(checkOnce(), nullptr);
            EXPECT_EQ(codeOf(ownerStopRequested()), ErrorCodes::ABORTED);
        }
        EXPECT_FALSE(insideOwed());
        EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::ABORTED);
    });
}

TEST(CASOwnerScope, SharedHidesEveryOwner)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        query.kill();
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::QUERY_WAS_CANCELLED);
        {
            auto shared = OwnerScope::shared();
            EXPECT_EQ(ownerStopRequested(), nullptr);
            EXPECT_EQ(checkOnce(), nullptr);
            EXPECT_FALSE(insideOwed());
            auto inner_background = backgroundStoppedBy(stop);
            EXPECT_EQ(ownerStopRequested(), nullptr);
        }
        EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::QUERY_WAS_CANCELLED);
    });
}

TEST(CASOwnerScope, ScopesNestAndRestore)
{
    onThread(false, [&]
    {
        std::atomic<bool> outer_stop{false};
        std::atomic<bool> inner_stop{false};
        auto outer = OwnerScope::background([&]
        {
            if (outer_stop.load())
                throw Exception(ErrorCodes::ABORTED, "outer");
        });
        {
            auto inner = OwnerScope::background([&]
            {
                if (inner_stop.load())
                    throw Exception(ErrorCodes::BAD_ARGUMENTS, "inner");
            });
            inner_stop = true;
            EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::BAD_ARGUMENTS);
            outer_stop = true;
            EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::ABORTED) << "an enclosing owner is checked first";
            {
                auto owed = OwnerScope::owed();
                auto background_in_owed = backgroundStoppedBy(inner_stop);
                EXPECT_TRUE(insideOwed()) << "a nested scope never lifts an enclosing owed";
                EXPECT_EQ(checkOnce(), nullptr);
                {
                    auto shared = OwnerScope::shared();
                    EXPECT_EQ(ownerStopRequested(), nullptr);
                }
                EXPECT_TRUE(insideOwed());
                EXPECT_NE(ownerStopRequested(), nullptr);
            }
            EXPECT_FALSE(insideOwed());
            outer_stop = false;
            EXPECT_EQ(codeOf(checkOnce()), ErrorCodes::BAD_ARGUMENTS);
        }
        EXPECT_EQ(checkOnce(), nullptr) << "the inner owner left with its scope";

        /// A moved-from scope restores nothing; the moved-to one restores once.
        std::optional<OwnerScope> first;
        first.emplace(OwnerScope::owed());
        {
            OwnerScope second(std::move(*first));
            EXPECT_TRUE(insideOwed());
        }
        EXPECT_FALSE(insideOwed());
        {
            auto owed = OwnerScope::owed();
            first.reset();
            EXPECT_TRUE(insideOwed()) << "destroying a moved-from scope must not restore";
        }
    });
}

TEST(CASOwnerScope, EffectiveCheckIsEmptyInsideOwedAndShared)
{
    onThread(false, [&]
    {
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        {
            auto owed = OwnerScope::owed();
            EXPECT_FALSE(OwnerScope::effectiveCheck());
        }
        {
            auto shared = OwnerScope::shared();
            EXPECT_FALSE(OwnerScope::effectiveCheck());
        }
        auto check = OwnerScope::effectiveCheck();
        ASSERT_TRUE(check);
        EXPECT_THROW(check(), Exception);
    });
}

/// The captured check carries the background owner to another thread and leaves the query behind: a shared
/// flush run for a killed leader proceeds as the leader's own requests would on plain S3.
TEST(CASOwnerScope, EffectiveCheckCapturesOnlyTheBackgroundOwner)
{
    onThread(true, [&]
    {
        std::atomic<bool> stop{false};
        auto background = backgroundStoppedBy(stop);
        auto check = OwnerScope::effectiveCheck();
        ASSERT_TRUE(check);

        std::exception_ptr seen;
        std::thread other([&]
        {
            stop = true;
            try
            {
                check();
            }
            catch (...)
            {
                seen = std::current_exception();
            }
        });
        other.join();
        EXPECT_EQ(codeOf(seen), ErrorCodes::ABORTED);
    });

    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        query.kill();
        EXPECT_FALSE(OwnerScope::effectiveCheck()) << "a killed query is not a background owner";
    });
}

/// Each check consults its own owner only; `owed` and `shared` silence both.
TEST(CASOwnerScope, BackgroundAndQueryChecksAreSeparate)
{
    const auto thrownBy = [](void (*check)()) -> int
    {
        try
        {
            check();
        }
        catch (...)
        {
            return codeOf(std::current_exception());
        }
        return 0;
    };
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        std::atomic<bool> stop{true};
        {
            auto background = backgroundStoppedBy(stop);
            EXPECT_EQ(thrownBy(throwIfBackgroundStopped), ErrorCodes::ABORTED);
            EXPECT_EQ(thrownBy(throwIfQueryStopped), 0) << "a live query is not stopped by a background owner";
        }
        query.kill();
        EXPECT_EQ(thrownBy(throwIfQueryStopped), ErrorCodes::QUERY_WAS_CANCELLED);
        EXPECT_EQ(thrownBy(throwIfBackgroundStopped), 0) << "a killed query is not a background owner";
        auto background = backgroundStoppedBy(stop);
        {
            auto owed = OwnerScope::owed();
            EXPECT_EQ(thrownBy(throwIfBackgroundStopped), 0);
            EXPECT_EQ(thrownBy(throwIfQueryStopped), 0);
        }
        {
            auto shared = OwnerScope::shared();
            EXPECT_EQ(thrownBy(throwIfBackgroundStopped), 0);
            EXPECT_EQ(thrownBy(throwIfQueryStopped), 0);
        }
    });
}

/// A killed query's wait ends when the condition holds, as a wait on plain S3 would; only a background owner
/// ends it early.
TEST(CASOwnerScope, WaitStoppableIgnoresAKilledQuery)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        query.kill();
        std::mutex mutex;
        std::condition_variable cv;
        std::unique_lock lock(mutex);
        int evaluations = 0;
        std::thread waker = wakeOnceWaiting(mutex, cv);
        EXPECT_NO_THROW(waitStoppable(cv, lock, [&] { return ++evaluations >= 2; }));
        EXPECT_EQ(evaluations, 2);
        EXPECT_TRUE(lock.owns_lock());
        lock.unlock();
        waker.join();
    });
}

TEST(CASOwnerScope, RealQueryKillIsThrown)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        EXPECT_EQ(checkOnce(), nullptr);
        query.kill();
        auto verdict = checkOnce();
        EXPECT_EQ(codeOf(verdict), ErrorCodes::QUERY_WAS_CANCELLED);

        std::exception_ptr worker_verdict;
        std::thread worker([&]
        {
            ThreadStatus worker_status;
            query.attachCurrentThread();
            worker_verdict = checkOnce();
        });
        worker.join();
        EXPECT_EQ(codeOf(worker_verdict), ErrorCodes::QUERY_WAS_CANCELLED) << "an attached worker sees the kill";
    });
}

TEST(CASOwnerScope, RealQueryStoredCauseIsThrown)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto cause = std::make_exception_ptr(Exception(ErrorCodes::BAD_ARGUMENTS, "stored cause"));
        query.kill(cause);
        auto verdict = checkOnce();
        EXPECT_EQ(codeOf(verdict), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_EQ(verdict, cause) << "the query's own exception, not a copy";
    });
}

TEST(CASOwnerScope, WaitStoppableChecksBeforeReturningWhenAlreadyReady)
{
    onThread(false, [&]
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> stop{false};
        auto background = backgroundStoppedBy(stop);

        std::unique_lock lock(mutex);
        EXPECT_NO_THROW(waitStoppable(cv, lock, [] { return true; }));
        EXPECT_TRUE(lock.owns_lock());

        stop = true;
        try
        {
            waitStoppable(cv, lock, [] { return true; });
            ADD_FAILURE() << "a stopped owner must be reported even when the wait is already satisfied";
        }
        catch (...)
        {
            EXPECT_EQ(codeOf(std::current_exception()), ErrorCodes::ABORTED);
        }
        EXPECT_TRUE(lock.owns_lock());
    });
}

TEST(CASOwnerScope, WaitStoppableHoldsTheLockOnThrow)
{
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::promise<void> waiting;
    std::promise<void> done;
    std::atomic<bool> held_on_throw{false};
    std::atomic<bool> asked_owner_while_locked{false};
    std::atomic<bool> give_up{false};
    std::atomic<int> code{0};

    std::thread waiter([&]
    {
        std::unique_lock lock(mutex);
        auto background = OwnerScope::background([&]
        {
            if (lock.owns_lock())
                asked_owner_while_locked = true;
            if (stop.load())
                throw Exception(ErrorCodes::ABORTED, "background owner stopped");
        });
        bool announced = false;
        try
        {
            /// Nobody ever notifies: only the slice timeout can bring the stop to the waiter.
            waitStoppable(cv, lock, [&]
            {
                if (!announced)
                {
                    announced = true;
                    waiting.set_value();
                }
                return give_up.load();
            });
        }
        catch (...)
        {
            held_on_throw = lock.owns_lock();
            code = codeOf(std::current_exception());
        }
        done.set_value();
    });

    const bool started = waiting.get_future().wait_for(WATCHDOG) == std::future_status::ready;
    stop = true;
    const bool returned = done.get_future().wait_for(WATCHDOG) == std::future_status::ready;
    if (!returned)
    {
        /// Release a waiter that ignores the stop, so the test fails instead of hanging.
        {
            std::lock_guard lock(mutex);
            give_up = true;
        }
        cv.notify_all();
    }
    waiter.join();
    ASSERT_TRUE(started) << "the waiter never evaluated its condition";
    ASSERT_TRUE(returned) << "a stopped waiter did not leave within the watchdog";
    EXPECT_EQ(code.load(), ErrorCodes::ABORTED);
    EXPECT_TRUE(held_on_throw.load());
    EXPECT_FALSE(asked_owner_while_locked.load()) << "the owner is asked with the lock released";
}

TEST(CASOwnerScope, WaitStoppableSatisfyingWakeRaceStillChecks)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool ready = false;
    std::atomic<bool> stop{false};
    std::promise<void> waiting;
    std::promise<void> done;
    std::atomic<int> code{0};

    std::thread waiter([&]
    {
        auto background = backgroundStoppedBy(stop);
        std::unique_lock lock(mutex);
        bool announced = false;
        try
        {
            waitStoppable(cv, lock, [&]
            {
                if (!announced)
                {
                    announced = true;
                    waiting.set_value();
                }
                return ready;
            });
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        done.set_value();
    });

    const bool started = waiting.get_future().wait_for(WATCHDOG) == std::future_status::ready;
    {
        /// The waiter holds the mutex from its first `ready` until it waits, so this wake meets a waiter.
        std::lock_guard lock(mutex);
        ready = true;
        stop = true;
    }
    cv.notify_all();
    const bool returned = done.get_future().wait_for(WATCHDOG) == std::future_status::ready;
    waiter.join();
    ASSERT_TRUE(started) << "the waiter never evaluated its condition";
    ASSERT_TRUE(returned) << "a woken waiter did not leave within the watchdog";
    EXPECT_EQ(code.load(), ErrorCodes::ABORTED) << "the wake that satisfied the wait must not hide the stop";
}

TEST(CASOwnerScope, WaitStoppableInsideOwedDoesNotThrow)
{
    onThread(false, [&]
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        auto owed = OwnerScope::owed();
        std::unique_lock lock(mutex);
        int evaluations = 0;
        std::thread waker = wakeOnceWaiting(mutex, cv);
        EXPECT_NO_THROW(waitStoppable(cv, lock, [&] { return ++evaluations >= 2; })) << "owed work finishes its wait";
        EXPECT_EQ(evaluations, 2);
        EXPECT_TRUE(lock.owns_lock());
        lock.unlock();
        waker.join();
    });
}

/// With no background owner to ask, nothing but a notification can end the wait, so it does not wake on a timer.
TEST(CASOwnerScope, WaitStoppableWithNoOwnerToAskWaitsForANotification)
{
    enum class Scope : uint8_t { None, OwedOverBackground, SharedOverBackground };
    for (const Scope scope : {Scope::None, Scope::OwedOverBackground, Scope::SharedOverBackground})
    {
        SCOPED_TRACE(static_cast<int>(scope));
        onThread(false, [&]
        {
            std::atomic<bool> stop{false};
            std::optional<OwnerScope> background;
            std::optional<OwnerScope> inner;
            if (scope != Scope::None)
            {
                background.emplace(backgroundStoppedBy(stop));
                inner.emplace(scope == Scope::OwedOverBackground ? OwnerScope::owed() : OwnerScope::shared());
            }
            std::mutex mutex;
            std::condition_variable cv;
            bool ready = false;
            int evaluations = 0;
            std::unique_lock lock(mutex);
            /// Three owner-stop slices: a waiter that polls evaluates `ready` at least once per slice.
            std::thread notifier([&]
            {
                std::this_thread::sleep_for(3 * kOwnerStopSlice);
                {
                    std::lock_guard notifier_lock(mutex);
                    ready = true;
                }
                cv.notify_all();
            });
            waitStoppable(cv, lock, [&]
            {
                ++evaluations;
                return ready;
            });
            EXPECT_TRUE(ready) << "the wait returned before the notification";
            lock.unlock();
            notifier.join();
            EXPECT_LE(evaluations, 2) << "the wait woke on a timer";
            inner.reset();
        });
    }
}

TEST(CASOwnerScope, ClientLikeBackendThrowsTheKillOnlyOnAnUnsuccessfulAnswer)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto backend = std::make_shared<CasClientLikeBackend>(std::make_shared<InMemoryBackend>());
        auto requests = openRequestsForTest(backend);
        auto op = requests.admit();
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create("present", "v", Retry::once())));
        EXPECT_FALSE(op.read("absent", Retry::once()).has_value());

        query.kill();
        EXPECT_TRUE(op.read("present", Retry::once()).has_value()) << "a successful answer is not checked";
        try
        {
            op.read("absent", Retry::once());
            ADD_FAILURE() << "a miss of a killed query must surface the kill";
        }
        catch (...)
        {
            EXPECT_EQ(codeOf(std::current_exception()), ErrorCodes::QUERY_WAS_CANCELLED);
        }
    });
}

TEST(CASOwnerScope, OwedInsideSharedStaysShared)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        query.kill();
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        auto shared = OwnerScope::shared();
        auto owed = OwnerScope::owed();
        EXPECT_TRUE(insideOwed());
        EXPECT_EQ(ownerStopRequested(), nullptr) << "owed work inside shared work still has no owner";
        EXPECT_FALSE(OwnerScope::effectiveCheck());
    });
}

TEST(CASOwnerScope, ScopesStayOnTheirThread)
{
    std::promise<void> scope_open;
    std::promise<void> peer_checked;
    std::thread holder([&]
    {
        std::atomic<bool> stop{true};
        auto background = backgroundStoppedBy(stop);
        auto owed = OwnerScope::owed();
        scope_open.set_value();
        peer_checked.get_future().wait_for(WATCHDOG);
    });

    const bool opened = scope_open.get_future().wait_for(WATCHDOG) == std::future_status::ready;
    onThread(false, [&]
    {
        EXPECT_EQ(ownerStopRequested(), nullptr) << "another thread's background owner is not this thread's";
        EXPECT_FALSE(insideOwed()) << "another thread's owed scope is not this thread's";
        EXPECT_EQ(checkOnce(), nullptr);
    });
    peer_checked.set_value();
    holder.join();
    ASSERT_TRUE(opened) << "the holder never opened its scope";
}

namespace
{

/// The first write of `key` stores its bytes and loses the response; the first read of `key` fails.
class LostWriteThenLostReadBackend final : public InMemoryBackend
{
public:
    explicit LostWriteThenLostReadBackend(String key_) : key(std::move(key_)) {}

    std::atomic<int> writes{0};
    std::atomic<int> reads{0};

    std::expected<String, RawConflict> write(const String & k, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (k == key && writes.fetch_add(1) == 0)
        {
            (void)InMemoryBackend::write(k, bytes, expected_value, access);
            throw Poco::TimeoutException("LostWriteThenLostReadBackend: stored, response lost");
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        if (k == key && reads.fetch_add(1) == 0)
            throw Poco::TimeoutException("LostWriteThenLostReadBackend: read response lost");
        return InMemoryBackend::read(k, access);
    }

private:
    const String key;
};

}

/// The settling read of a possibly-sent write is owed to that write: when its first attempt fails while
/// the owner is live and the owner stops during the backoff, the read is still reissued and proves the
/// write landed.
TEST(CASOwnerStop, OwedSettlingReadReissuesAfterAFailure)
{
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto backend = std::make_shared<LostWriteThenLostReadBackend>("settled");
        std::atomic<int> pauses{0};
        CasRequests requests(backend, Fence::open(), {}, [&](uint64_t)
        {
            if (backend->reads.load() > 0 && pauses.fetch_add(1) == 0)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();

        std::optional<WriteResult> written;
        EXPECT_NO_THROW(written = op.create("settled", "bytes", Retry::standard()));

        ASSERT_TRUE(written.has_value());
        EXPECT_TRUE(std::holds_alternative<Committed>(*written)) << "the reissued settling read proves the write landed";
        EXPECT_EQ(backend->writes.load(), 1) << "a landed write is not resent";
        EXPECT_EQ(backend->reads.load(), 2) << "the failed settling read was reissued";
        EXPECT_EQ(pauses.load(), 1);
        EXPECT_NE(codeOf(ownerStopRequested()), 0) << "the owner stopped before the reissue";
    });
}

namespace
{

/// Fails the first read or head of `key` with `failure`, and counts the requests to `key` and the
/// credential refreshes. Wrapped in `CasClientLikeBackend`, a stopped query's verdict replaces the
/// failure, as the S3 client does.
class FailFirstRequestBackend final : public InMemoryBackend
{
public:
    explicit FailFirstRequestBackend(String key_) : key(std::move(key_)) {}

    std::exception_ptr failure = std::make_exception_ptr(Exception(ErrorCodes::TIMEOUT_EXCEEDED, "the read timed out"));
    std::atomic<bool> fail_next{true};
    std::atomic<int> requests{0};
    std::atomic<int> refreshes{0};

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        countAndMaybeFail(k);
        return InMemoryBackend::read(k, access);
    }

    std::optional<RawMeta> head(const String & k, TransportAccess & access) override
    {
        countAndMaybeFail(k);
        return InMemoryBackend::head(k, access);
    }

    bool refreshCredentials() override
    {
        ++refreshes;
        return true;
    }

private:
    void countAndMaybeFail(const String & k)
    {
        if (k != key)
            return;
        ++requests;
        if (fail_next.exchange(false))
            std::rethrow_exception(failure);
    }

    const String key;
};

enum class ReadSurface { Read, ReadModifyWrite, ReadModifyWriteOnPresence };

struct FailedFirstRequest
{
    ReadSurface surface = ReadSurface::Read;
    /// What the first request to the key throws when no verdict replaces it.
    std::exception_ptr failure;
    /// Stops the query before the call; empty leaves the owner live.
    std::function<void(CasKillableQuery &)> stop;
    /// Runs the call under a stopped background owner instead.
    bool stopped_background = false;
    /// Runs during the pause before a reissue.
    std::function<void(CasKillableQuery &)> during_pause;
};

struct FailedFirstRequestOutcome
{
    int code = 0;
    String message;
    bool has_value = false;
    bool gave_up = false;
    int requests = 0;
    int refreshes = 0;
    int pauses = 0;
};

/// One call whose first request to an existing key fails. The single-attempt surfaces would turn a
/// failed observation into `GaveUp` with `NotObserved`.
FailedFirstRequestOutcome runFailedFirstRequest(const FailedFirstRequest & arm)
{
    FailedFirstRequestOutcome out;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<FailFirstRequestBackend>("k");
        if (arm.failure)
            inner->failure = arm.failure;
        auto backend = std::make_shared<CasClientLikeBackend>(inner);
        std::atomic<int> pauses{0};
        CasRequests requests(backend, Fence::open(), {}, [&](uint64_t)
        {
            ++pauses;
            if (arm.during_pause)
                arm.during_pause(query);
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create("k", "v", Retry::once())));
        inner->requests = 0;

        std::optional<OwnerScope> background;
        if (arm.stopped_background)
            background.emplace(OwnerScope::background([] { throw Exception(ErrorCodes::ABORTED, "background owner stopped"); }));
        if (arm.stop)
            arm.stop(query);
        try
        {
            switch (arm.surface)
            {
                case ReadSurface::Read:
                    out.has_value = op.read("k", Retry::standard()).has_value();
                    break;
                case ReadSurface::ReadModifyWrite:
                    out.gave_up = std::holds_alternative<GaveUp>(
                        op.readModifyWrite("k", [](const std::optional<DB::Cas::Object> &) { return std::optional<String>{}; }, Retry::once()));
                    break;
                case ReadSurface::ReadModifyWriteOnPresence:
                    out.gave_up = std::holds_alternative<GaveUp>(
                        op.readModifyWriteOnPresence("k", [](const std::optional<Meta> &) { return std::optional<String>{}; }, Retry::once()));
                    break;
            }
        }
        catch (...)
        {
            out.code = codeOf(std::current_exception());
            out.message = getCurrentExceptionMessage(/*with_stacktrace=*/false);
        }
        out.requests = inner->requests.load();
        out.refreshes = inner->refreshes.load();
        out.pauses = pauses.load();
    });
    return out;
}

/// `LocalObjectStorage` whose next small-object read fails the way the S3 client fails a request of a
/// stopped query: it asks the query first, so the query's verdict replaces the failure.
class ClientLikeLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    mutable std::atomic<bool> fail_next{false};
    mutable std::atomic<int> reads{0};

    DB::SmallObjectDataWithMetadata readSmallObjectAndGetObjectMetadata( /// NOLINT
        const DB::StoredObject & object,
        const DB::ReadSettings & read_settings,
        size_t max_size_bytes,
        std::optional<size_t> read_hint) const override
    {
        ++reads;
        if (fail_next.exchange(false))
        {
            CurrentThread::checkIfNotCancelled();
            throw Poco::TimeoutException("ClientLikeLocalObjectStorage: the read timed out");
        }
        return DB::LocalObjectStorage::readSmallObjectAndGetObjectMetadata(object, read_settings, max_size_bytes, read_hint);
    }
};

template <typename Storage>
std::shared_ptr<Storage> makeLocalObjectStorageOf()
{
    static std::atomic<uint64_t> counter{0};
    const auto root = (std::filesystem::temp_directory_path()
        / ("cas_owner_stop_unit_" + std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1)))).string();
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    DB::LocalObjectStorageSettings settings("test", root, /*read_only_=*/false);
    return DB::Cas::tests::ownedByDirectory(std::make_shared<Storage>(std::move(settings)), root);
}

std::shared_ptr<ClientLikeLocalObjectStorage> makeClientLikeLocalObjectStorage()
{
    return makeLocalObjectStorageOf<ClientLikeLocalObjectStorage>();
}

/// `LocalObjectStorage` whose small-object reads and container checks first run a hook, which may throw.
class HookedReadLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    std::function<void()> on_read;
    std::function<void()> on_exists;
    mutable std::atomic<int> reads{0};

    DB::SmallObjectDataWithMetadata readSmallObjectAndGetObjectMetadata( /// NOLINT
        const DB::StoredObject & object,
        const DB::ReadSettings & read_settings,
        size_t max_size_bytes,
        std::optional<size_t> read_hint) const override
    {
        ++reads;
        if (on_read)
            on_read();
        return DB::LocalObjectStorage::readSmallObjectAndGetObjectMetadata(object, read_settings, max_size_bytes, read_hint);
    }

    bool existsOrHasAnyChild(const std::string & path) const override
    {
        if (on_exists)
            on_exists();
        return DB::LocalObjectStorage::existsOrHasAnyChild(path);
    }
};

#if USE_AWS_S3
/// `LocalObjectStorage` that answers every small-object read with the store's own `NoSuchKey`.
class NoSuchKeyLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    mutable std::atomic<int> reads{0};

    DB::SmallObjectDataWithMetadata readSmallObjectAndGetObjectMetadata( /// NOLINT
        const DB::StoredObject & object, const DB::ReadSettings &, size_t, std::optional<size_t>) const override
    {
        ++reads;
        throw DB::S3Exception("the store answered NoSuchKey for " + object.remote_path, Aws::S3::S3Errors::NO_SUCH_KEY, "NoSuchKey");
    }
};
#endif

std::exception_ptr storedCause(int code)
{
    return std::make_exception_ptr(Exception(code, "the query's stored cause"));
}

}

/// A stopped owner's exception after a failed first attempt is not a store answer: it is not reissued,
/// refreshed or paced, is never read as absence, and a single-attempt observation surfaces it rather
/// than reporting `NotObserved`. A live owner's same failure is reissued.
TEST(CASOwnerStop, StoppedOwnersFailedReadIsNotReissued)
{
    struct Stop
    {
        const char * name;
        std::function<void(CasKillableQuery &)> stop;
        int code;
    };
    const std::vector<Stop> stops = {
        {"timeout", [](CasKillableQuery & q) { q.kill(storedCause(ErrorCodes::TIMEOUT_EXCEEDED)); }, ErrorCodes::TIMEOUT_EXCEEDED},
        {"kill", [](CasKillableQuery & q) { q.kill(); }, ErrorCodes::QUERY_WAS_CANCELLED},
        {"stored BAD_ARGUMENTS", [](CasKillableQuery & q) { q.kill(storedCause(ErrorCodes::BAD_ARGUMENTS)); }, ErrorCodes::BAD_ARGUMENTS},
        {"stored FILE_DOESNT_EXIST", [](CasKillableQuery & q) { q.kill(storedCause(ErrorCodes::FILE_DOESNT_EXIST)); },
         ErrorCodes::FILE_DOESNT_EXIST},
    };
    for (const auto & stop : stops)
    {
        for (const auto surface : {ReadSurface::Read, ReadSurface::ReadModifyWrite, ReadSurface::ReadModifyWriteOnPresence})
        {
            SCOPED_TRACE(fmt::format("{}, surface {}", stop.name, static_cast<int>(surface)));
            FailedFirstRequest arm;
            arm.surface = surface;
            arm.stop = stop.stop;
            const auto out = runFailedFirstRequest(arm);
            EXPECT_EQ(out.code, stop.code) << "the owner's verdict surfaces";
            EXPECT_FALSE(out.has_value);
            EXPECT_FALSE(out.gave_up) << "a stopped owner's failed observation is not NotObserved";
            EXPECT_EQ(out.requests, 1) << "no reissue";
            EXPECT_EQ(out.refreshes, 0);
            EXPECT_EQ(out.pauses, 0);
        }
    }

#if USE_AWS_S3
    {
        SCOPED_TRACE("background owner, credential failure");
        FailedFirstRequest arm;
        arm.failure = std::make_exception_ptr(
            DB::S3Exception("the store answered AccessDenied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
        arm.stopped_background = true;
        const auto out = runFailedFirstRequest(arm);
        EXPECT_EQ(out.code, ErrorCodes::ABORTED);
        EXPECT_EQ(out.refreshes, 0) << "a stopped owner's failure is not a credential answer";
        EXPECT_EQ(out.requests, 1);
    }
#endif

    {
        SCOPED_TRACE("live owner");
        const auto out = runFailedFirstRequest({});
        EXPECT_EQ(out.code, 0);
        EXPECT_TRUE(out.has_value) << "a live owner's timeout is reissued";
        EXPECT_EQ(out.requests, 2);
        EXPECT_EQ(out.pauses, 1);
    }

    {
        SCOPED_TRACE("stored FILE_DOESNT_EXIST through the native backend");
        auto storage = makeClientLikeLocalObjectStorage();
        const String key = DB::Cas::tests::nativeKeyUnder(storage, "present");
        {
            auto out = storage->writeObject(DB::StoredObject(key), DB::WriteMode::Rewrite);
            DB::writeString(String("body"), *out);
            out->finalize();
        }
        auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
        int code = 0;
        bool read_absent = false;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            query.kill(storedCause(ErrorCodes::FILE_DOESNT_EXIST));
            storage->reads = 0;
            storage->fail_next = true;
            try
            {
                read_absent = !op.read(key, Retry::standard()).has_value();
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
        });
        EXPECT_FALSE(read_absent) << "a stopped owner's stored FILE_DOESNT_EXIST is never absence";
        EXPECT_EQ(code, ErrorCodes::FILE_DOESNT_EXIST);
        EXPECT_EQ(storage->reads.load(), 1);
    }
}

/// A stop during the pause before a reissue starts no further request, even one that would succeed. The read ends
/// with its own failure, which came before the kill, so the client-like backend did not replace it.
TEST(CASOwnerStop, StopDuringBackoffStartsNoReissue)
{
    FailedFirstRequest arm;
    arm.during_pause = [](CasKillableQuery & q) { q.kill(); };
    const auto out = runFailedFirstRequest(arm);
    EXPECT_EQ(out.code, ErrorCodes::TIMEOUT_EXCEEDED);
    EXPECT_FALSE(out.has_value);
    EXPECT_EQ(out.pauses, 1);
    EXPECT_EQ(out.requests, 1) << "the reissue, configured to succeed, never starts";
}

namespace
{

/// The first write of `key` stores its bytes, runs `on_lost_response` and loses the response; the first
/// read of `key` runs `on_failed_read` and fails.
class LostResponseBackend final : public InMemoryBackend
{
public:
    explicit LostResponseBackend(String key_) : key(std::move(key_)) {}

    std::function<void()> on_lost_response;
    std::function<void()> on_failed_read;
    std::atomic<int> writes{0};
    std::atomic<int> reads{0};
    std::atomic<int> refreshes{0};

    std::expected<String, RawConflict> write(const String & k, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (k == key && writes.fetch_add(1) == 0)
        {
            (void)InMemoryBackend::write(k, bytes, expected_value, access);
            if (on_lost_response)
                on_lost_response();
            throw Poco::TimeoutException("LostResponseBackend: stored, response lost");
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        if (k == key && reads.fetch_add(1) == 0)
        {
            if (on_failed_read)
                on_failed_read();
            throw Poco::TimeoutException("LostResponseBackend: read response lost");
        }
        return InMemoryBackend::read(k, access);
    }

    bool refreshCredentials() override
    {
        ++refreshes;
        return true;
    }

private:
    const String key;
};

Retry withoutResend()
{
    Retry policy = Retry::standard();
    policy.resend_unresolved = false;
    return policy;
}

}

/// Inside `owed`, a stopped owner's failed settling read is neither refreshed nor reissued, and the
/// observation reports `NotObserved`: no local-bug rethrow for a stored `BAD_ARGUMENTS` either.
TEST(CASOwnerStop, OwedFailedReadBecomesNotObserved)
{
    const auto run_arm = [](int stored)
    {
        std::optional<WriteResult> written;
        int code = 0;
        int reads = 0;
        int refreshes = 0;
        int pauses = 0;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            auto inner = std::make_shared<LostResponseBackend>("settled");
            /// After the write's failure, so only the settling read sees the stop.
            inner->on_failed_read = [&] { query.kill(stored ? storedCause(stored) : nullptr); };
            auto backend = std::make_shared<CasClientLikeBackend>(inner);
            std::atomic<int> pause_count{0};
            CasRequests requests(backend, Fence::open(), {}, [&](uint64_t) { ++pause_count; }, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            try
            {
                written = op.create("settled", "bytes", withoutResend());
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
            reads = inner->reads.load();
            refreshes = inner->refreshes.load();
            pauses = pause_count.load();
        });
        EXPECT_EQ(code, 0);
        ASSERT_TRUE(written.has_value());
        const auto * gave_up = std::get_if<GaveUp>(&*written);
        ASSERT_NE(gave_up, nullptr) << "the settling read proved nothing";
        EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved) << "no bound refused the read";
        EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
        EXPECT_TRUE(gave_up->sent_any);
        EXPECT_TRUE(std::holds_alternative<NotObserved>(gave_up->last_seen));
        EXPECT_EQ(reads, 1) << "the failed settling read was not reissued";
        EXPECT_EQ(refreshes, 0);
        EXPECT_EQ(pauses, 0);
    };
    for (const int stored : {0, ErrorCodes::BAD_ARGUMENTS})
    {
        SCOPED_TRACE(stored ? "stored BAD_ARGUMENTS" : "kill");
        run_arm(stored);
    }
}

/// A fence refusal recorded by the settling read survives the owed observation of a stopped owner:
/// the write reports the lost fence, not an unresolved outcome.
TEST(CASOwnerStop, FenceRefusalSurvivesOwedObservation)
{
    std::optional<WriteResult> written;
    int reads = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto backend = std::make_shared<LostResponseBackend>("fenced");
        std::atomic<bool> fence_lost{false};
        backend->on_lost_response = [&]
        {
            fence_lost = true;
            query.kill();
        };
        Fence fence = Fence::open();
        fence.admit = [&fence_lost](uint64_t, uint64_t) { return fence_lost ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; };
        CasRequests requests(backend, fence, {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        written = op.create("fenced", "bytes", withoutResend());
        reads = backend->reads.load();
    });
    ASSERT_TRUE(written.has_value());
    const auto * gave_up = std::get_if<GaveUp>(&*written);
    ASSERT_NE(gave_up, nullptr);
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_EQ(reads, 0) << "the fence refused the settling read before it was sent";
}

/// The first attempt of a read is sent for an owner that already stopped, and a healthy answer is returned.
TEST(CASOwnerStop, HealthyFirstReadIsNotRefused)
{
    for (const bool background : {false, true})
    {
        SCOPED_TRACE(background ? "background owner" : "killed query");
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            auto backend = std::make_shared<InMemoryBackend>();
            auto requests = openRequestsForTest(backend);
            auto op = requests.admit();
            ASSERT_TRUE(std::holds_alternative<Committed>(op.create("present", "v", Retry::once())));

            std::optional<OwnerScope> scope;
            if (background)
                scope.emplace(OwnerScope::background([] { throw Exception(ErrorCodes::ABORTED, "background owner stopped"); }));
            else
                query.kill();
            ASSERT_NE(ownerStopRequested(), nullptr);

            const auto present = op.read("present", Retry::standard());
            ASSERT_TRUE(present.has_value());
            EXPECT_EQ(present->bytes, "v");
            EXPECT_FALSE(op.read("absent", Retry::standard()).has_value());
            EXPECT_TRUE(op.head("present", Retry::standard()).has_value());
            EXPECT_FALSE(op.head("absent", Retry::standard()).has_value());
        });
    }

#if USE_AWS_S3
    {
        /// The S3 client never sees a background owner, so the store's own 404 reaches the backend.
        SCOPED_TRACE("background owner, the store's NoSuchKey through the native backend");
        auto storage = makeLocalObjectStorageOf<NoSuchKeyLocalObjectStorage>();
        auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
        onThread(false, [&]
        {
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            auto scope = OwnerScope::background([] { throw Exception(ErrorCodes::ABORTED, "background owner stopped"); });
            ASSERT_NE(ownerStopRequested(), nullptr);
            std::optional<DB::Cas::Object> got;
            EXPECT_NO_THROW(got = op.read(DB::Cas::tests::nativeKeyUnder(storage, "absent"), Retry::standard()));
            EXPECT_FALSE(got.has_value()) << "a healthy 404 is absence";
        });
        EXPECT_EQ(storage->reads.load(), 1);
    }
#endif
}

/// A stopped owner's catalog update whose base read the fence refuses keeps the typed fence outcome.
TEST(CASOwnerStop, StoppedOwnersFencedCatalogReadIsFencedOut)
{
    onThread(false, [&]
    {
        auto backend = std::make_shared<InMemoryBackend>();
        const Layout layout("p");
        const CatalogEntry observed{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
        {
            /// A plane of its own, so the update below reads the catalog rather than starting from a cached hint.
            auto setup_requests = openRequestsForTest(backend);
            auto setup = setup_requests.admit();
            CasRefCatalog::initializeEmptyForNewPool(setup, layout);
            CasRefCatalog::casAdmitEntry(setup, layout, 1, observed);
        }
        auto requests = openRequestsForTest(backend);
        /// Admitted at the hot-key lane's own check, refused from the base read on.
        int admissions = 0;
        auto op = requests.admit([&admissions] { return admissions++ == 0; });
        auto scope = OwnerScope::background([] { throw Exception(ErrorCodes::ABORTED, "background owner stopped"); });

        std::optional<CasRefCatalog::BeginRemovingOutcome> outcome;
        EXPECT_NO_THROW(outcome = CasRefCatalog::beginRemoving(op, layout, observed, /*removal_started_round*/ 13));
        ASSERT_TRUE(outcome.has_value());
        EXPECT_EQ(*outcome, CasRefCatalog::BeginRemovingOutcome::FencedOut);
        EXPECT_GE(admissions, 2) << "the base read reached its own admission";
    });
}

namespace
{

enum class DecisionLoop : uint8_t { BodyReadModifyWrite, PresenceReadModifyWrite, Catalog };

struct ConflictPauseArm
{
    DecisionLoop loop;
    /// 50 writes that each take 500 ms on the manual clock and lose their precondition, the stop in the
    /// third pause; otherwise every write loses and the stop comes in the first pause.
    bool slow;
};

class CASOwnerStopLoop : public ::testing::TestWithParam<ConflictPauseArm>
{
};

enum class PauseOwner : uint8_t { Background, Query };

struct ConflictPauseRun
{
    std::exception_ptr error;
    /// The read-modify-write loops' answer; the catalog loop returns none.
    std::optional<WriteResult> result;
    int decisions = 0;
    int decisions_at_stop = -1;
    uint64_t writes = 0;
    uint64_t writes_at_stop = 0;
};

/// Runs `arm`'s loop against lost races and stops `who` in the first pause, or in the third on the slow schedule. A
/// background owner sees every write lose; a query sees only the writes before its kill lose, since an unsuccessful
/// answer after it would carry the kill.
ConflictPauseRun runConflictPause(const ConflictPauseArm & arm, PauseOwner who)
{
    const int stop_at_pause = arm.slow ? 3 : 1;
    ConflictPauseRun run;
    onThread(who == PauseOwner::Query, [&]
    {
        std::optional<CasKillableQuery> query;
        if (who == PauseOwner::Query)
            query.emplace(getContext().context);
        auto backend = std::make_shared<TimeTakingBackend>();
        auto clock = std::make_shared<ManualClock>();
        CasHotKeys hot_keys(16ULL << 20);
        const Layout layout("p");
        const String key = arm.loop == DecisionLoop::Catalog ? layout.refCatalogKey() : String("k");
        std::atomic<bool> stop{false};
        int pauses = 0;
        const auto sleep_on_clock = ManualClock::sleepFnOf(clock);
        const BackendPtr engine_backend = who == PauseOwner::Query
            ? BackendPtr(std::make_shared<CasClientLikeBackend>(backend)) : BackendPtr(backend);
        CasRequests requests(engine_backend, Fence::open(), ManualClock::nowFnOf(clock), [&](uint64_t ms)
        {
            sleep_on_clock(ms);
            if (++pauses == stop_at_pause)
            {
                stop = true;
                if (query)
                    query->kill();
                run.decisions_at_stop = run.decisions;
                run.writes_at_stop = backend->writeCount(key);
            }
        }, &hot_keys, kDefaultRequestWindowMs);
        auto op = requests.admit();
        if (arm.loop == DecisionLoop::Catalog)
        {
            CasRefCatalog::initializeEmptyForNewPool(op, layout);
            CasRefCatalog::casAdmitEntry(op, layout, 1,
                CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)});
        }
        else
        {
            ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, "v0", Retry::once())));
        }
        ASSERT_EQ(pauses, 0);

        int conflicts_left = arm.slow ? 50 : 1000;
        backend->onBeforeWrite(key, [&]
        {
            const bool lose = who == PauseOwner::Query ? !stop.load() : conflicts_left-- > 0;
            if (lose)
                backend->refuseNextWrite(key);
        });
        if (arm.slow)
            backend->armWith(TimeTakingBackend::Verb::Write, [key](const String & k) { return k == key; },
                             [clock] { clock->advance(500); }, 50);

        std::optional<OwnerScope> owner;
        if (who == PauseOwner::Background)
            owner.emplace(backgroundStoppedBy(stop));
        try
        {
            switch (arm.loop)
            {
                case DecisionLoop::BodyReadModifyWrite:
                    run.result = op.readModifyWrite(key, [&](const std::optional<DB::Cas::Object> & current)
                    {
                        ++run.decisions;
                        return std::optional<String>(current ? current->bytes + "+" : String("v"));
                    }, Retry::standard());
                    break;
                case DecisionLoop::PresenceReadModifyWrite:
                    run.result = op.readModifyWriteOnPresence(key, [&](const std::optional<Meta> &)
                    {
                        ++run.decisions;
                        return std::optional<String>("v" + std::to_string(run.decisions));
                    }, Retry::standard());
                    break;
                case DecisionLoop::Catalog:
                    (void)CasRefCatalog::casUpdate(op, layout, [&](const RefCatalog & current)
                    {
                        ++run.decisions;
                        return current;
                    });
                    break;
            }
        }
        catch (...)
        {
            run.error = std::current_exception();
        }
        run.writes = backend->writeCount(key);
    });
    return run;
}

/// Decisions before the stopping pause: one per lost race, plus the first lost race, which is retried at once
/// without a pause, plus, for the catalog, the first decision on the lane's cached catalog, whose loss is retried at
/// once on its resolve read.
int decisionsBeforeStop(const ConflictPauseArm & arm)
{
    return (arm.slow ? 3 : 1) + 1 + (arm.loop == DecisionLoop::Catalog ? 1 : 0);
}

}

/// A stop during the pause after a lost race ends the loop: no decision and no write after it.
TEST_P(CASOwnerStopLoop, StopDuringConflictPauseSkipsNextDecision)
{
    const ConflictPauseArm arm = GetParam();
    const ConflictPauseRun run = runConflictPause(arm, PauseOwner::Background);
    EXPECT_EQ(codeOf(run.error), ErrorCodes::ABORTED);
    EXPECT_EQ(run.decisions_at_stop, decisionsBeforeStop(arm)) << "one pause after each lost race";
    EXPECT_EQ(run.decisions, run.decisions_at_stop) << "no decision after the stop";
    EXPECT_EQ(run.writes, run.writes_at_stop) << "no write after the stop";
}

/// A killed query is not stopped by an accepted conflict: the pause after a lost race is not a safe point, so the
/// next decision runs and its write, sent on a healthy store, commits.
TEST_P(CASOwnerStopLoop, KilledQueryDecidesAgainAfterConflictPause)
{
    const ConflictPauseArm arm = GetParam();
    const ConflictPauseRun run = runConflictPause(arm, PauseOwner::Query);
    EXPECT_EQ(codeOf(run.error), 0) << messageOf(run.error);
    if (run.result)
        EXPECT_TRUE(std::holds_alternative<Committed>(*run.result));
    EXPECT_EQ(run.decisions_at_stop, decisionsBeforeStop(arm)) << "one pause after each lost race";
    EXPECT_EQ(run.decisions, run.decisions_at_stop + 1) << "one decision after the kill";
    EXPECT_EQ(run.writes, run.writes_at_stop + 1) << "one write after the kill";
}

INSTANTIATE_TEST_SUITE_P(CASOwnerStopLoops, CASOwnerStopLoop,
    ::testing::Values(
        ConflictPauseArm{DecisionLoop::BodyReadModifyWrite, false},
        ConflictPauseArm{DecisionLoop::PresenceReadModifyWrite, false},
        ConflictPauseArm{DecisionLoop::Catalog, false},
        ConflictPauseArm{DecisionLoop::BodyReadModifyWrite, true},
        ConflictPauseArm{DecisionLoop::PresenceReadModifyWrite, true},
        ConflictPauseArm{DecisionLoop::Catalog, true}),
    [](const ::testing::TestParamInfo<ConflictPauseArm> & param_info)
    {
        const char * loop = param_info.param.loop == DecisionLoop::BodyReadModifyWrite ? "Body"
            : param_info.param.loop == DecisionLoop::PresenceReadModifyWrite ? "Presence" : "Catalog";
        return std::string(loop) + (param_info.param.slow ? "Slow" : "Fast");
    });

namespace
{

/// Counts the reads of `key` after `arm` saw a new write of it, and runs `on_read_after_write` with that count.
class ReadAfterWriteHookBackend final : public CountingBackend
{
public:
    explicit ReadAfterWriteHookBackend(String key_) : key(std::move(key_)) {}

    std::function<void(int)> on_read_after_write;
    std::atomic<int> reads_after_write{0};

    void arm() { writes_at_arm = writeCount(key); }
    uint64_t writesSinceArm() const { return writeCount(key) - writes_at_arm.value_or(0); }

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        if (k == key && writes_at_arm && writeCount(key) > *writes_at_arm)
        {
            const int n = ++reads_after_write;
            if (on_read_after_write)
                on_read_after_write(n);
        }
        return CountingBackend::read(k, access);
    }

private:
    const String key;
    std::optional<uint64_t> writes_at_arm;
};

enum class UnresolvedStop : uint8_t
{
    /// During the owed settling read of the lost step-1 write.
    BeforePause,
    /// During the redecide pause.
    InsidePause,
    /// After the pause, past the first owner check that follows it.
    AfterPause,
    /// While the next submission waits behind a predecessor parked in its decision.
    InQueue,
    /// A killed query whose next base read fails: the client-like backend raises the kill, recorded by no owner check.
    UnrecordedClientVerdict,
    /// The fence is lost and the owner stops during the redecide pause.
    FenceLostInsidePause,
    /// Another server admits the namespace during the redecide pause; the owner stops between the check
    /// before `decide` and the marker that `decide` then throws.
    DecisionMarkerWhileStopping,
    /// The second decision's write is refused with 412, and the query is killed in the first read after that refusal:
    /// the next submission's base read, as a retried refusal is not settled.
    KillSettlingSecondWrite,
    /// As `KillSettlingSecondWrite`, but every write after the lost one is refused and the kill comes in the base read
    /// of the last submission the cap allows.
    KillReadingForLastAllowedWrite,
    /// As `KillReadingForLastAllowedWrite`, but the kill comes in the read that settles the last allowed write: no
    /// submission follows that write, so its refusal is settled.
    KillSettlingLastAllowedWrite,
    /// The second decision's write is refused as `READONLY` by the store while the owner stops, so the engine returns
    /// that refusal.
    RefusedSecondWriteWhileStopping,
    /// The second decision's write fails locally with `CORRUPTED_DATA`; the owner stays live.
    LocalFaultInSecondWrite,
};

struct UnresolvedCreateOutcome
{
    std::optional<CasRefCatalog::NamespaceCreationOutcome> outcome;
    int code = 0;
    String message;
    uint64_t catalog_writes = 0;
    int reads_after_write = 0;
    bool returned_while_parked = false;
    std::optional<WriteResult> predecessor;
    size_t lanes_after = 0;
    int sleeps = 0;
};

/// `createNamespace` whose step-1 write loses its reply without landing; the settling read finds the
/// catalog unchanged, which proves nothing under `resend_unresolved = false`, so step 1 decides again
/// after the redecide pause. `stop_at` empty keeps the owner live.
UnresolvedCreateOutcome runUnresolvedCreate(std::optional<UnresolvedStop> stop_at)
{
    UnresolvedCreateOutcome out;
    onThread(true, [&]
    {
        const Layout layout("p");
        const String key = layout.refCatalogKey();
        auto inner = std::make_shared<ReadAfterWriteHookBackend>(key);
        {
            auto setup_requests = openRequestsForTest(inner);
            auto setup = setup_requests.admit();
            CasRefCatalog::initializeEmptyForNewPool(setup, layout);
        }
        std::optional<CasKillableQuery> query;
        if (stop_at == UnresolvedStop::UnrecordedClientVerdict || stop_at == UnresolvedStop::KillSettlingSecondWrite
            || stop_at == UnresolvedStop::KillReadingForLastAllowedWrite || stop_at == UnresolvedStop::KillSettlingLastAllowedWrite)
            query.emplace(getContext().context);
        BackendPtr backend = query ? BackendPtr(std::make_shared<CasClientLikeBackend>(inner)) : BackendPtr(inner);

        auto clock = std::make_shared<ManualClock>();
        CasHotKeys hot_keys(16ULL << 20);
        std::atomic<bool> stop{false};
        std::atomic<bool> fence_lost{false};
        std::atomic<bool> caller_returned{false};
        std::atomic<bool> returned_while_parked{false};
        bool slept = false;
        int checks_after_sleep = 0;
        int sleeps = 0;
        std::latch parked{1};
        std::latch release{1};
        std::optional<std::thread> predecessor;
        std::optional<std::thread> watcher;
        CasRequests * requests_ptr = nullptr;
        auto competitor_requests = openRequestsForTest(inner);
        auto competitor = competitor_requests.admit();
        std::atomic<bool> competitor_admitted{false};
        std::atomic<bool> base_read_after_competitor{false};
        int checks_after_base_read = 0;

        Fence fence = Fence::open();
        fence.admit = [&fence_lost](uint64_t, uint64_t) { return fence_lost ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; };
        CasRequests requests(backend, fence, ManualClock::nowFnOf(clock), [&](uint64_t ms)
        {
            clock->advance(ms + 1);
            if (++sleeps != 1)
                return;
            slept = true;
            if (stop_at == UnresolvedStop::InsidePause)
                stop = true;
            if (stop_at == UnresolvedStop::DecisionMarkerWhileStopping)
            {
                CasRefCatalog::casAdmitEntry(competitor, layout, 1,
                    CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(7)});
                competitor_admitted = true;
            }
            if (stop_at == UnresolvedStop::FenceLostInsidePause)
            {
                fence_lost = true;
                stop = true;
            }
            if (stop_at == UnresolvedStop::InQueue)
            {
                predecessor.emplace([&]
                {
                    auto predecessor_op = requests_ptr->admit();
                    out.predecessor = hot_keys.submit(key, predecessor_op, Retry::standard(),
                        [&](const std::optional<DB::Cas::Object> &) -> std::optional<String>
                        {
                            parked.count_down();
                            release.wait();
                            return std::nullopt;
                        });
                });
                parked.wait();
                watcher.emplace([&]
                {
                    const auto deadline = std::chrono::steady_clock::now() + WATCHDOG;
                    while (hot_keys.queueDepthForTest(key) < 2 && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    stop = true;
                    while (!caller_returned && std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    returned_while_parked = caller_returned.load();
                    release.count_down();
                });
            }
        }, &hot_keys, kDefaultRequestWindowMs);
        requests_ptr = &requests;

        std::optional<OwnerScope> owner;
        if (!query)
            owner.emplace(OwnerScope::background([&]
            {
                if (stop_at == UnresolvedStop::AfterPause && slept && ++checks_after_sleep >= 2)
                    throw Exception(ErrorCodes::ABORTED, "background owner stopped");
                /// The first check after the caller's base read is the one before `decide`, which passes.
                if (base_read_after_competitor && ++checks_after_base_read >= 2)
                    throw Exception(ErrorCodes::ABORTED, "background owner stopped");
                if (stop)
                    throw Exception(ErrorCodes::ABORTED, "background owner stopped");
            }));
        inner->on_read_after_write = [&](int n)
        {
            if (competitor_admitted)
                base_read_after_competitor = true;
            if (stop_at == UnresolvedStop::BeforePause && n == 1)
                stop = true;
            if (stop_at == UnresolvedStop::UnrecordedClientVerdict && n == 2)
            {
                query->kill();
                throw Poco::TimeoutException("ReadAfterWriteHookBackend: the read timed out");
            }
            if (stop_at == UnresolvedStop::KillSettlingSecondWrite && inner->writesSinceArm() == 2)
            {
                query->kill();
                throw Poco::TimeoutException("ReadAfterWriteHookBackend: the settling read timed out");
            }
            if (stop_at == UnresolvedStop::KillReadingForLastAllowedWrite || stop_at == UnresolvedStop::KillSettlingLastAllowedWrite)
            {
                const uint64_t kill_after_write
                    = stop_at == UnresolvedStop::KillSettlingLastAllowedWrite ? kCasMaxDecisions : kCasMaxDecisions - 1;
                if (inner->writesSinceArm() == kill_after_write)
                {
                    query->kill();
                    throw Poco::TimeoutException("ReadAfterWriteHookBackend: the settling read timed out");
                }
                if (inner->writesSinceArm() >= 2)
                    inner->refuseNextWrite(key);
            }
        };

        auto op = requests.admit();
        inner->arm();
        inner->injectAmbiguousWrite(key);
        if (stop_at == UnresolvedStop::KillSettlingSecondWrite || stop_at == UnresolvedStop::KillReadingForLastAllowedWrite
            || stop_at == UnresolvedStop::KillSettlingLastAllowedWrite)
            inner->refuseNextWrite(key);
        if (stop_at == UnresolvedStop::RefusedSecondWriteWhileStopping)
            inner->onBeforeWrite(key, [&]
            {
                if (inner->writesSinceArm() != 2)
                    return;
                stop = true;
                throw Exception(ErrorCodes::READONLY, "ReadAfterWriteHookBackend: the store is read-only");
            });
        if (stop_at == UnresolvedStop::LocalFaultInSecondWrite)
            inner->onBeforeWrite(key, [&]
            {
                if (inner->writesSinceArm() == 2)
                    throw Exception(ErrorCodes::CORRUPTED_DATA, "ReadAfterWriteHookBackend: a local decode failure");
            });
        try
        {
            out.outcome = CasRefCatalog::createNamespace(op, layout, 1, RootNamespace{"a"},
                CreatorFence{.server_root_id = "srv1", .writer_epoch = 5, .fence_generation = 1});
        }
        catch (...)
        {
            out.code = codeOf(std::current_exception());
            out.message = messageOf(std::current_exception());
        }
        caller_returned = true;
        if (watcher)
            watcher->join();
        if (predecessor)
            predecessor->join();
        out.returned_while_parked = returned_while_parked;
        out.catalog_writes = inner->writesSinceArm();
        out.reads_after_write = inner->reads_after_write.load();
        out.lanes_after = hot_keys.laneCountForTest();
        out.sleeps = sleeps;
        owner.reset();
    });
    return out;
}

}

/// A stop after a sent step-1 write whose outcome stays unresolved keeps the unresolved exit: one write,
/// no second decision, the 210 that names the unresolved send. A live owner decides again and creates the row.
TEST(CASOwnerStop, StoppedNamespaceCreateKeepsUnresolvedExit)
{
    for (const UnresolvedStop stop_at : {UnresolvedStop::BeforePause, UnresolvedStop::InsidePause, UnresolvedStop::AfterPause,
                                         UnresolvedStop::InQueue, UnresolvedStop::UnrecordedClientVerdict})
    {
        SCOPED_TRACE(static_cast<int>(stop_at));
        const auto out = runUnresolvedCreate(stop_at);
        EXPECT_FALSE(out.outcome.has_value());
        EXPECT_EQ(out.code, ErrorCodes::NETWORK_ERROR);
        EXPECT_NE(out.message.find("a sent write's outcome is unresolved"), String::npos) << out.message;
        EXPECT_EQ(out.catalog_writes, 1u) << "one PUT";
        /// The settling read, plus the predecessor's base read or the failed base read where the arm has one:
        /// no base read for a second decision of the caller's.
        const int expected_reads = stop_at == UnresolvedStop::InQueue || stop_at == UnresolvedStop::UnrecordedClientVerdict ? 2 : 1;
        EXPECT_EQ(out.reads_after_write, expected_reads);
        EXPECT_EQ(out.lanes_after, 0u);
        if (stop_at == UnresolvedStop::BeforePause)
            EXPECT_EQ(out.sleeps, 0) << "the check before the redecide sleep ends the call";
        if (stop_at == UnresolvedStop::InQueue)
        {
            EXPECT_TRUE(out.returned_while_parked) << "the caller left while the predecessor was still parked";
            ASSERT_TRUE(out.predecessor.has_value());
            EXPECT_TRUE(std::holds_alternative<Declined>(*out.predecessor));
        }
    }

    {
        SCOPED_TRACE("a lost fence observed in the pause stays the typed fence outcome");
        const auto out = runUnresolvedCreate(UnresolvedStop::FenceLostInsidePause);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.outcome.has_value());
        EXPECT_EQ(*out.outcome, CasRefCatalog::NamespaceCreationOutcome::FencedOut);
        EXPECT_EQ(out.catalog_writes, 1u);
    }

    {
        SCOPED_TRACE("a marker the decision throws stays as it is");
        const auto out = runUnresolvedCreate(UnresolvedStop::DecisionMarkerWhileStopping);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.outcome.has_value());
        EXPECT_EQ(*out.outcome, CasRefCatalog::NamespaceCreationOutcome::Superseded);
        EXPECT_EQ(out.catalog_writes, 2u) << "the lost step-1 write and the other server's admission";
    }

    {
        SCOPED_TRACE("live owner");
        const auto out = runUnresolvedCreate(std::nullopt);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.outcome.has_value());
        EXPECT_EQ(*out.outcome, CasRefCatalog::NamespaceCreationOutcome::Live);
        EXPECT_EQ(out.catalog_writes, 3u) << "the lost step-1 write, its second decision, step 3";
    }
}

/// A refused write followed by a read that fails after a kill does not answer the first write, whose outcome is still
/// unknown: the call keeps the unresolved exit and sends nothing more, also in the last submission the cap allows,
/// whether the kill comes in that submission's read or in the read that settles its refused write.
TEST(CASOwnerStop, ObservedStopConflictKeepsAnEarlierUnresolvedExit)
{
    for (const auto & [stop_at, writes] : {std::pair{UnresolvedStop::KillSettlingSecondWrite, uint64_t{2}},
                                           std::pair{UnresolvedStop::KillReadingForLastAllowedWrite, uint64_t{kCasMaxDecisions - 1}},
                                           std::pair{UnresolvedStop::KillSettlingLastAllowedWrite, uint64_t{kCasMaxDecisions}}})
    {
        SCOPED_TRACE(writes);
        const auto out = runUnresolvedCreate(stop_at);
        EXPECT_FALSE(out.outcome.has_value());
        EXPECT_EQ(out.code, ErrorCodes::NETWORK_ERROR) << out.message;
        EXPECT_NE(out.message.find("a sent write's outcome is unresolved"), String::npos) << out.message;
        EXPECT_EQ(out.message.find("did not converge"), String::npos) << out.message;
        EXPECT_EQ(out.catalog_writes, writes) << "the lost write and the refused ones, nothing after";
    }
}

/// The store refuses the second decision's write while the owner stops. That refusal says nothing about the first
/// write, whose outcome is still unknown, so the call ends with the unresolved 210 rather than the refusal.
TEST(CASOwnerStop, RefusalUnderAStopKeepsAnEarlierUnresolvedExit)
{
    const auto out = runUnresolvedCreate(UnresolvedStop::RefusedSecondWriteWhileStopping);
    EXPECT_FALSE(out.outcome.has_value());
    EXPECT_EQ(out.code, ErrorCodes::NETWORK_ERROR) << out.message;
    EXPECT_NE(out.message.find("a sent write's outcome is unresolved"), String::npos) << out.message;
    EXPECT_NE(out.message.find("the store is read-only"), String::npos) << "names what ended the call: " << out.message;
    EXPECT_EQ(out.catalog_writes, 2u) << "the lost write and the refused one, nothing after";
}

/// A local fault after an unresolved write is a bug or resource report, not a store answer: it reaches the caller
/// unchanged rather than as the retryable unresolved exit.
TEST(CASOwnerStop, LocalFaultAfterAnUnresolvedWriteIsNotReplaced)
{
    const auto out = runUnresolvedCreate(UnresolvedStop::LocalFaultInSecondWrite);
    EXPECT_FALSE(out.outcome.has_value());
    EXPECT_EQ(out.code, ErrorCodes::CORRUPTED_DATA) << out.message;
    EXPECT_NE(out.message.find("a local decode failure"), String::npos) << out.message;
    EXPECT_EQ(out.message.find("a sent write's outcome is unresolved"), String::npos) << out.message;
    EXPECT_EQ(out.catalog_writes, 2u) << "the lost write and the failed one, nothing after";
}

/// A catalog update that lost two races to another server stops before its next submission starts when the owner
/// stops in the conflict pause after the second; the first is retried at once. A fence lost while the first write
/// is in flight stays the typed fence outcome.
TEST(CASOwnerStop, ConflictedCatalogUpdateStopsAtTheNextSubmission)
{
    for (const bool fence_lost_in_flight : {false, true})
    {
        SCOPED_TRACE(fence_lost_in_flight ? "fence lost while the write is in flight" : "stop in the conflict pause");
        onThread(false, [&]
        {
            const Layout layout("p");
            const String key = layout.refCatalogKey();
            auto backend = std::make_shared<CountingBackend>();
            const CatalogEntry observed{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)};
            auto competitor_requests = openRequestsForTest(backend);
            auto competitor = competitor_requests.admit();
            CasRefCatalog::initializeEmptyForNewPool(competitor, layout);
            CasRefCatalog::casAdmitEntry(competitor, layout, 1, observed);

            std::atomic<bool> stop{false};
            std::atomic<bool> fence_lost{false};
            int pauses = 0;
            uint64_t reads_at_pause = 0;
            Fence fence = Fence::open();
            fence.admit = [&fence_lost](uint64_t, uint64_t) { return fence_lost ? Fence::Admit::LostOrRearmed : Fence::Admit::Ok; };
            /// No cache: a next submission would start with a base read, so the read count shows whether it started.
            CasHotKeys hot_keys(0);
            CasRequests requests(backend, fence, {}, [&](uint64_t)
            {
                ++pauses;
                reads_at_pause = backend->getCount(key);
                stop = true;
            }, &hot_keys, kDefaultRequestWindowMs);
            auto op = requests.admit();

            bool competing = false;
            int competitor_writes = 0;
            backend->onBeforeWrite(key, [&]
            {
                if (competing || competitor_writes > 1)
                    return;
                competing = true;
                CasRefCatalog::casAdmitEntry(competitor, layout, 1,
                    CatalogEntry{.ns = RootNamespace{competitor_writes == 0 ? "b" : "c"}, .state = NsState::Live,
                                 .incarnation = UInt128(2 + competitor_writes)});
                ++competitor_writes;
                competing = false;
                if (fence_lost_in_flight)
                {
                    fence_lost = true;
                    stop = true;
                }
            });
            const uint64_t writes_before = backend->writeCount(key);

            auto owner = backgroundStoppedBy(stop);
            std::optional<CasRefCatalog::BeginRemovingOutcome> outcome;
            std::exception_ptr error;
            try
            {
                outcome = CasRefCatalog::beginRemoving(op, layout, observed, /*removal_started_round*/ 13);
            }
            catch (...)
            {
                error = std::current_exception();
            }
            EXPECT_EQ(backend->writeCount(key) - writes_before, fence_lost_in_flight ? 2u : 4u)
                << "each of the caller's writes and the competitor's write before it";
            if (fence_lost_in_flight)
            {
                EXPECT_EQ(codeOf(error), 0) << messageOf(error);
                ASSERT_TRUE(outcome.has_value());
                EXPECT_EQ(*outcome, CasRefCatalog::BeginRemovingOutcome::FencedOut);
                EXPECT_EQ(pauses, 0);
            }
            else
            {
                EXPECT_EQ(codeOf(error), ErrorCodes::ABORTED) << messageOf(error);
                EXPECT_EQ(pauses, 1);
                EXPECT_EQ(backend->getCount(key), reads_at_pause) << "no base read for a next submission";
            }
            const auto snap = CasRefCatalog::read(competitor, layout);
            ASSERT_EQ(snap.catalog.entries.size(), fence_lost_in_flight ? 2u : 3u);
            EXPECT_EQ(snap.catalog.entries.at(0).state, NsState::Live) << "the next submission never started";
        });
    }
}

namespace
{

/// The first remove of `key` meets a new incarnation another writer put there, so it answers `Mismatch`.
class ReplacedBeforeFirstRemoveBackend final : public CountingBackend
{
public:
    explicit ReplacedBeforeFirstRemoveBackend(String key_, int replacements_ = 1) : key(std::move(key_)), replacements(replacements_) {}

    std::function<void()> on_replaced;

    RawRemoval remove(const String & k, const String & expected_value, TransportAccess & access) override
    {
        if (k == key && replaced.fetch_add(1) < replacements)
        {
            const auto current = InMemoryBackend::head(k, access);
            (void)InMemoryBackend::write(k, "replaced", current->value, access);
            if (on_replaced)
                on_replaced();
        }
        return CountingBackend::remove(k, expected_value, access);
    }

private:
    const String key;
    const int replacements;
    std::atomic<int> replaced{0};
};

}

/// A stop around the reissue pause of `removeCurrent` sends no further HEAD. A stop at the first mismatch ends the
/// call at the checks of its zero pause, without sleeping; a stop during the pause after the second mismatch ends it
/// before the third HEAD.
TEST(CASOwnerStop, StopAroundRemoveReissuePauseSendsNoHead)
{
    for (const bool during_sleep : {false, true})
    {
        SCOPED_TRACE(during_sleep ? "stop during the sleep" : "stop before the pause");
        onThread(false, [&]
        {
            auto backend = std::make_shared<ReplacedBeforeFirstRemoveBackend>("k", /*replacements=*/2);
            std::atomic<bool> stop{false};
            int sleeps = 0;
            CasRequests requests(backend, Fence::open(), {}, [&](uint64_t)
            {
                ++sleeps;
                if (during_sleep)
                    stop = true;
            }, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            ASSERT_TRUE(std::holds_alternative<Committed>(op.create("k", "v", Retry::once())));
            if (!during_sleep)
                backend->on_replaced = [&stop] { stop = true; };

            auto owner = backgroundStoppedBy(stop);
            int code = 0;
            try
            {
                (void)op.removeCurrent("k", Retry::standard());
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
            EXPECT_EQ(code, ErrorCodes::ABORTED);
            EXPECT_EQ(backend->headCount("k"), during_sleep ? 2u : 1u);
            EXPECT_EQ(backend->deleteCount("k"), during_sleep ? 2u : 1u);
            EXPECT_EQ(sleeps, during_sleep ? 1 : 0);
        });
    }
}

namespace
{

/// The first write of `key` runs `on_first_write`, stores its bytes when `land` is set, and throws `failure`.
/// Every read of `key` runs `on_read` first.
class FirstWriteFailsBackend final : public InMemoryBackend
{
public:
    explicit FirstWriteFailsBackend(String key_) : key(std::move(key_)) {}

    std::exception_ptr failure = std::make_exception_ptr(Poco::TimeoutException("FirstWriteFailsBackend: the write timed out"));
    bool land = false;
    std::function<void()> on_first_write;
    std::function<void()> on_write;
    std::function<void()> on_read;
    std::atomic<int> writes{0};
    std::atomic<int> reads{0};
    std::atomic<int> refreshes{0};

    std::expected<String, RawConflict> write(const String & k, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        if (k == key)
        {
            if (on_write)
                on_write();
            if (writes.fetch_add(1) == 0 && failure)
            {
                if (land)
                    (void)InMemoryBackend::write(k, bytes, expected_value, access);
                if (on_first_write)
                    on_first_write();
                std::rethrow_exception(failure);
            }
        }
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        if (k == key)
        {
            ++reads;
            if (on_read)
                on_read();
        }
        return InMemoryBackend::read(k, access);
    }

    bool refreshCredentials() override
    {
        ++refreshes;
        return true;
    }

private:
    const String key;
};

/// One write call under an owner that `stop` stops: a background owner, or a query whose failed answers
/// the client-like backend replaces with its verdict.
struct StoppableWrite
{
    StoppedBy owner = StoppedBy::Background;
    /// Arms the backend; `stop` stops the owner.
    std::function<void(FirstWriteFailsBackend &, const std::function<void()> & stop)> prepare_backend;
    /// Runs during each pause, with `stop`.
    std::function<void(const std::function<void()> & stop)> during_pause;
    /// Whether the `create` of the key runs inside `OwnerScope::owed`.
    bool owed = false;
    /// The cause a killed query throws instead of `QUERY_WAS_CANCELLED`.
    std::exception_ptr kill_cause;
};

struct StoppableWriteOutcome
{
    std::optional<WriteResult> result;
    int code = 0;
    String message;
    std::exception_ptr error;
    int writes = 0;
    int reads = 0;
    int pauses = 0;
    int refreshes = 0;
};

constexpr auto STOPPABLE_KEY = "k";

StoppableWriteOutcome runStoppableWrite(const StoppableWrite & arm)
{
    StoppableWriteOutcome out;
    onThread(true, [&]
    {
        std::optional<CasKillableQuery> query;
        if (arm.owner == StoppedBy::Query)
            query.emplace(getContext().context);
        auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
        BackendPtr backend = query ? BackendPtr(std::make_shared<CasClientLikeBackend>(inner)) : BackendPtr(inner);
        std::atomic<bool> stopped{false};
        const std::function<void()> stop = [&]
        {
            stopped = true;
            if (query)
                query->kill(arm.kill_cause);
        };
        std::atomic<int> pauses{0};
        CasRequests requests(backend, Fence::open(), {}, [&](uint64_t)
        {
            ++pauses;
            if (arm.during_pause)
                arm.during_pause(stop);
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        if (arm.prepare_backend)
            arm.prepare_backend(*inner, stop);
        inner->writes = 0;
        inner->reads = 0;

        std::optional<OwnerScope> background;
        if (arm.owner == StoppedBy::Background)
            background.emplace(backgroundStoppedBy(stopped));
        try
        {
            std::optional<OwnerScope> owed;
            if (arm.owed)
                owed.emplace(OwnerScope::owed());
            out.result = op.create(STOPPABLE_KEY, "bytes", Retry::standard());
        }
        catch (...)
        {
            out.error = std::current_exception();
            out.code = codeOf(out.error);
            out.message = messageOf(out.error);
        }
        out.writes = inner->writes.load();
        out.reads = inner->reads.load();
        out.pauses = pauses.load();
        out.refreshes = inner->refreshes.load();
    });
    return out;
}

void expectUnresolved(const StoppableWriteOutcome & out)
{
    EXPECT_EQ(out.code, 0) << out.message;
    ASSERT_TRUE(out.result.has_value());
    const auto * gave_up = std::get_if<GaveUp>(&*out.result);
    ASSERT_NE(gave_up, nullptr) << "index " << out.result->index();
    EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
    EXPECT_EQ(gave_up->deadline_source, GaveUp::Source::Policy);
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_EQ(gave_up->attempts_sent, 1u);
}

void expectCommittedByRead(const StoppableWriteOutcome & out)
{
    EXPECT_EQ(out.code, 0) << out.message;
    ASSERT_TRUE(out.result.has_value());
    const auto * committed = std::get_if<Committed>(&*out.result);
    ASSERT_NE(committed, nullptr) << "index " << out.result->index();
    EXPECT_TRUE(committed->resolved_by_read);
    EXPECT_EQ(committed->attempts_sent, 1u);
}

}

/// A store's definite refusal of a write whose every attempt was answered is the call's answer, also when a background
/// owner stopped during that attempt: nothing landed, so there is nothing to settle and no credential refresh to try.
/// After an earlier unanswered attempt the same refusal proves nothing about it, so the write still settles.
TEST(CASOwnerStop, BackgroundStopKeepsADefiniteRefusal)
{
    std::vector<std::pair<const char *, std::function<std::exception_ptr()>>> refusals{
        {"READONLY", [] { return std::make_exception_ptr(DB::Exception(DB::ErrorCodes::READONLY, "the store is read-only")); }}};
#if USE_AWS_S3
    refusals.emplace_back("AccessDenied", []
    {
        return std::make_exception_ptr(
            DB::S3Exception("the store answered AccessDenied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    });
#endif
    for (const auto & [name, refusal] : refusals)
    {
        {
            SCOPED_TRACE(fmt::format("{}, every attempt answered", name));
            StoppableWrite arm;
            arm.prepare_backend = [make = refusal](FirstWriteFailsBackend & backend, const std::function<void()> & stop)
            {
                backend.failure = make();
                backend.on_first_write = stop;
            };
            const auto out = runStoppableWrite(arm);
            EXPECT_EQ(out.code, 0) << out.message;
            ASSERT_TRUE(out.result.has_value());
            EXPECT_TRUE(std::holds_alternative<Refused>(*out.result)) << "index " << out.result->index();
            EXPECT_EQ(out.writes, 1);
            EXPECT_EQ(out.reads, 0) << "a refusal needs no settling read";
            EXPECT_EQ(out.refreshes, 0);
        }
        {
            SCOPED_TRACE(fmt::format("{}, after an unanswered attempt", name));
            StoppableWrite arm;
            arm.prepare_backend = [make = refusal](FirstWriteFailsBackend & backend, const std::function<void()> & stop)
            {
                backend.on_write = [&backend, stop, make]
                {
                    if (backend.writes.load() == 1)
                    {
                        stop();
                        std::rethrow_exception(make());
                    }
                };
            };
            const auto out = runStoppableWrite(arm);
            EXPECT_EQ(out.code, 0) << out.message;
            ASSERT_TRUE(out.result.has_value());
            const auto * gave_up = std::get_if<GaveUp>(&*out.result);
            ASSERT_NE(gave_up, nullptr) << "index " << out.result->index();
            EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
            EXPECT_EQ(gave_up->attempts_sent, 2u);
        }
    }
}

/// A stop that lands while a possibly-sent write waits to resend ends it without the resend: one `PUT`.
/// The settling read is owed only where it had not run yet, which is the connect-hint pause.
TEST(CASOwnerStop, StopInsideEachPauseSettlesWithoutResend)
{
    for (const StoppedBy owner : {StoppedBy::Background, StoppedBy::Query})
    {
        {
            SCOPED_TRACE(fmt::format("{}, inside pauseAndReissue", nameOf(owner)));
            StoppableWrite arm;
            arm.owner = owner;
            arm.during_pause = [](const auto & stop) { stop(); };
            const auto out = runStoppableWrite(arm);
            expectUnresolved(out);
            EXPECT_EQ(out.writes, 1) << "no resend";
            EXPECT_EQ(out.reads, 1) << "the settling read before the pause, none after it";
            EXPECT_EQ(out.pauses, 1);
        }
        {
            SCOPED_TRACE(fmt::format("{}, after a repeatable settlement", nameOf(owner)));
            StoppableWrite arm;
            arm.owner = owner;
            arm.prepare_backend = [](FirstWriteFailsBackend & backend, const std::function<void()> & stop) { backend.on_read = stop; };
            const auto out = runStoppableWrite(arm);
            expectUnresolved(out);
            EXPECT_EQ(out.writes, 1) << "no resend";
            EXPECT_EQ(out.reads, 1);
            EXPECT_EQ(out.pauses, 0) << "no pause before a resend that cannot happen";
        }
#if USE_AWS_S3
        for (const bool landed : {false, true})
        {
            SCOPED_TRACE(fmt::format("{}, inside pauseFlat, {}", nameOf(owner), landed ? "landed" : "not landed"));
            StoppableWrite arm;
            arm.owner = owner;
            arm.prepare_backend = [landed](FirstWriteFailsBackend & backend, const std::function<void()> &)
            {
                backend.land = landed;
                backend.failure = std::make_exception_ptr(
                    DB::S3Exception("Connection refused", Aws::S3::S3Errors::NETWORK_CONNECTION));
            };
            arm.during_pause = [](const auto & stop) { stop(); };
            const auto out = runStoppableWrite(arm);
            if (landed)
                expectCommittedByRead(out);
            else
                expectUnresolved(out);
            EXPECT_EQ(out.writes, 1) << "no resend";
            EXPECT_EQ(out.reads, 1) << "the owed settling read the hinted attempt had not had";
            EXPECT_EQ(out.pauses, 1);
        }
#endif
    }
}

/// A query killed with a stored cause while its write's reply is lost: the client-like backend surfaces the cause, and it
/// is the query's verdict whatever its code or type. A local-bug code is no local bug of the attempt, an exception the
/// transport cannot raise is caught before any classification, and `READONLY` is no refusal. The write is settled.
TEST(CASOwnerStop, LostReplyWithStoredCauseSettles)
{
    const std::vector<std::pair<String, std::function<std::exception_ptr()>>> causes{
        {"BAD_ARGUMENTS", [] { return storedCause(ErrorCodes::BAD_ARGUMENTS); }},
        {"runtime_error", [] { return std::make_exception_ptr(std::runtime_error("the query's stored cause")); }},
        {"READONLY", [] { return storedCause(ErrorCodes::READONLY); }}};
    for (const auto & [name, cause] : causes)
    {
        for (const bool landed : {false, true})
        {
            SCOPED_TRACE(fmt::format("{}, {}", name, landed ? "landed" : "not landed"));
            StoppableWrite arm;
            arm.owner = StoppedBy::Query;
            arm.kill_cause = cause();
            arm.prepare_backend = [landed](FirstWriteFailsBackend & backend, const std::function<void()> & stop)
            {
                backend.land = landed;
                backend.on_first_write = stop;
            };
            const auto out = runStoppableWrite(arm);
            if (landed)
                expectCommittedByRead(out);
            else
                expectUnresolved(out);
            EXPECT_EQ(out.writes, 1) << "no resend";
            EXPECT_EQ(out.reads, 1) << "the owed settling read";
            EXPECT_EQ(out.pauses, 0);
        }
    }
}

#if USE_AWS_S3

namespace
{

/// Finalizes by running `on_finalize`, asking the thread's query as the S3 client does, then running `on_reply` and
/// failing with the store's own `412 PreconditionFailed`.
class PreconditionFailedWriteBuffer final : public DB::WriteBufferFromFileBase
{
public:
    PreconditionFailedWriteBuffer(std::function<void()> on_finalize_, std::function<void()> on_reply_)
        : DB::WriteBufferFromFileBase(DB::DBMS_DEFAULT_BUFFER_SIZE, nullptr, 0)
        , on_finalize(std::move(on_finalize_))
        , on_reply(std::move(on_reply_))
    {
    }

    void sync() override { next(); }
    std::string getFileName() const override { return "precondition-failed-write-buffer"; }

private:
    void nextImpl() override {}

    void finalizeImpl() override
    {
        next();
        if (on_finalize)
            on_finalize();
        CurrentThread::checkIfNotCancelled();
        if (on_reply)
            on_reply();
        throw DB::S3Exception("the store answered 412", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed");
    }

    std::function<void()> on_finalize;
    std::function<void()> on_reply;
};

class PreconditionFailedLocalObjectStorage final : public DB::LocalObjectStorage
{
public:
    using DB::LocalObjectStorage::LocalObjectStorage;

    std::function<void()> on_finalize;
    std::function<void()> on_reply;
    std::atomic<int> writes{0};

    std::unique_ptr<DB::WriteBufferFromFileBase> writeObject(
        const DB::StoredObject &, DB::WriteMode, std::optional<DB::ObjectAttributes>, size_t, const DB::WriteSettings &) override
    {
        ++writes;
        return std::make_unique<PreconditionFailedWriteBuffer>(on_finalize, on_reply);
    }
};

}

/// The native classifier reads a stopped query's `412` as no store answer: the S3 client throws the query's
/// stored cause in place of whatever the store said. A stopped background owner is invisible to the client,
/// so its genuine `412` stays a conflict.
TEST(CASOwnerStop, StoredPreconditionFailedIsNotAConflict)
{
    for (const StoppedBy owner : {StoppedBy::Query, StoppedBy::Background})
    {
        SCOPED_TRACE(nameOf(owner));
        auto storage = makeLocalObjectStorageOf<PreconditionFailedLocalObjectStorage>();
        const String key = DB::Cas::tests::nativeKeyUnder(storage, "absent");
        auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
        std::optional<WriteResult> written;
        int code = 0;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            std::atomic<bool> stopped{false};
            storage->on_finalize = [&]
            {
                stopped = true;
                if (owner == StoppedBy::Query)
                    query.kill(std::make_exception_ptr(
                        DB::S3Exception("the query's stored cause", Aws::S3::S3Errors::UNKNOWN, "PreconditionFailed")));
            };
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            std::optional<OwnerScope> background;
            if (owner == StoppedBy::Background)
                background.emplace(backgroundStoppedBy(stopped));
            try
            {
                written = op.create(key, "bytes", Retry::standard());
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
        });
        EXPECT_EQ(code, 0);
        ASSERT_TRUE(written.has_value());
        EXPECT_EQ(storage->writes.load(), 1);
        if (owner == StoppedBy::Query)
        {
            const auto * gave_up = std::get_if<GaveUp>(&*written);
            ASSERT_NE(gave_up, nullptr) << "a stopped query's stored 412 is no conflict; index " << written->index();
            EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
        }
        else
        {
            const auto * conflict = std::get_if<Conflict>(&*written);
            ASSERT_NE(conflict, nullptr) << "index " << written->index();
            EXPECT_FALSE(conflict->any_ambiguous);
        }
    }
}

/// The store's own `412` reached the classifier, and the query was killed only after the S3 client let it through:
/// the refusal is still the store's answer, so the write ends as a conflict, not as an unresolved send.
TEST(CASOwnerStop, AKillAfterTheStoresOwn412KeepsTheConflict)
{
    auto storage = makeLocalObjectStorageOf<PreconditionFailedLocalObjectStorage>();
    const String key = DB::Cas::tests::nativeKeyUnder(storage, "absent");
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
    std::optional<WriteResult> written;
    int code = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        storage->on_reply = [&] { query.kill(); };
        CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        try
        {
            written = op.create(key, "bytes", Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
    });
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(written.has_value());
    EXPECT_EQ(storage->writes.load(), 1);
    const auto * conflict = std::get_if<Conflict>(&*written);
    ASSERT_NE(conflict, nullptr) << "a genuine 412 is a conflict; index " << written->index();
    EXPECT_FALSE(conflict->any_ambiguous);
}

/// The probe's half of the test above: the store's own `NoSuchBucket` passed the S3 client, then the query was
/// killed before the probe classified it, and the answer stays the store's.
TEST(CASOwnerStop, AKillAfterTheStoresOwnProbeAnswerKeepsIt)
{
    auto storage = makeLocalObjectStorageOf<HookedReadLocalObjectStorage>();
    const String key = DB::Cas::tests::nativeKeyUnder(storage, "sentinel");
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
    std::optional<SentinelProbeResult> probed;
    int code = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        storage->on_read = [&]
        {
            CurrentThread::checkIfNotCancelled();
            query.kill();
            throw DB::S3Exception("the store answered NoSuchBucket", Aws::S3::S3Errors::NO_SUCH_BUCKET);
        };
        CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        try
        {
            probed = op.probeSentinel(key, Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
    });
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(probed.has_value());
    EXPECT_EQ(probed->outcome, ProbeOutcome::ContainerAbsent);
    EXPECT_EQ(storage->reads.load(), 1);
}

#endif

namespace
{

/// Counts the requests that change the store, and fails the next one with a transport-class error while `fail_next`
/// is set.
class MutationCountingBackend final : public InMemoryBackend
{
public:
    std::atomic<int> mutations{0};
    std::atomic<bool> fail_next{false};

    std::expected<String, RawConflict> write(const String & k, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        countAndMaybeFail();
        return InMemoryBackend::write(k, bytes, expected_value, access);
    }

    RawRemoval remove(const String & k, const String & expected_value, TransportAccess & access) override
    {
        countAndMaybeFail();
        return InMemoryBackend::remove(k, expected_value, access);
    }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        countAndMaybeFail();
        InMemoryBackend::removeManyWriteOnce(keys, access);
    }

    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        countAndMaybeFail();
        InMemoryBackend::publish(request, access);
    }

private:
    void countAndMaybeFail()
    {
        ++mutations;
        if (fail_next.exchange(false))
            throw Exception(ErrorCodes::S3_ERROR, "the first mutation failed");
    }
};

enum class MutatingVerb : uint8_t { Create, Replace, Remove, BulkRemove, Publish };

struct MutationRun
{
    int code = 0;
    int mutations = 0;
};

/// Calls `verb` once on a store holding `present`, through a backend that answers a failure under a killed query with
/// the query's verdict, as the S3 client does. `stopped_before` stops its owner before the call; `fail_first` fails
/// the call's first mutation and kills the query in the pause after it.
MutationRun runMutatingVerb(MutatingVerb verb, std::optional<StoppedBy> stopped_before, bool fail_first)
{
    MutationRun run;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto backend = std::make_shared<MutationCountingBackend>();
        CasRequests requests(std::make_shared<CasClientLikeBackend>(backend), Fence::open(), {}, [&](uint64_t)
        {
            if (fail_first)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        const auto present = op.create("present", "v", Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>(present));
        const Etag seen = std::get<Committed>(present).etag;
        backend->mutations = 0;
        backend->fail_next = fail_first;

        std::atomic<bool> stopped{true};
        std::optional<OwnerScope> background;
        if (stopped_before == StoppedBy::Background)
            background.emplace(backgroundStoppedBy(stopped));
        else if (stopped_before == StoppedBy::Query)
            query.kill();
        const String payload = "payload";
        try
        {
            switch (verb)
            {
                case MutatingVerb::Create: (void)op.create("new", "v", Retry::standard()); break;
                case MutatingVerb::Replace: (void)op.replace("present", "w", seen, Retry::standard()); break;
                case MutatingVerb::Remove: (void)op.remove("present", seen, Retry::standard()); break;
                case MutatingVerb::BulkRemove:
                    op.removeManyWriteOnce({Layout("p").writeOnceBlobIntentProbeKey("probe")}, Retry::standard());
                    break;
                case MutatingVerb::Publish:
                    op.publish(BlobPublishRequest{
                        .destination_key = "blob",
                        .publication = StreamingBlobPublication{
                            .payload_size = payload.size(),
                            .fresh_envelope = "fresh",
                            .open_payload = [payload] { return std::make_unique<DB::ReadBufferFromOwnString>(payload); },
                            .expected_ref = DB::Cas::tests::idOf(payload)}}, Retry::standard());
                    break;
            }
        }
        catch (...)
        {
            run.code = codeOf(std::current_exception());
        }
        run.mutations = backend->mutations.load();
    });
    return run;
}

/// Calls each mutating verb once `owner` stopped.
void runEachMutatingVerbAfterStop(StoppedBy owner)
{
    using enum MutatingVerb;
    for (const MutatingVerb verb : {Create, Replace, Remove, BulkRemove, Publish})
    {
        SCOPED_TRACE(fmt::format("{}, verb {}", nameOf(owner), static_cast<int>(verb)));
        const MutationRun run = runMutatingVerb(verb, owner, /*fail_first=*/false);
        if (owner == StoppedBy::Background)
        {
            EXPECT_EQ(run.code, ErrorCodes::ABORTED);
            EXPECT_EQ(run.mutations, 0) << "nothing sent";
        }
        else
        {
            EXPECT_EQ(run.code, 0) << "a healthy first attempt of a killed query succeeds";
            EXPECT_EQ(run.mutations, 1) << "exactly the first attempt is sent";
        }
    }
}

}

/// A background owner that stopped before a mutating call gets its verdict before anything is sent.
TEST(CASOwnerStop, BackgroundStopBeforeFirstSendSendsNothing)
{
    runEachMutatingVerbAfterStop(StoppedBy::Background);
}

/// A killed query's first attempt of each mutating verb is sent and, on a healthy store, succeeds, as on plain S3.
TEST(CASOwnerStop, KilledQuerysFirstAttemptIsSent)
{
    runEachMutatingVerbAfterStop(StoppedBy::Query);
}

/// A body read-modify-write whose write meets a competitor's replacement while the owner stops sends one
/// `PUT` and decides once. A background owner's verdict surfaces at the next decision. A killed query's write
/// failed with the kill, so its settled conflict is the answer and no decision follows. A stored credential cause
/// is no credential answer: no refresh, no resend.
TEST(CASOwnerStop, ConflictThenStopUnderStandardRmwDecidesOnce)
{
    struct Arm
    {
        const char * name;
        StoppedBy owner;
        std::function<std::exception_ptr()> cause;
        /// 0: the call returns a `Conflict`.
        int code;
    };
    std::vector<Arm> arms = {
        {"background owner", StoppedBy::Background, [] { return std::exception_ptr{}; }, ErrorCodes::ABORTED},
        {"killed query", StoppedBy::Query, [] { return std::exception_ptr{}; }, 0},
    };
#if USE_AWS_S3
    arms.push_back({"killed query, stored AccessDenied", StoppedBy::Query,
        [] { return std::make_exception_ptr(DB::S3Exception("the query's stored cause", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied")); },
        0});
#endif
    for (const auto & arm : arms)
    {
        SCOPED_TRACE(arm.name);
        int code = 0;
        std::optional<WriteResult> written;
        int decisions = 0;
        int writes = 0;
        int refreshes = 0;
        onThread(true, [&]
        {
            std::optional<CasKillableQuery> query;
            if (arm.owner == StoppedBy::Query)
                query.emplace(getContext().context);
            auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
            inner->failure = nullptr;
            BackendPtr backend = query ? BackendPtr(std::make_shared<CasClientLikeBackend>(inner)) : BackendPtr(inner);
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            auto competitor_requests = openRequestsForTest(inner);
            auto competitor = competitor_requests.admit();
            ASSERT_TRUE(std::holds_alternative<Committed>(competitor.create(STOPPABLE_KEY, "v0", Retry::once())));

            std::atomic<bool> stopped{false};
            bool competing = false;
            inner->writes = 0;
            inner->on_write = [&]
            {
                if (competing || stopped)
                    return;
                competing = true;
                const auto current = competitor.read(STOPPABLE_KEY, Retry::once());
                ASSERT_TRUE(std::holds_alternative<Committed>(competitor.replace(STOPPABLE_KEY, "competitor", current->etag, Retry::once())));
                competing = false;
                stopped = true;
                if (query)
                    query->kill(arm.cause());
            };
            std::optional<OwnerScope> background;
            if (arm.owner == StoppedBy::Background)
                background.emplace(backgroundStoppedBy(stopped));
            const int writes_before = inner->writes.load();
            try
            {
                written = op.readModifyWrite(STOPPABLE_KEY, [&](const std::optional<DB::Cas::Object> & current)
                {
                    ++decisions;
                    return std::optional<String>(current ? current->bytes + "+" : String("v"));
                }, Retry::standard());
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
            /// The competitor's own write passes through the same backend.
            writes = inner->writes.load() - writes_before - 1;
            refreshes = inner->refreshes.load();
        });
        EXPECT_EQ(code, arm.code);
        if (arm.code == 0)
        {
            ASSERT_TRUE(written.has_value());
            const auto * conflict = std::get_if<Conflict>(&*written);
            ASSERT_NE(conflict, nullptr) << "index " << written->index();
            EXPECT_TRUE(conflict->observed_stop);
            EXPECT_TRUE(std::holds_alternative<DB::Cas::Object>(conflict->seen)) << "the settling read saw the competitor";
        }
        EXPECT_EQ(decisions, 1);
        EXPECT_EQ(writes, 1) << "one PUT";
        EXPECT_EQ(refreshes, 0);
    }
}

/// A clean 412 followed by a read that fails after the kill. A one-shot caller settles the 412 with that read and gets
/// the `Conflict`. A loop does not settle it: the failed read is the one after its pause, which ends the call with the
/// read's error. Neither starts a decision or a write after it.
TEST(CASOwnerStop, KilledQuerysConflictWithFailedSettlementEndsTheLoop)
{
    for (const bool once : {true, false})
    {
        SCOPED_TRACE(once ? "Retry::once" : "Retry::standard");
        std::optional<WriteResult> written;
        int code = 0;
        int decisions = 0;
        int writes = 0;
        int reads = 0;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
            inner->failure = nullptr;
            CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [](uint64_t) {}, nullptr,
                                 kDefaultRequestWindowMs);
            auto op = requests.admit();
            ASSERT_TRUE(std::holds_alternative<Committed>(op.create(STOPPABLE_KEY, "v0", Retry::once())));
            inner->refuseNextWrite(STOPPABLE_KEY);
            inner->writes = 0;
            inner->reads = 0;
            inner->on_read = [&]
            {
                if (inner->writes.load() == 0)
                    return;
                query.kill();
                throw Poco::TimeoutException("FirstWriteFailsBackend: the settling read timed out");
            };
            try
            {
                written = op.readModifyWrite(STOPPABLE_KEY, [&](const std::optional<DB::Cas::Object> & current)
                {
                    ++decisions;
                    return std::optional<String>(current->bytes + "+");
                }, once ? Retry::once() : Retry::standard());
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
            writes = inner->writes.load();
            reads = inner->reads.load();
        });
        if (once)
        {
            EXPECT_EQ(code, 0) << "the conflict is the answer, not the kill";
            ASSERT_TRUE(written.has_value());
            const auto * conflict = std::get_if<Conflict>(&*written);
            ASSERT_NE(conflict, nullptr) << "index " << written->index();
            EXPECT_TRUE(std::holds_alternative<NotObserved>(conflict->seen));
        }
        else
        {
            EXPECT_EQ(code, ErrorCodes::QUERY_WAS_CANCELLED) << "the failed read after the pause ends the call with the kill";
            EXPECT_FALSE(written.has_value());
        }
        EXPECT_EQ(decisions, 1);
        EXPECT_EQ(writes, 1) << "the refused PUT only";
        EXPECT_EQ(reads, 2) << "the base read and the one failed read after the refusal, not retried";
    }
}

/// A catalog update whose 412 is followed by a settling read that fails after the kill submits nothing more: no
/// reread, no decision, no write.
TEST(CASOwnerStop, KilledQuerysCatalogUpdateStopsAfterAFailedSettlement)
{
    int code = 0;
    int writes = 0;
    int reads = 0;
    int reads_at_failed_settlement = -1;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        const Layout layout("p");
        const String key = layout.refCatalogKey();
        auto inner = std::make_shared<FirstWriteFailsBackend>(key);
        inner->failure = nullptr;
        CasHotKeys hot_keys(16ULL << 20);
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [](uint64_t) {}, &hot_keys,
                             kDefaultRequestWindowMs);
        auto op = requests.admit();
        CasRefCatalog::initializeEmptyForNewPool(op, layout);
        CasRefCatalog::casAdmitEntry(op, layout, 1,
            CatalogEntry{.ns = RootNamespace{"a"}, .state = NsState::Live, .incarnation = UInt128(1)});
        inner->refuseNextWrite(key);
        inner->writes = 0;
        inner->reads = 0;
        inner->on_read = [&]
        {
            if (inner->writes.load() == 0 || query.status->isKilled())
                return;
            reads_at_failed_settlement = inner->reads.load();
            query.kill();
            throw Poco::TimeoutException("FirstWriteFailsBackend: the settling read timed out");
        };
        try
        {
            (void)CasRefCatalog::casUpdate(op, layout, [](const RefCatalog & current) { return current; });
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        writes = inner->writes.load();
        reads = inner->reads.load();
    });
    EXPECT_EQ(code, ErrorCodes::ABORTED) << "the conflict ends the update";
    EXPECT_EQ(writes, 1) << "the refused PUT only";
    EXPECT_GE(reads_at_failed_settlement, 1);
    EXPECT_EQ(reads, reads_at_failed_settlement) << "no read after the failed settlement";
}

/// A catalog erase whose 412 is followed by a settling read that fails after the kill still runs its mandatory
/// resolution read, and sends no second erase.
TEST(CASOwnerStop, KilledQuerysCatalogEraseSendsNoSecondErase)
{
    std::optional<CasRefCatalog::CompletedRemovingDeleteOutcome> outcome;
    int code = 0;
    int writes = 0;
    int reads = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        const Layout layout("p");
        const String key = layout.refCatalogKey();
        auto inner = std::make_shared<FirstWriteFailsBackend>(key);
        inner->failure = nullptr;
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [](uint64_t) {}, nullptr,
                             kDefaultRequestWindowMs);
        auto op = requests.admit();
        const CatalogEntry removing{
            .ns = RootNamespace{"a"}, .state = NsState::Removing, .incarnation = UInt128{7}, .removal_started_round = 13};
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, encodeRefCatalog(RefCatalog{.entries = {removing}}), Retry::once())));
        CasFoldSeal ready_parent;
        ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
            .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
            .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
        CasRefCatalog::Snapshot cut = CasRefCatalog::read(op, layout);

        /// A competitor adds a row between the cut and the erase, so the erase meets a genuine 412.
        auto competitor_requests = openRequestsForTest(inner);
        auto competitor = competitor_requests.admit();
        const CatalogEntry other{.ns = RootNamespace{"b"}, .state = NsState::Live, .incarnation = UInt128{8}};
        const auto seen_by_competitor = competitor.read(key, Retry::once());
        ASSERT_TRUE(seen_by_competitor.has_value());
        ASSERT_TRUE(std::holds_alternative<Committed>(competitor.replace(
            key, encodeRefCatalog(RefCatalog{.entries = {removing, other}}), seen_by_competitor->etag, Retry::once())));
        inner->writes = 0;
        inner->reads = 0;
        bool settlement_failed = false;
        inner->on_read = [&]
        {
            if (inner->writes.load() == 0 || std::exchange(settlement_failed, true))
                return;
            query.kill();
            throw Poco::TimeoutException("FirstWriteFailsBackend: the settling read timed out");
        };
        try
        {
            outcome = CasRefCatalog::deleteCompletedRemovingAtSnapshot(op, layout, std::move(cut), removing, ready_parent, {}, {}, {})
                          .outcome;
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        writes = inner->writes.load();
        reads = inner->reads.load();
    });
    EXPECT_FALSE(outcome.has_value()) << "outcome " << static_cast<int>(*outcome);
    EXPECT_EQ(code, ErrorCodes::ABORTED) << "the conflict ends the erase";
    EXPECT_EQ(writes, 1) << "no second erase";
    EXPECT_EQ(reads, 2) << "the failed settling read and the mandatory resolution read";
}

/// A stopped background owner's unresolved catalog erase is followed by a resolution read that fails locally. The
/// local fault is the call's answer, not the erase's unresolved outcome.
TEST(CASOwnerStop, StoppedCatalogEraseKeepsALocalFaultOfItsResolutionRead)
{
    int code = 0;
    int writes = 0;
    int failed_reads = 0;
    onThread(true, [&]
    {
        const Layout layout("p");
        const String key = layout.refCatalogKey();
        auto inner = std::make_shared<FirstWriteFailsBackend>(key);
        CasRequests requests(inner, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        const CatalogEntry removing{
            .ns = RootNamespace{"a"}, .state = NsState::Removing, .incarnation = UInt128{7}, .removal_started_round = 13};
        const auto failure = std::exchange(inner->failure, nullptr);
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key, encodeRefCatalog(RefCatalog{.entries = {removing}}), Retry::once())));
        inner->failure = failure;
        inner->writes = 0;
        CasFoldSeal ready_parent;
        ready_parent.ref_lives.emplace(UInt128{7}, RefLifeFoldState{
            .coverage = RefCoverage{.classification = CoverageClass::Folded, .last_folded_ref_id = RefTxnId{1, 2}},
            .cleanup_evidence = RefCleanupEvidence{.remove_txn_id = RefTxnId{1, 2}}});
        CasRefCatalog::Snapshot cut = CasRefCatalog::read(op, layout);

        /// The erase's own settling read stops the owner, so the erase ends unresolved without a resend.
        std::atomic<bool> stopped{false};
        int reads_after_erase = 0;
        inner->on_read = [&]
        {
            if (inner->writes.load() == 0)
                return;
            if (++reads_after_erase == 1)
            {
                stopped = true;
                return;
            }
            ++failed_reads;
            throw Exception(ErrorCodes::CORRUPTED_DATA, "the resolution read decoded a malformed catalog");
        };
        auto background = backgroundStoppedBy(stopped);
        try
        {
            (void)CasRefCatalog::deleteCompletedRemovingAtSnapshot(op, layout, std::move(cut), removing, ready_parent, {}, {}, {});
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        writes = inner->writes.load();
    });
    EXPECT_EQ(writes, 1) << "one erase, whose outcome is unresolved";
    EXPECT_EQ(failed_reads, 1) << "only the resolution read fails, after the erase's settling read";
    EXPECT_EQ(code, ErrorCodes::CORRUPTED_DATA) << "the local fault was replaced";
}

/// A transport fault, then a 412 settled by a successful read: the conflict is an answer, so a kill in the pause after
/// it does not stop the next decision, whose write commits on a healthy store.
TEST(CASOwnerStop, SettledConflictAfterAFaultPermitsTheNextDecisionAfterAKill)
{
    std::optional<WriteResult> written;
    int code = 0;
    int decisions = 0;
    int writes = 0;
    int reads = 0;
    int pauses = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [&](uint64_t)
        {
            /// The first pause precedes the resend of the timed-out attempt, the second follows the settled conflict.
            if (++pauses == 2)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto competitor_requests = openRequestsForTest(inner);
        auto competitor = competitor_requests.admit();
        /// The backend fails only the first write it sees; the seed is not it.
        const auto failure = std::exchange(inner->failure, nullptr);
        const auto created = competitor.create(STOPPABLE_KEY, "v0", Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>(created));
        const Etag base = std::get<Committed>(created).etag;
        inner->failure = failure;
        inner->writes = 0;
        inner->reads = 0;
        bool competing = false;
        inner->on_write = [&]
        {
            if (competing || inner->writes.load() != 1)
                return;
            competing = true;
            ASSERT_TRUE(std::holds_alternative<Committed>(competitor.replace(STOPPABLE_KEY, "competitor", base, Retry::once())));
        };
        auto op = requests.admit();
        try
        {
            written = op.readModifyWrite(STOPPABLE_KEY, [&](const std::optional<DB::Cas::Object> & current)
            {
                ++decisions;
                return std::optional<String>(current->bytes + "+");
            }, Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        /// The competitor's write passes through the same backend.
        writes = inner->writes.load() - 1;
        reads = inner->reads.load();
    });
    EXPECT_EQ(code, 0) << "the kill does not stop a decision after a settled conflict";
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(std::holds_alternative<Committed>(*written)) << "index " << written->index();
    EXPECT_EQ(pauses, 2);
    EXPECT_EQ(decisions, 2);
    EXPECT_EQ(writes, 3) << "the timed-out PUT, its refused resend, and the PUT of the next decision";
    EXPECT_EQ(reads, 4) << "the base read, two settling reads and the read after the conflict pause";
}

/// A clean 412 inside a retrying read-modify-write is not settled: the read after the pause replaces it. The first one
/// is retried at once; a query killed during the pause after the second is consulted when the read after that pause
/// fails, before it is retried, and nothing is written after.
TEST(CASOwnerStop, AKillDuringTheConflictPauseIsObservedByTheFailedReadAfterIt)
{
    std::optional<WriteResult> written;
    int code = 0;
    int writes = 0;
    int reads_after_write = 0;
    int pauses = 0;
    int pauses_at_first_read_after_write = -1;
    bool settled_before_pause = false;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
        inner->failure = nullptr;
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [&](uint64_t)
        {
            if (++pauses == 1)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto competitor_requests = openRequestsForTest(inner);
        auto competitor = competitor_requests.admit();
        const auto created = competitor.create(STOPPABLE_KEY, "v0", Retry::once());
        ASSERT_TRUE(std::holds_alternative<Committed>(created));
        const Etag base = std::get<Committed>(created).etag;
        inner->writes = 0;
        bool competing = false;
        int moves = 0;
        Etag competitor_etag = base;
        /// The competitor wins the first two races: the first loss is retried at once, the second one pauses.
        inner->on_write = [&]
        {
            if (competing || moves >= 2)
                return;
            competing = true;
            const auto moved = competitor.replace(STOPPABLE_KEY, "competitor" + std::to_string(moves), competitor_etag, Retry::once());
            ASSERT_TRUE(std::holds_alternative<Committed>(moved));
            competitor_etag = std::get<Committed>(moved).etag;
            ++moves;
            competing = false;
        };
        inner->on_read = [&]
        {
            if (inner->writes.load() - moves >= 2 && pauses == 0)
                settled_before_pause = true;
            if (pauses == 0)
                return;
            if (++reads_after_write == 1)
                pauses_at_first_read_after_write = pauses;
            throw Poco::TimeoutException("FirstWriteFailsBackend: the read timed out");
        };
        auto op = requests.admit();
        try
        {
            written = op.readModifyWrite(STOPPABLE_KEY, [&](const std::optional<DB::Cas::Object> & current)
            {
                return std::optional<String>(current->bytes + "+");
            }, Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        /// The competitor's writes pass through the same backend.
        writes = inner->writes.load() - moves;
    });
    EXPECT_FALSE(written.has_value()) << "index " << (written ? written->index() : 0);
    EXPECT_EQ(code, ErrorCodes::QUERY_WAS_CANCELLED) << "the failed read ends the call with the kill";
    EXPECT_EQ(writes, 2) << "no write after the kill";
    EXPECT_FALSE(settled_before_pause) << "the second 412 was not settled before the pause";
    EXPECT_EQ(pauses_at_first_read_after_write, 1);
    EXPECT_EQ(reads_after_write, 1) << "the stop is consulted after the failed read, which is not retried";
    EXPECT_EQ(pauses, 1);
}

/// A genuine refused precondition stays a conflict when the owner stops while its settling read is in
/// flight: the read is owed, and its failure is "nothing observed", not the owner's verdict.
TEST(CASOwnerStop, GenuineConflictThenStopDuringSettlementIsAConflict)
{
    std::optional<WriteResult> written;
    int code = 0;
    int reads = 0;
    onThread(false, [&]
    {
        auto backend = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
        backend->failure = nullptr;
        CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(STOPPABLE_KEY, "v0", Retry::once())));
        backend->refuseNextWrite(STOPPABLE_KEY);
        backend->reads = 0;

        std::atomic<bool> stopped{false};
        backend->on_read = [&]
        {
            if (backend->writes.load() < 2)
                return;
            stopped = true;
            throw Poco::TimeoutException("FirstWriteFailsBackend: the settling read timed out");
        };
        auto owner = backgroundStoppedBy(stopped);
        try
        {
            written = op.readModifyWrite(STOPPABLE_KEY, [](const std::optional<DB::Cas::Object> & current)
            {
                return std::optional<String>(current->bytes + "+");
            }, Retry::once());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        reads = backend->reads.load();
    });
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(written.has_value());
    const auto * conflict = std::get_if<Conflict>(&*written);
    ASSERT_NE(conflict, nullptr) << "index " << written->index();
    EXPECT_TRUE(std::holds_alternative<NotObserved>(conflict->seen));
    EXPECT_EQ(conflict->attempts_sent, 1u);
    EXPECT_EQ(reads, 2) << "the base read and the failed settling read";
}

/// A `PUT` that succeeds while the owner stops is committed and reaches the caller's bookkeeping.
TEST(CASOwnerStop, SuccessfulPutJustBeforeStopReachesBookkeeping)
{
    for (const StoppedBy owner : {StoppedBy::Background, StoppedBy::Query})
    {
        SCOPED_TRACE(nameOf(owner));
        StoppableWrite arm;
        arm.owner = owner;
        arm.prepare_backend = [](FirstWriteFailsBackend & backend, const std::function<void()> & stop)
        {
            backend.failure = nullptr;
            backend.on_write = stop;
        };
        const auto out = runStoppableWrite(arm);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.result.has_value());
        const auto * committed = std::get_if<Committed>(&*out.result);
        ASSERT_NE(committed, nullptr) << "index " << out.result->index();
        EXPECT_FALSE(committed->resolved_by_read);
        EXPECT_EQ(out.writes, 1);
        EXPECT_EQ(out.reads, 0);
    }
}

/// Owed work runs without the owner's stop: an owed write whose attempt failed while the owner was live
/// is resent after a stop in its pause, as an owed read is reissued.
TEST(CASOwnerStop, OwedWriteResendsAfterAStopInItsPause)
{
    for (const StoppedBy owner : {StoppedBy::Background, StoppedBy::Query})
    {
        SCOPED_TRACE(nameOf(owner));
        StoppableWrite arm;
        arm.owner = owner;
        arm.during_pause = [](const auto & stop) { stop(); };
        arm.owed = true;
        const auto out = runStoppableWrite(arm);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.result.has_value());
        const auto * committed = std::get_if<Committed>(&*out.result);
        ASSERT_NE(committed, nullptr) << "index " << out.result->index();
        EXPECT_EQ(out.writes, 2) << "the resend";
        EXPECT_EQ(out.pauses, 1);
    }
}

/// An attempt that fails after the owner stopped is settled and never resent, inside `owed` too.
TEST(CASOwnerStop, OwedWriteFailedForAStoppedOwnerIsNotResent)
{
    for (const StoppedBy owner : {StoppedBy::Background, StoppedBy::Query})
    {
        SCOPED_TRACE(nameOf(owner));
        StoppableWrite arm;
        arm.owner = owner;
        arm.prepare_backend = [](FirstWriteFailsBackend & backend, const std::function<void()> & stop) { backend.on_first_write = stop; };
        arm.owed = true;
        const auto out = runStoppableWrite(arm);
        expectUnresolved(out);
        EXPECT_EQ(out.writes, 1) << "no resend";
        EXPECT_EQ(out.reads, 1) << "the owed settling read";
        EXPECT_EQ(out.pauses, 0);
    }
}

#if USE_AWS_S3

/// A killed owner whose generation was replaced during the connect-hint pause leaves through the exit's
/// owed settling read, which the original generation refuses: the typed fence outcome and `sent_any` are
/// kept, and nothing is sent under the replacement generation.
TEST(CASOwnerStop, KilledOwnerWhoseGenerationRefusesSettlement)
{
    std::optional<WriteResult> written;
    int code = 0;
    int writes = 0;
    int reads = 0;
    int admits_under_replacement = 0;
    int pauses = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<FirstWriteFailsBackend>(STOPPABLE_KEY);
        inner->failure = std::make_exception_ptr(DB::S3Exception("Connection refused", Aws::S3::S3Errors::NETWORK_CONNECTION));
        auto backend = std::make_shared<CasClientLikeBackend>(inner);

        std::atomic<uint64_t> live_generation{1};
        std::atomic<int> replacement_admits{0};
        Fence fence = Fence::open();
        fence.generation = [&] { return live_generation.load(); };
        fence.admit = [&](uint64_t admitted, uint64_t)
        {
            if (admitted != 1)
                ++replacement_admits;
            return admitted == live_generation.load() ? Fence::Admit::Ok : Fence::Admit::LostOrRearmed;
        };
        std::atomic<int> pause_count{0};
        CasRequests requests(backend, fence, {}, [&](uint64_t)
        {
            ++pause_count;
            live_generation = 2;
            query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        try
        {
            written = op.create(STOPPABLE_KEY, "bytes", Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        writes = inner->writes.load();
        reads = inner->reads.load();
        admits_under_replacement = replacement_admits.load();
        pauses = pause_count.load();
    });
    EXPECT_EQ(code, 0);
    ASSERT_TRUE(written.has_value());
    const auto * gave_up = std::get_if<GaveUp>(&*written);
    ASSERT_NE(gave_up, nullptr) << "index " << written->index();
    EXPECT_EQ(gave_up->why, GaveUp::Why::FenceLost) << "the refused settling read keeps its typed outcome";
    EXPECT_TRUE(gave_up->sent_any);
    EXPECT_EQ(gave_up->attempts_sent, 1u);
    EXPECT_EQ(pauses, 1);
    EXPECT_EQ(writes, 1) << "no resend";
    EXPECT_EQ(reads, 0) << "the fence refused the settling read before it was sent";
    EXPECT_EQ(admits_under_replacement, 0) << "nothing is admitted under the replacement generation";
}

#endif

/// A local fault of a stopped owner's write attempt is not settled as a possibly-sent write: it propagates,
/// and no settling read runs.
TEST(CASOwnerStop, LocalFaultOfAStoppedOwnersWriteIsNotSettled)
{
    struct LocalFault
    {
        const char * name;
        std::exception_ptr error;
    };
    const LocalFault faults[] = {
        {"BAD_ARGUMENTS", std::make_exception_ptr(Exception(ErrorCodes::BAD_ARGUMENTS, "a local bug of the attempt"))},
        {"std::bad_alloc", std::make_exception_ptr(std::bad_alloc())},
    };
    for (const LocalFault & fault : faults)
    {
        SCOPED_TRACE(fault.name);
        StoppableWrite arm;
        arm.owner = StoppedBy::Background;
        arm.prepare_backend = [&fault](FirstWriteFailsBackend & backend, const std::function<void()> & stop)
        {
            backend.failure = fault.error;
            backend.on_first_write = stop;
        };
        const auto out = runStoppableWrite(arm);
        EXPECT_FALSE(out.result.has_value()) << "index " << (out.result ? out.result->index() : 0);
        EXPECT_EQ(out.error, fault.error) << "not the fault itself: " << out.message;
        EXPECT_EQ(out.writes, 1);
        EXPECT_EQ(out.reads, 0) << "a local fault got a settling read";
    }
}

namespace
{

/// Every read and `HEAD` of `key` runs `on_request`, then throws `failure`. The sentinel probe is the default adapter's,
/// over these two.
class LocalFaultReadBackend final : public InMemoryBackend
{
public:
    explicit LocalFaultReadBackend(String key_) : key(std::move(key_)) {}

    using InMemoryBackend::head;

    std::exception_ptr failure;
    std::function<void()> on_request;

    std::optional<Raw> read(const String & k, TransportAccess & access) override
    {
        if (k == key)
            fail();
        return InMemoryBackend::read(k, access);
    }

    std::optional<RawMeta> head(const String & k, TransportAccess & access) override
    {
        if (k == key)
            fail();
        return InMemoryBackend::head(k, access);
    }

private:
    void fail()
    {
        if (on_request)
            on_request();
        std::rethrow_exception(failure);
    }

    const String key;
};

}

/// A local fault of a read, of an observation or of a sentinel probe propagates unchanged when a background owner
/// stopped during the attempt, inside `owed` too: the owner's verdict does not replace it, and an observation does not
/// report it as nothing observed, as the write loop does not settle it.
TEST(CASOwnerStop, LocalFaultOfAStoppedOwnersReadOrProbeIsNotReplaced)
{
    enum class Surface : uint8_t { Read, Observation, Probe };
    struct LocalFault
    {
        const char * name;
        std::function<std::exception_ptr()> error;
    };
    const LocalFault faults[] = {
        {"CORRUPTED_DATA", [] { return std::make_exception_ptr(Exception(ErrorCodes::CORRUPTED_DATA, "a local decode failure")); }},
        {"std::bad_alloc", [] { return std::make_exception_ptr(std::bad_alloc()); }},
    };
    for (const LocalFault & fault : faults)
    {
        for (const Surface surface : {Surface::Read, Surface::Observation, Surface::Probe})
        {
            for (const bool owed : {false, true})
            {
                SCOPED_TRACE(fmt::format("{}, surface {}{}", fault.name, static_cast<int>(surface), owed ? ", owed" : ""));
                onThread(false, [&]
                {
                    auto backend = std::make_shared<LocalFaultReadBackend>("k");
                    std::atomic<bool> stop{false};
                    backend->failure = fault.error();
                    backend->on_request = [&stop] { stop = true; };
                    auto requests = openRequestsForTest(backend);
                    auto op = requests.admit();
                    auto background = backgroundStoppedBy(stop);
                    std::optional<OwnerScope> owed_scope;
                    if (owed)
                        owed_scope.emplace(OwnerScope::owed());
                    std::exception_ptr error;
                    try
                    {
                        if (surface == Surface::Probe)
                            (void)op.probeSentinel("k", Retry::standard());
                        else if (surface == Surface::Observation)
                            (void)op.readModifyWrite("k", [](const std::optional<DB::Cas::Object> &) { return String("v"); },
                                                     Retry::standard());
                        else
                            (void)op.read("k", Retry::standard());
                    }
                    catch (...)
                    {
                        error = std::current_exception();
                    }
                    ASSERT_TRUE(stop.load()) << "the attempt never ran";
                    EXPECT_EQ(error, backend->failure) << "not the fault itself: " << messageOf(error);
                });
            }
        }
    }
}

/// The object-storage adapter's probe, native and emulated, lets a local fault of the storage call through as the same
/// exception; a transport failure stays `Indeterminate`.
TEST(CASOwnerStop, ObjectStorageProbeLetsALocalFaultThrough)
{
    struct Failure
    {
        const char * name;
        std::function<std::exception_ptr()> error;
        bool local;
    };
    const Failure failures[] = {
        {"CORRUPTED_DATA", [] { return std::make_exception_ptr(Exception(ErrorCodes::CORRUPTED_DATA, "a local decode failure")); }, true},
        {"std::bad_alloc", [] { return std::make_exception_ptr(std::bad_alloc()); }, true},
        {"transport timeout", [] { return std::make_exception_ptr(Poco::TimeoutException("the probe timed out")); }, false},
    };
    for (const auto mode : {ObjectStorageBackend::Mode::Native, ObjectStorageBackend::Mode::EmulatedSingleProcess})
    {
        for (const Failure & failure : failures)
        {
            const bool native = mode == ObjectStorageBackend::Mode::Native;
            SCOPED_TRACE(fmt::format("{}, {}", native ? "native" : "emulated", failure.name));
            auto storage = makeLocalObjectStorageOf<HookedReadLocalObjectStorage>();
            const std::exception_ptr error = failure.error();
            (native ? storage->on_read : storage->on_exists) = [error] { std::rethrow_exception(error); };
            auto backend = std::make_shared<ObjectStorageBackend>(storage, mode);
            const String key = native ? DB::Cas::tests::nativeKeyUnder(storage, "sentinel") : String("sentinel");
            std::optional<SentinelProbeResult> probed;
            std::exception_ptr thrown;
            onThread(false, [&]
            {
                CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
                auto op = requests.admit();
                try
                {
                    probed = op.probeSentinel(key, Retry::once());
                }
                catch (...)
                {
                    thrown = std::current_exception();
                }
            });
            if (failure.local)
            {
                EXPECT_FALSE(probed.has_value());
                EXPECT_EQ(thrown, error) << "not the fault itself: " << messageOf(thrown);
            }
            else
            {
                EXPECT_EQ(thrown, nullptr) << messageOf(thrown);
                ASSERT_TRUE(probed.has_value());
                EXPECT_EQ(probed->outcome, ProbeOutcome::Indeterminate);
            }
        }
    }
}

/// A stop that lands after a failed base read's own owner check, before `observe` reports the failure, is
/// reported as the owner's verdict, not as the store's failure.
TEST(CASOwnerStop, StopAfterTheReadsOwnCheckIsReportedByObserve)
{
    onThread(false, [&]
    {
        auto backend = std::make_shared<FailFirstRequestBackend>("k");
        backend->failure = std::make_exception_ptr(Exception(ErrorCodes::NETWORK_ERROR, "the read failed"));
        auto requests = openRequestsForTest(backend);
        auto op = requests.admit();
        int checks_after_failure = 0;
        const auto owner = OwnerScope::background([&]
        {
            if (backend->requests.load() > 0 && checks_after_failure++ > 0)
                throw Exception(ErrorCodes::ABORTED, "background owner stopped");
        });
        int code = 0;
        try
        {
            /// A single-attempt submission reads its base through `observe` with no hint.
            (void)op.hotKeys().submit("k", op, Retry::once(), [](const std::optional<Cas::Object> &) { return std::optional<String>("bytes"); });
        }
        catch (const Exception & e)
        {
            code = e.code();
        }
        EXPECT_EQ(checks_after_failure, 2) << "premise: the read's own check saw no stop, and observe's did";
        EXPECT_EQ(code, ErrorCodes::ABORTED);
    });
}

/// A decision on a warm catalog hint that finds the GC authority lost while the owner is stopped reports the
/// lost authority, as the same decision on a fresh read does.
TEST(CASOwnerStop, StoppedOwnersLostAuthorityOnAWarmHintIsFencedOut)
{
    onThread(false, [&]
    {
        auto backend = std::make_shared<CountingBackend>();
        const Layout layout("p");
        const String key = layout.refCatalogKey();
        const CatalogEntry stalled{
            .ns = RootNamespace{"a"}, .state = NsState::Creating, .incarnation = UInt128(1),
            .creator = CreatorFence{.server_root_id = "srv", .writer_epoch = 1, .fence_generation = 1}};
        /// One plane with a cached lane for the setup and the cancel, so the cancel starts from the hint the setup left.
        CasHotKeys hot_keys(/*cache_budget_bytes=*/1 << 20);
        CasRequests requests(backend, Fence::open(), {}, {}, &hot_keys, kDefaultRequestWindowMs);
        {
            auto setup = requests.admit();
            CasRefCatalog::initializeEmptyForNewPool(setup, layout);
            CasRefCatalog::casAdmitEntry(setup, layout, 1, stalled);
        }
        std::atomic<bool> authority_held = true;
        std::atomic<bool> stopped = false;
        auto op = requests.admit([&authority_held] { return authority_held.load(); });
        const auto owner = backgroundStoppedBy(stopped);
        const uint64_t reads_before = backend->getCount(key);
        std::optional<uint64_t> reads_at_first_decision;

        std::optional<CasRefCatalog::StalledCreatingCancelOutcome> outcome;
        EXPECT_NO_THROW(outcome = CasRefCatalog::cancelStalledCreating(
            op, layout, stalled, [](const CreatorFence &) { return true; },
            [&](const Retry &)
            {
                if (!reads_at_first_decision)
                    reads_at_first_decision = backend->getCount(key);
                authority_held = false;
                stopped = true;
            }));
        ASSERT_TRUE(reads_at_first_decision.has_value());
        EXPECT_EQ(*reads_at_first_decision, reads_before) << "premise: the first decision ran on the warm hint";
        ASSERT_TRUE(outcome.has_value());
        EXPECT_EQ(*outcome, CasRefCatalog::StalledCreatingCancelOutcome::FencedOut);
    });
}

/// A killed query's first attempt is sent on a healthy store; when it fails and the query stops before the reissue,
/// nothing more is sent and the attempt's own error propagates, not the query's verdict.
TEST(CASOwnerStop, KilledQuerysFailedFirstAttemptKeepsItsError)
{
    using enum MutatingVerb;
    for (const MutatingVerb verb : {Remove, BulkRemove, Publish})
    {
        SCOPED_TRACE(fmt::format("verb {}", static_cast<int>(verb)));
        const MutationRun run = runMutatingVerb(verb, std::nullopt, /*fail_first=*/true);
        EXPECT_EQ(run.code, ErrorCodes::S3_ERROR) << "the failed attempt's own error";
        EXPECT_EQ(run.mutations, 1) << "no reissue after the kill";
    }
}

/// A long pause before a reissue is slept in slices, each checking the query: a kill in the third slice ends it there,
/// with the failed read's own error and no further request.
TEST(CASOwnerStop, LongFailurePauseChecksTheQueryEverySlice)
{
    std::vector<uint64_t> slices;
    int code = 0;
    int sent = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<FailFirstRequestBackend>("k");
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [&](uint64_t ms)
        {
            slices.push_back(ms);
            if (slices.size() == 3)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create("k", "v", Retry::once())));
        inner->requests = 0;
        Retry policy = Retry::standard();
        /// A pause of at least 1600 ms: eight slices or more.
        policy.attempt_spacing_ms = 2000;
        try
        {
            (void)op.read("k", policy);
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        sent = inner->requests.load();
    });
    EXPECT_EQ(code, ErrorCodes::TIMEOUT_EXCEEDED) << "the failed read's own error";
    EXPECT_EQ(slices.size(), 3u) << "the pause ends at the slice the kill landed in";
    /// A literal, not the constant: 200 ms is the published bound on how long a pause outlives a stop.
    for (const uint64_t slice : slices)
        EXPECT_LE(slice, 200u);
    EXPECT_EQ(sent, 1) << "no reissue";
}

/// A query's stored cause with the `NOT_IMPLEMENTED` code is no batch-capability refusal: no single-key deletes.
TEST(CASOwnerStop, StoredNotImplementedStartsNoSingleKeyDeletes)
{
    class BulkFailsBackend final : public InMemoryBackend
    {
    public:
        std::atomic<int> bulk{0};
        std::atomic<int> single{0};

        void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
        {
            if (keys.size() > 1)
            {
                ++bulk;
                throw Poco::TimeoutException("BulkFailsBackend: the batch delete timed out");
            }
            ++single;
            InMemoryBackend::removeManyWriteOnce(keys, access);
        }
    };

    int code = 0;
    int bulk = 0;
    int single = 0;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<BulkFailsBackend>();
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [](uint64_t) {}, nullptr,
                             kDefaultRequestWindowMs);
        auto op = requests.admit();
        const Layout layout("p");
        query.kill(storedCause(ErrorCodes::NOT_IMPLEMENTED));
        try
        {
            (void)removeChunkWriteOnceOrOneByOne(
                op, {layout.writeOnceBlobIntentProbeKey("a"), layout.writeOnceBlobIntentProbeKey("b")}, Retry::standard());
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
        bulk = inner->bulk.load();
        single = inner->single.load();
    });
    EXPECT_EQ(code, ErrorCodes::NOT_IMPLEMENTED) << "the query's stored cause propagates";
    EXPECT_EQ(bulk, 1);
    EXPECT_EQ(single, 0) << "no single-key delete after the stored cause";
}

/// A local miss is the store's answer under a stopped background owner too, as an S3 `NoSuchKey` is: a background
/// verdict is a fresh exception, so it never arrives as a miss.
TEST(CASOwnerStop, LocalMissUnderAStoppedBackgroundOwnerIsAbsence)
{
    auto storage = makeLocalObjectStorageOf<DB::LocalObjectStorage>();
    auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
    int code = 0;
    bool read_absent = false;
    onThread(false, [&]
    {
        CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        const auto background = OwnerScope::background([] { throw Exception(ErrorCodes::ABORTED, "background owner stopped"); });
        try
        {
            read_absent = !op.read(DB::Cas::tests::nativeKeyUnder(storage, "absent"), Retry::standard()).has_value();
        }
        catch (...)
        {
            code = codeOf(std::current_exception());
        }
    });
    EXPECT_EQ(code, 0);
    EXPECT_TRUE(read_absent);
}

#if USE_AWS_S3
/// A credential refusal the engine would have retried after a refresh ends as retry-later when the query stops before
/// the retry, carrying the refusal: a raw `AccessDenied` would read as damage to the part loader.
TEST(CASOwnerStop, VetoedCredentialRetryEndsAsRetryLater)
{
    FailedFirstRequest arm;
    arm.failure = std::make_exception_ptr(
        DB::S3Exception("the store answered AccessDenied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
    arm.during_pause = [](CasKillableQuery & q) { q.kill(); };
    const auto out = runFailedFirstRequest(arm);
    EXPECT_EQ(out.code, ErrorCodes::NETWORK_ERROR);
    EXPECT_NE(out.message.find("the store answered AccessDenied"), String::npos)
        << "the refusal is not the cause: " << out.message;
    EXPECT_EQ(out.refreshes, 1) << "the live query's refusal was refreshed";
    EXPECT_EQ(out.requests, 1) << "the retry never starts";
}

namespace
{

/// Writes of "k" throw `failures` in order, then succeed; every refresh installs new credentials.
class ScriptedWriteBackend final : public InMemoryBackend
{
public:
    std::vector<std::exception_ptr> failures;
    std::atomic<int> writes{0};
    std::atomic<int> refreshes{0};

    std::expected<String, RawConflict> write(const String & key, const String & bytes, const std::optional<String> & expected_value,
                                             TransportAccess & access) override
    {
        if (key == "k")
        {
            const size_t n = static_cast<size_t>(writes++);
            if (n < failures.size())
                std::rethrow_exception(failures[n]);
        }
        return InMemoryBackend::write(key, bytes, expected_value, access);
    }

    bool refreshCredentials() override
    {
        ++refreshes;
        return true;
    }
};

struct VetoedWriteOutcome
{
    std::optional<WriteResult> result;
    int code = 0;
    String message;
    int writes = 0;
    int refreshes = 0;
};

/// `create("k")` whose writes fail with `failures`; the query is killed in pause number `kill_in_pause`.
VetoedWriteOutcome runVetoedWrite(std::vector<std::exception_ptr> failures, int kill_in_pause)
{
    VetoedWriteOutcome out;
    onThread(true, [&]
    {
        CasKillableQuery query(getContext().context);
        auto inner = std::make_shared<ScriptedWriteBackend>();
        inner->failures = std::move(failures);
        int pauses = 0;
        CasRequests requests(std::make_shared<CasClientLikeBackend>(inner), Fence::open(), {}, [&](uint64_t)
        {
            if (++pauses == kill_in_pause)
                query.kill();
        }, nullptr, kDefaultRequestWindowMs);
        auto op = requests.admit();
        try
        {
            out.result = op.create("k", "v", Retry::standard());
        }
        catch (...)
        {
            out.code = codeOf(std::current_exception());
            out.message = getCurrentExceptionMessage(/*with_stacktrace=*/false);
        }
        out.writes = inner->writes.load();
        out.refreshes = inner->refreshes.load();
    });
    return out;
}

std::exception_ptr accessDenied()
{
    return std::make_exception_ptr(
        DB::S3Exception("the store answered AccessDenied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
}

}

/// A conditional write whose credential retry the query vetoes ends as retry-later carrying the refusal: the store
/// refused that attempt, so its outcome is known. After an earlier unanswered attempt the write stays unresolved.
TEST(CASOwnerStop, VetoedCredentialRetryOfAWriteEndsAsRetryLater)
{
    {
        SCOPED_TRACE("refused first attempt");
        const auto out = runVetoedWrite({accessDenied()}, /*kill_in_pause=*/1);
        EXPECT_FALSE(out.result.has_value());
        EXPECT_EQ(out.code, ErrorCodes::NETWORK_ERROR) << out.message;
        EXPECT_NE(out.message.find("the store answered AccessDenied"), String::npos) << out.message;
        EXPECT_NE(out.message.find("credential retry"), String::npos) << out.message;
        EXPECT_EQ(out.writes, 1) << "the retry is not sent";
        EXPECT_EQ(out.refreshes, 1);
    }
    {
        SCOPED_TRACE("refused after an unanswered attempt");
        const auto out = runVetoedWrite(
            {std::make_exception_ptr(Poco::TimeoutException("the write timed out")), accessDenied()}, /*kill_in_pause=*/2);
        EXPECT_EQ(out.code, 0) << out.message;
        ASSERT_TRUE(out.result.has_value());
        const auto * gave_up = std::get_if<GaveUp>(&*out.result);
        ASSERT_NE(gave_up, nullptr);
        EXPECT_EQ(gave_up->why, GaveUp::Why::Unresolved);
        EXPECT_TRUE(gave_up->sent_any);
        EXPECT_EQ(out.writes, 2);
    }
}

/// A stored `NoSuchKey` of a stopped query is not absence. A genuine `NoSuchKey` is the store's answer and stays
/// absence when the query is already killed.
TEST(CASOwnerStop, StoredNoSuchKeyIsNotAbsenceAGenuineOneIs)
{
    {
        SCOPED_TRACE("stored NoSuchKey");
        auto storage = makeClientLikeLocalObjectStorage();
        const String key = DB::Cas::tests::nativeKeyUnder(storage, "present");
        {
            auto out = storage->writeObject(DB::StoredObject(key), DB::WriteMode::Rewrite);
            DB::writeString(String("body"), *out);
            out->finalize();
        }
        auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
        int code = 0;
        bool read_absent = false;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            query.kill(std::make_exception_ptr(
                DB::S3Exception("the query's stored cause", Aws::S3::S3Errors::NO_SUCH_KEY, "NoSuchKey")));
            storage->reads = 0;
            storage->fail_next = true;
            try
            {
                read_absent = !op.read(key, Retry::standard()).has_value();
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
        });
        EXPECT_FALSE(read_absent) << "a stored NoSuchKey is never absence";
        EXPECT_EQ(code, ErrorCodes::S3_ERROR);
        EXPECT_EQ(storage->reads.load(), 1);
    }
    {
        SCOPED_TRACE("genuine NoSuchKey after the kill");
        auto storage = makeLocalObjectStorageOf<NoSuchKeyLocalObjectStorage>();
        auto backend = std::make_shared<ObjectStorageBackend>(storage, ObjectStorageBackend::Mode::Native);
        int code = 0;
        bool read_absent = false;
        onThread(true, [&]
        {
            CasKillableQuery query(getContext().context);
            CasRequests requests(backend, Fence::open(), {}, [](uint64_t) {}, nullptr, kDefaultRequestWindowMs);
            auto op = requests.admit();
            query.kill();
            try
            {
                read_absent = !op.read(DB::Cas::tests::nativeKeyUnder(storage, "absent"), Retry::standard()).has_value();
            }
            catch (...)
            {
                code = codeOf(std::current_exception());
            }
        });
        EXPECT_EQ(code, 0);
        EXPECT_TRUE(read_absent) << "the store's own miss is absence";
        EXPECT_EQ(storage->reads.load(), 1);
    }
}
#endif

namespace
{

/// Outlives the positive control's allocation, so the compiler cannot elide it.
[[maybe_unused]] String own_copy_allocation_sink;

}

/// A receiver at its memory limit still gets its own copy of a shared failure, not `MEMORY_LIMIT_EXCEEDED`: copying the
/// failure allocates, and that allocation is not where the limit is reported.
TEST(CASOwnerStop, OwnCopyOfASharedFailureIgnoresTheMemoryLimit)
{
#if defined(SANITIZER)
    GTEST_SKIP() << "a sanitizer build does not charge operator new to the memory tracker";
#else
    /// Only allocations of at least 8 MiB are checked against the 16 MiB limit; the copied text is 32 MiB.
    const String text(32 << 20, 'x');
    const std::exception_ptr shared = std::make_exception_ptr(Exception(ErrorCodes::NETWORK_ERROR, "shared failure {}", text));
    const UInt64 saved_threshold = CurrentMemoryTracker::getMinAllocationSizeBytesToThrow();
    CurrentMemoryTracker::setMinAllocationSizeBytesToThrow(8 << 20);
    SCOPE_EXIT({ CurrentMemoryTracker::setMinAllocationSizeBytesToThrow(saved_threshold); });

    int control_code = 0;
    int code = 0;
    bool own_copy = false;
    std::thread limited([&]
    {
        MemoryTracker limit_tracker(&total_memory_tracker, VariableContext::Process, /*log_peak_memory_usage_in_destructor_=*/false);
        limit_tracker.setHardLimit(16 << 20);
        ThreadStatus thread_status;
        thread_status.memory_tracker.setParent(&limit_tracker);
        thread_status.untracked_memory_limit = 0;
        try
        {
            own_copy_allocation_sink = text;
        }
        catch (const Exception & e)
        {
            control_code = e.code();
        }
        own_copy_allocation_sink.clear();
        own_copy_allocation_sink.shrink_to_fit();
        try
        {
            rethrowOwnCopy(shared);
        }
        catch (const Exception & e)
        {
            code = e.code();
            own_copy = std::current_exception() != shared;
        }
    });
    limited.join();

    ASSERT_EQ(control_code, ErrorCodes::MEMORY_LIMIT_EXCEEDED) << "premise: the limit is enforced at that size";
    EXPECT_EQ(code, ErrorCodes::NETWORK_ERROR);
    EXPECT_TRUE(own_copy);
#endif
}

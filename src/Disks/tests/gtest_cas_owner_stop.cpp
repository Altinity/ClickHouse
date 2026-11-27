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


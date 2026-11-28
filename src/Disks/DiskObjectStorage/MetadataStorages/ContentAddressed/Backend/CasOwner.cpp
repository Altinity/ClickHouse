#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>

#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/LockMemoryExceptionInThread.h>
#include <Common/ThreadStatus.h>
#include <Interpreters/Context.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace DB::ErrorCodes
{
    extern const int CANNOT_SCHEDULE_TASK;
    extern const int LOGICAL_ERROR;
}

namespace DB::Cas
{

thread_local OwnerScope::State OwnerScope::current;

namespace
{

/// The exception `check` throws, or null.
template <typename Check>
std::exception_ptr exceptionOf(Check && check)
{
    try
    {
        check();
    }
    catch (...)
    {
        return std::current_exception();
    }
    return nullptr;
}

}

std::exception_ptr ownerStopRequested()
{
    const auto & owner = OwnerScope::current;
    if (owner.shared)
        return nullptr;
    return exceptionOf([&]
    {
        CurrentThread::checkIfNotCancelled();
        if (owner.background)
            owner.background();
    });
}

bool queryStopRequested()
{
    return !OwnerScope::current.shared && exceptionOf([] { CurrentThread::checkIfNotCancelled(); }) != nullptr;
}

void throwIfBackgroundStopped()
{
    const auto & owner = OwnerScope::current;
    if (!owner.owed && !owner.shared && owner.background)
        owner.background();
}

void throwIfQueryStopped()
{
    const auto & owner = OwnerScope::current;
    if (!owner.owed && !owner.shared)
        CurrentThread::checkIfNotCancelled();
}

bool insideOwed()
{
    return OwnerScope::current.owed;
}

std::exception_ptr backgroundStopRequested()
{
    const auto & owner = OwnerScope::current;
    if (owner.shared || !owner.background)
        return nullptr;
    return exceptionOf(owner.background);
}

namespace
{

/// The throwing check builds a new exception on each call for a query killed without a stored cause, so it runs only
/// after the kill flag that check reads is set.
bool isStoredCauseOfThisThreadsQuery(const std::exception_ptr & e)
{
    if (!e)
        return false;
    const ContextPtr query = CurrentThread::tryGetQueryContext();
    if (!query || !query->isCurrentQueryKilled())
        return false;
    return e == exceptionOf([] { CurrentThread::checkIfNotCancelled(); });
}

}

bool isQueryStoredCause(const std::exception_ptr & e)
{
    return !OwnerScope::current.shared && isStoredCauseOfThisThreadsQuery(e);
}

void addMessageUnlessQueryStoredCause(Exception & e, const std::string & message)
{
    if (!isStoredCauseOfThisThreadsQuery(std::current_exception()))
        e.addMessage(message);
}

void rethrowOwnCopy(const std::exception_ptr & e)
{
    if (!e)
        throw Exception(ErrorCodes::LOGICAL_ERROR, "rethrowOwnCopy: no exception to rethrow");
    if (isStoredCauseOfThisThreadsQuery(e))
        std::rethrow_exception(e);
    try
    {
        std::rethrow_exception(e);
    }
    catch (const Poco::Exception & shared)
    {
        /// The copy allocates; a receiver at its memory limit must still get the shared failure.
        LockMemoryExceptionInThread lock_memory_tracker(VariableContext::Global);
        shared.rethrow();
    }
}

void rethrowIfStoppedOwnersFailure()
{
    if (auto stop = backgroundStopRequested())
        std::rethrow_exception(std::move(stop));
    if (queryStopRequested())
        throw;
}

void throwUnlessInThreadGroup(const ThreadGroupPtr & expected, std::string_view worker)
{
    if (expected && CurrentThread::getGroup() != expected)
        throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK,
            "{} could not join its thread group, so it sends nothing", worker);
}

bool sleepUnlessQueryStops(uint64_t ms, const std::function<void(uint64_t)> & sleep)
{
    if (insideOwed() || !CurrentThread::getGroup())
    {
        sleep(ms);
        return true;
    }
    constexpr auto slice_ms = static_cast<uint64_t>(kOwnerStopSlice.count());
    uint64_t left = ms;
    do
    {
        if (queryStopRequested())
            return false;
        const uint64_t slice = std::min(left, slice_ms);
        sleep(slice);
        left -= slice;
    } while (left > 0);
    return !queryStopRequested();
}

void waitStoppable(std::condition_variable & cv, std::unique_lock<std::mutex> & lock, const std::function<bool()> & ready)
{
    /// Only a background owner is polled; without one in effect a notification is the only way out.
    if (!OwnerScope::effectiveCheck())
    {
        cv.wait(lock, ready);
        return;
    }
    while (true)
    {
        /// The predicate is leaf-locked by contract (`OwnerScope::background`); unlocking keeps the caller's mutex
        /// free while it builds its exception.
        lock.unlock();
        std::exception_ptr verdict = backgroundStopRequested();
        lock.lock();
        if (verdict)
            std::rethrow_exception(std::move(verdict));
        if (ready())
            return;
        cv.wait_for(lock, kOwnerStopSlice);
    }
}

OwnerScope::OwnerScope(State next)
    : saved(std::exchange(current, std::move(next)))
{
}

OwnerScope::OwnerScope(OwnerScope && other) noexcept
    : saved(std::move(other.saved))
    , active(std::exchange(other.active, false))
{
}

OwnerScope::~OwnerScope()
{
    if (active)
        current = std::move(saved);
}

OwnerScope OwnerScope::background(std::function<void()> throw_if_stopped)
{
    if (current.background)
        throw_if_stopped = [outer = current.background, inner = std::move(throw_if_stopped)]
        {
            outer();
            inner();
        };
    return OwnerScope(State{current.owed, current.shared, std::move(throw_if_stopped)});
}

OwnerScope OwnerScope::owed()
{
    return OwnerScope(State{true, current.shared, current.background});
}

OwnerScope OwnerScope::shared()
{
    return OwnerScope(State{current.owed, true, current.background});
}

std::function<void()> OwnerScope::effectiveCheck()
{
    if (current.owed || current.shared)
        return {};
    return current.background;
}

}

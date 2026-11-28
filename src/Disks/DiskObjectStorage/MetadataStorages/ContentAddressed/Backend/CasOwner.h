#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace DB
{
class Exception;
class ThreadGroup;
using ThreadGroupPtr = std::shared_ptr<ThreadGroup>;
}

namespace DB::Cas
{

/// Lets the owner of CAS work stop it: the query, or a background owner. The background owners are the GC scheduler's
/// stop and server shutdown's removal of an `Atomic` table's namespace. Pool teardown is not one: it ends work through
/// the operation's liveness and the detached-work stop token.
///
/// Decision points, where work may end. Inside `owed` or `shared` they do nothing, except that the catch helper
/// still acts inside `owed` and the thread-group guard throws in every scope:
///
/// | Point in the code                                  | Helper                                                  |
/// |----------------------------------------------------|---------------------------------------------------------|
/// | Before more work: a retry, a decision, a next page | `throwIfBackgroundStopped`                              |
/// | Head of a CAS-owned resumable scan                 | `throwIfQueryStopped`, then `throwIfBackgroundStopped`  |
/// | A pause after a failure                            | `sleepUnlessQueryStops`                                 |
/// | A wait on a condition variable                     | `waitStoppable`                                         |
/// | A worker that switched to the submitter's group    | `throwUnlessInThreadGroup`                              |
/// | A catch that would swallow, convert or retry       | `rethrowIfStoppedOwnersFailure`, also inside `owed`     |
///
/// Facts, for code that classifies a failure. They hold inside `owed` too and decide nothing:
///
/// | Question                                           | Helper                                                  |
/// |----------------------------------------------------|---------------------------------------------------------|
/// | Did an owner ask to stop, and with what verdict    | `ownerStopRequested`, `queryStopRequested`, `backgroundStopRequested` |
/// | Is this exception the query's stored cause         | `isQueryStoredCause`                                    |
/// | Annotating or rethrowing a caught exception        | `addMessageUnlessQueryStoredCause`, `rethrowOwnCopy`    |
///
/// A query is consulted only where stopping leaves the state a storage failure there would leave: after a failed
/// attempt (its retry, its pause, a catch that would swallow or convert it) and at a scan head. It does not end a first
/// attempt, a decision after an accepted conflict, a wait, compensation, `abandon`, namespace removal, `FORGET`, or
/// recovery's first walk. The `SYSTEM CAS FSCK` scan has no scan head.
///
/// What a stop ends with:
/// - A stopped owner's exception is not a store answer: neither absence, nor a refusal, nor a local fault. A local
///   fault is surfaced unchanged.
/// - A vetoed retry of a read or a blob publication ends with the failed attempt's exception, whatever its code: a
///   timed-out attempt's `TIMEOUT_EXCEEDED` is that attempt's own. A vetoed retry after a credential refusal ends as
///   a retry-later `NETWORK_ERROR` carrying the refusal, unless the read already refreshed after an earlier failure.
/// - A vetoed resend of a conditional write ends as the retry-later `NETWORK_ERROR` "unresolved", naming what its
///   settling read saw, not the store's error. With no earlier attempt unanswered, a refusal keeps the store's error,
///   except that a vetoed resend with refreshed credentials ends as a retry-later `NETWORK_ERROR` carrying the
///   refusal. A scan head throws the query's verdict.
/// - A killed query's conditional write that loses a race can end as `ABORTED` "conflict": the S3 client replaces the
///   412 with the query's exception, and the settling read shows the other writer's object.
/// - An unresolved sent write keeps its "outcome unknown" exit; a later owner verdict does not overwrite it.
/// - `OwnerScope::owed` removes the checks before work and the query's cut of a pause: the pause runs to its end, and
///   the next attempt is sent unless the fence, the lease or a deadline refuses it after the pause. A failure after a
///   stop is still not retried, and a background owner's verdict still replaces it. `OwnerScope::shared` removes the
///   owner. Neither hides a query from the S3 client, which replaces a failed request's answer with the query's
///   verdict; a bulk delete that succeeds with an error for one key keeps that key's error.
/// - Code above the engine may still do follow-up work its own counts bound, such as another recovery walk. With a
///   lease loss or a credential refusal at the same time, the error can be the less specific of the two.

/// How late a waiter that nobody notifies notices its owner's stop.
inline constexpr std::chrono::milliseconds kOwnerStopSlice{200};

/// The verdict of the owner of this thread's work if it asked to stop: the query's
/// (`CurrentThread::checkIfNotCancelled`), then the background owners' in scope. Null inside `shared`.
std::exception_ptr ownerStopRequested();

/// True when the thread's query, not a background owner, asked to stop. False inside `shared`.
/// The S3 client throws the query's verdict in place of a failed request's answer, but not of one key's error in a
/// bulk delete that succeeded, and it cannot see a background owner.
bool queryStopRequested();

/// Rethrows the verdict of the background owners in scope, except inside `owed` or `shared`. Ignores the query.
void throwIfBackgroundStopped();

/// Rethrows the query's verdict, except inside `owed` or `shared`. Ignores background owners. For a scan head only.
void throwIfQueryStopped();

/// True inside `owed`: owner checks do not throw, and `observe` reports a stopped owner's failed read as not observed.
bool insideOwed();

/// The background owners' verdict, also inside `owed`; ignores the query. Null inside `shared`.
std::exception_ptr backgroundStopRequested();

/// True when `e` is the query's stored cancellation cause, the one verdict rethrown as the same object to every thread
/// the query stops; false inside `shared`. A kill or timeout without a stored cause and every background owner's verdict
/// are built anew on each check, so they never match. Such a cause is neither a store answer nor a local fault,
/// whatever its code or type.
bool isQueryStoredCause(const std::exception_ptr & e);

/// Adds `message` to `e`, the exception being handled, unless `e` is the query's stored cause, whatever the scope:
/// several threads may hold that one object, so annotating it would race.
void addMessageUnlessQueryStoredCause(Exception & e, const std::string & message);

/// Rethrows `e`, which several threads may hold, as this thread's own copy made by `Poco::Exception::rethrow`, so a
/// caller that annotates what it catches races with nobody. The query's stored cause and an exception that is not a
/// `Poco::Exception` are rethrown as they are. A memory limit does not replace the copy.
[[noreturn]] void rethrowOwnCopy(const std::exception_ptr & e);

/// For a catch about to swallow, convert or retry the exception being handled: rethrows the background owners'
/// verdict, or the exception being handled when the query stopped. A killed query keeps the error of its own failure.
void rethrowIfStoppedOwnersFailure();

/// For work that switched to `expected`, such as a pool worker joining the group that submitted it: throws
/// `CANNOT_SCHEDULE_TASK` unless the thread is in it; no check when it is null. `ThreadGroupSwitcher` swallows its own
/// failure, and a request sent outside the intended group sees the wrong query.
void throwUnlessInThreadGroup(const ThreadGroupPtr & expected, std::string_view worker);

/// Sleeps `ms` through `sleep`. Outside `owed`, on a thread with a thread group, sleeps in `kOwnerStopSlice` slices and
/// checks the query before each slice and after the last; false when it stopped, leaving the rest unslept.
bool sleepUnlessQueryStops(uint64_t ms, const std::function<void(uint64_t)> & sleep);

/// Waits until `ready` holds, throwing as `throwIfBackgroundStopped` does. Checks the owners with `lock` released
/// before each evaluation of `ready`; with no background owner in effect it is a plain `cv.wait`. Returns and throws
/// with `lock` held.
void waitStoppable(std::condition_variable & cv, std::unique_lock<std::mutex> & lock, const std::function<bool()> & ready);

/// Thread-scoped like query cancellation, so no owner parameter crosses the request engine. Scopes nest;
/// a nested scope never lifts an enclosing `owed` or `shared`. Destruction restores the enclosing scope.
class OwnerScope
{
public:
    /// An owner with no thread group predicate; `throw_if_stopped` throws its verdict. It runs under CAS
    /// mutexes, so it is lock-free or takes a leaf lock. Enclosing background owners stay checked first.
    static OwnerScope background(std::function<void()> throw_if_stopped);
    /// Work owed to a possibly-sent write, a failed commit or a claimed resource: no throwing check.
    static OwnerScope owed();
    /// Work for several callers: CAS owner checks see no owner. The S3 client still replaces a failed answer with the
    /// exception of the thread's query.
    static OwnerScope shared();

    /// This thread's background owner check, to keep calling inside a later `shared` scope. Valid only while the
    /// scope that installed it is alive. Empty without a background owner and inside `owed` or `shared`.
    static std::function<void()> effectiveCheck();

    OwnerScope(OwnerScope && other) noexcept;
    ~OwnerScope();

private:
    struct State
    {
        bool owed = false;
        bool shared = false;
        std::function<void()> background;
    };

    explicit OwnerScope(State next);

    friend std::exception_ptr ownerStopRequested();
    friend bool queryStopRequested();
    friend bool insideOwed();
    friend void throwIfBackgroundStopped();
    friend void throwIfQueryStopped();
    friend std::exception_ptr backgroundStopRequested();
    friend bool isQueryStoredCause(const std::exception_ptr & e);
    friend std::function<void()> uploadStopCheck();
    static thread_local State current;

    State saved;
    bool active = true;
};

/// The check an upload of a new build runs while it sends, built on the publishing thread; the callable may run on any
/// thread. Empty inside `owed` or `shared` and on a thread with no thread group. Otherwise it runs the group's throwing
/// cancellation predicate, which never throws for a background group such as a merge or a mutation. It holds the thread
/// group, so it may outlive the scope that built it. Background owners are not part of it.
std::function<void()> uploadStopCheck();

/// Carries the stop check of the build being published from the publishing thread to the code that builds each
/// body's throttler. Scopes nest; destruction restores the enclosing value.
class UploadStopScope
{
public:
    explicit UploadStopScope(std::function<void()> stop);
    ~UploadStopScope();

    UploadStopScope(const UploadStopScope &) = delete;
    UploadStopScope & operator=(const UploadStopScope &) = delete;

    /// Empty outside a scope.
    static const std::function<void()> & current();

private:
    std::function<void()> saved;
};

}

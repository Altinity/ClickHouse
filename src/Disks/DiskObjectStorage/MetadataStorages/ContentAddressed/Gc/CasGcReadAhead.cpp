#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcReadAhead.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasOwner.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>

#include <Common/CurrentThread.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/ThreadGroupSwitcher.h>
#include <base/defines.h>
#include <base/scope_guard.h>

#include <chrono>

namespace ProfileEvents
{
    extern const Event CASGCReadAheadHit;
    extern const Event CASGCReadAheadMiss;
    extern const Event CASGCReadAheadWasted;
}

namespace DB::Cas
{

namespace
{

/// Runs `fn` in `group` and leaves it on return or unwind, before the caller publishes the answer: the round may end its
/// query once it takes the answer.
template <typename Fn>
auto runInGroup(const ThreadGroupPtr & group, Fn && fn)
{
    ThreadGroupSwitcher switcher(group, ThreadName::UNKNOWN);
    throwUnlessInThreadGroup(group, "CAS GC read-ahead worker");
    return fn();
}

}

GcReadAhead::GcReadAhead(CasOperation & op_, CasRequests & requests_, ThreadPool & pool_, size_t concurrency_)
    : op(op_), requests(requests_), pool(pool_), concurrency(concurrency_), generation(op_.generation())
{
}

GcReadAhead::~GcReadAhead()
{
    /// A worker holds its own slot and touches `requests`, which outlives this object; waiting here is
    /// what keeps every worker inside the round that issued it. `wait`, not `get`: an exception nobody
    /// took is dropped with the result it belongs to, and a destructor may not throw.
    for (const auto & [id, create] : creates)
        create.wait();
    size_t wasted = 0;
    for (auto & [key, slot] : reads)
    {
        slot->future.wait();
        ++wasted;
    }
    for (auto & [key, slot] : heads)
    {
        slot->future.wait();
        ++wasted;
    }
    if (wasted != 0)
        ProfileEvents::increment(ProfileEvents::CASGCReadAheadWasted, wasted);
}

template <typename T, typename Request>
void GcReadAhead::hint(Slots<T> & slots, const String & key, Request request, ReadFamily family)
{
    if (concurrency <= 1 || slots.contains(key))
        return;

    auto slot = std::make_shared<Slot<T>>();
    slots.emplace(key, slot);
    try
    {
        /// The worker joins a manual round's query, so the query's stop vetoes the worker's retry of a failed request.
        /// A scheduled round has no query group, and the switch does nothing.
        pool.scheduleOrThrowOnError([slot, key, requests_ptr = &requests, gen = generation, request,
                                     group = CurrentThread::getGroup()]
        {
            try
            {
                slot->result = runInGroup(group, [&]
                {
                    CasOperation worker = requests_ptr->resume(gen);
                    return request(worker, key);
                });
            }
            catch (...)
            {
                slot->promise.set_exception(std::current_exception());
                return;
            }
            slot->promise.set_value();
        });
    }
    catch (...)
    {
        /// Nothing will ever satisfy this slot's promise, so a later take would wait on it forever.
        /// Drop it and let the take read inline; the scheduling failure itself propagates to the
        /// hinting site, which is a round-thread site like any other.
        slots.erase(key);
        throw;
    }
    if (accounts(family))
        ++stats[static_cast<size_t>(family)].issued;
}

template <typename T, typename Inline>
std::optional<T> GcReadAhead::take(Slots<T> & slots, const String & key, Inline inline_request, ReadFamily family)
{
    const bool tracked = accounts(family);
    UInt64 wait_from_ns = 0;
    if (tracked)
    {
        wait_from_ns = clock_ns();
        closeApplyAt(wait_from_ns);
    }
    ReadFamilyStats * family_stats = tracked ? &stats[static_cast<size_t>(family)] : nullptr;

    std::shared_ptr<Slot<T>> slot;
    if (const auto it = slots.find(key); it != slots.end())
    {
        slot = std::move(it->second);
        slots.erase(it);
        ProfileEvents::increment(ProfileEvents::CASGCReadAheadHit);
    }
    else
    {
        ProfileEvents::increment(ProfileEvents::CASGCReadAheadMiss);
    }

    if (!tracked)
    {
        /// Rethrows the worker's exception at the site that would otherwise have read inline, so a
        /// transport failure fails the round from the same place and with the same type it always did.
        if (!slot)
            return inline_request(op, key);
        slot->future.get();
        return std::move(slot->result);
    }

    ++family_stats->takes;
    if (!slot)
    {
        ++family_stats->issued;
        ++family_stats->awaited;
    }
    else
    {
        if (slot->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            ++family_stats->awaited;
    }

    std::optional<T> result;
    UInt64 returned_ns = 0;
    {
        /// Adds the wait on every exit, so a take that throws still reports the time it spent.
        SCOPE_EXIT({
            returned_ns = clock_ns();
            family_stats->wait_ns += returned_ns - wait_from_ns;
        });
        if (slot)
        {
            slot->future.get();
            result = std::move(slot->result);
        }
        else
        {
            result = inline_request(op, key);
        }
    }
    apply_family = family;
    apply_since_ns = returned_ns;
    return result;
}

void GcReadAhead::hintRead(const String & key, ReadFamily family)
{
    hint<Object>(reads, key,
        [](CasOperation & worker, const String & k) { return worker.read(k, Retry::standard()); }, family);
}

void GcReadAhead::hintHead(const String & key)
{
    hint<Meta>(heads, key,
        [](CasOperation & worker, const String & k) { return worker.head(k, Retry::standard()); }, ReadFamily::Other);
}

template <typename T>
void GcReadAhead::discard(Slots<T> & slots, const String & key)
{
    const auto it = slots.find(key);
    if (it == slots.end())
        return;
    std::shared_ptr<Slot<T>> slot = std::move(it->second);
    slots.erase(it);
    /// `wait`, not `get`: the result and any exception belong to a request nobody wanted.
    slot->future.wait();
    ProfileEvents::increment(ProfileEvents::CASGCReadAheadWasted);
}

void GcReadAhead::discardRead(const String & key)
{
    discard<Object>(reads, key);
}

void GcReadAhead::discardHead(const String & key)
{
    discard<Meta>(heads, key);
}

std::function<IntentCreateResult()> GcReadAhead::startCreate(std::function<IntentCreateResult(CasOperation &)> request)
{
    if (concurrency <= 1)
    {
        IntentCreateResult answer = request(op);
        return [result = std::move(answer)] { return result; };
    }

    auto promise = std::make_shared<std::promise<IntentCreateResult>>();
    auto job = [promise, fn = std::move(request), requests_ptr = &requests, gen = generation, failure = create_failure,
                group = CurrentThread::getGroup()]
    {
        if (failure->failed.load())
        {
            std::lock_guard lock(failure->mutex);
            promise->set_exception(failure->first);
            return;
        }
        IntentCreateResult answer;
        try
        {
            answer = runInGroup(group, [&]
            {
                CasOperation worker = requests_ptr->resume(gen);
                return fn(worker);
            });
        }
        catch (...)
        {
            const auto error = std::current_exception();
            {
                std::lock_guard lock(failure->mutex);
                if (!failure->first)
                    failure->first = error;
            }
            failure->failed.store(true);
            promise->set_exception(error);
            return;
        }
        promise->set_value(std::move(answer));
    };
    const uint64_t id = next_create++;
    /// Registered before the worker exists, so the destructor waits for every worker that was scheduled.
    creates.emplace(id, promise->get_future());
    try
    {
        pool.scheduleOrThrowOnError(std::move(job));
    }
    catch (...)
    {
        creates.erase(id);
        throw;
    }
    /// Taking the answer releases the create's state, so a fold holds only the creates still outstanding.
    return [this, id]
    {
        auto node = creates.extract(id);
        chassert(!node.empty());
        return node.mapped().get();
    };
}

std::optional<Object> GcReadAhead::takeRead(const String & key, ReadFamily family)
{
    return take<Object>(reads, key,
        [](CasOperation & inline_op, const String & k) { return inline_op.read(k, Retry::standard()); }, family);
}

std::optional<Meta> GcReadAhead::takeHead(const String & key)
{
    return take<Meta>(heads, key,
        [](CasOperation & inline_op, const String & k) { return inline_op.head(k, Retry::standard()); }, ReadFamily::Other);
}

const Object * GcReadAhead::peekRead(const String & key) const
{
    const auto it = reads.find(key);
    if (it == reads.end())
        return nullptr;
    const Slot<Object> & slot = *it->second;
    if (slot.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready || !slot.result)
        return nullptr;
    return &*slot.result;
}

UInt64 steadyNowNs() noexcept
{
    return static_cast<UInt64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void GcReadAhead::enableFamilyAccounting(UInt64 (*clock)() noexcept)
{
    clock_ns = clock;
}

void GcReadAhead::closeApply()
{
    if (clock_ns && apply_family)
        closeApplyAt(clock_ns());
}

void GcReadAhead::closeApplyAt(UInt64 now_ns)
{
    if (!apply_family)
        return;
    stats[static_cast<size_t>(*apply_family)].apply_ns += now_ns - apply_since_ns;
    apply_family.reset();
}

}

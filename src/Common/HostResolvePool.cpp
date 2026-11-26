#include <Common/HostResolvePool.h>

#include <Common/DNSResolver.h>
#include <Common/NetException.h>
#include <Common/SharedLockGuard.h>
#include <Common/scope_guard_safe.h>
#include <Common/thread_local_rng.h>
#include <Common/MemoryTrackerSwitcher.h>

#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <Poco/Timespan.h>


namespace ProfileEvents
{
    extern const Event AddressesDiscovered;
    extern const Event AddressesExpired;
    extern const Event AddressesMarkedAsFailed;
    extern const Event AddressesLatencyReported;
}

namespace CurrentMetrics
{
    extern const Metric AddressesActive;
    extern const Metric AddressesBanned;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int DNS_ERROR;
}

std::atomic<bool> HostResolver::latency_aware_selection{false};

void HostResolver::setLatencyAwareSelection(bool enabled)
{
    latency_aware_selection.store(enabled, std::memory_order_relaxed);
}

bool HostResolver::latencyAwareSelectionEnabled()
{
    return latency_aware_selection.load(std::memory_order_relaxed);
}

HostResolverMetrics HostResolver::getMetrics()
{
    return HostResolverMetrics{
        .discovered = ProfileEvents::AddressesDiscovered,
        .expired = ProfileEvents::AddressesExpired,
        .failed = ProfileEvents::AddressesMarkedAsFailed,
        .active_count = CurrentMetrics::AddressesActive,
        .banned_count = CurrentMetrics::AddressesBanned,
    };
}

HostResolver::WeakPtr HostResolver::getWeakFromThis()
{
    return weak_from_this();
}

HostResolver::HostResolver(String host_, Poco::Timespan history_)
    : HostResolver(
        [](const String & host_to_resolve) { return DNSResolver::instance().resolveHostAllInOriginOrder(host_to_resolve); },
        host_,
        history_)
{}

HostResolver::HostResolver(
    ResolveFunction && resolve_function_, String host_, Poco::Timespan history_)
    : host(std::move(host_))
    , history(history_)
    , resolve_interval(history_.totalMicroseconds() / 3)
    , resolve_function(std::move(resolve_function_))
{
    update();
}

HostResolver::~HostResolver()
{
    /// The records (and the `IPAddress` implementations in them) and the scratch buffer were
    /// allocated under `total_memory_tracker` (see `update`), while this destructor runs in whatever
    /// thread drops the last reference, under that thread's query tracker. Release them under the
    /// tracker they were charged to, and explicitly: members are destroyed only after the body,
    /// when the switcher is already gone.
    MemoryTrackerSwitcher switcher{&total_memory_tracker};

    std::lock_guard lock(mutex);

    auto banned_count = 0;
    for (const auto & rec: records)
        banned_count += rec.failed;
    CurrentMetrics::sub(metrics.banned_count, banned_count);

    CurrentMetrics::sub(metrics.active_count, records.size());
    Records().swap(records);
    std::vector<double>().swap(measured_latencies_scratch);
}

void HostResolver::Entry::setFail()
{
    skip_success_callback = true;

    if (auto lock = pool.lock())
        lock->setFail(address);
}

void HostResolver::Entry::setUnused()
{
    skip_success_callback = true;
}

HostResolver::Entry::~Entry()
{
    if (!skip_success_callback)
    {
        if (auto lock = pool.lock())
            lock->setSuccess(address);
    }
}

void HostResolver::update()
{
    MemoryTrackerSwitcher switcher{&total_memory_tracker};

    auto next_gen = resolve_function(host);
    if (next_gen.empty())
        throw NetException(ErrorCodes::DNS_ERROR, "no endpoints resolved for host {}", host);

    std::sort(next_gen.begin(), next_gen.end());

    Poco::Timestamp now;

    std::lock_guard lock(mutex);
    updateImpl(now, next_gen);
}

void HostResolver::reset()
{
    /// `clear` destroys the `IPAddress` implementations allocated under `total_memory_tracker`.
    MemoryTrackerSwitcher switcher{&total_memory_tracker};

    std::lock_guard lock(mutex);

    CurrentMetrics::sub(metrics.active_count, records.size());
    records.clear();
    weights_dirty = false;
    has_measured_latency.store(false, std::memory_order_relaxed);
}

void HostResolver::updateWeights()
{
    updateWeightsImpl();

    if (getTotalWeight() == 0 && !records.empty())
    {
        CurrentMetrics::sub(metrics.banned_count, records.size());
        for (auto & rec : records)
            rec.failed = false;

        updateWeightsImpl();
    }

    chassert((getTotalWeight() > 0 && !records.empty()) || records.empty());
}

HostResolver::Entry HostResolver::resolve()
{
    if (isUpdateNeeded())
        update();

    std::lock_guard lock(mutex);
    return Entry(*this, selectBest());
}

void HostResolver::setSuccess(const Poco::Net::IPAddress & address)
{
    std::lock_guard lock(mutex);

    auto it = find(address);
    if (it == records.end())
        return;

    auto old_weight = it->getWeight();
    if (it->setSuccess())
        CurrentMetrics::sub(metrics.banned_count);
    auto new_weight = it->getWeight();

    if (old_weight != new_weight)
        updateWeights();
}

void HostResolver::reportLatency(const Poco::Net::IPAddress & address, UInt64 microseconds)
{
    if (microseconds == 0)
        return;

    std::lock_guard lock(mutex);

    auto it = find(address);
    if (it == records.end())
        return;

    it->setLatency(microseconds);
    has_measured_latency.store(true, std::memory_order_relaxed);
    ProfileEvents::increment(ProfileEvents::AddressesLatencyReported);

    /// Defer the O(R) median + prefix-sum recomputation to `selectBest`: pooled requests report
    /// latency at the full S3 read rate while `resolve` runs only when opening a new connection,
    /// and deferring also keeps `reportLatency` allocation-free and keeps it from running `updateWeights`
    /// (whose all-banned fallback would un-ban addresses during `setFail`'s DNS refresh window).
    if (latency_aware_selection.load(std::memory_order_relaxed))
        weights_dirty = true;
}

HostResolver::LatencySnapshot::AddressKey HostResolver::LatencySnapshot::AddressKey::from(const Poco::Net::IPAddress & address)
{
    AddressKey key;
    const size_t address_length = address.length();
    chassert(address_length <= MAX_ADDRESS_LENGTH);
    key.length = static_cast<UInt8>(address_length);
    key.scope = address.scope();
    std::memcpy(key.bytes.data(), address.addr(), address_length);
    return key;
}

bool HostResolver::LatencySnapshot::AddressKey::operator<(const AddressKey & other) const
{
    /// The same order as `Poco::Net::IPAddress::operator<`.
    if (length != other.length)
        return length < other.length;
    if (scope != other.scope)
        return scope < other.scope;
    return std::memcmp(bytes.data(), other.bytes.data(), length) < 0;
}

bool HostResolver::LatencySnapshot::AddressKey::operator==(const AddressKey & other) const
{
    return length == other.length && scope == other.scope && std::memcmp(bytes.data(), other.bytes.data(), length) == 0;
}

double HostResolver::LatencySnapshot::lookupLatencyMs(const Poco::Net::IPAddress & address) const
{
    const auto key = AddressKey::from(address);
    auto it = std::lower_bound(
        entries.begin(),
        entries.end(),
        key,
        [](const auto & entry, const AddressKey & value) { return entry.first < value; });
    if (it != entries.end() && it->first == key)
        return it->second;
    return 0;
}

HostResolver::LatencySnapshot HostResolver::getLatencySnapshot()
{
    LatencySnapshot snapshot;
    if (!has_measured_latency.load(std::memory_order_relaxed))
        return snapshot;

    std::lock_guard lock(mutex);

    snapshot.entries.reserve(records.size());
    for (const auto & rec : records)
    {
        if (rec.latency_ms_ewma <= 0)
            continue;
        snapshot.entries.emplace_back(LatencySnapshot::AddressKey::from(rec.address), rec.latency_ms_ewma);
        if (!rec.failed && (snapshot.min_non_failed_ms == 0 || rec.latency_ms_ewma < snapshot.min_non_failed_ms))
            snapshot.min_non_failed_ms = rec.latency_ms_ewma;
    }
    return snapshot;
}

void HostResolver::setFail(const Poco::Net::IPAddress & address)
{
    Poco::Timestamp now;

    {
        std::lock_guard lock(mutex);

        auto it = find(address);
        if (it == records.end())
            return;

        if (it->setFail(now))
        {
            CurrentMetrics::add(metrics.banned_count);

            /// The address just transitioned to failed, so its weight is now zero.
            /// Recompute `weight_prefix_sum` right here, under the lock, before the
            /// DNS refresh in `update` below - which can throw (e.g. NXDOMAIN or an
            /// empty result) and never reach `updateImpl`/`updateWeights`. Without
            /// this, the failed address would keep its stale weight in the selection
            /// table and `selectBest` could keep handing it out even though it is
            /// known-bad.
            ///
            /// Use `updateWeightsImpl` (the plain prefix-sum recomputation), not
            /// `updateWeights`: the latter also runs the "all addresses banned"
            /// fallback that un-bans every record once the total weight reaches zero.
            /// Applying that fallback here - before the refresh - would un-ban the
            /// address we just failed, so a subsequent `update` that discovers a new
            /// healthy address would still keep the bad one selectable, losing the
            /// pessimization. The fallback is applied below only if the refresh fails.
            updateWeightsImpl();
        }
    }

    ProfileEvents::increment(metrics.failed);

    try
    {
        update();
    }
    catch (...)
    {
        /// The refresh failed, so no new address became available. If every cached
        /// address is now banned the selection table has zero total weight, which
        /// `selectBest` cannot handle. Apply the "all addresses banned" fallback now
        /// to keep at least one address selectable, then propagate the error.
        std::lock_guard lock(mutex);
        updateWeights();
        throw;
    }
}

const Poco::Net::IPAddress & HostResolver::selectBest()
{
    chassert(!records.empty());

    /// Apply any latency samples accumulated since the last weight recomputation. Uses
    /// `updateWeightsImpl` (not `updateWeights`) so a concurrent `resolve` during `setFail`'s
    /// DNS refresh window leaves `total_weight == 0` intact for the uniform fallback below
    /// rather than prematurely clearing bans.
    if (weights_dirty || weights_latency_aware != latency_aware_selection.load(std::memory_order_relaxed))
        updateWeightsImpl();

    /// Every address can be banned at once, giving a zero total weight. This happens
    /// transiently and without holding the lock continuously: `setFail` zeroes the
    /// just-failed address's weight via `updateWeightsImpl` under the lock and then
    /// releases it to run the DNS refresh in `update`, which only re-adds weight
    /// afterwards (or the "all addresses banned" fallback in `updateWeights` does).
    /// A concurrent `resolve` can therefore observe a zero total weight here. Hand out
    /// a uniformly chosen address in that case instead of computing
    /// `getTotalWeight() - 1`, which would underflow to `SIZE_MAX` and make
    /// `partition_point` return `records.end()` - tripping the assertion below in debug
    /// builds and dereferencing the end iterator in release builds.
    const size_t total_weight = getTotalWeight();
    if (total_weight == 0)
        return records[std::uniform_int_distribution<size_t>(0, records.size() - 1)(thread_local_rng)].address;

    auto random_weight_picker = std::uniform_int_distribution<size_t>(0, total_weight - 1);
    size_t weight = random_weight_picker(thread_local_rng);
    auto it = std::partition_point(records.begin(), records.end(), [&](const Record & rec) { return rec.weight_prefix_sum <= weight; });
    chassert(it != records.end());
    return it->address;
}

HostResolver::Records::iterator HostResolver::find(const Poco::Net::IPAddress & addr) TSA_REQUIRES(mutex)
{
    auto it = std::lower_bound(
        records.begin(), records.end(), addr, [](const Record & rec, const Poco::Net::IPAddress & value) { return rec.address < value; });

    if (it != records.end() && it->address != addr)
        return records.end();

    return it;
}

bool HostResolver::isUpdateNeeded()
{
    Poco::Timestamp now;

    std::lock_guard lock(mutex);
    return last_resolve_time + resolve_interval < now || records.empty();
}

void HostResolver::updateImpl(Poco::Timestamp now, std::vector<Poco::Net::IPAddress> & next_gen)
    TSA_REQUIRES(mutex)
{
    const auto last_effective_resolve = now - history;

    /// Everything that can throw runs before the first change of shared state: the allocations of
    /// the scratch buffer and of the merged records (including the `IPAddress` copies), then the
    /// swap, which does not throw. The metrics are applied at the very end from local counters, so
    /// a failure leaves `records`, `last_resolve_time` and the metrics as they were.
    /// The scratch buffer is reserved first so `capacity() >= records.size()` holds on every exit
    /// path (`merged` never has more records than the old and the new generation together).
    measured_latencies_scratch.reserve(records.size() + next_gen.size());

    Records merged;
    merged.reserve(records.size() + next_gen.size());

    size_t expired_count = 0;
    size_t discovered_count = 0;
    size_t banned_dropped_count = 0;

    auto it_before = records.begin();
    auto it_next = next_gen.begin();

    while (it_before != records.end() || it_next != next_gen.end())
    {
        if (it_next == next_gen.end() || (it_before != records.end() && it_before->address < *it_next))
        {
            if (it_before->resolve_time >= last_effective_resolve)
                merged.push_back(*it_before);
            else
            {
                ++expired_count;
                if (it_before->failed)
                    ++banned_dropped_count;
            }
            ++it_before;
        }
        else if (it_before == records.end() || (it_next != next_gen.end() && *it_next < it_before->address))
        {
            /// there are could be duplicates in next_gen vector
            if (merged.empty() || merged.back().address != *it_next)
            {
                ++discovered_count;
                merged.push_back(Record(*it_next, now));
            }
            ++it_next;
        }
        else
        {
            merged.push_back(*it_before);
            merged.back().resolve_time = now;

            ++it_before;
            ++it_next;
        }
    }

    for (auto & record : merged)
    {
        if (!record.failed || !record.consecutive_fail_count)
            continue;

        /// Exponential increased time for each consecutive fail
        auto banned_until = now - Poco::Timespan(history.totalMicroseconds() * (1ull << (record.consecutive_fail_count - 1)));
        if (record.fail_time < banned_until)
        {
            record.failed = false;
            ++banned_dropped_count;
        }
    }

    chassert(std::is_sorted(merged.begin(), merged.end()));
    // check that merged contains unuque elements
    chassert(std::adjacent_find(merged.begin(), merged.end()) == merged.end());

    records.swap(merged);
    last_resolve_time = now;

    CurrentMetrics::add(metrics.active_count, static_cast<Int64>(discovered_count) - static_cast<Int64>(expired_count));
    CurrentMetrics::sub(metrics.banned_count, static_cast<Int64>(banned_dropped_count));
    ProfileEvents::increment(metrics.expired, expired_count);
    ProfileEvents::increment(metrics.discovered, discovered_count);

    bool any_measured = false;
    for (const auto & rec : records)
    {
        if (rec.latency_ms_ewma > 0)
        {
            any_measured = true;
            break;
        }
    }
    has_measured_latency.store(any_measured, std::memory_order_relaxed);

    if (records.empty())
        throw NetException(ErrorCodes::DNS_ERROR, "no endpoints resolved for host {}", host);

    updateWeights();
}


size_t HostResolver::getTotalWeight() const noexcept
{
    if (records.empty())
        return 0;
    return records.back().weight_prefix_sum;
}


void HostResolver::updateWeightsImpl()
{
    weights_dirty = false;

    /// When latency-aware selection is on, compute the median latency (TTFB EWMA) across the
    /// addresses that have a measurement, and bias each address's base weight by how its own
    /// latency compares to that median. The bias is clamped and floored so that no address is
    /// starved: even a slow one keeps a weight of at least 1 and is still probed, which keeps
    /// its EWMA fresh and lets it recover if it speeds up again.
    double median_latency_ms = 0;
    const bool latency_aware = latency_aware_selection.load(std::memory_order_relaxed);
    weights_latency_aware = latency_aware;
    if (latency_aware)
    {
        /// `measured_latencies_scratch` is pre-reserved to `records.size()` in `updateImpl`,
        /// so clearing and filling it here does not allocate under `mutex`.
        measured_latencies_scratch.clear();
        for (const auto & rec : records)
            if (rec.latency_ms_ewma > 0)
                measured_latencies_scratch.push_back(rec.latency_ms_ewma);

        if (!measured_latencies_scratch.empty())
        {
            auto mid = measured_latencies_scratch.begin() + measured_latencies_scratch.size() / 2;
            std::nth_element(measured_latencies_scratch.begin(), mid, measured_latencies_scratch.end());
            median_latency_ms = *mid;
        }
    }

    size_t total_weight_next = 0;

    for (auto & rec: records)
    {
        size_t weight = rec.getWeight();

        if (latency_aware && weight > 0 && median_latency_ms > 0 && rec.latency_ms_ewma > 0)
        {
            /// ratio > 1 for faster-than-median addresses, < 1 for slower ones. Squared to
            /// concentrate new-connection opens on the fast front-ends (so the pool composition
            /// shifts toward them), with a wide clamp and a floor of 1 so slow addresses are
            /// still probed occasionally and can recover.
            const double ratio = median_latency_ms / rec.latency_ms_ewma;
            const double factor = std::clamp(ratio * ratio, 0.1, 16.0);
            weight = std::max<size_t>(1, static_cast<size_t>(std::lround(static_cast<double>(weight) * factor)));
        }

        total_weight_next += weight;
        rec.weight_prefix_sum = total_weight_next;
    }
}

HostResolversPool & HostResolversPool::instance()
{
    static HostResolversPool instance;
    return instance;
}

void HostResolversPool::dropCache()
{
    MemoryTrackerSwitcher switcher{&total_memory_tracker};
    std::lock_guard lock(mutex);
    host_pools.clear();
    /// Detach the in-flight lookups: their leaders and waiters keep their own references, but a
    /// caller arriving after the drop starts a fresh lookup instead of joining one that began
    /// before it, and a leader that finds its slot gone does not publish its result (see `getResolver`).
    pending_resolvers.clear();
}

HostResolver::Ptr HostResolversPool::getResolver(const String & host)
{
    {
        SharedLockGuard lock(mutex);
        if (auto it = host_pools.find(host); it != host_pools.end())
            return it->second;
    }

    ResolverFactory factory;
    std::shared_ptr<PendingResolver> pending;
    std::shared_future<HostResolver::Ptr> in_flight;
    {
        /// The pool is global: the `PendingResolver` slot, its future state, and its map node
        /// belong to `total_memory_tracker`, not to whichever query reached this cold host first,
        /// and may be freed later by another thread.
        MemoryTrackerSwitcher switcher{&total_memory_tracker};
        std::lock_guard lock(mutex);
        if (auto it = host_pools.find(host); it != host_pools.end())
            return it->second;

        if (auto it = pending_resolvers.find(host); it != pending_resolvers.end())
        {
            in_flight = it->second->result;
        }
        else
        {
            factory = resolver_factory_for_test;
            pending = std::make_shared<PendingResolver>();
            pending_resolvers.emplace(host, pending);
        }
    }

    /// Another thread is already resolving this host: wait for its outcome (a resolver, or the
    /// exception of its failed lookup) instead of running the same DNS lookup again.
    if (!pending)
    {
        SCOPE_EXIT_SAFE({
            MemoryTrackerSwitcher switcher{&total_memory_tracker};
            in_flight = {};
        });
        try
        {
            return in_flight.get();
        }
        catch (...)
        {
            /// All waiters of one lookup get the same stored exception. Give each waiter its own
            /// copy so a caller that mutates its exception (e.g. `addMessage`) does not race with
            /// the others.
            std::rethrow_exception(copyMutableException(std::current_exception()));
        }
    }

    /// Construct the resolver outside `mutex`: `HostResolver::create` runs the initial DNS lookup,
    /// and holding `mutex` across network I/O would block `tryGetResolver` and `getResolver` for
    /// every other host in the process. Concurrent callers for the same cold host wait on `pending`.
    /// Switch to `total_memory_tracker` for the whole leader path so the `HostResolver` object and
    /// `host_pools` node are charged to the global pool (matching `dropCache`), and a query at its
    /// own memory limit neither fails to resolve nor publishes its query-specific exception to
    /// unrelated waiting queries. Declared outside `pending`'s release scope so `~PendingResolver`
    /// also runs under `total_memory_tracker`.
    SCOPE_EXIT_SAFE({
        MemoryTrackerSwitcher switcher{&total_memory_tracker};
        pending.reset();
    });

    HostResolver::Ptr resolver;
    try
    {
        MemoryTrackerSwitcher switcher{&total_memory_tracker};
        auto created = factory ? factory(host) : HostResolver::create(host);

        std::lock_guard lock(mutex);
        /// `dropCache` removes the slot of a lookup in flight. Then this result may come from DNS
        /// data older than the drop, so answer the callers of this lookup with it but do not cache it.
        auto slot = pending_resolvers.find(host);
        if (slot != pending_resolvers.end() && slot->second == pending)
        {
            resolver = host_pools.try_emplace(host, std::move(created)).first->second;
            pending_resolvers.erase(slot);
        }
        else
        {
            resolver = std::move(created);
        }
    }
    catch (...)
    {
        {
            MemoryTrackerSwitcher switcher{&total_memory_tracker};
            std::lock_guard lock(mutex);
            /// Only this lookup's own slot: after `dropCache` the key may belong to a newer lookup.
            if (auto slot = pending_resolvers.find(host); slot != pending_resolvers.end() && slot->second == pending)
                pending_resolvers.erase(slot);
        }
        /// Store an immutable copy as the template for the waiters so this caller can mutate its
        /// own exception up the stack without racing with `copyMutableException` in a waiter.
        {
            MemoryTrackerSwitcher switcher{&total_memory_tracker};
            pending->promise.set_exception(copyMutableException(std::current_exception()));
        }
        throw;
    }

    pending->promise.set_value(resolver);
    return resolver;
}

HostResolver::Ptr HostResolversPool::tryGetResolver(const String & host)
{
    SharedLockGuard lock(mutex);

    if (auto it = host_pools.find(host); it != host_pools.end())
        return it->second;

    return nullptr;
}

void HostResolversPool::injectResolverForTest(const String & host, HostResolver::Ptr resolver)
{
    std::lock_guard lock(mutex);
    host_pools[host] = std::move(resolver);
}

void HostResolversPool::setResolverFactoryForTest(ResolverFactory factory)
{
    std::lock_guard lock(mutex);
    resolver_factory_for_test = std::move(factory);
}

}

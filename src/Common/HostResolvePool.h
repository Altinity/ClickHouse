#pragma once

#include <Common/CurrentMetrics.h>
#include <Common/HTTPConnectionPool.h>
#include <Common/ProfileEvents.h>
#include <Common/SharedMutex.h>
#include <Common/logger_useful.h>

#include <base/defines.h>

#include <Poco/Net/IPAddress.h>

#include <array>
#include <atomic>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#include <vector>

// That class resolves host into multiply addresses
// Features:
// - balance address usage.
//    `selectBest()` chooses the address by random with weights.
//    The more ip is used the lesser weight it has. When new address is happened, it takes more weight.
//    But still not all requests are assigned to the new address.
// - join resolve results
//    In case when host is resolved into different set of addresses, this class join all that addresses and use them.
//    An address expires after `history_` time.
// - failed address pessimization
//    If an address marked with `setFail()` it is marked as faulty. Such address won't be selected until either
//    a) it still occurs in resolve set after `history_` time or b) all other addresses are pessimized as well.
// - resolve schedule
//    Addresses are resolved through `DB::DNSResolver::instance()`.
//    Usually it does not happen more often than 3 times in `history_` period.
//    But also new resolve performed each `setFail()` call.
// - latency-aware selection (optional, off by default)
//    When enabled via `setLatencyAwareSelection()`, each address keeps an EWMA of its
//    observed request latency (time-to-first-byte of read requests, reported via
//    `setLatency()`). `selectBest()` biases the weighted random choice towards addresses
//    whose latency is below the median across the resolved set, while every address keeps a
//    floor of weight so it is still probed occasionally (keeps its EWMA fresh and lets a
//    recovered address climb back). TTFB is used rather than TCP-connect RTT because the
//    per-address latency spread is dominated by a stable, steerable backend-path component
//    that connect RTT does not capture (connect RTT reflects only network proximity).

namespace DB
{

struct HostResolverMetrics
{
    const ProfileEvents::Event discovered = ProfileEvents::end();
    const ProfileEvents::Event expired = ProfileEvents::end();
    const ProfileEvents::Event failed = ProfileEvents::end();

    const CurrentMetrics::Metric active_count = CurrentMetrics::end();
    const CurrentMetrics::Metric banned_count = CurrentMetrics::end();
};

constexpr size_t DEFAULT_RESOLVE_TIME_HISTORY_SECONDS = 2*60;
constexpr size_t RECORD_CONSECTIVE_FAIL_COUNT_LIMIT = 6;


class HostResolver : public std::enable_shared_from_this<HostResolver>
{
private:
    using WeakPtr = std::weak_ptr<HostResolver>;

public:
    using Ptr = std::shared_ptr<HostResolver>;

    template<class... Args>
    static Ptr create(Args&&... args)
    {
        struct make_shared_enabler : public HostResolver
        {
            explicit make_shared_enabler(Args&&... args) : HostResolver(std::forward<Args>(args)...) {}
        };
        return std::make_shared<make_shared_enabler>(std::forward<Args>(args)...);
    }

    virtual ~HostResolver();

    class Entry
    {
    public:
        Entry(Entry && entry) = default;
        Entry(Entry & entry) = delete;

        // no access as r-value
        const String * operator->() && = delete;
        const String * operator->() const && = delete;
        const String & operator*() && = delete;
        const String & operator*() const && = delete;

        const String * operator->() & { return &resolved_host; }
        const String * operator->() const & { return &resolved_host; }
        const String & operator*() & { return resolved_host; }
        const String & operator*() const & { return resolved_host; }

        /// Mark the address as failed in the pool. Also suppresses the destructor's success callback.
        void setFail();
        /// Suppress the destructor's success callback without reporting a failure to the pool.
        /// Use when the address was selected via `resolve` but never actually attempted
        void setUnused();
        ~Entry();

    private:
        friend class HostResolver;

        Entry(HostResolver & pool_, const Poco::Net::IPAddress & address_)
            : pool(pool_.getWeakFromThis())
            , address(address_)
            , resolved_host(address.toString())
        { }

        HostResolver::WeakPtr pool;
        const Poco::Net::IPAddress address;
        const String resolved_host;

        /// When true, the destructor will not call `pool->setSuccess`.
        bool skip_success_callback = false;
    };

    /// can throw NetException(ErrorCodes::DNS_ERROR, ...), Exception(ErrorCodes::BAD_ARGUMENTS, ...)
    Entry resolve();
    void update();
    void reset();

    /// Enable/disable latency-aware address selection process-wide (all resolvers). Off by default.
    static void setLatencyAwareSelection(bool enabled);
    static bool latencyAwareSelectionEnabled();

    /// Report a measured request latency (microseconds, typically time-to-first-byte) for an
    /// address. Feeds the per-address latency EWMA used by latency-aware selection. Cheap no-op
    /// when the address is unknown; marks weights dirty so `selectBest` recomputes them lazily on
    /// the next `resolve`. Does not allocate in the steady state (no heap use after the lock is
    /// taken), but it is not declared `noexcept`: the lock and `Poco::Net::IPAddress` comparisons
    /// are not guaranteed not to throw. Callers let such an exception propagate.
    void reportLatency(const Poco::Net::IPAddress & address, UInt64 microseconds);

    /// Consistent point-in-time snapshot of per-address latency EWMAs, captured under a single
    /// `mutex` acquisition before `EndpointConnectionPool` takes its own lock. Keeps the resolver
    /// mutex completely out of the endpoint pool critical section and gives non-measured endpoints
    /// (for example non-S3 HTTP pools) an immediate O(1) fast exit without touching the heap.
    struct LatencySnapshot
    {
        /// Fixed-size copy of the identity of a `Poco::Net::IPAddress`. Copying an `IPAddress`
        /// allocates its implementation object, which would cost one heap allocation per address
        /// under the resolver mutex on every borrow. Orders exactly as `Poco::Net::IPAddress`
        /// (length, then IPv6 scope, then bytes), so `entries` keeps the order of `records`.
        struct AddressKey
        {
            static constexpr size_t MAX_ADDRESS_LENGTH = 16; /// `in6_addr`

            static AddressKey from(const Poco::Net::IPAddress & address);

            bool operator<(const AddressKey & other) const;
            bool operator==(const AddressKey & other) const;

            UInt32 scope = 0;
            UInt8 length = 0;
            std::array<UInt8, MAX_ADDRESS_LENGTH> bytes{};
        };

        /// Sorted by `first` (`records` in `HostResolver` is sorted by `address`), containing only
        /// addresses with `latency_ms_ewma > 0`. Empty when no address has a measurement.
        std::vector<std::pair<AddressKey, double>> entries;
        /// Smallest EWMA across non-failed measured addresses (0 if none).
        double min_non_failed_ms = 0;

        bool empty() const noexcept { return entries.empty(); }

        /// Returns the recorded EWMA (ms) for `address`, or 0 if unmeasured / absent.
        double lookupLatencyMs(const Poco::Net::IPAddress & address) const;
    };

    LatencySnapshot getLatencySnapshot();

    static HostResolverMetrics getMetrics();

protected:
    explicit HostResolver(
        String host_,
        Poco::Timespan history_ = Poco::Timespan(DEFAULT_RESOLVE_TIME_HISTORY_SECONDS, 0));

    using ResolveFunction = std::function<std::vector<Poco::Net::IPAddress> (const String & host)>;
    HostResolver(ResolveFunction && resolve_function_,
                    String host_,
                    Poco::Timespan history_);

    friend class Entry;
    WeakPtr getWeakFromThis();

    void setSuccess(const Poco::Net::IPAddress & address);
    void setFail(const Poco::Net::IPAddress & address);

    struct Record
    {
        Record(Poco::Net::IPAddress address_, Poco::Timestamp resolve_time_)
            : address(address_)
            , resolve_time(resolve_time_)
        {}

        Record(Record && rec) = default;
        Record& operator=(Record && s) = default;

        Record(const Record & rec) = default;
        Record& operator=(const Record & s) = default;

        Poco::Net::IPAddress address;
        Poco::Timestamp resolve_time;
        size_t usage = 0;
        bool failed = false;
        Poco::Timestamp fail_time = 0;
        size_t consecutive_fail_count = 0;

        /// EWMA of observed request latency (time-to-first-byte) in milliseconds. 0 means
        /// "not measured yet". Only used when latency-aware selection is enabled.
        double latency_ms_ewma = 0;

        size_t weight_prefix_sum{};

        bool operator <(const Record & r) const
        {
            return address < r.address;
        }

        bool operator ==(const Record & r) const
        {
            return address == r.address;
        }

        size_t getWeight() const
        {
            if (failed)
                return 0;

            /// There is no goal to make usage's distribution ideally even
            /// The goal is to chose more often new address, but still use old addresses as well
            /// when all addresses have usage counter greater than 10000,
            /// no more corrections are needed, just random choice is ok
            if (usage > 10000)
                return 1;
            if (usage > 1000)
                return 5;
            if (usage > 100)
                return 8;
            return 10;
        }

        bool setFail(const Poco::Timestamp & now)
        {
            bool was_ok = !failed;

            failed = true;
            fail_time = now;

            if (was_ok)
            {
                if (consecutive_fail_count < RECORD_CONSECTIVE_FAIL_COUNT_LIMIT)
                    ++consecutive_fail_count;
            }

            return was_ok;
        }

        /// Returns true if the address was failed and is now cleared, so the caller can
        /// adjust the banned-count metric. A successful connection proves the address is
        /// reachable, so it must be un-banned even if it was handed out while failed (the
        /// all-banned zero-weight branch in `selectBest` can do that under a transient race).
        bool setSuccess()
        {
            bool was_failed = failed;

            failed = false;
            consecutive_fail_count = 0;
            ++usage;

            return was_failed;
        }

        void setLatency(UInt64 microseconds)
        {
            if (microseconds == 0)
                return;

            /// Alpha weights the newest sample; small enough to smooth per-request jitter (and the
            /// per-object backend-fetch variance) but responsive enough to follow a front-end that
            /// persistently gets slower/faster.
            static constexpr double alpha = 0.2;
            const double ms = static_cast<double>(microseconds) / 1000.0;
            latency_ms_ewma = latency_ms_ewma > 0 ? alpha * ms + (1 - alpha) * latency_ms_ewma : ms;
        }
    };

    using Records = std::vector<Record>;

    const Poco::Net::IPAddress & selectBest() TSA_REQUIRES(mutex);
    Records::iterator find(const Poco::Net::IPAddress & address) TSA_REQUIRES(mutex);
    bool isUpdateNeeded();

    void updateImpl(Poco::Timestamp now, std::vector<Poco::Net::IPAddress> & next_gen) TSA_REQUIRES(mutex);
    void updateWeights() TSA_REQUIRES(mutex);
    void updateWeightsImpl() TSA_REQUIRES(mutex);
    size_t getTotalWeight() const noexcept TSA_REQUIRES(mutex);
    Poco::Timespan getRecordHistoryTime(const Record&) const;

    const String host;
    const Poco::Timespan history;
    const Poco::Timespan resolve_interval;
    const HostResolverMetrics metrics = getMetrics();

    // for tests purpose
    const ResolveFunction resolve_function;

    std::mutex mutex;

    Poco::Timestamp last_resolve_time TSA_GUARDED_BY(mutex) = Poco::Timestamp::TIMEVAL_MIN;
    Records records TSA_GUARDED_BY(mutex);
    /// Scratch buffer pre-reserved to `records.size()` during `updateImpl` so `updateWeightsImpl`
    /// never heap-allocates or throws under `mutex`.
    std::vector<double> measured_latencies_scratch TSA_GUARDED_BY(mutex);
    /// Set by `reportLatency` instead of recomputing weights on every sample; cleared whenever
    /// `updateWeightsImpl` runs (lazily in `selectBest`, or eagerly on `update`/`setFail`/`setSuccess`).
    bool weights_dirty TSA_GUARDED_BY(mutex) = false;
    /// Value of `latency_aware_selection` that `updateWeightsImpl` last computed the weights with.
    /// The toggle is process-wide and flips without touching the resolvers, so `selectBest` compares
    /// it with the current value and recomputes when it differs; otherwise biased weights would
    /// outlive turning the setting off (and unbiased ones would outlive turning it on).
    bool weights_latency_aware TSA_GUARDED_BY(mutex) = false;

    /// True once at least one record in `records` has `latency_ms_ewma > 0`. Reset in `reset()`
    /// and refreshed on `updateImpl` (an expired address may have held the only measurement).
    /// Lets `getLatencySnapshot` return in O(1) without locking `mutex` on endpoints that never
    /// report latency (for example non-S3 HTTP pools).
    std::atomic<bool> has_measured_latency{false};

    /// Process-wide toggle for latency-aware selection. Shared by all resolvers.
    static std::atomic<bool> latency_aware_selection;

    Poco::Logger * log = &Poco::Logger::get("ConnectionPool");
};

class HostResolversPool
{
private:
    HostResolversPool() = default;

public:
    HostResolversPool(const HostResolversPool &) = delete;
    HostResolversPool & operator=(const HostResolversPool &) = delete;

    static HostResolversPool & instance();

    void dropCache();

    HostResolver::Ptr getResolver(const String & host);

    /// Return the cached resolver for `host`, or `nullptr` if none exists (for example after
    /// `dropCache`, or for a proxied endpoint whose target was never resolved locally). Never
    /// creates a resolver or runs DNS, so it is cheap and safe to call under another lock
    /// or after a response has already been received.
    HostResolver::Ptr tryGetResolver(const String & host);

    /// Pre-populate the pool with a resolver for `host`, replacing any cached entry.
    /// For tests: allows simulating multi-address DNS responses without injecting into
    /// the real `DNSResolver`, so the `prepareNewConnection` retry path can be exercised
    /// deterministically.
    void injectResolverForTest(const String & host, HostResolver::Ptr resolver);

    using ResolverFactory = std::function<HostResolver::Ptr(const String &)>;
    /// Override how `getResolver` constructs a missing resolver. For tests only.
    void setResolverFactoryForTest(ResolverFactory factory);

private:
    /// The in-flight creation of the resolver for one host. The first caller for a cold host (the
    /// leader) creates the resolver and publishes the outcome, a resolver or the exception of the
    /// failed lookup, through `result`; concurrent callers wait on it instead of running the DNS
    /// lookup again. The slot is removed from `pending_resolvers` on both outcomes.
    struct PendingResolver
    {
        std::promise<HostResolver::Ptr> promise;
        std::shared_future<HostResolver::Ptr> result{promise.get_future().share()};
    };

    SharedMutex mutex;
    std::unordered_map<String, HostResolver::Ptr> host_pools TSA_GUARDED_BY(mutex);
    std::unordered_map<String, std::shared_ptr<PendingResolver>> pending_resolvers TSA_GUARDED_BY(mutex);
    ResolverFactory resolver_factory_for_test TSA_GUARDED_BY(mutex);
};

}

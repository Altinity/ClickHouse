#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/tests/cas_test_helpers.h>
#include <Poco/Exception.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace DB::Cas::tests
{

/// A clock a test moves by exact amounts. A sleep moves it by one millisecond more than it was asked
/// for, because a jittered pause can be zero and a clock that stands still never closes a window.
/// Held by `shared_ptr`: a pool keeps the closures and can outlive the test's frame.
class ManualClock
{
public:
    explicit ManualClock(uint64_t start_ms = 1'000'000) : now_ms(start_ms) {}

    uint64_t now() const { return now_ms.load(); }
    void advance(uint64_t ms) { now_ms.fetch_add(ms); }

    static std::function<uint64_t()> nowFnOf(std::shared_ptr<ManualClock> owned)
    {
        return [clock = std::move(owned)] { return clock->now(); };
    }
    static std::function<void(uint64_t)> sleepFnOf(std::shared_ptr<ManualClock> owned)
    {
        return [clock = std::move(owned)](uint64_t ms) { clock->advance(ms + 1); };
    }

private:
    std::atomic<uint64_t> now_ms;
};

/// A store in which a chosen request takes time: the clock the request engine reads moves INSIDE the
/// request, after the engine bound its policy and before the answer comes back. An instantaneous store
/// cannot show a deadline that was fixed before a long step and is consulted after it.
class TimeTakingBackend final : public CountingBackend
{
public:
    enum class Verb : uint8_t { Read, Head, List, Write, Remove, Publish, Stream };
    enum class Ending : uint8_t
    {
        Answered,
        /// The request fails with a timeout and changes nothing.
        LostBeforeLanding,
        /// The request takes effect and its response is lost.
        LostAfterLanding,
    };

    /// One request as the store received it.
    struct Seen
    {
        Verb verb;
        String key;
        uint64_t at_ms;
    };

    void useClock(std::shared_ptr<VirtualRetryClock> clock_)
    {
        std::lock_guard lock(rules_mutex);
        clock = std::move(clock_);
    }

    /// The clock the journal stamps requests with.
    void stampWith(std::function<uint64_t()> now_ms_)
    {
        std::lock_guard lock(rules_mutex);
        now_ms = std::move(now_ms_);
    }

    /// Each of the next `times` matching requests takes `takes_ms` and then ends as `ending`.
    void arm(Verb verb, std::function<bool(const String &)> matches, uint64_t takes_ms, size_t times = 1,
             Ending ending = Ending::Answered)
    {
        armWith(verb, std::move(matches), [this, takes_ms]
        {
            std::shared_ptr<VirtualRetryClock> current;
            {
                std::lock_guard lock(rules_mutex);
                current = clock;
            }
            if (current && takes_ms)
                current->advance(takes_ms);
        }, times, ending);
    }

    /// `while_in_flight` runs inside each of the next `times` matching requests, on the requesting
    /// thread and with no lock of this backend held: it is what happens elsewhere while the request
    /// is in flight, and it may send requests of its own.
    void armWith(Verb verb, std::function<bool(const String &)> matches, std::function<void()> while_in_flight,
                 size_t times = 1, Ending ending = Ending::Answered)
    {
        std::lock_guard lock(rules_mutex);
        rules.push_back(Rule{verb, std::move(matches), std::move(while_in_flight), times, ending});
    }

    /// The keys of the requests a rule applied to, in order.
    std::vector<String> slowedKeys() const
    {
        std::lock_guard lock(rules_mutex);
        return slowed_keys;
    }

    /// Every request received, in order, a refused or failed one included.
    std::vector<Seen> seen() const
    {
        std::lock_guard lock(rules_mutex);
        return seen_requests;
    }

    /// Requests of `verb` to `key` received at or after position `from` of the journal.
    size_t seenSince(size_t from, Verb verb, const String & key) const
    {
        std::lock_guard lock(rules_mutex);
        size_t count = 0;
        for (size_t i = from; i < seen_requests.size(); ++i)
            if (seen_requests[i].verb == verb && seen_requests[i].key == key)
                ++count;
        return count;
    }

    size_t seenCount() const
    {
        std::lock_guard lock(rules_mutex);
        return seen_requests.size();
    }

    /// Publications REQUESTED for `key`, whether or not they stored anything.
    uint64_t publicationsOf(const String & key) const
    {
        std::lock_guard lock(rules_mutex);
        const auto it = publications.find(key);
        return it == publications.end() ? 0 : it->second;
    }

    uint64_t serverSideCopiesOf(const String & key) const
    {
        std::lock_guard lock(rules_mutex);
        const auto it = server_side_copies.find(key);
        return it == server_side_copies.end() ? 0 : it->second;
    }

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        failIfLostBeforeLanding(spend(Verb::Read, key), "read");
        return CountingBackend::read(key, access);
    }

    std::optional<RawMeta> head(const String & key, TransportAccess & access) override
    {
        failIfLostBeforeLanding(spend(Verb::Head, key), "head");
        return CountingBackend::head(key, access);
    }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        failIfLostBeforeLanding(spend(Verb::List, prefix), "list");
        return CountingBackend::list(prefix, cursor, limit, access);
    }

    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override
    {
        const Ending ending = spend(Verb::Write, key).value_or(Ending::Answered);
        failIfLostBeforeLanding(ending, "write");
        auto answer = CountingBackend::write(key, bytes, expected_value, access);
        if (ending == Ending::LostAfterLanding)
            throw Poco::TimeoutException("TimeTakingBackend: the write was applied and its response was lost");
        return answer;
    }

    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override
    {
        failIfLostBeforeLanding(spend(Verb::Remove, key), "remove");
        return CountingBackend::remove(key, expected_value, access);
    }

    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override
    {
        failIfLostBeforeLanding(spend(Verb::Stream, key), "stream");
        return CountingBackend::stream(key, access);
    }

    void publish(const BlobPublishRequest & request, TransportAccess & access) override
    {
        {
            std::lock_guard lock(rules_mutex);
            ++publications[request.destination_key];
            if (std::holds_alternative<VerbatimStagedBlobPublication>(request.publication))
                ++server_side_copies[request.destination_key];
        }
        const Ending ending = spend(Verb::Publish, request.destination_key).value_or(Ending::Answered);
        if (ending == Ending::LostBeforeLanding)
            throw Poco::TimeoutException("TimeTakingBackend: the publication timed out and stored nothing");
        CountingBackend::publish(request, access);
        if (ending == Ending::LostAfterLanding)
            throw Poco::TimeoutException("TimeTakingBackend: the publication was stored and its response was lost");
    }

private:
    struct Rule
    {
        Verb verb;
        std::function<bool(const String &)> matches;
        std::function<void()> while_in_flight;
        size_t remaining;
        Ending ending;
    };

    static void failIfLostBeforeLanding(std::optional<Ending> ending, const char * verb)
    {
        if (ending == Ending::LostBeforeLanding)
            throw Poco::TimeoutException(String("TimeTakingBackend: the ") + verb + " timed out");
    }

    std::optional<Ending> spend(Verb verb, const String & key)
    {
        std::function<void()> while_in_flight;
        Ending ending = Ending::Answered;
        {
            std::lock_guard lock(rules_mutex);
            seen_requests.push_back(Seen{verb, key, now_ms ? now_ms() : 0});
            auto rule = rules.begin();
            for (; rule != rules.end(); ++rule)
                if (rule->verb == verb && rule->remaining != 0 && rule->matches(key))
                    break;
            if (rule == rules.end())
                return std::nullopt;
            --rule->remaining;
            slowed_keys.push_back(key);
            while_in_flight = rule->while_in_flight;
            ending = rule->ending;
        }
        while_in_flight();
        return ending;
    }

    mutable std::mutex rules_mutex;
    std::shared_ptr<VirtualRetryClock> clock;
    std::function<uint64_t()> now_ms;
    std::vector<Rule> rules;
    std::vector<String> slowed_keys;
    std::vector<Seen> seen_requests;
    std::map<String, uint64_t> publications;
    std::map<String, uint64_t> server_side_copies;
};

}

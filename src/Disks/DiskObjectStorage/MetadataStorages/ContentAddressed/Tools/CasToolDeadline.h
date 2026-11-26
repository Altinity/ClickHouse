#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Common/Exception.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <string_view>

namespace DB::ErrorCodes
{
    extern const int NETWORK_ERROR;
    extern const int TIMEOUT_EXCEEDED;
}

namespace DB::Cas
{

/// The deadline of one offline tool run (`cas-fsck`, `cas-migrate-pool`), fixed once on the clock of the request plane
/// the tool sends through. The tool's checks and the engine's refusal of a late request read that one clock, so running
/// out of time is the tool's `TIMEOUT_EXCEEDED`, never a failed request. Default-constructed: no deadline.
/// Safe to read from several threads.
class ToolDeadline
{
public:
    ToolDeadline() = default;

    /// `timeout` from now on the clock of `requests`, which must outlive this object. `advice` ends the timeout message
    /// and tells the operator what to do next.
    ToolDeadline(const CasRequests & requests_, std::chrono::milliseconds timeout, std::string tool_, std::string advice_)
        : requests(&requests_)
        , deadline_ms(saturatingAdd(requests_.nowMs(), static_cast<uint64_t>(std::max<int64_t>(timeout.count(), 0))))
        , tool(std::move(tool_))
        , advice(std::move(advice_))
    {
    }

    bool passed() const { return requests && requests->nowMs() >= deadline_ms; }

    /// Milliseconds left; 0 once passed. Without a deadline, the largest value.
    uint64_t remainingMs() const
    {
        if (!requests)
            return std::numeric_limits<uint64_t>::max();
        const uint64_t now = requests->nowMs();
        return now >= deadline_ms ? 0 : deadline_ms - now;
    }

    [[noreturn]] void throwExceeded(std::string_view phase) const
    {
        throw Exception(ErrorCodes::TIMEOUT_EXCEEDED, "{}: exceeded the deadline during '{}' — {}.", tool, phase, advice);
    }

    /// Throws `TIMEOUT_EXCEEDED` naming `phase` once the deadline has passed.
    void check(std::string_view phase) const
    {
        if (passed())
            throwExceeded(phase);
    }

    /// The policy of one request: the standard budget, ending no later than the deadline. Throws the timeout when one
    /// attempt no longer fits, as the engine would refuse it with a retry-later error a tolerant caller counts as a finding.
    Retry bound(const CasOperation & op, std::string_view phase) const
    {
        check(phase);
        if (!requests)
            return Retry::standard();
        const Retry chosen = op.freeze(Retry::standard()).cappedAt(deadline_ms);
        if (!op.oneAttemptFitsBefore(*chosen.policy_deadline_ms))
            throwExceeded(phase);
        return chosen;
    }

    /// For a caller that tolerates a failed request: throws the timeout when `failure` is the engine's retry-later
    /// give-up and no attempt fits before the deadline any more. The engine refuses an attempt that does not fit and the
    /// clock only moves forward, so a request given up for lack of time is the timeout, not the caller's finding.
    void rethrowIfOutOfTime(const CasOperation & op, const Exception & failure, std::string_view phase) const
    {
        if (requests && failure.code() == ErrorCodes::NETWORK_ERROR && !op.oneAttemptFitsBefore(deadline_ms))
            throwExceeded(phase);
    }

private:
    static uint64_t saturatingAdd(uint64_t a, uint64_t b)
    {
        return a > std::numeric_limits<uint64_t>::max() - b ? std::numeric_limits<uint64_t>::max() : a + b;
    }

    const CasRequests * requests = nullptr;
    uint64_t deadline_ms = 0;
    std::string tool;
    std::string advice;
};

}

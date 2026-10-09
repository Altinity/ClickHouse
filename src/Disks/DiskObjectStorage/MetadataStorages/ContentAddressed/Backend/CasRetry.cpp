#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>

#include <Common/thread_local_rng.h>

#include <algorithm>
#include <limits>

namespace DB::Cas
{

uint64_t Retry::backoff(uint32_t attempt)
{
    if (attempt == 0)
        return 0;
    const uint32_t doublings = std::min<uint32_t>(attempt - 1, 20);
    const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << doublings);
    return thread_local_rng() % (ceiling + 1);     /// full jitter: uniform(0, ceiling)
}

uint64_t Retry::spacedPause(uint64_t spacing_draw_ms, uint64_t request_started_ms, uint64_t now_ms)
{
    const uint64_t taken_ms = now_ms > request_started_ms ? now_ms - request_started_ms : 0;
    return taken_ms >= spacing_draw_ms ? 0 : spacing_draw_ms - taken_ms;
}

uint64_t Retry::drawSpacing(uint64_t attempt_spacing_ms_)
{
    const uint64_t spread = attempt_spacing_ms_ / 5;
    const uint64_t low = attempt_spacing_ms_ - spread;
    const uint64_t high = attempt_spacing_ms_ > std::numeric_limits<uint64_t>::max() - spread
        ? std::numeric_limits<uint64_t>::max()
        : attempt_spacing_ms_ + spread;
    return low + thread_local_rng() % (high - low + 1);
}

Retry::Bound Retry::bind(uint64_t now_ms) const
{
    const uint64_t own_deadline_ms = policy_deadline_ms
        ? *policy_deadline_ms
        : (now_ms > std::numeric_limits<uint64_t>::max() - window_ms
               ? std::numeric_limits<uint64_t>::max()
               : now_ms + window_ms);
    if (lease_deadline_ms && *lease_deadline_ms < own_deadline_ms)
        return {*lease_deadline_ms, true};
    return {own_deadline_ms, false};
}

}

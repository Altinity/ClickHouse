#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRetry.h>

#include <Common/Stopwatch.h>
#include <Common/thread_local_rng.h>

#include <algorithm>

namespace DB::Cas
{

namespace
{

/// `Backend/` has no dependency on `Pool/` today (no file under `Backend/` includes a `Pool/`
/// header) -- `CasMountRuntime::bootMs()` lives in `Pool/CasMountRuntime.h` and pulling it in here
/// would be the first such edge. Until the lock moves this contract's clock source, use the same
/// boot-time-shaped monotonic clock directly via `Common/Stopwatch.h` instead of crossing the layer.
uint64_t bootMsNow()
{
    return clock_gettime_ns(CLOCK_MONOTONIC) / 1000000;
}

}

uint64_t Retry::backoff(uint32_t attempt)
{
    if (attempt == 0)
        return 0;
    const uint32_t doublings = std::min<uint32_t>(attempt - 1, 20);
    const uint64_t ceiling = std::min<uint64_t>(5000, 200ull << doublings);
    return thread_local_rng() % (ceiling + 1);     /// full jitter: uniform(0, ceiling)
}

Retry Retry::within(uint64_t ms)
{
    return Retry{bootMsNow() + ms, false};
}

Retry Retry::standard()
{
    return within(90'000);
}

Retry Retry::untilLeaseSafe(uint64_t lease_deadline_ms, uint64_t safety_margin_ms)
{
    const uint64_t lease_bound = lease_deadline_ms > safety_margin_ms ? lease_deadline_ms - safety_margin_ms : 0;
    return Retry{std::min(standard().deadline_ms, lease_bound), false};
}

Retry Retry::once()
{
    return Retry{standard().deadline_ms, true};
}

}

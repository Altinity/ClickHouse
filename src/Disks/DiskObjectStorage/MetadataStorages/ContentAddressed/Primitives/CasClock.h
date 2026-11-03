#pragma once
#include <chrono>
#include <cstdint>

namespace DB::Cas
{

/// Wall-clock milliseconds since the epoch, the unit of every `published_at_ms` and lease timestamp.
inline uint64_t nowMs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

}

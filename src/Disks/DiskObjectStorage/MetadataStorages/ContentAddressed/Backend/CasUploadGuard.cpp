#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasUploadGuard.h>

#include <limits>
#include <utility>

namespace DB::Cas
{

namespace
{

class UploadGuard final : public IThrottler
{
public:
    UploadGuard(ThrottlerPtr bandwidth_, std::function<void()> stop_)
        : bandwidth(std::move(bandwidth_))
        , stop(std::move(stop_))
    {
    }

    bool throttle(size_t amount, size_t max_block_ns) override
    {
        stop();
        const bool blocked = bandwidth && bandwidth->throttle(amount, max_block_ns);
        stop();
        return blocked;
    }

    bool isThrottling() const override { return bandwidth && bandwidth->isThrottling(); }
    Int64 getAvailable() override { return bandwidth ? bandwidth->getAvailable() : std::numeric_limits<Int64>::max(); }
    UInt64 getMaxSpeed() const override { return bandwidth ? bandwidth->getMaxSpeed() : 0; }
    UInt64 getMaxBurst() const override { return bandwidth ? bandwidth->getMaxBurst() : 0; }

private:
    ThrottlerPtr bandwidth;
    std::function<void()> stop;
};

thread_local String bound_key;
thread_local ThrottlerPtr bound_throttler;

}

ThrottlerPtr makeUploadGuard(ThrottlerPtr bandwidth, std::function<void()> stop)
{
    if (!stop)
        return bandwidth;
    return std::make_shared<UploadGuard>(std::move(bandwidth), std::move(stop));
}

BodyGuardScope::BodyGuardScope(String key, ThrottlerPtr throttler)
    : saved{std::exchange(bound_key, std::move(key)), std::exchange(bound_throttler, std::move(throttler))}
{
}

BodyGuardScope::~BodyGuardScope()
{
    bound_key = std::move(saved.key);
    bound_throttler = std::move(saved.throttler);
}

ThrottlerPtr BodyGuardScope::forKey(const String & key)
{
    return bound_throttler && bound_key == key ? bound_throttler : nullptr;
}

}

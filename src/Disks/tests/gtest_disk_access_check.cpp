#include <gtest/gtest.h>

#include <Core/ServerUUID.h>
#include <Disks/tests/gtest_disk.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace
{

/// Counts the access probes running at the same time. A probe waits for a second one to arrive, so
/// two unserialized probes meet inside `checkAccessImpl` and a serialized pair runs one after the other.
struct ProbeMeter
{
    std::mutex mutex;
    std::condition_variable arrived;
    size_t active = 0;
    size_t max_active = 0;
};

class ProbeDisk : public DB::DiskLocal
{
public:
    ProbeDisk(const String & name_, const String & path_, ProbeMeter & meter_) : DiskLocal(name_, path_), meter(meter_) { }

protected:
    void checkAccessImpl(const String &) override
    {
        std::unique_lock lock(meter.mutex);
        ++meter.active;
        meter.max_active = std::max(meter.max_active, meter.active);
        meter.arrived.notify_all();
        meter.arrived.wait_for(lock, std::chrono::milliseconds(300), [&] { return meter.active == 2; });
        --meter.active;
    }

private:
    ProbeMeter & meter;
};

}

/// Every disk writes the probe `clickhouse_access_check_<server uuid>`; disks created in parallel
/// (`Context::getOrCreateDisk`) can share a backing store, so their probes must not overlap.
TEST(DiskAccessCheck, ProbesOfDisksOverOneDirectoryDoNotOverlap)
{
    DB::ServerUUID::setRandomForUnitTests();
    auto backing = createDisk("access_check");
    ProbeMeter meter;
    auto a = std::make_shared<ProbeDisk>("a", backing->getPath(), meter);
    auto b = std::make_shared<ProbeDisk>("b", backing->getPath(), meter);

    std::thread ta([&] { a->startup(/*skip_access_check=*/false); });
    std::thread tb([&] { b->startup(/*skip_access_check=*/false); });
    ta.join();
    tb.join();

    EXPECT_EQ(meter.max_active, 1u);
    destroyDisk(backing);
}

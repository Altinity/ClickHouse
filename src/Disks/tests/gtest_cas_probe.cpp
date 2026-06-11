#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasProbe.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;

TEST(CasProbe, PassesOnEnforcingBackend)
{
    auto b = std::make_shared<InMemoryBackend>();
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
    EXPECT_TRUE(b->list("p/.cas_probe", "", 10).keys.empty());   // probe cleans up after itself
}

TEST(CasProbe, FailsClosedOnNonEnforcingDelete)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setEnforceTokens(false);                                  // the MinIO-OSS failure mode
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
}

TEST(CasProbe, FailsClosedOnDeleteMarkers)
{
    auto b = std::make_shared<InMemoryBackend>();
    b->setSimulateDeleteMarkers(true);                           // versioning enabled on the prefix
    EXPECT_THROW(runCapabilityProbe(*b, "p/.cas_probe"), DB::Exception);
}

TEST(CasProbe, PassesOnEmulatedLocal)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::EmulatedSingleProcess);
    EXPECT_NO_THROW(runCapabilityProbe(*b, "p/.cas_probe"));
}

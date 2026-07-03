#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;

/// Every Token{...} the backend mints must carry native_token_type instead of a hardcoded
/// TokenType::ETag (Task 5). Mode::Native over a LocalObjectStorage has no write-time ETag, so
/// putIfAbsent's PutResult falls back to a HEAD internally — that HEAD is also a stamping site,
/// so the assertion below exercises both the direct-etag and the HEAD-fallback mint paths.
TEST(CasBackendGeneration, StampedTokenTypeFollowsNativeKind)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    b->setNativeTokenTypeForTest(TokenType::Generation);

    const auto put = b->putIfAbsent("p/gen/tok", "v1");
    EXPECT_EQ(put.token.type, TokenType::Generation);

    const auto hr = b->head("p/gen/tok");
    ASSERT_TRUE(hr.exists);
    EXPECT_EQ(hr.token.type, TokenType::Generation);
}

/// checkStorePreconditions on a Native, generation-dialect (GCS) backend consults
/// isBucketVersioningEnabled. LocalObjectStorage does not override that method, so it inherits the
/// IObjectStorage base default, which returns nullopt (the check is inconclusive). Per the hook's
/// documented behaviour, an inconclusive check must NOT fail closed — only a CONFIRMED `true` throws.
TEST(CasBackendGeneration, CheckStorePreconditionsProceedsOnUnknownVersioning)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    b->setNativeTokenTypeForTest(TokenType::Generation);

    EXPECT_NO_THROW(b->checkStorePreconditions());
}

/// The ETag-dialect (AWS-compatible) backend never consults bucket versioning at all — the check is
/// a silent no-op for any backend that is not Native + TokenType::Generation.
TEST(CasBackendGeneration, CheckStorePreconditionsNoOpOnEtagDialect)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    ASSERT_EQ(b->nativeTokenType(), TokenType::ETag);

    EXPECT_NO_THROW(b->checkStorePreconditions());
}

/// GCS enforces NO preconditions on CompleteMultipartUpload (measured 2026-07-03), so a conditional
/// write on a generation-token store must never take the multipart path. conditionalWriteSettings
/// must force the single-PUT path (and raise the single-part cap to conditional_single_put_cap) when
/// the backend's native token kind is Generation, and stay a no-op otherwise (ETag dialect).
TEST(CasBackendGeneration, ConditionalWriteSettingsForceSinglePutOnGenerationStores)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native,
        /*conditional_single_put_cap=*/123);
    b->setNativeTokenTypeForTest(TokenType::Generation);
    const auto ws = b->conditionalWriteSettingsForTest();
    EXPECT_TRUE(ws.s3_force_single_part_upload);
    EXPECT_EQ(ws.s3_single_part_upload_max_bytes_override, 123u);

    b->setNativeTokenTypeForTest(TokenType::ETag);
    const auto ws2 = b->conditionalWriteSettingsForTest();
    EXPECT_FALSE(ws2.s3_force_single_part_upload);
    EXPECT_EQ(ws2.s3_single_part_upload_max_bytes_override, 0u);
}

#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>
#include <Disks/tests/cas_test_helpers.h>

using namespace DB::Cas;

/// Every Token{...} the backend mints must carry native_token_type instead of a hardcoded
/// TokenType::ETag (Task 5). Mode::Native over a LocalObjectStorage has no write-time ETag, so
/// putIfAbsent's PutResult falls back to a HEAD internally — that HEAD is also a stamping site,
/// so the assertion below exercises both the direct-etag and the HEAD-fallback mint paths.
TEST(CASBackendGeneration, StampedTokenTypeFollowsNativeKind)
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

/// checkPoolPreconditions on a Native, generation-dialect (GCS) backend consults
/// isBucketVersioningEnabled. LocalObjectStorage does not override that method, so it inherits the
/// IObjectStorage base default, which returns nullopt (the check is inconclusive). Per the hook's
/// documented behaviour, an inconclusive check must NOT fail closed — only a CONFIRMED `true` throws.
TEST(CASBackendGeneration, CheckPoolPreconditionsProceedsOnUnknownVersioning)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    b->setNativeTokenTypeForTest(TokenType::Generation);

    EXPECT_NO_THROW(b->checkPoolPreconditions());
}

/// The ETag-dialect (AWS-compatible) backend never consults bucket versioning at all — the check is
/// a silent no-op for any backend that is not Native + TokenType::Generation.
TEST(CASBackendGeneration, CheckPoolPreconditionsNoOpOnEtagDialect)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    ASSERT_EQ(b->nativeTokenType(), TokenType::ETag);

    EXPECT_NO_THROW(b->checkPoolPreconditions());
}

/// GCS enforces NO preconditions on CompleteMultipartUpload (measured 2026-07-03), so a conditional
/// write on a generation-token store must never take the multipart path. conditionalWriteSettings
/// must force the single-PUT path (and raise the single-part cap to conditional_single_put_cap) when
/// the backend's native token kind is Generation, and stay a no-op otherwise (ETag dialect).
TEST(CASBackendGeneration, ListTokensDisabledOnGenerationStores)
{
    /// XML LIST bodies carry MD5-style ETags that the dialect cannot rewrite to generations; a
    /// list-derived token on a generation store is a poisoned If-Match (live GC on GCS died there).
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);
    EXPECT_TRUE(b->supportsListTokens());
    b->setNativeTokenTypeForTest(TokenType::Generation);
    EXPECT_FALSE(b->supportsListTokens());
    b->setNativeTokenTypeForTest(TokenType::ETag);
    EXPECT_TRUE(b->supportsListTokens());
}

TEST(CASBackendGeneration, ConditionalWriteSettingsForceSinglePutOnGenerationStores)
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

/// C1: the three token-policy helpers are the single source of truth for how a Native-mode backend
/// mints a HEAD/PUT token, gates a LIST token, and compares tokens. Characterizes the behavior the
/// scattered call sites have today so the consolidation stays byte-for-byte behavior-preserving.
TEST(CASBackendGeneration, TokenPolicyHelpersAreConsistentWithDialect)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native);

    /// ETag dialect: head/put tokens carry ETag; list surfaces the same-typed token for a non-empty etag.
    ASSERT_EQ(b->nativeTokenType(), TokenType::ETag);
    EXPECT_EQ(b->tokenForHead("abc").type, TokenType::ETag);
    EXPECT_EQ(b->tokenForHead("abc"), (Token{"abc", TokenType::ETag}));
    ASSERT_TRUE(b->tokenForList("abc").has_value());
    EXPECT_EQ(*b->tokenForList("abc"), b->tokenForHead("abc"));   /// list token == head token (same etag)
    EXPECT_FALSE(b->tokenForList("").has_value());                /// empty etag => no list token

    /// Generation dialect (GCS): head token flips to Generation; list tokens are disabled wholesale
    /// (poisoned If-Match), so tokenForList is always nullopt regardless of the etag.
    b->setNativeTokenTypeForTest(TokenType::Generation);
    EXPECT_EQ(b->tokenForHead("g1").type, TokenType::Generation);
    EXPECT_FALSE(b->tokenForList("g1").has_value());

    /// tokenMatches is exact identity (value AND type) — a same-value/different-type token never matches.
    EXPECT_TRUE(ObjectStorageBackend::tokenMatches(Token{"x", TokenType::ETag}, Token{"x", TokenType::ETag}));
    EXPECT_FALSE(ObjectStorageBackend::tokenMatches(Token{"x", TokenType::ETag}, Token{"x", TokenType::Emulated}));
}

#include <config.h>

#if USE_JWT_CPP && USE_SSL

#include <Client/OAuthLogin.h>
#include <Common/Base64.h>
#include <Common/OpenSSLHelpers.h>
#include <gtest/gtest.h>

#include <algorithm>

using namespace DB;

// ---------------------------------------------------------------------------
// PKCE building blocks
//
// generatePKCE() is in the anonymous namespace so we test its constituent
// operations (base64url encoding and SHA-256) directly. This verifies the
// exact properties that RFC 7636 §4 requires of the verifier and challenge.
// ---------------------------------------------------------------------------

TEST(OAuthLogin, Base64UrlEncodingProperties)
{
    // 32 bytes → 43 base64url chars (no padding, RFC 7636 §4.1 requires 43-128).
    const std::string raw(32, '\xAB');
    const std::string encoded = base64Encode(raw, /*url_encoding=*/true, /*no_padding=*/true);

    EXPECT_EQ(encoded.size(), 43u);

    // Must contain only URL-safe base64 chars: A-Z a-z 0-9 - _
    const bool all_safe = std::all_of(encoded.begin(), encoded.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_';
    });
    EXPECT_TRUE(all_safe) << "base64url output contains non-URL-safe characters: " << encoded;

    // Must NOT contain padding or standard base64 symbols.
    EXPECT_EQ(encoded.find('='), std::string::npos);
    EXPECT_EQ(encoded.find('+'), std::string::npos);
    EXPECT_EQ(encoded.find('/'), std::string::npos);
}

TEST(OAuthLogin, PKCEChallengeDerivation)
{
    // SHA256(verifier) encodes to 32 bytes; base64url(32 bytes) = 43 chars.
    const std::string verifier = base64Encode(std::string(32, '\x01'), true, true);
    const std::string sha = encodeSHA256(verifier);
    EXPECT_EQ(sha.size(), 32u);

    const std::string challenge = base64Encode(sha, true, true);
    EXPECT_EQ(challenge.size(), 43u);

    // Challenge must differ from verifier.
    EXPECT_NE(challenge, verifier);

    // Challenge must be deterministic for the same verifier.
    EXPECT_EQ(base64Encode(encodeSHA256(verifier), true, true), challenge);

    // Different verifiers must produce different challenges.
    const std::string verifier2 = base64Encode(std::string(32, '\x02'), true, true);
    EXPECT_NE(base64Encode(encodeSHA256(verifier2), true, true), challenge);
}

#endif // USE_JWT_CPP && USE_SSL

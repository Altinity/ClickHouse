#pragma once
#include <base/types.h>

namespace DB::Cas
{

/// How the backend identifies one physical incarnation of a key (protocol spec §2).
enum class TokenType : uint8_t
{
    ETag = 1,        /// S3-family / Azure
    Generation = 2,  /// GCS (binding deferred; fail-closed until probed)
    Emulated = 3,    /// test backends (in-memory fake, Local emulation)
};

/// A backend-native incarnation token. Opaque; sent back to the backend EXACTLY as observed.
struct Token
{
    String value;
    TokenType type = TokenType::ETag;

    bool empty() const { return value.empty(); }
    bool operator==(const Token &) const = default;
};

}

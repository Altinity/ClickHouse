#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <map>
#include <mutex>

namespace DB::Cas
{

/// Thread-safe, token-enforcing in-memory Backend implementation.
///
/// All successful writes mint a monotonically increasing token (TokenType::Emulated).
/// Tokens NEVER repeat across the lifetime of a backend instance.
///
/// Fault-injection controls are added in a separate layer (Task 4).
class InMemoryBackend final : public Backend
{
public:
    InMemoryBackend() = default;

    std::optional<GetResult> get(const String & key, Range range = {}) override;
    HeadResult head(const String & key) override;
    PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token = nullptr) override;
    PutOutcome putOverwrite(const String & key, const String & bytes, const Token & expected, Token * out_token = nullptr) override;
    CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected, Token * out_token = nullptr) override;
    DeleteOutcome deleteExact(const String & key, const Token & token) override;
    ListPage list(const String & prefix, const String & cursor, size_t limit) override;

private:
    struct Object
    {
        String bytes;
        Token token;
    };

    Token mintToken();

    mutable std::mutex mutex_;
    std::map<String, Object> store_;
    uint64_t token_seq_ = 0;
};

}

#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace DB::Cas
{

/// Thread-safe, token-enforcing in-memory Backend implementation.
///
/// All successful writes mint a monotonically increasing token (TokenType::Emulated).
/// Tokens NEVER repeat across the lifetime of a backend instance.
///
/// Also exposes fault-injection controls for use in Probe tests and CAS correctness tests:
///   - setHoldDeletes / landPendingDelete: simulate async/delayed conditional deletes
///   - failNextCasPut:                     inject a one-shot conflict
///   - setEnforceTokens(false):            mimic a "dumb" backend that ignores token checks
///   - setSimulateDeleteMarkers:           mimic S3 versioning-enabled buckets
///
/// Not `final`: tests subclass it to distort single behaviors (e.g. clamp list page size to force
/// pagination) while delegating everything else to this base.
class InMemoryBackend : public Backend
{
public:
    InMemoryBackend() = default;

    // ---- Backend interface ----

    std::optional<GetResult> get(const String & key, Range range = {}) override;
    HeadResult head(const String & key) override;
    PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token = nullptr,
                           const ObjectMeta & meta = {}) override;
    WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) override;
    PutOutcome putOverwrite(const String & key, const String & bytes, const Token & expected,
                            Token * out_token = nullptr, const ObjectMeta & meta = {}) override;
    CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                      Token * out_token = nullptr, const ObjectMeta & meta = {}) override;
    DeleteOutcome deleteExact(const String & key, const Token & token) override;
    ListPage list(const String & prefix, const String & cursor, size_t limit) override;

    // ---- Fault-injection controls ----

    /// When true, deleteExact enqueues deletes rather than applying them immediately.
    /// The caller sees Deleted (the "send" accepted), but the object remains until landPendingDelete.
    void setHoldDeletes(bool hold);

    /// Number of currently held deletes.
    size_t pendingDeletes() const;

    /// Apply held delete at index i: re-evaluates the token against the current object at LAND time.
    /// Erases the entry regardless (whether TokenMismatch or Deleted).
    DeleteOutcome landPendingDelete(size_t i);

    /// Inject a one-shot artificial Conflict on the next casPut for this key.
    void failNextCasPut(const String & key);

    /// When false, token checks on delete/overwrite/cas are skipped (all match).
    void setEnforceTokens(bool enforce);

    /// When true, successful deletes set created_delete_marker = true in the outcome.
    void setSimulateDeleteMarkers(bool simulate);

private:
    struct Object
    {
        String bytes;
        Token token;
        ObjectMeta meta;
    };

    struct PendingDelete
    {
        String key;
        Token token;
    };

    Token mintToken();
    DeleteOutcome applyDelete(const String & key, const Token & token);

    mutable std::mutex mutex_;
    std::map<String, Object> store_;
    uint64_t token_seq_ = 0;

    // Fault-injection state
    bool hold_deletes_ = false;
    std::vector<PendingDelete> pending_deletes_;
    std::set<String> fail_next_cas_;
    bool enforce_tokens_ = true;
    bool simulate_delete_markers_ = false;
};

}

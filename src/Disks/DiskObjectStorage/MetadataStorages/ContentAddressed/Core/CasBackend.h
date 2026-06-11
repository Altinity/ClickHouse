#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <memory>
#include <optional>
#include <vector>

namespace DB::Cas
{

struct Range
{
    uint64_t offset = 0;
    std::optional<uint64_t> length;   /// nullopt => to the end
    bool whole() const { return offset == 0 && !length; }
};

struct GetResult
{
    String bytes;
    Token token;       /// token of the incarnation the bytes came from
};

struct HeadResult
{
    bool exists = false;
    uint64_t size = 0;
    Token token;
};

enum class PutOutcome : uint8_t
{
    Done,                 /// object written; out_token (if requested) is the new incarnation's token
    PreconditionFailed,   /// If-None-Match hit an existing key / If-Match mismatched — nothing changed
};

enum class CasOutcome : uint8_t
{
    Committed,
    Conflict,             /// expected token (or absence) did not match — nothing changed
};

struct DeleteOutcome
{
    enum class Kind : uint8_t { Deleted, TokenMismatch, NotFound } kind = Kind::NotFound;
    /// TRUE if the backend reported a delete marker was created (versioning enabled) — the probe
    /// fails the pool on this (protocol spec §2: current-object mode requires versioning never enabled).
    bool created_delete_marker = false;
};

struct ListedKey { String key; uint64_t size = 0; };
struct ListPage
{
    std::vector<ListedKey> keys;
    String next_cursor;       /// empty => no more pages
};

/// The ~8-op token-aware storage seam (design §3). TOKEN SEMANTICS ARE THE CONTRACT:
///   - every present key has exactly one current incarnation identified by an opaque Token;
///   - putOverwrite/casPut succeed only against the expected current token (or expected absence);
///   - deleteExact removes ONLY the incarnation whose token matches — wrong token MUST be a
///     TokenMismatch with the object untouched (backends that silently ignore the condition are
///     rejected by Cas::Probe);
///   - conditional PUTs are protocol hygiene; casPut and deleteExact are SAFETY-critical.
///
/// Most ops take/return whole `String` bodies — sufficient for manifests, trees, and probe/GC
/// objects. LARGE content blobs stream through `putIfAbsentStream` (see `WriteSink`); reads stay
/// String-based because blob payload reads go through the wiring's read stack, not this seam.

/// Streaming conditional create (If-None-Match:* semantics). The caller writes the FULL object body
/// (envelope header + payload) into buffer(), then calls finalize() exactly once:
///   - Done                ⇒ the object is durable; out_token (if requested) is the new incarnation's token
///   - PreconditionFailed  ⇒ the key already existed — NOTHING was changed (same contract as putIfAbsent)
/// finalize() may throw on storage errors; PreconditionFailed is an OUTCOME, never an exception.
/// cancel() (or destruction before finalize) abandons the upload: the key is never created by it.
class WriteSink
{
public:
    virtual ~WriteSink() = default;
    virtual WriteBuffer & buffer() = 0;
    virtual PutOutcome finalize(Token * out_token) = 0;
    virtual void cancel() noexcept = 0;
};

using WriteSinkPtr = std::unique_ptr<WriteSink>;

class Backend
{
public:
    virtual ~Backend() = default;

    virtual std::optional<GetResult> get(const String & key, Range range = {}) = 0;   /// nullopt = absent
    virtual HeadResult head(const String & key) = 0;
    virtual PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token = nullptr) = 0;
    /// Streaming variant of putIfAbsent — see WriteSink. Large content blobs use this; whole-String
    /// ops remain for manifests, trees, probe and GC objects.
    virtual WriteSinkPtr putIfAbsentStream(const String & key) = 0;
    virtual PutOutcome putOverwrite(const String & key, const String & bytes, const Token & expected, Token * out_token = nullptr) = 0;
    /// expected == nullopt => create-if-absent CAS (the first write of a root manifest).
    virtual CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected, Token * out_token = nullptr) = 0;
    virtual DeleteOutcome deleteExact(const String & key, const Token & token) = 0;
    virtual ListPage list(const String & prefix, const String & cursor, size_t limit) = 0;
};

using BackendPtr = std::shared_ptr<Backend>;

}

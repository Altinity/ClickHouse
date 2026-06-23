#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <IO/WriteBuffer.h>
#include <base/types.h>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DB::Cas
{

/// User metadata carried alongside an object (S3 x-amz-meta-*). The CA store uses exactly one entry,
/// "cas_owner" = "<server_id_hex>:<epoch>:<build_seq>" — the owner triple the GC watermark reads.
using ObjectMeta = std::map<String, String>;

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
    ObjectMeta attributes;
};

struct HeadResult
{
    bool exists = false;
    uint64_t size = 0;
    Token token;
    ObjectMeta attributes;
};

enum class PutOutcome : uint8_t
{
    Done,                 /// object written; the returned PutResult.token is the new incarnation's token
    PreconditionFailed,   /// If-None-Match hit an existing key / If-Match mismatched — nothing changed
};

enum class CasOutcome : uint8_t
{
    Committed,
    Conflict,             /// expected token (or absence) did not match — nothing changed
};

/// Result of a backend write: the outcome plus the resulting object token (previously a `Token * out_token`
/// out-parameter). `token` is set ONLY when the write actually landed an incarnation (a `Done`/`Committed`
/// outcome); on `PreconditionFailed`/`Conflict` nothing was written and `token` is left default-constructed,
/// exactly mirroring the old contract where callers only read `*out_token` on success.
template <typename Outcome>
struct WriteResultT
{
    Outcome outcome;
    Token token;
};

using PutResult = WriteResultT<PutOutcome>;
using CasResult = WriteResultT<CasOutcome>;

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

/// Streaming conditional create (If-None-Match:* semantics). The caller writes the FULL object body
/// (envelope header + payload) into buffer, then calls finalize exactly once:
///   - Done                ⇒ the object is durable; the returned PutResult.token is the new incarnation's token
///   - PreconditionFailed  ⇒ the key already existed — NOTHING was changed (same contract as putIfAbsent)
/// finalize may throw on storage errors; PreconditionFailed is an OUTCOME, never an exception.
/// cancel (or destruction before finalize) abandons the upload: the key is never created by it.
///
/// MISUSE/LIFETIME CONTRACT: after finalize or cancel the sink is DEAD — any further finalize,
/// cancel, or write into buffer is a programming error (finalize asserts on it in debug builds).
/// The caller must not call the underlying buffer's own finalize/cancel directly — only through
/// the sink. A sink is single-caller: it is NOT thread-safe (only Backend itself is), and it must
/// not outlive the Backend that created it.
class WriteSink
{
public:
    virtual ~WriteSink() = default;
    virtual WriteBuffer & buffer() = 0;
    virtual PutResult finalize() = 0;
    virtual void cancel() noexcept = 0;
};

using WriteSinkPtr = std::unique_ptr<WriteSink>;

/// The ~8-op token-aware storage seam (design §3). TOKEN SEMANTICS ARE THE CONTRACT:
///   - every present key has exactly one current incarnation identified by an opaque Token;
///   - putOverwrite/casPut succeed only against the expected current token (or expected absence);
///   - deleteExact removes ONLY the incarnation whose token matches — wrong token MUST be a
///     TokenMismatch with the object untouched (backends that silently ignore the condition are
///     rejected by Cas::Probe);
///   - conditional PUTs are protocol hygiene; casPut and deleteExact are SAFETY-critical.
///
/// TOKEN ⟹ CONTENT PRECONDITION (read-path caches depend on this): a token must uniquely identify
/// the byte-content of the incarnation it labels — i.e. `head(k).token == prior get(k).token` MUST
/// imply the bytes are unchanged. The protocol's SAFETY only needs the contrapositive (changed
/// bytes ⟹ a new token, so a stale CAS/delete is rejected), but `Cas::Store`'s read-path decode
/// cache (`readShardDecoded`) skips a re-`get`+decode on a token match, so a backend whose token
/// could REPEAT across different content would make it serve stale manifests (wrong results). Holds
/// for every backend in use: S3 ETag is content-derived; the emulated/in-memory backends mint a
/// strictly-monotonic sequence that is never reused. A backend with a weak/recycled token must NOT
/// be used as a Cas pool (and Probe should grow a check for this — tracked in the backlog).
///
/// Most ops take/return whole `String` bodies — sufficient for manifests, trees, and probe/GC
/// objects. LARGE content blobs stream through `putIfAbsentStream` (see `WriteSink`); reads stay
/// String-based because blob payload reads go through the wiring's read stack, not this seam.
class Backend
{
public:
    virtual ~Backend() = default;

    virtual std::optional<GetResult> get(const String & key, Range range = {}) = 0;   /// nullopt = absent
    virtual HeadResult head(const String & key) = 0;
    virtual PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) = 0;
    /// Streaming variant of putIfAbsent — see WriteSink. Large content blobs use this; whole-String
    /// ops remain for manifests, trees, probe and GC objects.
    virtual WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) = 0;
    virtual PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                                   const ObjectMeta & meta = {}) = 0;
    /// expected == nullopt => create-if-absent CAS (the first write of a root manifest).
    virtual CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                             const ObjectMeta & meta = {}) = 0;
    virtual DeleteOutcome deleteExact(const String & key, const Token & token) = 0;
    virtual ListPage list(const String & prefix, const String & cursor, size_t limit) = 0;
};

using BackendPtr = std::shared_ptr<Backend>;

}

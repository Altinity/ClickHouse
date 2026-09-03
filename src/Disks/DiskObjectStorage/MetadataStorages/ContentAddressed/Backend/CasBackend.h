#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasTransportAccess.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteBuffer.h>
#include <Common/Exception.h>
#include <base/defines.h>
#include <base/types.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace DB::ErrorCodes
{
    extern const int CAS_WRITE_UNATTRIBUTED;
    extern const int CORRUPTED_DATA;
    extern const int LOGICAL_ERROR;
    extern const int NOT_IMPLEMENTED;
}

namespace DB::Cas
{

/// User metadata carried alongside an object (S3 x-amz-meta-*). RETIRED: the transport neither
/// writes nor reads attributes, and this alias survives only in the legacy signatures below.
using ObjectMeta = std::map<String, String>;

/// A byte window requested from an object. An absent length means that the window extends to EOF.
/// RETIRED for materialized reads: `get` accepts only a whole-object window. It survives on
/// `getStream`, which is not a forwarder. The offset is exact, while a backend may expose an advisory
/// end when its underlying read buffer cannot enforce one.
struct Range
{
    uint64_t offset = 0;
    std::optional<uint64_t> length;   /// nullopt => to the end
    bool whole() const { return offset == 0 && !length; }
};

/// Materialized object bytes together with the incarnation and user metadata observed by the read.
/// The token identifies the exact object version whose bytes are in `bytes`; callers may use it to
/// validate a subsequent token-conditional mutation.
struct GetResult
{
    String bytes;
    Token token;       /// token of the incarnation the bytes came from
    ObjectMeta attributes;
};

/// A forward-only read of a WRITE-ONCE object (runs, seals): nothing is materialized by the seam.
/// MUTABLE objects (root shards, gc/state, mounts) MUST keep using `get` — their bytes may change
/// under an open stream. `token` identifies the incarnation the stream reads, same as `get`.
struct GetStreamResult
{
    std::unique_ptr<ReadBuffer> stream;
    Token token;
};

/// Metadata returned by `Backend::head`. For an absent key, `exists` is false and the other fields
/// retain their defaults; for a present key, `size`, `token`, and `attributes` describe one current
/// incarnation as observed by the backend.
struct HeadResult
{
    bool exists = false;
    uint64_t size = 0;
    Token token;
    ObjectMeta attributes;
};

/// Outcome of a write-once create or a token-conditional overwrite. A precondition failure means
/// that the backend preserved the existing object; it is an expected result, not an exception.
enum class PutOutcome : uint8_t
{
    Done,                 /// object written; the returned PutResult.token is the new incarnation's token
    PreconditionFailed,   /// If-None-Match hit an existing key / If-Match mismatched — nothing changed
};

/// Outcome of a compare-and-set write. `Conflict` means that the expected token (or expected
/// absence) did not match and that the backend left the object unchanged.
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

/// Result of deleting one exact incarnation. `TokenMismatch` and `NotFound` are deliberately
/// distinct: the former proves that another incarnation is now current, while the latter means
/// there is no object to remove. `created_delete_marker` exposes a storage-versioning behavior
/// that is incompatible with current-object reclamation.
struct DeleteOutcome
{
    enum class Kind : uint8_t { Deleted, TokenMismatch, NotFound } kind = Kind::NotFound;
    /// TRUE if the backend reported a delete marker was created because versioning is enabled. The
    /// capability probe rejects this for the current-object storage model: exact deletion must reclaim
    /// the current object rather than archive a noncurrent version.
    bool created_delete_marker = false;
};

/// A key returned by `Backend::list`. The `token` field is populated ONLY when the backend
/// returns TRUE from `supportsListTokens` — it identifies the key's current incarnation, matching
/// what `head` would return for the same key at that instant. Callers that do not need the token
/// (e.g. GC fence sweep, orphan sweep) ignore the field; GC discover uses it to skip unchanged
/// root shards.
struct ListedKey
{
    String key;
    uint64_t size = 0;
    std::optional<Token> token;   /// present iff supportsListTokens() == true
};
/// One page returned by `Backend::list`. `keys` contains only the requested prefix and the cursor
/// resumes strictly after the last returned key; an empty cursor marks the end of the enumeration.
struct ListPage
{
    std::vector<ListedKey> keys;
    String next_cursor;       /// Last returned key; empty => no more pages.
};

/// Typed erasure evidence for one key or one prefix. `head`/
/// `get` deliberately flatten every kind of miss (a clean absence, a missing bucket/container, a
/// permission failure, a transport fault) into one "not found" result, which is exactly right for
/// their callers (a plain read) but wrong for lifecycle recovery, which must never treat a
/// transport/permission failure as proof that data is gone. `ProbeOutcome` keeps the four cases
/// distinct: only a backend's OWN authoritative "not found" evidence earns `KeyAbsent` — a timeout,
/// a 5xx, or an unclassifiable error is ALWAYS `Indeterminate`, never promoted to absence.
enum class ProbeOutcome : uint8_t
{
    Present,           /// the key (or, for a prefix probe, at least one object under it) exists
    KeyAbsent,         /// authoritative miss: the container is alive, the key itself is not there
    ContainerAbsent,   /// the bucket/prefix-parent itself is gone, not merely the key
    AccessDenied,      /// the probe was rejected on permissions — absence was never established
    Indeterminate,     /// a transport/timeout/unclassifiable error — absence was NEVER proven
};

/// Result of `Backend::probeSentinelRaw`. `body` carries the materialized bytes only when the outcome is
/// `Present`.
struct SentinelProbeResult
{
    ProbeOutcome outcome;
    std::optional<String> body;
};

/// Re-readable payload transport for one unconditional blob publication. `fresh_envelope` is the
/// complete CAS envelope to prepend, while `payload_size` is the exact number of bytes the source
/// must yield before the backend can make the destination visible.
struct StreamingBlobPublication
{
    uint64_t payload_size;
    String fresh_envelope;
    std::function<std::unique_ptr<ReadBuffer>()> open_payload;
};

/// A complete, already-size-verified CAS object that can be copied byte-for-byte to the destination.
/// This transport requires a provider-native same-store copy; the backend must never replace it with
/// a client-side read/write fallback.
struct VerbatimStagedBlobPublication
{
    String object_key;
    uint64_t object_size;
};

using BlobPublication = std::variant<StreamingBlobPublication, VerbatimStagedBlobPublication>;

/// Transport-only request for an unconditional blob rewrite. Publication does not inspect destination
/// state or freshness metadata and does not produce an incarnation token.
struct BlobPublishRequest
{
    String destination_key;
    BlobPublication publication;
};

namespace blob_publication_detail
{

struct BlobPayloadCopyResult
{
    uint64_t copied = 0;
    bool has_excess = false;

    bool exact(uint64_t expected) const
    {
        return copied == expected && !has_excess;
    }
};

/// Copy no more than the declared payload, then consume one byte solely to distinguish exact input
/// from a long source. The probe byte never reaches `to`, so callers can cancel without having sent
/// more than the promised body to their destination transport.
inline BlobPayloadCopyResult copyBlobPayloadBounded(ReadBuffer & from, WriteBuffer & to, uint64_t expected)
{
    uint64_t remaining = expected;
    while (remaining != 0 && !from.eof())
    {
        const size_t available = static_cast<size_t>(from.buffer().end() - from.position());
        const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, available));
        to.write(from.position(), count);
        from.position() += count;
        remaining -= count;
    }

    BlobPayloadCopyResult result{.copied = expected - remaining};
    if (remaining == 0)
    {
        char excess;
        result.has_excess = from.read(excess);
    }
    return result;
}

}

/// Token-aware storage seam used by the content-addressed pool. TOKEN SEMANTICS ARE THE CONTRACT:
///   - every present key has exactly one current incarnation identified by an opaque Token;
///   - putOverwrite/casPut succeed only against the expected current token (or expected absence);
///   - deleteExact removes ONLY the incarnation whose token matches — wrong token MUST be a
///     TokenMismatch with the object untouched (backends that silently ignore the condition are
///     rejected by `Cas::Probe`);
///   - conditional PUTs are protocol hygiene; casPut and deleteExact are SAFETY-critical.
///
/// TOKEN ⟹ CONTENT PRECONDITION (read-path caches depend on this): a token must uniquely identify
/// the byte-content of the incarnation it labels — i.e. `head(k).token == prior get(k).token` MUST
/// imply the bytes are unchanged. The protocol's SAFETY only needs the contrapositive (changed
/// bytes ⟹ a new token, so a stale CAS/delete is rejected), but `Cas::Pool`'s read-path decode
/// cache (`readShardDecoded`) skips a re-`get`+decode on a token match, so a backend whose token
/// could REPEAT across different content would make it serve stale manifests (wrong results). Holds
/// for every backend in use: S3 ETag is content-derived; the emulated/in-memory backends mint a
/// strictly-monotonic sequence that is never reused. A backend with a weak/recycled token must NOT
/// be used as a Cas pool. The capability probe currently verifies conditional-operation behavior but
/// does not test token non-reuse across different contents, so this invariant remains a requirement
/// of every backend implementation.
///
/// Most ops take/return whole `String` bodies — sufficient for manifests, trees, and probe/GC
/// objects. Large content blobs use the transport-only `publishBlob` seam; reads stay String-based
/// because blob payload reads go through the wiring's read stack, not this seam.
class Backend
{
public:
    Backend();
    virtual ~Backend() = default;

    /// ---- The transport primitives ----
    ///
    /// These are the ONLY methods that reach the store. Each takes a `TransportAccess`, which nothing
    /// outside `CasRequests` can construct, so no caller can reach the store without the request
    /// contract's retry, deadline and fence rules. They deal in the store's own strings: an
    /// incarnation VALUE means no more here than "what the store answered", and every grammar,
    /// key-binding and dialect check on it belongs to `CasRequests`.

    /// An object's bytes together with the value naming the incarnation they were read from.
    struct Raw     { String bytes; String value; };
    /// One object's size and incarnation value, without its body.
    struct RawMeta { uint64_t size; String value; };
    /// One listed key; `value` is present only on a backend that surfaces per-key incarnations
    /// through LIST -- see `supportsListTokens`.
    struct RawListedKey { String key; uint64_t size; std::optional<String> value; };
    struct RawListPage  { std::vector<RawListedKey> keys; String next_cursor; };
    /// The store refused the write's precondition. Nothing was written; what the key holds now is
    /// whatever a read finds. An expected outcome, never an error.
    struct RawConflict {};
    /// `DeleteMarker` is a removal that did NOT reclaim: a versioned bucket archived a noncurrent
    /// version instead. Distinct from `Removed` because reclaiming the storage is the point.
    enum class RawRemoval : uint8_t { Removed, Gone, Mismatch, DeleteMarker };

    /// Reads the whole object, or nullopt when the key is absent.
    virtual std::optional<Raw>     read  (const String & key, TransportAccess &) = 0;
    /// One point-in-time observation of the current incarnation's size and value; nullopt when absent.
    virtual std::optional<RawMeta> head  (const String & key, TransportAccess &) = 0;
    /// One page of keys under `prefix`, resuming strictly after `cursor`; an empty `next_cursor`
    /// marks the end of the enumeration.
    virtual RawListPage            list  (const String & prefix, const String & cursor, size_t limit, TransportAccess &) = 0;
    /// Removes ONLY the incarnation whose value equals `expected_value`; a mismatch must leave the
    /// object untouched.
    virtual RawRemoval             remove(const String & key, const String & expected_value, TransportAccess &) = 0;
    /// Creates the key (`expected_value == nullopt`) or replaces exactly the incarnation named by
    /// `expected_value`. Returns the store's own value for what it just wrote, UNVALIDATED: a value
    /// that fails the dialect grammar means the write may still have landed, which the caller settles
    /// by reading the key back, and which this seam must never report as corruption.
    virtual std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                                     const std::optional<String> & expected_value, TransportAccess &) = 0;
    /// A forward-only read of a WRITE-ONCE object (runs, seals): nothing is materialized by the seam.
    /// MUTABLE objects (root shards, gc/state, mounts) MUST use `read` -- their bytes may change
    /// under an open stream. Null when the key is absent.
    virtual std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess &) = 0;
    /// Executes one unconditional blob publication and returns once the complete destination is
    /// visible. Transport only: it observes no destination state and produces no incarnation.
    virtual void publish(const BlobPublishRequest & request, TransportAccess &) = 0;

    /// Authoritative, cache-bypassing probe of one key -- see `ProbeOutcome`. DEFAULT (used by every
    /// backend without sharper raw-error evidence, e.g. `InMemoryBackend`): derived from `head`/`read`
    /// alone, so it can only distinguish `Present` from `KeyAbsent`, and ANY exception from either
    /// call is `Indeterminate` -- never promoted to `KeyAbsent`. A backend able to surface real
    /// container/permission evidence (the S3-native and Local paths of `ObjectStorageBackend`)
    /// overrides this to sharpen the classification.
    virtual SentinelProbeResult probeSentinelRaw(const String & key, TransportAccess & access)
    {
        try
        {
            const auto meta = head(key, access);
            if (!meta)
                return {ProbeOutcome::KeyAbsent, std::nullopt};
            auto raw = read(key, access);
            /// Vanished between the two: still a clean, authoritative miss, not an error.
            if (!raw)
                return {ProbeOutcome::KeyAbsent, std::nullopt};
            return {ProbeOutcome::Present, std::move(raw->bytes)};
        }
        catch (...)
        {
            return {ProbeOutcome::Indeterminate, std::nullopt};
        }
    }

    /// The dialect this backend mints its incarnation values in.
    virtual Dialect dialect() const = 0;

    /// Identifies this backend INSTANCE, so an incarnation observed elsewhere can be refused rather
    /// than used as a precondition here. Assigned at construction and never reused in this process.
    uint64_t backendId() const { return backend_id; }

    /// The budget for one HTTP attempt in milliseconds; 0 when this backend has no such notion. The
    /// request contract reserves it before every attempt it starts.
    virtual uint64_t attemptTimeoutMs() const { return 0; }

    /// Asks the storage to re-acquire credentials. TRUE when fresh ones were installed, so the
    /// caller's reissue can sign with them; FALSE when this backend has no refresh mechanism, which
    /// makes an expired-credential failure terminal for the caller's policy rather than retryable.
    virtual bool refreshCredentials() { return false; }

    /// Capability fact about the LIST seam: TRUE iff this backend can surface a per-key incarnation
    /// value through `list` (i.e. each `RawListedKey` carries a value that uniquely identifies the
    /// current incarnation of that key, matching what `head` would return).
    ///
    /// Why this matters: S3 ETags are content-derived and are returned in list responses; the
    /// in-memory backend mints a monotonic value it can also surface through `list`. A backend that
    /// cannot surface per-key values through `list` MUST return FALSE.
    ///
    /// FALSE ⇒ GC `discover` must read every root-shard body to learn the current incarnation (fail
    /// closed). TRUE  ⇒ `discover` may skip an unchanged root-shard body read when the listed value
    /// equals the persisted folded one, saving a GET per unchanged shard.
    virtual bool supportsListTokens() const = 0;

    /// Pool-level preconditions beyond per-op conditional semantics — checked by the capability
    /// probe BEFORE the op battery. Default: nothing to check. The S3 backend refuses here when a
    /// generation-dialect (GCS) bucket is verified to have object versioning enabled: a token-exact
    /// DELETE against a versioned bucket archives a noncurrent generation instead of reclaiming
    /// storage, so GC "reclaim" would silently stop reclaiming. A probe that cannot answer is not
    /// evidence of that, so it warns and the mount proceeds.
    virtual void checkPoolPreconditions() {}

    /// Fail-closed precondition: may this backend serve a WRITABLE mount that skips the access-check
    /// battery? `PoolConfig::skip_access_check` is a preflight convenience, so it is available only to
    /// backends whose correctness does not depend on the battery having run. Default: available.
    /// See ObjectStorageBackend's override for the one combination that refuses it.
    virtual void checkSkipAccessCheckSupport() {}

    /// Fail-closed precondition: a Native-mode backend MUST have a
    /// working single-attempt conditional-write path before it coordinates a WRITABLE pool — silently
    /// running CAS conditional writes under the disk's default (~500-attempt) transparent retry policy
    /// is exactly the hazard this seam forbids. Checked by the capability probe alongside
    /// checkPoolPreconditions. Default: nothing to check (EmulatedSingleProcess and non-S3 backends
    /// are not gated here — see ObjectStorageBackend's override for the one backend that is).
    virtual void checkConditionalWriteSingleAttemptSupport() {}

    /// ---- The legacy Token-typed surface ----
    ///
    /// Every one of these obtains the migration key and calls the primitive above, so a fault
    /// injection written against a PRIMITIVE intercepts a legacy caller too. They stay virtual while
    /// the migration runs, so a test double that overrides one of THESE keeps working until the site
    /// it instruments moves; the whole block, and `migrationAccess` with it, is deleted at the lock.
    /// `Range` and `ObjectMeta` are already retired: a non-whole window is refused, and object
    /// attributes are neither written nor returned.
    virtual std::optional<GetResult> get(const String & key, Range range)
    {
        if (!range.whole())
            throw Exception(ErrorCodes::NOT_IMPLEMENTED, "CAS backend: a ranged get is retired; read the object whole");
        auto access = migrationAccess();
        auto raw = read(key, access);
        if (!raw)
            return std::nullopt;
        return GetResult{std::move(raw->bytes), legacyMintObserved(key, std::move(raw->value)), {}};
    }
    std::optional<GetResult> get(const String & key) { return get(key, {}); }

    /// Forward-only stream over the object's `range` (default: whole object) for WRITE-ONCE objects
    /// (runs, seals). Not a forwarder: `stream` returns no incarnation, and a forwarder would have to
    /// fill `GetStreamResult::token` with a default-constructed `Token` that names nothing. Each
    /// backend keeps its own implementation until the last caller moves onto `stream`.
    /// CAVEAT: the window END is advisory on storages where `setReadUntilPosition` is a hint
    /// (LocalObjectStorage) — the stream may yield bytes past the window; consumers MUST bound their
    /// own consumption (RunFileReader bounds to its data_end). The window START is always exact.
    virtual std::optional<GetStreamResult> getStream(const String & key, Range range) = 0;
    std::optional<GetStreamResult> getStream(const String & key) { return getStream(key, {}); }

    virtual HeadResult head(const String & key)
    {
        auto access = migrationAccess();
        auto raw = head(key, access);
        if (!raw)
            return {};
        return HeadResult{true, raw->size, legacyMintObserved(key, std::move(raw->value)), {}};
    }

    virtual PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & /*meta*/)
    {
        auto access = migrationAccess();
        auto r = write(key, bytes, std::nullopt, access);
        if (!r)
            return PutResult{PutOutcome::PreconditionFailed, {}};
        return PutResult{PutOutcome::Done, legacyMintWritten(key, std::move(*r))};
    }
    PutResult putIfAbsent(const String & key, const String & bytes) { return putIfAbsent(key, bytes, {}); }

    virtual PutResult putOverwrite(const String & key, const String & bytes, const Token & expected,
                                   const ObjectMeta & /*meta*/)
    {
        if (legacyTokenIsForeign(key, expected))
            return PutResult{PutOutcome::PreconditionFailed, {}};
        auto access = migrationAccess();
        auto r = write(key, bytes, expected.value, access);
        if (!r)
            return PutResult{PutOutcome::PreconditionFailed, {}};
        return PutResult{PutOutcome::Done, legacyMintWritten(key, std::move(*r))};
    }
    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected)
    {
        return putOverwrite(key, bytes, expected, {});
    }

    virtual CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                             const ObjectMeta & /*meta*/)
    {
        if (expected && legacyTokenIsForeign(key, *expected))
            return CasResult{CasOutcome::Conflict, {}};
        auto access = migrationAccess();
        auto r = write(key, bytes, expected ? std::optional<String>(expected->value) : std::nullopt, access);
        if (!r)
            return CasResult{CasOutcome::Conflict, {}};
        return CasResult{CasOutcome::Committed, legacyMintWritten(key, std::move(*r))};
    }
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected)
    {
        return casPut(key, bytes, expected, {});
    }

    virtual DeleteOutcome deleteExact(const String & key, const Token & token)
    {
        if (legacyTokenIsForeign(key, token))
            return DeleteOutcome{DeleteOutcome::Kind::TokenMismatch, false};
        auto access = migrationAccess();
        switch (remove(key, token.value, access))
        {
            case RawRemoval::Removed:      return {DeleteOutcome::Kind::Deleted, false};
            case RawRemoval::Gone:         return {DeleteOutcome::Kind::NotFound, false};
            case RawRemoval::Mismatch:     return {DeleteOutcome::Kind::TokenMismatch, false};
            case RawRemoval::DeleteMarker: return {DeleteOutcome::Kind::Deleted, true};
        }
        UNREACHABLE();
    }

    virtual ListPage list(const String & prefix, const String & cursor, size_t limit)
    {
        auto access = migrationAccess();
        auto raw = list(prefix, cursor, limit, access);
        ListPage page;
        page.next_cursor = std::move(raw.next_cursor);
        page.keys.reserve(raw.keys.size());
        for (auto & k : raw.keys)
        {
            std::optional<Token> token;
            if (k.value)
                token = legacyMintObserved(k.key, std::move(*k.value));
            page.keys.push_back(ListedKey{std::move(k.key), k.size, std::move(token)});
        }
        return page;
    }

    virtual void publishBlob(const BlobPublishRequest & request)
    {
        auto access = migrationAccess();
        publish(request, access);
    }

    virtual SentinelProbeResult probeSentinelRaw(const String & key)
    {
        auto access = migrationAccess();
        return probeSentinelRaw(key, access);
    }

protected:
    /// The migration key. Every legacy forwarder above obtains one; nothing else may, and the whole
    /// mechanism is deleted with the forwarders at the lock.
    static TransportAccess migrationAccess() { return TransportAccess{}; }

    /// ---- Where a raw response becomes a legacy `Token` ----
    ///
    /// The primitives return the store's value as it arrived: judging it belongs to the caller that
    /// can act on the judgement. A legacy caller cannot -- it puts the `Token` straight into its next
    /// request -- so these two are where a legacy response is judged, and the ONLY places a legacy
    /// `Token` is minted. A backend that overrides a legacy method for the migration window mints
    /// through them too.

    /// An OBSERVED value (read, head, list) that is not an incarnation means the response fell
    /// through unmapped: nothing was changed by it, and the caller must not act on it.
    Token legacyMintObserved(const String & key, String value)
    {
        if (!isIncarnationValue(dialect(), value))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS backend: the store answered for '{}' with a value '{}' that is not a valid incarnation",
                key, value);
        return Token{std::move(value), dialect()};
    }

    /// A value from a SUCCESSFUL write that is not an incarnation is the ambiguous case, not the
    /// corrupt one: the write may well have landed, and only reading the key back can say. Hence
    /// `CAS_WRITE_UNATTRIBUTED`, which names that duty, and never `CORRUPTED_DATA`, which a caller
    /// may treat as a deterministic failure and stop.
    Token legacyMintWritten(const String & key, String value)
    {
        if (!isIncarnationValue(dialect(), value))
            throw Exception(ErrorCodes::CAS_WRITE_UNATTRIBUTED,
                "CAS backend: the store accepted a write of '{}' but answered with '{}', which is not an "
                "incarnation; the write may have committed and must be resolved by reading back",
                key, value);
        return Token{std::move(value), dialect()};
    }

    /// The dialect half of the legacy token check: the primitives take a bare VALUE and cannot see
    /// the dialect a `Token` declares, so a foreign-dialect token is answered here as an ordinary
    /// non-match rather than being forwarded to a wire (or a value space) that was never designed to
    /// discriminate it. A MALFORMED value is refused FIRST, under its own declared dialect, so a
    /// token that is both malformed and foreign is still reported as the caller bug it is.
    bool legacyTokenIsForeign(const String & key, const Token & token)
    {
        if (!isIncarnationValue(token.type, token.value))
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "CAS backend: refusing a conditional mutation of '{}' with a malformed token '{}' (dialect {}): "
                "an empty, wildcard or list token would turn the precondition into an unconditional write",
                key, token.value, static_cast<int>(token.type));
        return token.type != dialect();
    }

private:
    uint64_t backend_id;
};

inline Backend::Backend()
{
    /// Per instance, monotonic, never reused: an incarnation carries the id of the backend that
    /// observed it, so it can be refused anywhere else. Starts at 1, leaving 0 naming no backend.
    static std::atomic<uint64_t> next_backend_id{1};
    backend_id = next_backend_id.fetch_add(1, std::memory_order_relaxed);
}

using BackendPtr = std::shared_ptr<Backend>;

/// Walk every key under `prefix` exactly once, resuming by the backend's explicit last-returned-key
/// cursor (`ListPage::next_cursor`, empty => done). This centralizes the pagination contract shared by
/// GC, fsck, and cleanup sweeps: each returned key is delivered once, and the backend's cursor is the
/// only state used to request the next page.
///
/// `on_page_fetched`, if set, fires exactly once per physical `backend.list` call (including an
/// empty/undersized final page) — a GC-owned caller's hook for a page-level ProfileEvents counter,
/// without misattributing a non-GC caller (e.g. fsck) that leaves it unset. Trails `page_limit`
/// (rather than sitting before it) so the two existing callers that override `page_limit`
/// (`Gc::fold`, `CasFsck.cpp`'s `listAll`) can override `page_limit` without changing callback order.
inline void forEachListedKey(Backend & backend, const String & prefix,
                             const std::function<void(const ListedKey &)> & cb,
                             size_t page_limit = 1000,
                             const std::function<void()> & on_page_fetched = {})
{
    String cursor;
    for (;;)
    {
        const ListPage page = backend.list(prefix, cursor, page_limit);
        if (on_page_fetched)
            on_page_fetched();
        for (const ListedKey & k : page.keys)
            cb(k);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

/// The normalized verdict of a token-exact delete, unifying the DeleteOutcome::Kind three-way that GC
/// (blob + manifest delete) and the orphan-manifest sweep each mapped by hand.
enum class DeleteClass : uint8_t { Deleted, Absent, Replaced };

/// Converts a backend-specific delete outcome into the three states used by cleanup callers. The
/// default branch is fail-safe: an unknown value is treated as `Replaced`, so cleanup never reports
/// an unverified deletion as successful.
inline DeleteClass classifyDeleteOutcome(const DeleteOutcome & d)
{
    switch (d.kind)
    {
        case DeleteOutcome::Kind::Deleted:       return DeleteClass::Deleted;
        case DeleteOutcome::Kind::NotFound:      return DeleteClass::Absent;
        case DeleteOutcome::Kind::TokenMismatch: return DeleteClass::Replaced;
    }
    return DeleteClass::Replaced;   /// unreachable; fail-safe toward "leave it" (never a false Deleted)
}

/// Returns the stable lowercase label used when reporting a normalized delete result. Unknown enum
/// values are labeled `replaced`, matching `classifyDeleteOutcome`'s fail-safe behavior.
inline std::string_view deleteClassName(DeleteClass c)
{
    switch (c)
    {
        case DeleteClass::Deleted:  return "deleted";
        case DeleteClass::Absent:   return "absent";
        case DeleteClass::Replaced: return "replaced";
    }
    return "replaced";
}

}

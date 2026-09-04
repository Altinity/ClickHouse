#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <deque>
#include <map>
#include <mutex>

#include "config.h"

namespace DB::Cas
{

/// Fold and point GETs commonly read tiny bodies (about 3.7 KiB on the measured workload), while the
/// default `ReadBufferFromS3` allocation is about 1 MiB. If the caller already knows the object size,
/// use `ReadSettings::adjustBufferSize` to request a buffer of `known_size + slack`, without exceeding
/// the caller's configured default. A zero `known_size` means that the size is unknown and preserves
/// the supplied settings unchanged.
constexpr uint64_t CAS_FOLD_READ_SLACK_BYTES = 4096;
ReadSettings casSizedReadSettings(const ReadSettings & base, uint64_t known_size);

#if USE_AWS_S3
namespace detail
{
/// Finalize a conditional write (the condition rode on the buffer's WriteSettings) and map a
/// precondition loss to an OUTCOME — anything else propagates. This is the classifier for the
/// typed `S3Exception` signal; exposed here for unit tests only — production callers go through
/// `ObjectStorageBackend`. See the definition for the exact matching rules.
PutOutcome finalizeConditionalWrite(WriteBuffer & buf);
}
#endif

/// Production Backend over IObjectStorage.
///
/// Native mode (S3-like): conditions ride the existing plumbing — WriteSettings
/// object_storage_write_if_none_match / object_storage_write_if_match (consumed by WriteBufferFromS3)
/// and IObjectStorage::removeObjectIfTokenMatches. Tokens are backend ETags from getObjectMetadata.
/// Trust is NEVER assumed: Cas::Probe validates enforcement per pool at open.
///
/// EmulatedSingleProcess mode (LocalObjectStorage — tests and local development ONLY): the object
/// storage has no conditional ops, so this adapter provides EXACT token semantics itself with a
/// process-wide mutex and an in-memory per-key token MINTED FROM the object's own etag (mtime-ns on
/// LocalObjectStorage) — see emuMintToken. Every emulated token IS the object's current etag (not
/// merely "seeded" for pre-existing keys); this is what keeps token-exact semantics correct ACROSS a
/// process restart, which a plain in-process counter cannot do (codex-review-triage §3.18, 19c): a
/// counter restarts at 0 and can re-mint a value colliding with a persisted pre-restart delete token
/// for a completely different incarnation, while a republished body's mtime is always later. Semantics
/// otherwise hold within ONE process only — exactly what unit tests need.
class ObjectStorageBackend final : public Backend
{
public:
    /// Unhide the base overloads this class's own declarations would otherwise shadow: the
    /// convenience forms that omit `Range`, and the keyed primitives that share a legacy name.
    using Backend::get;
    using Backend::getStream;
    using Backend::head;
    using Backend::list;
    using Backend::probeSentinelRaw;

    enum class Mode { Native, EmulatedSingleProcess };

    /// Construct a backend over `object_storage`. Native mode uses the storage's conditional
    /// operations and native token dialect; `EmulatedSingleProcess` serializes operations locally for
    /// tests and local development. A Native generation-token store must use a single PUT because
    /// its multipart completion path does not enforce the precondition.
    ///
    /// `single_attempt_control_plane` selects the SingleAttempt retry profile for the READ-class
    /// requests below: a writable Native mount owns its own retry policy and a transparently retried
    /// request would outlive the caller's deadline, while a read-only mount has no such deadline and
    /// keeps the storage's default. `attempt_timeout_ms` bounds ONE attempt of those requests; 0
    /// leaves the storage's own timeout in place. Both are supplied by the mount that opens the pool;
    /// the defaults are what a narrow unit test constructing a bare backend gets.
    ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_,
                         bool single_attempt_control_plane_ = false, uint64_t attempt_timeout_ms_ = 0);

    /// Read the whole object, or return `nullopt` if it is absent. Native mode reads the incarnation
    /// value out of the GET response itself, so no HEAD precedes it; a not-found race is reported as
    /// `nullopt`, while unrelated storage errors propagate.
    std::optional<Raw> read(const String & key, TransportAccess & access) override;
    /// Return the current size and incarnation value, or `nullopt` when the key is absent.
    std::optional<RawMeta> head(const String & key, TransportAccess & access) override;
    /// Return a page after `cursor`; the next cursor is the last returned key and is empty at the end.
    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override;
    /// Remove only the incarnation named by `expected_value`, preserving the object on a mismatch and
    /// reporting a versioned bucket's delete marker as the non-reclaiming removal it is.
    RawRemoval remove(const String & key, const String & expected_value, TransportAccess & access) override;
    /// Create the key (`expected_value == nullopt`) or replace exactly the incarnation it names. The
    /// returned value is the write response's own; a response that carries none at all is
    /// `CAS_WRITE_UNATTRIBUTED`, never patched over by a follow-up read.
    std::expected<String, RawConflict> write(const String & key, const String & bytes,
                                             const std::optional<String> & expected_value, TransportAccess & access) override;
    /// Open a forward-only whole-object stream for a write-once object, or null when the key is
    /// absent. Nothing is materialized; mutable objects must use `read` because their contents may
    /// change while the stream is open.
    std::unique_ptr<ReadBuffer> stream(const String & key, TransportAccess & access) override;
    /// Execute the selected unconditional blob transport without observing destination state or
    /// returning a write-response value. Streaming uses ordinary write settings; staged bytes require
    /// a native same-store copy.
    void publish(const BlobPublishRequest & request, TransportAccess & access) override;
    /// Native mints its store's own dialect (ETag or GCS generation); the emulated adapter mints its
    /// own values.
    Dialect dialect() const override { return mode == Mode::Native ? native_token_type : Dialect::Emulated; }
    /// The budget for one attempt of a read-class request, as configured by the mount.
    uint64_t attemptTimeoutMs() const override { return attempt_timeout_ms; }
    /// Ask the storage to re-acquire credentials through its refresh callback.
    bool refreshCredentials() override { return object_storage->tryRefreshCredentialsViaCallback(); }

    /// ---- The legacy surface, kept for the migration window ----
    ///
    /// Not inherited from `Backend`: its forwarders would issue these requests under the profile the
    /// keyed primitives use, which on a writable Native mount is SingleAttempt. That is right for a
    /// primitive -- `CasRequests` is the retry loop around it -- and wrong for a legacy caller, which
    /// has no loop at all and would lose the storage's own retries on the first blip. These keep the
    /// storage's default profile until their callers move onto the engine, and are deleted at the
    /// lock. Each mints its `Token` through the base's legacy mint, so a malformed response is
    /// judged in exactly one place.
    std::optional<GetResult> get(const String & key, Range range) override;
    HeadResult head(const String & key) override;
    ListPage list(const String & prefix, const String & cursor, size_t limit) override;
    DeleteOutcome deleteExact(const String & key, const Token & token) override;
    SentinelProbeResult probeSentinelRaw(const String & key) override;
    /// Open a forward-only ranged stream for a write-once object. See `Backend::getStream` for why it
    /// is not a forwarder.
    std::optional<GetStreamResult> getStream(const String & key, Range range) override;
    /// S3 ETags are content-derived and surfaced in list responses — TRUE for ETag-token Native
    /// and EmulatedSingleProcess modes. FALSE on a generation-token store (GCS): the XML LIST
    /// surfaces MD5-style ETags in the response BODY, which the header-level response adaptation
    /// cannot map to generations. A list-derived token would therefore be an invalid
    /// `If-Match` token; generation stores deliberately omit it and make GC re-read each shard.
    /// Consumers already treat absent list tokens as Read/fail-closed (GC discover re-reads every
    /// shard — a cost, not a correctness change).
    bool supportsListTokens() const override { return native_token_type != TokenType::Generation; }

    /// Pool-level precondition: on a Native, generation-dialect (GCS) backend, reject the pool when
    /// object versioning is verified ENABLED; warn and continue when the probe cannot answer — see
    /// Backend::checkPoolPreconditions.
    void checkPoolPreconditions() override;

    /// Fail-closed precondition: a Native, generation-dialect (GCS) backend refuses a writable mount
    /// that asked to skip the access check — see Backend::checkSkipAccessCheckSupport.
    void checkSkipAccessCheckSupport() override;

    /// Fail-closed precondition for writable Native mode: require that the object storage supports the
    /// SingleAttempt retry profile (ObjectStorageRetryProfile), which disables transparent
    /// conditional-write retries. Without it, an SDK retry could cross the mount lease boundary or turn
    /// an uncertain result into a misleading precondition failure. A non-S3 object storage used for
    /// test construction reports no support; this check is the mount-time gate. No-op for
    /// `EmulatedSingleProcess`.
    void checkConditionalWriteSingleAttemptSupport() override;

    /// See Backend::probeSentinelRaw. Native: ONE `read`, classified by the S3 error it throws. A GET
    /// 404 carries a response body, so the SDK can parse its `<Code>` and tell `NoSuchKey` from
    /// `NoSuchBucket` -- a distinction a bodyless HEAD 404 cannot make.
    /// EmulatedSingleProcess (Local): stats the configured container directory (`emu_root`) first --
    /// `ContainerAbsent` if it is gone -- then the key.
    SentinelProbeResult probeSentinelRaw(const String & key, TransportAccess & access) override;

    /// The token kind this backend's object storage mints: TokenType::ETag for AWS-compatible
    /// stores, TokenType::Generation when the storage mints GCS generations (the
    /// generation rides the ETag plumbing; the VALUE stays opaque either way).
    TokenType nativeTokenType() const { return native_token_type; }
    void setNativeTokenTypeForTest(TokenType t) { native_token_type = t; }

    /// ---- Token policy (single source of truth; see the .cpp) ----
    /// A GCS generation reaches this layer through the AWS SDK's ETag field, which the HTTP boundary
    /// fills with an ETag-shaped — that is, quoted — value. A generation is a number, and quotes are
    /// transport syntax that must not enter CAS protocol state, where token values are compared for
    /// equality and written into persisted manifests. Strip them here, where the meaning changes from
    /// "an ETag field" to "an incarnation token".
    ///
    /// Generation-scoped on purpose: an ETag-dialect token IS the quoted ETag, and those quotes are
    /// required syntax when the value goes back out as `If-Match`. Stripping unconditionally would
    /// corrupt the AWS-compatible path.
    String normalizeTokenValue(const String & etag) const
    {
        if (native_token_type != TokenType::Generation)
            return etag;
        if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"')
            return etag.substr(1, etag.size() - 2);
        return etag;
    }

    /// The `Token` form of an observed ETag/generation: normalized, and stamped with this backend's
    /// native dialect. The transport itself deals in bare values; this is the normalize-and-stamp
    /// step on its own, with `tokenForList` as its LIST-side sibling.
    Token tokenForHead(const String & etag) const
    {
        return Token{normalizeTokenValue(etag), native_token_type};
    }

    /// The token to surface for a LISTED key: present iff this backend surfaces per-key list tokens
    /// (supportsListTokens — FALSE on a generation store, where a list-derived token is a poisoned
    /// If-Match) AND the listing carried a non-empty etag. Matches what tokenForHead would return.
    std::optional<Token> tokenForList(const String & etag) const
    {
        if (!supportsListTokens() || etag.empty())
            return std::nullopt;
        return Token{etag, native_token_type};
    }

    /// Whether an observed incarnation token satisfies an expected one: exact identity (value AND
    /// type). Every conditional compare in this backend goes through here.
    static bool tokenMatches(const Token & observed, const Token & expected)
    {
        return observed == expected;
    }

    /// The per-dialect grammar a response value must meet to be an incarnation. Generation: canonical
    /// positive decimal AFTER the SDK ETag-field quote strip (no leading zero, not "0" — zero is the
    /// dialect's absence sentinel). ETag: non-empty, not "*" after trimming whitespace, no comma (a
    /// list matches any member). Emulated: non-empty.
    static bool isValidTokenValue(TokenType type, const String & value);

    /// Settings for a Native COMPARE/CREATE write (create-if-absent, compare-and-set): mark the request
    /// conditional, make exactly one attempt at every retry layer, skip the racy post-upload
    /// existence/size check, and force a single PUT on generation stores because GCS does not
    /// enforce the condition on multipart completion.
    WriteSettings conditionalWriteSettings() const;
    WriteSettings conditionalWriteSettingsForTest() const { return conditionalWriteSettings(); }
    /// Override the emulated backend's wall clock for deterministic expiry tests.
    void setEmuNowNsForTest(uint64_t now_ns);
    /// Return the guarded per-key token-state size for expiry tests.
    size_t emuTokenStateSizeForTest() const;

private:
    const ObjectStoragePtr object_storage;
    const Mode mode;
    TokenType native_token_type = TokenType::ETag;
    /// See the constructor: what the READ-class requests (read, head, list, remove) carry.
    const bool single_attempt_control_plane;
    const uint64_t attempt_timeout_ms;
    ObjectStorageRetryProfile controlPlaneProfile() const
    {
        return single_attempt_control_plane ? ObjectStorageRetryProfile::SingleAttempt : ObjectStorageRetryProfile::Default;
    }
    /// The read settings a request carries: the native conditional dialect, plus the retry profile and
    /// per-attempt bound its caller is entitled to.
    ReadSettings readSettingsFor(ObjectStorageRetryProfile profile, uint64_t timeout_ms) const;

    /// The bodies a keyed primitive and its legacy override share. They differ in one thing: the
    /// keyed call passes `controlPlaneProfile(), attempt_timeout_ms`, the legacy one the storage's
    /// defaults. The legacy arguments disappear with the legacy methods.
    std::optional<Raw> readUnder(const String & key, ObjectStorageRetryProfile profile, uint64_t timeout_ms);
    std::optional<RawMeta> headUnder(const String & key, ObjectStorageRetryProfile profile, uint64_t timeout_ms);
    RawListPage listUnder(const String & prefix, const String & cursor, size_t limit,
                          ObjectStorageRetryProfile profile, uint64_t timeout_ms);
    RawRemoval removeUnder(const String & key, const String & expected_value,
                           ObjectStorageRetryProfile profile, uint64_t timeout_ms);
    SentinelProbeResult probeSentinelUnder(const String & key, ObjectStorageRetryProfile profile, uint64_t timeout_ms);
    /// EmulatedSingleProcess state: per-key {etag, disambiguator} — see emuMintToken. A successfully
    /// deleted entry is retained only while its etag is recent enough that an immediate recreate could
    /// land in the same mtime quantum. `deleteExact` erases already-old entries immediately and queues
    /// recent ones for the bounded lazy sweep in emuMintToken, so a key need not be revisited to expire.
    /// The queue records the exact state generation deleted; a subsequent re-mint makes the record
    /// obsolete rather than allowing it to erase the live incarnation's token state.
    mutable std::mutex emu_mutex;
    std::map<String, std::pair<String, uint64_t>> emu_token_state;
    struct EmuTokenExpiry
    {
        uint64_t queued_at_ns;
        String key;
        std::pair<String, uint64_t> token_state;
    };
    std::deque<EmuTokenExpiry> emu_token_expiry;
    uint64_t emu_now_ns_for_test = 0;

    /// Look up Native metadata and normalize the storage ETag or generation into an incarnation
    /// value. The value is returned as the store gave it: whether it IS an incarnation is judged by
    /// whoever can act on the answer, never here.
    std::optional<RawMeta> nativeHead(const String & key, ObjectStorageRetryProfile profile, uint64_t timeout_ms);

    /// Write a body with the condition already encoded in `ws`, finalize it, map a lost precondition
    /// onto `RawConflict`, and return the write response's own value on success -- normalized, and
    /// otherwise untouched: an empty one means the response named no incarnation, which is the
    /// caller's to resolve, not this seam's to refuse.
    std::expected<String, RawConflict> nativeConditionalPut(const String & key, const String & bytes, const WriteSettings & ws);

    /// §3.18 №19 hardening: whether `t` is the dialect this backend itself mints (native_token_type
    /// for Native mode, always TokenType::Emulated for EmulatedSingleProcess). Every conditional
    /// mutation checks this BEFORE touching the wire (Native forwards only Token::value as the
    /// If-Match/removeObjectIfTokenMatches argument, blind to Token::type) or comparing values
    /// (Emulated) — a foreign-dialect token is rejected locally rather than trusted to the remote
    /// backend, or to a value-space that was never designed to discriminate it.
    bool mintingTypeMatches(TokenType t) const { return t == (mode == Mode::Native ? native_token_type : TokenType::Emulated); }

    /// ---- Emulated helpers (caller holds emu_mutex) ----
    ///
    /// EmulatedSingleProcess resolves logical keys under the object storage's common key prefix (its
    /// root), so each backend instance is physically isolated — a real object store likewise scopes keys
    /// to a bucket/prefix. The token map is keyed by the LOGICAL key (prefix-independent).
    const String emu_root;             /// object_storage->getCommonKeyPrefix() captured at construction
    String emuPath(const String & key) const;   /// logical key -> physical object-storage path

    /// The caller holds `emu_mutex` for all five helpers below, preserving the exists/read and
    /// observe/write checks as one process-local operation.
    bool emuExists(const String & key) const;
    String emuRead(const String & key) const;
    /// Write a body as the new incarnation of `key` and return its freshly minted token (the
    /// object's own post-write etag — see emuMintToken).
    Token emuWrite(const String & key, const String & bytes);
    /// Write a complete blob body to a sibling temporary local object, then atomically replace `key`
    /// and advance any existing same-ETag disambiguator. A failure before the rename leaves the old
    /// destination and its token state untouched and cleans the temporary.
    /// Streams `envelope` + exactly `payload_size` bytes of `payload` into a temporary sibling of
    /// `key`, then renames it into place -- nothing is visible at the destination until the byte count
    /// has been validated, and the rename keeps publication atomic. Takes `emu_mutex` itself (for the
    /// rename + token-state bump only); the caller must NOT hold it.
    void emuPublishBlobAtomically(const String & key, const String & envelope, ReadBuffer & payload, uint64_t payload_size);
    /// Return the current emulated token for a key we just read/HEAD'd, reflecting its on-disk etag —
    /// does NOT advance the same-etag disambiguator (that only applies to a just-completed write).
    Token emuObserveToken(const String & key);
    uint64_t emuNowNs() const;
    /// Examine a fixed number of oldest deleted-state records, expiring only an exact current match.
    void emuPruneTokenState(uint64_t now_ns);
    /// Single source of truth for minting an emulated token from an observed `etag`: the wire value IS
    /// the etag while it is the first thing minted for `key` at that etag, or `etag#N` once a SAME-etag
    /// rewrite forces a disambiguator (`just_wrote` — see the mtime-quantum note in emu_token_state's
    /// declaration). An empty `etag` means the storage could not identify the object at all, and
    /// there is nothing to invent from: a just-completed write cannot be attributed
    /// (`CAS_WRITE_UNATTRIBUTED`) and an observation has no incarnation to report (`CORRUPTED_DATA`).
    Token emuMintToken(const String & key, const String & etag, bool just_wrote);
};

}

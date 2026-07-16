#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <map>
#include <mutex>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3/Client.h>
#endif

namespace DB::Cas
{

/// §1 (Round-B): the fold/point GETs read tiny bodies (~3.7 KB avg) but a default ReadBufferFromS3
/// preallocates ~1 MiB. When the body size is already known (the Native `get` HEADs it first), shrink
/// the read buffer to `size + slack`, capped at the caller's default — a pure reuse of
/// `ReadSettings::adjustBufferSize`. `known_size == 0` means "unknown", leave the settings untouched.
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

/// Retry strategy for the single-attempt conditional-write client (RFC cas-s3-timeout-retry-control
/// §disable-transparent-conditional-write-retries): `ShouldRetry` always refuses, and every
/// consultation is counted (CasConditionalWriteSdkRetries — see CasRequestControl.h) since it proves
/// the SDK believed the first attempt was inconclusive or failed. Exposed here for unit tests only —
/// production callers get it wired into `ObjectStorageBackend`'s ctor automatically.
class SingleAttemptRetryStrategy final : public Aws::Client::RetryStrategy
{
public:
    bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const override;
    long CalculateDelayBeforeNextRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors> &, long) const override;
    long GetMaxAttempts() const override { return 1; }
};
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
/// process-wide mutex and an in-memory per-key token counter (seeded lazily from the file's etag for
/// pre-existing keys). Semantics hold within ONE process only — exactly what unit tests need.
class ObjectStorageBackend final : public Backend
{
public:
    enum class Mode { Native, EmulatedSingleProcess };

    /// conditional_single_put_cap_: the GCS single-PUT budget for conditional writes on a
    /// generation-token store (see conditionalWriteSettings) — irrelevant on ETag-dialect stores.
    /// Defaulted so existing call sites (AWS/ETag stores, tests) compile unchanged.
    ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_, uint64_t conditional_single_put_cap_ = 1ULL << 30);

    std::optional<GetResult> get(const String & key, Range range) override;
    std::optional<GetStreamResult> getStream(const String & key, Range range = {}) override;
    HeadResult head(const String & key) override;
    /// S3 ETags are content-derived and surfaced in list responses — TRUE for ETag-token Native
    /// and EmulatedSingleProcess modes. FALSE on a generation-token store (GCS): the XML LIST
    /// surfaces MD5-style ETags in the response BODY, which the conditional dialect's header-level
    /// rewrite cannot map to generations — a list-derived "token" is a poisoned `If-Match` (the
    /// first live GC round on GCS died exactly there, fail-closed by the dialect's format guard).
    /// Consumers already treat absent list tokens as Read/fail-closed (GC discover re-reads every
    /// shard — a cost, not a correctness change).
    bool supportsListTokens() const override { return native_token_type != TokenType::Generation; }
    PutResult putIfAbsent(const String & key, const String & bytes, const ObjectMeta & meta = {}) override;

    /// Native mode: true streaming — bytes flow straight into the object storage's write buffer with
    /// `If-None-Match: *` riding on the request. EmulatedSingleProcess mode: memory-buffered delegation
    /// to putIfAbsent (acceptable: this mode exists for unit tests only).
    WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) override;
    PutResult putOverwrite(const String & key, const String & bytes, const Token & expected, const ObjectMeta & meta = {}) override;
    CasResult casPut(const String & key, const String & bytes, const std::optional<Token> & expected, const ObjectMeta & meta = {}) override;
    DeleteOutcome deleteExact(const String & key, const Token & token) override;
    ListPage list(const String & prefix, const String & cursor, size_t limit) override;

    /// S3-native staging promote seams (spec 2026-07-11-cas-s3-native-staging §8). Native mode only —
    /// EmulatedSingleProcess (LocalObjectStorage) has no server-side conditional copy and is never
    /// selected for S3 staging, so both throw `NOT_IMPLEMENTED` there (fail closed).
    /// `promoteStaged`: WRITE-ONCE conditional copy via `IObjectStorage::copyObjectConditional`.
    /// `resurrectStaged`: reads the staging PAYLOAD (skipping its `staging_payload_offset` header),
    /// prepends `fresh_header`, and UNCONDITIONALLY writes `[fresh_header][payload]` to `blob_key` (fresh
    /// tag ⇒ distinct ETag from the condemned incarnation, INV-NO-RETURN), then a fresh HEAD for the ETag.
    PutResult promoteStaged(const String & staging_key, const String & blob_key) override;
    Token resurrectStaged(const String & staging_key, const String & blob_key,
                          const String & fresh_header, uint64_t staging_payload_offset) override;

    /// Pool-level precondition: on a Native, generation-dialect (GCS) backend, fail closed if the
    /// bucket has object versioning enabled — see Backend::checkPoolPreconditions.
    void checkPoolPreconditions() override;

    /// Fail-closed precondition (RFC cas-s3-timeout-retry-control): throws LOGICAL_ERROR when Native
    /// mode has no single-attempt client (single_attempt_s3_client, built in the ctor) — see
    /// Backend::checkConditionalWriteSingleAttemptSupport. No-op for EmulatedSingleProcess.
    void checkConditionalWriteSingleAttemptSupport() override;

    /// The token kind this backend's object storage mints: TokenType::ETag for AWS-compatible
    /// stores, TokenType::Generation when the storage runs the GCS conditional dialect (the
    /// generation rides the ETag plumbing; the VALUE stays opaque either way).
    TokenType nativeTokenType() const { return native_token_type; }
    void setNativeTokenTypeForTest(TokenType t) { native_token_type = t; }

    /// ---- Token policy (single source of truth; see the .cpp) ----
    /// Mint the incarnation token for a key we just HEAD'd or wrote: the object ETag/generation
    /// string carried under this backend's native dialect (native_token_type).
    Token tokenForHead(const String & etag) const
    {
        return Token{etag, native_token_type};
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

    /// Base WriteSettings for every Native conditional write — see the .cpp for the full rationale
    /// (skips the post-upload existence/size HEAD; forces single-PUT on generation-token stores).
    WriteSettings conditionalWriteSettings() const;
    WriteSettings conditionalWriteSettingsForTest() const { return conditionalWriteSettings(); }

private:
    const ObjectStoragePtr object_storage;
    const Mode mode;
    TokenType native_token_type = TokenType::ETag;
    /// GCS single-PUT budget for conditional writes (generation-token stores only); see ctor.
    const uint64_t conditional_single_put_cap;

#if USE_AWS_S3
    /// Single-attempt S3 client for conditional writes (RFC cas-s3-timeout-retry-control
    /// §disable-transparent-conditional-write-retries): a cheap clone of the underlying object
    /// storage's shared client (same connection pool/credentials, see Client::cloneWithConfigurationOverride)
    /// with SDK retries disabled, so a lost response surfaces to CAS immediately instead of the
    /// generic ~500-attempt retry policy silently continuing past the mount lease. Built once in the
    /// ctor for Native mode; null when the object storage is not S3-backed (EmulatedSingleProcess /
    /// local tests) — conditionalWriteSettings then leaves the disk's own client in place, so Native
    /// mode over a non-S3 backend keeps compiling but is NOT single-attempt (not expected in practice:
    /// Native mode is only ever mounted over an S3-like conditional dialect).
    std::shared_ptr<const S3::Client> single_attempt_s3_client;
#endif

    /// EmulatedSingleProcess state: per-key current token (monotone counter as string).
    std::mutex emu_mutex;
    std::map<String, uint64_t> emu_tokens;
    uint64_t emu_seq = 0;

    /// ---- Native helpers ----
    std::optional<HeadResult> nativeHead(const String & key);
    PutResult nativeConditionalPut(const String & key, const String & bytes, const WriteSettings & ws, const ObjectMeta & meta);

    /// ---- Emulated helpers (caller holds emu_mutex) ----
    ///
    /// EmulatedSingleProcess resolves logical keys under the object storage's common key prefix (its
    /// root), so each backend instance is physically isolated — a real object store likewise scopes keys
    /// to a bucket/prefix. The token map is keyed by the LOGICAL key (prefix-independent).
    const String emu_root;             /// object_storage->getCommonKeyPrefix() captured at construction
    String emuPath(const String & key) const;   /// logical key -> physical object-storage path

    bool emuExists(const String & key) const;
    String emuRead(const String & key, Range range) const;
    void emuWrite(const String & key, const String & bytes, const ObjectMeta & meta);
    Token emuObserveToken(const String & key);   /// seeds emu_tokens lazily for pre-existing keys
};

}

#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <map>
#include <mutex>

#include "config.h"

namespace DB::Cas
{

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
    /// `resurrectStaged`: UNCONDITIONAL copy via `IObjectStorage::copyObject` + a fresh HEAD for the ETag.
    PutResult promoteStaged(const String & staging_key, const String & blob_key) override;
    Token resurrectStaged(const String & staging_key, const String & blob_key) override;

    /// Store-level precondition: on a Native, generation-dialect (GCS) backend, fail closed if the
    /// bucket has object versioning enabled — see Backend::checkStorePreconditions.
    void checkStorePreconditions() override;

    /// The token kind this backend's object storage mints: TokenType::ETag for AWS-compatible
    /// stores, TokenType::Generation when the storage runs the GCS conditional dialect (the
    /// generation rides the ETag plumbing; the VALUE stays opaque either way).
    TokenType nativeTokenType() const { return native_token_type; }
    void setNativeTokenTypeForTest(TokenType t) { native_token_type = t; }

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

#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h>
#include <IO/WriteBufferFromString.h>
#include <map>
#include <mutex>

namespace DB::Cas
{

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

    ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_);

    std::optional<GetResult> get(const String & key, Range range) override;
    HeadResult head(const String & key) override;
    PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token) override;

    /// TEMPORARY (Task 1): memory-buffered delegation; Task 2 replaces with true streaming for Native mode.
    WriteSinkPtr putIfAbsentStream(const String & key) override
    {
        class BufferedSink final : public WriteSink
        {
        public:
            BufferedSink(Backend & backend_, String key_) : backend(backend_), key(std::move(key_)) {}

            WriteBuffer & buffer() override { return buf; }

            PutOutcome finalize(Token * out_token) override
            {
                done = true;
                return backend.putIfAbsent(key, buf.str(), out_token);
            }

            void cancel() noexcept override
            {
                done = true;
                buf.cancel();
            }

            ~BufferedSink() override
            {
                if (!done)
                    cancel();
            }

        private:
            Backend & backend;
            String key;
            WriteBufferFromOwnString buf;
            bool done = false;
        };
        return std::make_unique<BufferedSink>(*this, key);
    }
    PutOutcome putOverwrite(const String & key, const String & bytes, const Token & expected, Token * out_token) override;
    CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected, Token * out_token) override;
    DeleteOutcome deleteExact(const String & key, const Token & token) override;
    ListPage list(const String & prefix, const String & cursor, size_t limit) override;

private:
    const ObjectStoragePtr object_storage;
    const Mode mode;

    /// EmulatedSingleProcess state: per-key current token (monotone counter as string).
    std::mutex emu_mutex;
    std::map<String, uint64_t> emu_tokens;
    uint64_t emu_seq = 0;

    /// ---- Native helpers ----
    std::optional<HeadResult> nativeHead(const String & key);
    PutOutcome nativeConditionalPut(const String & key, const String & bytes, const WriteSettings & ws, Token * out_token);

    /// ---- Emulated helpers (caller holds emu_mutex) ----
    ///
    /// EmulatedSingleProcess resolves logical keys under the object storage's common key prefix (its
    /// root), so each backend instance is physically isolated — a real object store likewise scopes keys
    /// to a bucket/prefix. The token map is keyed by the LOGICAL key (prefix-independent).
    const String emu_root;             /// object_storage->getCommonKeyPrefix() captured at construction
    String emuPath(const String & key) const;   /// logical key -> physical object-storage path

    bool emuExists(const String & key) const;
    String emuRead(const String & key, Range range) const;
    void emuWrite(const String & key, const String & bytes);
    Token emuObserveToken(const String & key);   /// seeds emu_tokens lazily for pre-existing keys
};

}

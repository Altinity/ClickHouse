#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h>

#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Core/Defines.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteSettings.h>

#include <Common/Exception.h>

#include <algorithm>

namespace DB
{
namespace ErrorCodes
{
    extern const int S3_ERROR;
}
}

namespace DB::Cas
{

ObjectStorageBackend::ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_)
    : object_storage(std::move(object_storage_))
    , mode(mode_)
    , emu_root(object_storage->getCommonKeyPrefix())
{
}

/// =========================================================================================
/// Native helpers
/// =========================================================================================

std::optional<HeadResult> ObjectStorageBackend::nativeHead(const String & key)
{
    auto metadata = object_storage->tryGetObjectMetadata(key, /*with_tags=*/false);
    if (!metadata)
        return std::nullopt;

    HeadResult hr;
    hr.exists = true;
    hr.size = metadata->size_bytes;
    hr.token = Token{metadata->etag, TokenType::ETag};
    return hr;
}

/// Issue a conditional PUT (the condition rides on `ws`) and map a precondition loss.
///
/// A backend reports a lost condition as an `S3Exception` (code `S3_ERROR`) whose message carries
/// `PreconditionFailed` — the exact signal `PoolCoordination.cpp`'s `condCreateViaIfNoneMatch` detects.
/// A `404 NoSuchKey` on an `If-Match` PUT (the key was deleted out from under us) is treated identically:
/// protocol callers handle 'mismatch' and 'gone' the same way (re-validate), so both collapse onto
/// `PreconditionFailed`.
///
/// The detection matches error-name substrings only (`PreconditionFailed`, `NoSuchKey`) — never bare
/// status digits, since the formatted message embeds the object key and free digits could match
/// spuriously. The substring match itself is fail-safe in direction (a misread transient error becomes
/// a retryable PreconditionFailed/Conflict, never a false success). FOLLOW-UP (pre-integration): have
/// `WriteBufferFromS3` surface the precondition loss as a typed signal so this matches the SDK error
/// type instead of free text, mirroring `S3ObjectStorage::removeObjectIfTokenMatches`.
PutOutcome ObjectStorageBackend::nativeConditionalPut(const String & key, const String & bytes, const WriteSettings & ws, Token * out_token)
{
    try
    {
        auto buf = object_storage->writeObject(
            StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, ws);
        buf->write(bytes.data(), bytes.size());
        buf->finalize();
    }
    catch (const Exception & e)
    {
        if (e.code() == ErrorCodes::S3_ERROR
            && (e.message().find("PreconditionFailed") != std::string::npos
                || e.message().find("NoSuchKey") != std::string::npos))
            return PutOutcome::PreconditionFailed;
        throw;
    }

    if (out_token)
    {
        auto hr = nativeHead(key);
        *out_token = hr ? hr->token : Token{};
    }
    return PutOutcome::Done;
}

/// Read the whole object at `path` and honor `range` by substr. Objects on this path are small (root
/// manifests, tree/blob bodies in tests), so read-whole + substr is the correct and simplest way to
/// serve a sub-range — no seek/limit machinery needed.
static String readObjectRanged(IObjectStorage & object_storage, const String & path, Range range)
{
    auto buf = object_storage.readObject(StoredObject(path), getReadSettings(), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);

    if (range.whole())
        return content;

    const size_t offset = static_cast<size_t>(range.offset);
    if (offset >= content.size())
        return {};
    if (range.length.has_value())
        return content.substr(offset, static_cast<size_t>(*range.length));
    return content.substr(offset);
}

/// =========================================================================================
/// Emulated helpers (caller holds emu_mutex)
/// =========================================================================================

String ObjectStorageBackend::emuPath(const String & key) const
{
    if (emu_root.empty())
        return key;
    if (!emu_root.empty() && emu_root.back() == '/')
        return emu_root + key;
    return emu_root + "/" + key;
}

bool ObjectStorageBackend::emuExists(const String & key) const
{
    return object_storage->exists(StoredObject(emuPath(key)));
}

String ObjectStorageBackend::emuRead(const String & key, Range range) const
{
    return readObjectRanged(*object_storage, emuPath(key), range);
}

void ObjectStorageBackend::emuWrite(const String & key, const String & bytes)
{
    auto buf = object_storage->writeObject(StoredObject(emuPath(key)), WriteMode::Rewrite);
    buf->write(bytes.data(), bytes.size());
    buf->finalize();
}

Token ObjectStorageBackend::emuObserveToken(const String & key)
{
    auto it = emu_tokens.find(key);
    if (it != emu_tokens.end())
        return Token{std::to_string(it->second), TokenType::Emulated};

    /// First time we see a key that already exists on disk: seed a fresh emulated token for it so that
    /// pre-existing objects participate in token-exact semantics within this process.
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    return Token{std::to_string(seq), TokenType::Emulated};
}

/// =========================================================================================
/// Backend interface
/// =========================================================================================

std::optional<GetResult> ObjectStorageBackend::get(const String & key, Range range)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        if (!hr)
            return std::nullopt;

        GetResult gr;
        gr.bytes = readObjectRanged(*object_storage, key, range);
        gr.token = hr->token;
        return gr;
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    GetResult gr;
    gr.bytes = emuRead(key, range);
    gr.token = emuObserveToken(key);
    return gr;
}

HeadResult ObjectStorageBackend::head(const String & key)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        return hr ? *hr : HeadResult{};
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return HeadResult{};

    auto metadata = object_storage->tryGetObjectMetadata(emuPath(key), /*with_tags=*/false);
    HeadResult hr;
    hr.exists = true;
    hr.size = metadata ? metadata->size_bytes : 0;
    hr.token = emuObserveToken(key);
    return hr;
}

PutOutcome ObjectStorageBackend::putIfAbsent(const String & key, const String & bytes, Token * out_token)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws;
        ws.object_storage_write_if_none_match = "*";
        return nativeConditionalPut(key, bytes, ws, out_token);
    }

    std::lock_guard lock(emu_mutex);
    if (emuExists(key))
        return PutOutcome::PreconditionFailed;

    emuWrite(key, bytes);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    if (out_token)
        *out_token = Token{std::to_string(seq), TokenType::Emulated};
    return PutOutcome::Done;
}

PutOutcome ObjectStorageBackend::putOverwrite(const String & key, const String & bytes, const Token & expected, Token * out_token)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws;
        ws.object_storage_write_if_match = expected.value;
        return nativeConditionalPut(key, bytes, ws, out_token);
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return PutOutcome::PreconditionFailed;
    if (emuObserveToken(key) != expected)
        return PutOutcome::PreconditionFailed;

    emuWrite(key, bytes);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    if (out_token)
        *out_token = Token{std::to_string(seq), TokenType::Emulated};
    return PutOutcome::Done;
}

CasOutcome ObjectStorageBackend::casPut(const String & key, const String & bytes, const std::optional<Token> & expected, Token * out_token)
{
    if (mode == Mode::Native)
    {
        WriteSettings ws;
        if (expected.has_value())
            ws.object_storage_write_if_match = expected->value;
        else
            ws.object_storage_write_if_none_match = "*";

        /// The PUT-side outcomes (Done / PreconditionFailed) collapse onto CAS outcomes 1:1: a lost
        /// condition — whether a mismatched If-Match or a 404 on an If-Match PUT — is a Conflict.
        return nativeConditionalPut(key, bytes, ws, out_token) == PutOutcome::Done
            ? CasOutcome::Committed
            : CasOutcome::Conflict;
    }

    std::lock_guard lock(emu_mutex);
    const bool exists = emuExists(key);

    if (!expected.has_value())
    {
        if (exists)
            return CasOutcome::Conflict;
    }
    else
    {
        if (!exists)
            return CasOutcome::Conflict;
        if (emuObserveToken(key) != *expected)
            return CasOutcome::Conflict;
    }

    emuWrite(key, bytes);
    const uint64_t seq = ++emu_seq;
    emu_tokens[key] = seq;
    if (out_token)
        *out_token = Token{std::to_string(seq), TokenType::Emulated};
    return CasOutcome::Committed;
}

DeleteOutcome ObjectStorageBackend::deleteExact(const String & key, const Token & token)
{
    if (mode == Mode::Native)
    {
        /// `removeObjectIfTokenMatches` maps onto `DeleteOutcome` one-to-one. `NOT_IMPLEMENTED` from a
        /// backend that does not enforce conditional removal propagates — fail-closed by construction.
        auto result = object_storage->removeObjectIfTokenMatches(StoredObject(key), token.value);
        DeleteOutcome d;
        d.created_delete_marker = result.created_delete_marker;
        switch (result.outcome)
        {
            case ConditionalRemoveOutcome::Removed:
                d.kind = DeleteOutcome::Kind::Deleted;
                break;
            case ConditionalRemoveOutcome::TokenMismatch:
                d.kind = DeleteOutcome::Kind::TokenMismatch;
                break;
            case ConditionalRemoveOutcome::NotFound:
                d.kind = DeleteOutcome::Kind::NotFound;
                break;
        }
        return d;
    }

    std::lock_guard lock(emu_mutex);
    DeleteOutcome d;
    if (!emuExists(key))
    {
        d.kind = DeleteOutcome::Kind::NotFound;
        return d;
    }
    if (emuObserveToken(key) != token)
    {
        d.kind = DeleteOutcome::Kind::TokenMismatch;
        return d;
    }

    object_storage->removeObjectIfExists(StoredObject(emuPath(key)));
    emu_tokens.erase(key);
    d.kind = DeleteOutcome::Kind::Deleted;
    return d;
}

ListPage ObjectStorageBackend::list(const String & prefix, const String & cursor, size_t limit)
{
    /// Enumerate the prefix, then apply cursor/limit over the SORTED keys exactly like the in-memory
    /// backend (the object storage listing order is unspecified, so we sort to make pagination stable).
    /// In EmulatedSingleProcess mode physical paths are scoped under the storage root, so we list under
    /// the resolved physical prefix and strip the root back to the logical key the caller passed in.
    const String physical_prefix = (mode == Mode::EmulatedSingleProcess) ? emuPath(prefix) : prefix;
    const String strip = (mode == Mode::EmulatedSingleProcess) ? emuPath("") : String{};

    RelativePathsWithMetadata children;
    object_storage->listObjects(physical_prefix, children, /*max_keys=*/0);

    std::vector<ListedKey> all;
    all.reserve(children.size());
    for (const auto & child : children)
    {
        if (child->relative_path.substr(0, physical_prefix.size()) != physical_prefix)
            continue;
        ListedKey lk;
        lk.key = child->relative_path.substr(strip.size());
        lk.size = child->metadata ? child->metadata->size_bytes : 0;
        all.push_back(std::move(lk));
    }
    std::sort(all.begin(), all.end(), [](const ListedKey & a, const ListedKey & b) { return a.key < b.key; });

    ListPage page;
    const String & start = (cursor > prefix) ? cursor : prefix;
    auto it = std::lower_bound(all.begin(), all.end(), start,
        [](const ListedKey & a, const String & s) { return a.key < s; });

    while (it != all.end() && page.keys.size() < limit)
    {
        page.keys.push_back(*it);
        ++it;
    }

    if (it != all.end())
        page.next_cursor = it->key;

    return page;
}

}

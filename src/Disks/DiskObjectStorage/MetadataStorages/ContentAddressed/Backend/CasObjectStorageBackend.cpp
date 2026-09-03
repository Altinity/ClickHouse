#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasIncarnation.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.h>
#include <Disks/DiskObjectStorage/ObjectStorages/Local/LocalObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/ObjectStorageIterator.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <Disks/WriteMode.h>

#include <Core/Defines.h>
#include <Core/UUID.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ReadHelpers.h>
#include <IO/ReadSettings.h>
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <IO/WriteSettings.h>

#include <Common/Exception.h>
#include <Common/logger_useful.h>

#include "config.h"

#if USE_AWS_S3
#include <IO/S3Common.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
    extern const int CAS_WRITE_UNATTRIBUTED;
}
}

namespace DB::Cas
{

ObjectStorageBackend::ObjectStorageBackend(ObjectStoragePtr object_storage_, Mode mode_,
                                           bool single_attempt_control_plane_, uint64_t attempt_timeout_ms_)
    : object_storage(std::move(object_storage_))
    , mode(mode_)
    , single_attempt_control_plane(single_attempt_control_plane_)
    , attempt_timeout_ms(attempt_timeout_ms_)
    , emu_root(object_storage->getCommonKeyPrefix())
{
    if (mode == Mode::Native && object_storage->conditionalOpsUseGenerationTokens())
        native_token_type = TokenType::Generation;
}

/// See Backend::checkPoolPreconditions. Only the Native, generation-dialect (GCS) combination has
/// anything to check: a token-exact DELETE on a versioned bucket archives a noncurrent generation
/// instead of reclaiming storage, so GC "reclaim" would silently stop reclaiming. A bucket VERIFIED
/// to have versioning enabled refuses the mount. A probe that cannot answer does not: it is not
/// evidence of a versioned bucket, its usual cause is a credential without permission to read the
/// bucket configuration, and refusing on it would turn a missing IAM grant into a hard outage. The
/// mount proceeds with a warning that names what was not verified and how the operator can verify it.
void ObjectStorageBackend::checkPoolPreconditions()
{
    if (mode != Mode::Native || native_token_type != TokenType::Generation)
        return;

    const auto versioned = object_storage->isBucketVersioningEnabled();
    if (!versioned.has_value())
    {
        LOG_WARNING(getLogger("CasObjectStorageBackend"),
            "CAS on GCS: could not VERIFY the bucket-versioning precondition (the versioning check "
            "request failed, e.g. the credential lacks permission to read the bucket configuration, "
            "or this backend cannot answer it). Mounting anyway. If versioning IS enabled on this "
            "bucket, token-exact DELETEs archive noncurrent generations instead of reclaiming storage "
            "and GC silently stops reclaiming space. Confirm by hand that versioning is disabled, or "
            "grant the credential permission to read the bucket's versioning configuration "
            "(storage.buckets.get on GCS) so the next mount can verify it.");
        return;
    }

    if (*versioned)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS on GCS: the bucket has object VERSIONING enabled. A token-exact DELETE on a "
            "versioned bucket archives a noncurrent generation instead of reclaiming storage — GC "
            "would silently stop reclaiming space. Disable versioning on the bucket (and prefer "
            "soft-delete duration 0 for CAS pools) and retry the mount.");
}

/// See Backend::checkSkipAccessCheckSupport. A writable generation-dialect (GCS) mount is the one
/// combination whose correctness depends on the MUTATING capability battery having run: the battery
/// is what proves a numeric generation actually reaches GCS as x-goog-if-generation-match on a
/// DELETE, and nothing else in the mount path proves it.
void ObjectStorageBackend::checkSkipAccessCheckSupport()
{
    if (mode != Mode::Native || native_token_type != TokenType::Generation)
        return;

    throw Exception(ErrorCodes::NOT_IMPLEMENTED,
        "CAS on GCS: skip_access_check=true is not supported on a WRITABLE generation-token mount. "
        "The capability battery this setting skips is what verifies that a token-exact DELETE "
        "actually honours the generation precondition; without it GC could delete an incarnation it "
        "did not condemn, and the bucket-versioning precondition would go unchecked too. Remove "
        "skip_access_check from this disk, or mount it read-only.");
}

/// See Backend::checkConditionalWriteSingleAttemptSupport. This is a MOUNT-TIME gate, deliberately
/// separate from the ctor: narrow, targeted unit tests can keep constructing a raw Native-mode backend
/// over a non-S3 IObjectStorage (LocalObjectStorage) to exercise OTHER behaviors in isolation — the
/// established convention throughout this test suite (see e.g. gtest_cas_backend_generation.cpp). A
/// REAL writable mount, by contrast, always reaches this check: runCapabilityProbe (CasProbe.cpp) calls
/// it for every non-read-only Pool::open, so production never silently runs Native-mode conditional
/// writes under the disk's default (~500-attempt) transparent retry policy.
void ObjectStorageBackend::checkConditionalWriteSingleAttemptSupport()
{
    if (mode != Mode::Native)
        return;

    /// The property checked is now backend CAPABILITY, not client presence: whether this object
    /// storage can honor the SingleAttempt retry profile at all (S3ObjectStorage always can; a non-S3
    /// object storage like LocalObjectStorage cannot).
    const bool single_attempt_supported = object_storage->supportsRetryProfile(ObjectStorageRetryProfile::SingleAttempt);
    if (!single_attempt_supported)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS Native-mode conditional writes require an object storage that supports the "
            "SingleAttempt retry profile (RFC cas-s3-timeout-retry-control), but this one does not "
            "(IObjectStorage::supportsRetryProfile returned false, or this build has no AWS S3 "
            "support) — refusing to mount writable. Native mode is designed for an S3-like "
            "conditional dialect only; a non-S3 object storage should use EmulatedSingleProcess.");
}

/// =========================================================================================
/// Native helpers
/// =========================================================================================

bool ObjectStorageBackend::isValidTokenValue(TokenType type, const String & value)
{
    return isIncarnationValue(type, value);
}

std::optional<Backend::RawMeta> ObjectStorageBackend::nativeHead(const String & key)
{
    auto metadata = object_storage->tryGetObjectMetadataWithNativeToken(
        key, /*with_tags=*/false, controlPlaneProfile(), attempt_timeout_ms);
    if (!metadata)
        return std::nullopt;

    const Token token = tokenForHead(metadata->etag);
    /// The store guarantees a well-formed incarnation value under this backend's dialect on every
    /// successful HEAD; a missing or malformed value (a proxy dropping the header, a service
    /// regression) means the response fell through unmapped. Surface that here rather than handing
    /// the value to the first conditional operation that trusts it.
    if (!isValidTokenValue(native_token_type, token.value))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "CAS backend: the store answered for '{}' with a value '{}' that is not a valid {} incarnation",
            key, metadata->etag, native_token_type == TokenType::Generation ? "generation" : "ETag");
    return RawMeta{metadata->size_bytes, token.value};
}

/// Finalize a conditional write (the condition rode on the buffer's WriteSettings) and map a
/// precondition loss to an OUTCOME — anything else propagates.
///
/// A backend reports a lost condition as an `S3Exception` carrying the canonical S3 error code string
/// from the response XML `<Code>` (`S3Exception::getExceptionName`); a conditional-write 412 is
/// UNMODELED for the AWS SDK (its enum value is UNKNOWN), so `S3Exception::isPreconditionFailed` is the
/// typed signal — the `PreconditionFailed` name, or that token in the raw body for S3-compatible stores
/// (RustFS) whose non-AWS body the SDK cannot parse into a name. A `404 NoSuchKey` on an `If-Match` PUT
/// (the key was deleted out from under us) is treated identically: protocol callers handle 'mismatch'
/// and 'gone' the same way (re-validate), so both collapse onto `PreconditionFailed`. `NoSuchKey` IS
/// modeled by the SDK, and `WriteBufferFromS3` retries it internally surfacing the exhaustion with the
/// typed enum code (and no name), so the enum is matched as well as the name. The mapping is fail-safe in
/// direction: a misread error becomes a retryable PreconditionFailed/Conflict, never a false success.
///
/// Native conditional writes require an S3-compatible integration environment for end-to-end
/// coverage. Unit tests cover the emulated semantics, the typed exception path, and this classifier
/// through the test-only `detail` declaration.
#if USE_AWS_S3
PutOutcome detail::finalizeConditionalWrite(WriteBuffer & buf)
{
    try
    {
        buf.finalize();
    }
    catch (const S3Exception & e)
    {
        if (e.isPreconditionFailed()
            || e.getExceptionName() == "NoSuchKey"
            || e.getS3ErrorCode() == Aws::S3::S3Errors::NO_SUCH_KEY)
            return PutOutcome::PreconditionFailed;
        throw;
    }
    return PutOutcome::Done;
}
#endif

/// Build-dispatching shim for the write paths below: without the AWS SDK there is no S3Exception
/// to classify, so the errors of finalize simply propagate.
static PutOutcome finalizeConditionalWrite(WriteBuffer & buf)
{
#if USE_AWS_S3
    return detail::finalizeConditionalWrite(buf);
#else
    buf.finalize();
    return PutOutcome::Done;
#endif
}

/// Instrument the same single `finalize` call used by both Native write paths without changing their
/// `Done`/`PreconditionFailed`-or-rethrow contract. A classified precondition loss is `Unresolved`,
/// not `Committed` or a definite exception, because the response does not prove who created or
/// replaced the object; the higher-level request controller may then resolve it with exact-key state.
static PutOutcome finalizeConditionalWriteInstrumented(WriteBuffer & buf)
{
    recordConditionalWriteAttemptStarted();
    try
    {
        const PutOutcome legacy = finalizeConditionalWrite(buf);
        recordConditionalWriteOutcome(
            legacy == PutOutcome::Done ? classifyConditionalWriteResult() : CasWriteOutcome::Unresolved);
        return legacy;
    }
    catch (const std::exception & e)
    {
        recordConditionalWriteOutcome(classifyConditionalWriteResult(e));
        throw;
    }
}

/// Issue a conditional PUT (the condition rides on `ws`) and map a precondition loss — see
/// finalizeConditionalWrite. The condition is checked by the backend when the object is completed,
/// so the precondition loss always surfaces from the buffer's finalize, never from write.
std::expected<String, Backend::RawConflict> ObjectStorageBackend::nativeConditionalPut(
    const String & key, const String & bytes, const WriteSettings & ws)
{
    auto buf = object_storage->writeObject(
        StoredObject(key), WriteMode::Rewrite, /*attributes=*/std::nullopt, DBMS_DEFAULT_BUFFER_SIZE, ws);
    buf->write(bytes.data(), bytes.size());
    if (finalizeConditionalWriteInstrumented(*buf) == PutOutcome::PreconditionFailed)
        return std::unexpected(RawConflict{});

    /// Attribute the incarnation WE just wrote -- see tokenFromWriteResult for the exact
    /// generation-vs-ETag policy. The S3 write returns its object ETag/generation in the
    /// PutObject/CompleteMultipartUpload response, so no follow-up HEAD is needed.
    return tokenFromWriteResult(key, buf->getResultObjectETag()).value;
}

namespace
{

}

/// True when an exception from a read means "the KEY is simply not there".
/// Two surfaces:
///   1. S3/RustFS: `S3Exception` with `S3Errors::NO_SUCH_KEY` (the modeled enum — the primary
///      signal), `getExceptionName() == "NoSuchKey"` (the canonical XML `<Code>` string, present when
///      the SDK was able to parse it; mirrors `finalizeConditionalWrite`'s detection), or
///      `RESOURCE_NOT_FOUND`, the generic code the SDK derives from a 404 whose body it could not
///      parse into a name.
///   2. Local / emulated: `DB::Exception` with `ErrorCodes::FILE_DOESNT_EXIST` (from
///      `ReadBufferFromFile` when `open(2)` returns ENOENT).
///
/// `NO_SUCH_BUCKET` is deliberately NOT here even though it is the third member of the store's own
/// 404 family: a vanished CONTAINER is not an absent key, and answering "absent" for it would let a
/// caller read an empty pool out of an outage. It propagates, and `probeSentinelRaw` classifies it.
/// Any other error (network, auth, throttle, corruption) propagates unchanged — fail-closed.
static bool isObjectNotFound(const std::exception & e)
{
#if USE_AWS_S3
    if (const auto * s3e = dynamic_cast<const S3Exception *>(&e))
        return s3e->getS3ErrorCode() == Aws::S3::S3Errors::NO_SUCH_KEY
            || s3e->getS3ErrorCode() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND
            || s3e->getExceptionName() == "NoSuchKey";
#endif
    if (const auto * dbe = dynamic_cast<const Exception *>(&e))
        return dbe->code() == ErrorCodes::FILE_DOESNT_EXIST;
    return false;
}

/// Read the whole object at `path`. A caller that already knows the size passes it so the read
/// buffer is sized to the body instead of the storage's ~1 MiB default.
static String readWholeObject(IObjectStorage & object_storage, const String & path, uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
    String content;
    readStringUntilEOF(content, *buf);
    return content;
}

/// Open a forward-only stream over `range` of the object at `path`, positioned at the window's first
/// byte and bounded to its last. Nothing is materialized whole: the caller reads at its own pace. An
/// offset at or past EOF yields an empty stream rather than an error.
static std::unique_ptr<ReadBuffer> openObjectRangedStream(IObjectStorage & object_storage, const String & path, Range range,
                                                          uint64_t known_size = 0)
{
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
    if (range.whole())
        return buf;

    /// `seek` past the object size may throw depending on the storage, so fail-close against the known
    /// size before touching the buffer position. A caller-supplied size avoids another metadata round
    /// trip; zero means that the size is unknown and must be fetched.
    const uint64_t object_size = known_size != 0 ? known_size
        : object_storage.getObjectMetadata(path, /*with_tags=*/false).size_bytes;
    if (range.offset >= object_size)
        return std::make_unique<ReadBufferFromString>(std::string_view{});

    /// `setReadUntilPosition` is only a hint (LocalObjectStorage does not honor it), but for a returned
    /// stream it is the only bound available — the caller drains to EOF, so a storage that DOES honor
    /// the hint stops at the window end, and one that does not over-reads only the trailing bytes.
    if (range.length.has_value())
        buf->setReadUntilPosition(range.offset + *range.length);
    buf->seek(static_cast<off_t>(range.offset), SEEK_SET);
    return buf;
}

ReadSettings casSizedReadSettings(const ReadSettings & base, uint64_t known_size)
{
    if (known_size == 0)
        return base;
    return base.adjustBufferSize(known_size + CAS_FOLD_READ_SLACK_BYTES);
}

/// =========================================================================================
/// Emulated helpers (caller holds emu_mutex)
/// =========================================================================================

namespace
{

/// The mtime-quantum guard (emuMintToken) only needs a key's `emu_token_state` entry while a
/// same-quantum tie is still POSSIBLE for a FRESH recreate — i.e. while the just-deleted
/// incarnation's own etag (mtime-ns, see emuMintToken) is recent. Once it is comfortably behind
/// "now", no later recreate can land in the same mtime quantum, so retaining the entry serves no
/// purpose (codex-review-triage §3.18, Important #1). 2 seconds is far above any filesystem's mtime
/// tick coarseness while still bounding the map to the recently-deleted-key population.
constexpr uint64_t EMU_TOKEN_STALE_AGE_NS = 2'000'000'000ULL;
constexpr size_t EMU_TOKEN_EXPIRY_SWEEP_SIZE = 16;

/// True iff `etag` parses as a plain nanosecond count (emuMintToken's `.first` is always the BARE
/// etag, never the `etag#N` disambiguated form) that is at least EMU_TOKEN_STALE_AGE_NS behind now.
/// An etag that fails to parse (e.g. a test double's non-numeric stub) is conservatively treated as
/// NOT stale — never erasing is always safe, merely un-bounded, so an unparseable value must not be
/// mistaken for a recent one.
bool etagComfortablyInThePast(const String & etag, uint64_t now_ns)
{
    if (etag.empty() || !std::all_of(etag.begin(), etag.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;

    uint64_t etag_ns = 0;
    try
    {
        etag_ns = std::stoull(etag);
    }
    catch (...)
    {
        return false;
    }

    return now_ns > etag_ns && (now_ns - etag_ns) >= EMU_TOKEN_STALE_AGE_NS;
}

}

String ObjectStorageBackend::emuPath(const String & key) const
{
    if (emu_root.empty())
        return key;
    if (!emu_root.empty() && emu_root.back() == '/')
        return emu_root + key;
    return emu_root + "/" + key;
}

uint64_t ObjectStorageBackend::emuNowNs() const
{
    if (emu_now_ns_for_test != 0)
        return emu_now_ns_for_test;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

void ObjectStorageBackend::setEmuNowNsForTest(uint64_t now_ns)
{
    std::lock_guard lock(emu_mutex);
    emu_now_ns_for_test = now_ns;
}

size_t ObjectStorageBackend::emuTokenStateSizeForTest() const
{
    std::lock_guard lock(emu_mutex);
    return emu_token_state.size();
}

void ObjectStorageBackend::emuPruneTokenState(uint64_t now_ns)
{
    for (size_t checked = 0; checked < EMU_TOKEN_EXPIRY_SWEEP_SIZE && !emu_token_expiry.empty(); ++checked)
    {
        const auto & candidate = emu_token_expiry.front();
        auto current = emu_token_state.find(candidate.key);

        /// A later mint (including a delete+recreate in the same mtime quantum) supersedes this exact
        /// deleted state. Its queue record can be discarded immediately without touching the map.
        if (current == emu_token_state.end() || current->second != candidate.token_state)
        {
            emu_token_expiry.pop_front();
            continue;
        }

        /// Deletion time is monotonic within this mutex-protected FIFO. If its oldest record has not
        /// crossed the safety window, every later matching record is too recent as well.
        if (now_ns <= candidate.queued_at_ns || now_ns - candidate.queued_at_ns < EMU_TOKEN_STALE_AGE_NS)
            break;

        /// The record has aged enough to inspect its etag. Unparseable or otherwise uncertain etags
        /// stay in the map (fail safe), but their queue records cannot block pruning of later keys.
        if (etagComfortablyInThePast(current->second.first, now_ns))
            emu_token_state.erase(current);
        emu_token_expiry.pop_front();
    }
}

bool ObjectStorageBackend::emuExists(const String & key) const
{
    return object_storage->exists(StoredObject(emuPath(key)));
}

String ObjectStorageBackend::emuRead(const String & key) const
{
    return readWholeObject(*object_storage, emuPath(key));
}

Token ObjectStorageBackend::emuWrite(const String & key, const String & bytes)
{
    auto buf = object_storage->writeObject(StoredObject(emuPath(key)), WriteMode::Rewrite);
    buf->write(bytes.data(), bytes.size());
    buf->finalize();

    const auto metadata = object_storage->tryGetObjectMetadata(emuPath(key), /*with_tags=*/false);
    return emuMintToken(key, metadata ? metadata->etag : String{}, /*just_wrote=*/true);
}

void ObjectStorageBackend::emuPublishBlobAtomically(const String & key, const String & envelope, ReadBuffer & payload, uint64_t payload_size)
{
    if (object_storage->getType() != ObjectStorageType::Local)
        throw Exception(
            ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::publish: atomic emulated publication requires local object storage");

    const String destination_object = emuPath(key);
    const String temporary_object = destination_object + ".publish-" + toString(UUIDHelpers::generateV4()) + ".tmp";
    const String root = object_storage->getCommonKeyPrefix();
    const String destination_path = resolvePathRelativelyToBase(destination_object, root);
    const String temporary_path = resolvePathRelativelyToBase(temporary_object, root);

    /// The body is STREAMED into the temporary file -- envelope, then a bounded copy of the payload --
    /// never materialized in memory. (An earlier revision accumulated envelope+payload in one String,
    /// whose growth doubling made the peak allocation up to 2x the payload, and serialized every
    /// publication behind a dedicated mutex just to bound that peak to one body at a time; streaming
    /// removes both.) The destination stays untouched until the byte count has been validated: a short
    /// or long source aborts on the temporary file, which is then removed.
    try
    {
        auto out = object_storage->writeObject(StoredObject(temporary_object), WriteMode::Rewrite);
        out->write(envelope.data(), envelope.size());
        const auto copy_result = blob_publication_detail::copyBlobPayloadBounded(payload, *out, payload_size);
        if (!copy_result.exact(payload_size))
        {
            out->cancel();
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ObjectStorageBackend::publish: source yielded {}{} payload bytes for {}, declared {} -- nothing was published",
                copy_result.has_excess ? "more than " : "",
                copy_result.copied,
                key,
                payload_size);
        }
        out->finalize();
    }
    catch (...)
    {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }

    /// Publication is transport-only and cannot HEAD to learn the replacement's ETag. Advancing an
    /// existing disambiguator is sufficient: if the next observation sees the same ETag, it returns
    /// a token distinct from the old incarnation; if the ETag changed, emuMintToken resets the state
    /// to that new ETag. With no existing state, this backend has issued no same-process stale token
    /// that needs fencing. The post-rename increment cannot allocate or throw. `emu_mutex` spans the
    /// rename and the bump so a concurrent emulated observation never sees the new incarnation with
    /// the old disambiguator.
    std::lock_guard lock(emu_mutex);
    const auto existing_token_state = emu_token_state.find(key);
    try
    {
        std::filesystem::rename(temporary_path, destination_path);
    }
    catch (...)
    {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary_path, cleanup_error);
        throw;
    }
    if (existing_token_state != emu_token_state.end())
        ++existing_token_state->second.second;
}

Token ObjectStorageBackend::emuObserveToken(const String & key)
{
    const auto metadata = object_storage->tryGetObjectMetadata(emuPath(key), /*with_tags=*/false);
    return emuMintToken(key, metadata ? metadata->etag : String{}, /*just_wrote=*/false);
}

Token ObjectStorageBackend::emuMintToken(const String & key, const String & etag, bool just_wrote)
{
    emuPruneTokenState(emuNowNs());

    /// Anomalous: the object storage reported no etag at all (LocalObjectStorage always does; this
    /// guards a hypothetical future/test double). Mint a fresh, UNPERSISTED value — never worse than
    /// the old counter for this case, but never masquerading as a real etag-derived identity.
    if (etag.empty())
        return Token{std::to_string(++emu_seq), TokenType::Emulated};

    auto it = emu_token_state.find(key);
    if (it != emu_token_state.end() && it->second.first == etag)
    {
        /// The etag has not advanced since the last token we minted for this key. For a read-only
        /// observation that is expected (the object simply has not changed) and the SAME value must be
        /// returned. For a just-completed WRITE it means this write's mtime landed in the same quantum
        /// as the previous incarnation's — two DIFFERENT incarnations must still never mint identical
        /// tokens, so bump a small per-key disambiguator (mtime-quantum guard, triage §3.18 19c step 4).
        if (just_wrote)
            ++it->second.second;
        const String value = it->second.second == 0 ? etag : etag + "#" + std::to_string(it->second.second);
        return Token{value, TokenType::Emulated};
    }

    /// The etag advanced (or this key is seen for the first time): the bare etag is the token, and any
    /// previous disambiguator is dropped — a genuinely new incarnation starts clean.
    emu_token_state[key] = {etag, 0};
    return Token{etag, TokenType::Emulated};
}

/// =========================================================================================
/// Backend interface
/// =========================================================================================

ReadSettings ObjectStorageBackend::controlPlaneReadSettings() const
{
    ReadSettings rs = getReadSettings();
    /// Mark the request for the store's native conditional dialect, so a GCS read is answered with a
    /// generation rather than an MD5-shaped ETag.
    rs.object_storage_request_mode = ObjectStorageRequestMode::NativeConditional;
    rs.object_storage_retry_profile = controlPlaneProfile();
    rs.object_storage_attempt_timeout_ms = attempt_timeout_ms;
    return rs;
}

std::optional<Backend::Raw> ObjectStorageBackend::read(const String & key, TransportAccess &)
{
    if (mode == Mode::Native)
    {
        /// ONE request: an S3 GET answers with the incarnation of the bytes it returned, so no HEAD
        /// is needed to name them and no HEAD-to-GET window exists in which the two could disagree.
        /// The value is returned UNVALIDATED -- `CasRequests` is where a response value becomes an
        /// incarnation, and it is the one place that can decide what a malformed one means.
        try
        {
            auto got = object_storage->readSmallObjectAndGetObjectMetadata(
                StoredObject(key), controlPlaneReadSettings(), casMaxStoredObjectBytes());
            return Raw{std::move(got.data), normalizeTokenValue(got.metadata.etag)};
        }
        catch (const std::exception & e)
        {
            /// The object is simply not there; every other error (network, auth, corruption)
            /// propagates unchanged -- fail-closed by construction.
            if (isObjectNotFound(e))
                return std::nullopt;
            throw;
        }
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    /// The emulated path holds emu_mutex across the exists-check and the read, so no concurrent
    /// caller in this process can delete the file in between. External deletion (e.g. a test teardown
    /// racing a read) is still handled: convert FILE_DOESNT_EXIST to nullopt rather than letting it
    /// escape as an unexplained exception.
    Raw raw;
    try
    {
        raw.bytes = emuRead(key);
    }
    catch (const std::exception & e)
    {
        if (isObjectNotFound(e))
            return std::nullopt;
        throw;
    }
    raw.value = emuObserveToken(key).value;
    return raw;
}

std::optional<GetStreamResult> ObjectStorageBackend::getStream(const String & key, Range range)
{
    if (mode == Mode::Native)
    {
        auto hr = nativeHead(key);
        if (!hr)
            return std::nullopt;

        /// The object may be deleted between the HEAD above and the stream open below. Honor the
        /// `optional` contract on a not-found signal; any other error (network, auth, corruption)
        /// propagates unchanged — fail-closed by construction.
        GetStreamResult sr;
        try
        {
            sr.stream = openObjectRangedStream(*object_storage, key, range, hr->size);
        }
        catch (const std::exception & e)
        {
            if (isObjectNotFound(e))
                return std::nullopt;
            throw;
        }
        sr.token = Token{hr->value, native_token_type};
        return sr;
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    /// The emulated path holds emu_mutex across the exists-check and the stream open, matching `read`.
    /// External deletion still converts to nullopt rather than escaping as an unexplained exception.
    GetStreamResult sr;
    try
    {
        sr.stream = openObjectRangedStream(*object_storage, emuPath(key), range);
    }
    catch (const std::exception & e)
    {
        if (isObjectNotFound(e))
            return std::nullopt;
        throw;
    }
    sr.token = emuObserveToken(key);
    return sr;
}

std::unique_ptr<ReadBuffer> ObjectStorageBackend::stream(const String & key, TransportAccess &)
{
    /// Absence must be a null buffer, and a lazily-issued object-storage read cannot report it at
    /// open, so this keeps `getStream`'s HEAD-then-open shape and folds that body in here once
    /// `getStream`'s last caller is gone.
    auto sr = getStream(key, Range{});
    if (!sr)
        return nullptr;
    return std::move(sr->stream);
}

std::optional<Backend::RawMeta> ObjectStorageBackend::head(const String & key, TransportAccess &)
{
    if (mode == Mode::Native)
        return nativeHead(key);

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return std::nullopt;

    auto metadata = object_storage->tryGetObjectMetadata(emuPath(key), /*with_tags=*/false);
    /// A path that exists on the Local filesystem but yields no object metadata is a directory, not
    /// an object (`tryGetObjectMetadata` returns nullopt for a directory). HEAD must report it as
    /// not-an-object — otherwise existsFile/getStorageObjects treat a pool sub-dir (e.g. `store`,
    /// traversed by system.remote_data_paths) as a file and a later body read throws EISDIR.
    if (!metadata)
        return std::nullopt;
    return RawMeta{metadata->size_bytes, emuObserveToken(key).value};
}

/// See Backend::probeSentinelRaw / CasBackend.h's ProbeOutcome for the semantics this classifies.
SentinelProbeResult ObjectStorageBackend::probeSentinelRaw(const String & key, TransportAccess & access)
{
    if (mode == Mode::Native)
    {
        try
        {
            /// One `read`: unlike a bodyless HEAD 404, a GET 404 carries a response body, so the
            /// SDK can parse its `<Code>` and a missing key and a missing bucket arrive as different
            /// errors -- which is the whole distinction this probe exists to make.
            auto raw = read(key, access);
            if (!raw)
                return {ProbeOutcome::KeyAbsent, std::nullopt};
            return {ProbeOutcome::Present, std::move(raw->bytes)};
        }
#if USE_AWS_S3
        catch (const S3Exception & e)
        {
            /// `read` already answers the key-absent half of the store's 404 family (see
            /// `isObjectNotFound`), so what reaches here is what it deliberately does not flatten.
            switch (e.getS3ErrorCode())
            {
                case Aws::S3::S3Errors::NO_SUCH_BUCKET:
                    return {ProbeOutcome::ContainerAbsent, std::nullopt};
                case Aws::S3::S3Errors::ACCESS_DENIED:
                    return {ProbeOutcome::AccessDenied, std::nullopt};
                default:
                    /// Everything else (timeouts, 5xx, throttling, an unmodeled code) is inconclusive —
                    /// NEVER promoted to KeyAbsent, per the IAM permutation table in spec §2.
                    return {ProbeOutcome::Indeterminate, std::nullopt};
            }
        }
#endif
        catch (...)
        {
            return {ProbeOutcome::Indeterminate, std::nullopt};
        }
    }

    /// EmulatedSingleProcess (Local): stat the configured container directory FIRST — `emuExists`/`read`
    /// alone cannot distinguish "this key is absent" from "the whole pool directory is gone" (Local
    /// listing is best-effort and silently reports zero either way, see LocalObjectStorage::listObjects).
    try
    {
        if (!object_storage->existsOrHasAnyChild(emu_root))
            return {ProbeOutcome::ContainerAbsent, std::nullopt};

        auto raw = read(key, access);
        if (!raw)
            return {ProbeOutcome::KeyAbsent, std::nullopt};
        return {ProbeOutcome::Present, std::move(raw->bytes)};
    }
    catch (...)
    {
        return {ProbeOutcome::Indeterminate, std::nullopt};
    }
}

/// Settings for a genuine Native conditional write. Mark the request for the typed conditional
/// dialect and, on a generation-token store, force a single PUT: GCS does not enforce the condition
/// on multipart completion. Blob publication never uses these settings; it remains an ordinary
/// unconditional multipart-capable write. CAS-mutable keys (shard manifests, gc/state, the registry)
/// also skip the racy post-upload existence/size check; a publish's manifest CAS was observed racing
/// the GC fence there.
WriteSettings ObjectStorageBackend::conditionalWriteSettings() const
{
    WriteSettings ws;
    ws.object_storage_request_mode = ObjectStorageRequestMode::NativeConditional;
    if (native_token_type == TokenType::Generation)
        ws.s3_force_single_part_upload = true;
    ws.s3_check_objects_after_upload_override = false;
    /// Exactly one attempt at the WriteBufferFromS3 layer too: makeSinglepartUpload/
    /// completeMultipartUpload run their OWN retry loop above the S3 client, reissuing the identical
    /// (conditional!) request on NO_SUCH_KEY — a client-level override alone does not bound it. Plain
    /// size_t field, harmless (ignored) for a non-S3 write path.
    ws.s3_max_unexpected_write_error_retries_override = 1;
    /// Exactly one HTTP attempt for every conditional write: the object storage resolves the
    /// profile to its own single-attempt client. A backend that cannot honor it is rejected for
    /// writable Native mounts by checkConditionalWriteSingleAttemptSupport (fail closed).
    ws.object_storage_retry_profile = ObjectStorageRetryProfile::SingleAttempt;
    return ws;
}

/// See the declaration in the header for the policy. Centralizes the generation-vs-ETag attribution
/// decision for all successful conditional non-blob writes, including create-if-absent artifacts
/// and conditional replacements.
///
/// The strict Generation-dialect check below is gated on `etag.has_value()`, not merely on
/// `native_token_type`: `WriteBufferFromS3` unconditionally assigns `object_etag = outcome.GetResult().GetETag()`
/// on BOTH of its success paths -- `makeSinglepartUpload` (WriteBufferFromS3.cpp) and
/// `completeMultipartUpload` (WriteBufferFromS3.cpp) -- so a successful S3 write always leaves
/// `getResultObjectETag()` holding a value, empty string included; `has_value()` is exactly "this was
/// a real S3-style write response". `S3ObjectStorage::writeObject` returns that `WriteBufferFromS3`
/// directly, undecorated, so this holds for the whole CAS-over-S3 write path with no wrapping in
/// between. A storage with no write-time-token concept at all (local files) reports `nullopt`
/// structurally rather than because anything broke -- but it still cannot attribute the write, which
/// is why both cases end in the same refusal.
Token ObjectStorageBackend::tokenFromWriteResult(const String & key, const std::optional<String> & etag)
{
    if (native_token_type == TokenType::Generation && etag.has_value())
    {
        /// Validate the MINTED value, not the raw one: the HTTP boundary presents the generation
        /// through the SDK's ETag field and therefore quotes it, and `tokenForHead` is what strips
        /// that transport syntax. Validating before the strip would reject every real GCS write.
        /// The message still reports the raw arrival, since that is what needs diagnosing.
        const Token token = tokenForHead(*etag);
        if (!isValidTokenValue(TokenType::Generation, token.value))
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS on GCS: a conditional write to {} succeeded but its response carried no "
                "valid generation ({}) -- there is no follow-up HEAD to patch this over, so the write "
                "cannot be attributed to an incarnation",
                key, *etag);
        return token;
    }

    if (etag && !etag->empty())
        return tokenForHead(*etag);

    /// No incarnation came back: an empty value from a real write response, or no value at all from a
    /// storage that has no write-time token. Either way this call cannot say which incarnation it
    /// created, and no follow-up read could either -- the object it read back might be someone
    /// else's. The write may have committed, so the caller must resolve it, not this seam.
    throw Exception(ErrorCodes::CAS_WRITE_UNATTRIBUTED,
        "CAS backend: the store accepted a write of '{}' but returned no incarnation; the write may "
        "have committed and must be resolved by reading back", key);
}

std::expected<String, Backend::RawConflict> ObjectStorageBackend::write(
    const String & key, const String & bytes, const std::optional<String> & expected_value, TransportAccess &)
{
    /// An empty, wildcard or list value would turn the precondition into an unconditional write --
    /// refuse it as a caller bug before anything else runs.
    if (expected_value && !isValidTokenValue(dialect(), *expected_value))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS backend: refusing a conditional mutation of '{}' with a malformed token '{}' (dialect {}): "
            "an empty, wildcard or list token would turn the precondition into an unconditional write",
            key, *expected_value, static_cast<int>(dialect()));

    if (mode == Mode::Native)
    {
        WriteSettings ws = conditionalWriteSettings();
        if (expected_value)
            ws.object_storage_write_if_match = *expected_value;
        else
            ws.object_storage_write_if_none_match = "*";
        return nativeConditionalPut(key, bytes, ws);
    }

    std::lock_guard lock(emu_mutex);
    const bool exists = emuExists(key);
    if (!expected_value)
    {
        if (exists)
            return std::unexpected(RawConflict{});
    }
    else
    {
        if (!exists)
            return std::unexpected(RawConflict{});
        if (!tokenMatches(emuObserveToken(key), Token{*expected_value, TokenType::Emulated}))
            return std::unexpected(RawConflict{});
    }

    return emuWrite(key, bytes).value;
}

void ObjectStorageBackend::publish(const BlobPublishRequest & request, TransportAccess &)
{
    if (const auto * streaming = std::get_if<StreamingBlobPublication>(&request.publication))
    {
        std::unique_ptr<ReadBuffer> payload = streaming->open_payload();
        if (!payload)
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ObjectStorageBackend::publish: payload source for {} returned no reader",
                request.destination_key);

        if (mode != Mode::Native)
        {
            /// Streams straight into the temporary file and renames -- see emuPublishBlobAtomically.
            emuPublishBlobAtomically(
                request.destination_key, streaming->fresh_envelope, *payload, streaming->payload_size);
            return;
        }

        /// Ordinary unconditional rewrite: default request mode, retry profile, and multipart policy.
        /// In particular, generation stores are not restricted by the conditional single-PUT cap.
        auto out = object_storage->writeObject(
            StoredObject(request.destination_key),
            WriteMode::Rewrite,
            /*attributes=*/std::nullopt,
            DBMS_DEFAULT_BUFFER_SIZE,
            WriteSettings{});
        out->write(streaming->fresh_envelope.data(), streaming->fresh_envelope.size());
        blob_publication_detail::BlobPayloadCopyResult copy_result;
        try
        {
            copy_result = blob_publication_detail::copyBlobPayloadBounded(*payload, *out, streaming->payload_size);
        }
        catch (...)
        {
            out->cancel();
            throw;
        }
        if (!copy_result.exact(streaming->payload_size))
        {
            out->cancel();
            throw Exception(
                ErrorCodes::CORRUPTED_DATA,
                "ObjectStorageBackend::publish: source yielded {}{} payload bytes for {}, declared {} -- upload aborted, nothing published",
                copy_result.has_excess ? "more than " : "",
                copy_result.copied,
                request.destination_key,
                streaming->payload_size);
        }
        out->finalize();
        return;
    }

    const auto & staged = std::get<VerbatimStagedBlobPublication>(request.publication);
    if (mode != Mode::Native)
        throw Exception(
            ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::publish: verbatim staged publication requires Native mode");

    WriteSettings write_settings;
    write_settings.object_storage_copy_mode = ObjectStorageCopyMode::NativeOnly;
    if (!object_storage->supportsCopyMode(write_settings.object_storage_copy_mode))
        throw Exception(
            ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::publish: object storage {} does not support native-only same-store copy",
            object_storage->getName());

    object_storage->copyObject(
        StoredObject(staged.object_key),
        StoredObject(request.destination_key),
        getReadSettings(),
        write_settings);
}

Backend::RawRemoval ObjectStorageBackend::remove(const String & key, const String & expected_value, TransportAccess &)
{
    /// Same grammar guard as `write`, and for the same reason: an empty, wildcard or list value would
    /// turn the condition into an unconditional delete.
    if (!isValidTokenValue(dialect(), expected_value))
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS backend: refusing a conditional mutation of '{}' with a malformed token '{}' (dialect {}): "
            "an empty, wildcard or list token would turn the precondition into an unconditional write",
            key, expected_value, static_cast<int>(dialect()));

    if (mode == Mode::Native)
    {
        /// `NOT_IMPLEMENTED` from a storage that does not enforce conditional removal propagates —
        /// fail-closed by construction.
        auto result = object_storage->removeObjectIfTokenMatches(
            StoredObject(key), expected_value, controlPlaneProfile(), attempt_timeout_ms);
        switch (result.outcome)
        {
            case ConditionalRemoveOutcome::Removed:
                /// A delete marker means the storage archived a noncurrent version instead of
                /// reclaiming the current object -- a removal that did not reclaim.
                return result.created_delete_marker ? RawRemoval::DeleteMarker : RawRemoval::Removed;
            case ConditionalRemoveOutcome::TokenMismatch:
                return RawRemoval::Mismatch;
            case ConditionalRemoveOutcome::NotFound:
                return RawRemoval::Gone;
        }
        UNREACHABLE();
    }

    std::lock_guard lock(emu_mutex);
    if (!emuExists(key))
        return RawRemoval::Gone;
    if (!tokenMatches(emuObserveToken(key), Token{expected_value, TokenType::Emulated}))
        return RawRemoval::Mismatch;

    object_storage->removeObjectIfExists(StoredObject(emuPath(key)));
    /// Keep the deleted incarnation's last-minted etag around ONLY while a same-mtime-quantum
    /// collision with an immediate recreate is still possible (emuMintToken) — once it is
    /// comfortably old, erase it so `emu_token_state` does not grow for the lifetime of the backend
    /// instance.
    if (auto it = emu_token_state.find(key); it != emu_token_state.end())
    {
        const uint64_t now_ns = emuNowNs();
        if (etagComfortablyInThePast(it->second.first, now_ns))
            emu_token_state.erase(it);
        else
            emu_token_expiry.push_back(EmuTokenExpiry{now_ns, key, it->second});
    }
    return RawRemoval::Removed;
}

Backend::RawListPage ObjectStorageBackend::list(const String & prefix, const String & cursor, size_t limit, TransportAccess &)
{
    /// Use the lazy object-storage iterator instead of `listObjects(..., max_keys=0)`: the latter
    /// materialized the whole prefix, then sliced client-side, so a paginated walk re-fetched the full
    /// subtree for every page. The backend cursor is "last key returned" (exclusive on resume).
    ///
    /// Some backends ignore `start_after`; filtering `key <= cursor` keeps the contract correct there,
    /// only losing the resume optimization. S3 honors `start_after` and avoids the hot-path re-scan.
    if (limit == 0)
        return {};

    const String physical_prefix = (mode == Mode::EmulatedSingleProcess) ? emuPath(prefix) : prefix;
    const String strip = (mode == Mode::EmulatedSingleProcess) ? emuPath("") : String{};
    if (mode == Mode::EmulatedSingleProcess)
    {
        RelativePathsWithMetadata children;
        object_storage->listObjects(physical_prefix, children, /*max_keys=*/0);

        /// Hold emu_mutex across the whole scan: emuMintToken below reads/updates emu_token_state, the
        /// same per-key state read/head/write/remove mutate under this lock (see the "caller holds
        /// emu_mutex" contract on the private emu* helpers).
        std::lock_guard lock(emu_mutex);

        std::vector<RawListedKey> all;
        all.reserve(children.size());
        for (const auto & child : children)
        {
            if (!child->relative_path.starts_with(physical_prefix))
                continue;
            RawListedKey lk;
            lk.key = child->relative_path.substr(strip.size());
            lk.size = child->metadata ? child->metadata->size_bytes : 0;
            if (child->metadata)
                lk.value = emuMintToken(lk.key, child->metadata->etag, /*just_wrote=*/false).value;
            all.push_back(std::move(lk));
        }
        std::sort(all.begin(), all.end(), [](const RawListedKey & a, const RawListedKey & b) { return a.key < b.key; });

        RawListPage page;
        auto all_it = cursor.empty()
            ? std::lower_bound(all.begin(), all.end(), prefix, [](const RawListedKey & a, const String & s) { return a.key < s; })
            : std::upper_bound(all.begin(), all.end(), cursor, [](const String & s, const RawListedKey & a) { return s < a.key; });
        while (all_it != all.end() && page.keys.size() < limit)
        {
            page.keys.push_back(*all_it);
            ++all_it;
        }
        if (!page.keys.empty() && all_it != all.end())
            page.next_cursor = page.keys.back().key;
        return page;
    }

    const std::optional<String> start_after = cursor.empty()
        ? std::nullopt
        : std::optional<String>(cursor);

    RawListPage page;
    auto it = object_storage->iterate(
        physical_prefix, /*max_keys=*/0, /*with_tags=*/false, start_after, controlPlaneProfile(), attempt_timeout_ms);
    for (; it->isValid(); it->next())
    {
        const auto child = it->current();
        if (!child->relative_path.starts_with(physical_prefix))
            continue;

        RawListedKey lk;
        lk.key = child->relative_path.substr(strip.size());
        if (!cursor.empty() && lk.key <= cursor)
            continue;

        lk.size = child->metadata ? child->metadata->size_bytes : 0;
        /// Surface the per-key incarnation value (matching what `head` would return) so the
        /// `supportsListTokens() == true` capability is honest. A listing without an etag leaves it
        /// unset, which GC discover treats as Read (fail closed). The supportsListTokens()+
        /// empty-etag gate lives in tokenForList, which is also why the check below names the ETag
        /// dialect: a generation store surfaces no list value at all.
        if (child->metadata)
        {
            if (const auto token = tokenForList(child->metadata->etag))
            {
                /// Same grammar check as nativeHead: a listed value that fails this backend's dialect
                /// grammar is a broken response, not a value to hand a future conditional operation.
                if (!isValidTokenValue(token->type, token->value))
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS backend: the store answered for '{}' with a value '{}' that is not a valid ETag incarnation",
                        lk.key, token->value);
                lk.value = token->value;
            }
        }

        if (page.keys.size() == limit)
        {
            page.next_cursor = page.keys.back().key;
            break;
        }
        page.keys.push_back(std::move(lk));
    }

    return page;
}

}

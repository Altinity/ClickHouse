#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h>
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace DB { class WriteBuffer; }

namespace DB::Cas
{

/// Re-readable source for one content-addressed blob upload.
///
/// `write_payload` supplies exactly `size` logical bytes and may be invoked more than once: a conditional
/// upload can race with another writer or with GC, in which case the transaction retries from the writer's
/// own source. `server_side_copy_from` is set only for an S3 staging object; the backend then copies from
/// that object instead of streaming through ClickHouse. The staging object must remain available through a
/// condemned-object resurrection.
struct BlobSource
{
    uint64_t size = 0;
    std::function<void(WriteBuffer &)> write_payload;   /// must write exactly `size` bytes
    /// When set, the blob's bytes already live in an S3 staging object with this key, and `putBlob` promotes it by a
    /// WRITE-ONCE conditional SERVER-SIDE COPY (`Backend::promoteStaged`) instead of streaming
    /// `write_payload` — and resurrects a condemned incarnation by an unconditional server-side copy
    /// from the SAME staging object (`Backend::resurrectStaged`), never a read of the condemned blob
    /// (revival must always be a fresh write from the source). Unset (the default, `StagingBackend::Local`) ⇒ the local
    /// streaming path is byte-for-byte unchanged and `write_payload` is the source.
    std::optional<String> server_side_copy_from;
    /// Build a re-readable source backed by an owned string; intended for small payloads and tests.
    static BlobSource fromString(String bytes);
};

/// putBlob's return value: the `BlobRef` it was addressed by (the write mint's algo + digest pair)
/// plus the admitted logical size.
struct PutBlobResult
{
    BlobRef ref;
    uint64_t size = 0;
};

/// Hash `payload` with `algo` using the same convention as the streaming blob writer and return the complete
/// `BlobRef` identity. The algorithm travels with the digest; callers must not reconstruct a blob identity from
/// a bare digest or from an independently supplied digest width.
BlobRef poolContentHash(BlobHashAlgo algo, std::string_view payload);

/// Coordinates one part write from manifest staging through blob admission and ref publication. The transaction
/// owns the in-memory dependency set and the identities of manifests staged by this build; `Pool` owns the
/// durable object store and ref-log operations. A transaction is normally used by one writer thread, while
/// `cancelForNamespaceRemoval` may set its cancellation flag from the namespace-removal thread.
///
/// The durable write order is `stageManifest` → `precommitAdd` → `putBlob` → `promote`. The precommit edge
/// must be durable before any existing blob incarnation is adopted, because the edge is what protects that
/// incarnation from GC while this build is in flight. `promote` then moves the same manifest owner binding
/// atomically from precommit to committed. A failed or abandoned transaction never resumes after a process
/// restart; its precommit is removed by the live owner or a fenced successor, and GC reclaims the resulting
/// debris only after the corresponding ref-log decrements are durable.
class PartWriteTxn
{
public:
    /// Start a build and emit its durable in-flight-build attribution. The identity arguments identify the
    /// build for ownership and GC fencing; `info_` is retained as immutable build context.
    PartWriteTxn(PoolPtr store_, UInt128 build_id_,
          uint64_t build_seq_, uint64_t epoch_, PartWriteInfo info_);

    /// Retire this build's sequence so the pool's active-build watermark can advance. This is idempotent when
    /// `promote` or `abandon` already retired the sequence, and also covers destruction during unwinding.
    ~PartWriteTxn();

    /// Every upload attempt mints a fresh random `incarnation_tag`.
    /// New content: streaming PUT If-None-Match:*; on PreconditionFailed ⇒ the cold-reuse rule
    /// (observe current token; condemned ⇒ uploadFromSource — re-upload from the writer's source
    /// bytes; else adopt — free).
    /// Ordering: `putBlob` is always called after `precommitAdd` (the wiring order is
    /// `stageManifest` → `precommitAdd` → `putBlob` → `promote`). Its
    /// ADOPT paths observe an existing incarnation, so they are safe only under this build's durable
    /// precommit closure — enforced by a fail-closed throw (LOGICAL_ERROR, not a `chassert`, which is
    /// compiled out in release) in observeAndAdmit. A FRESH upload before precommit is legal
    /// (newborn-debris watermark), but production never does it.
    PutBlobResult putBlob(const BlobRef & ref, BlobSource source);

    /// Return whether this build holds a TOKENED Blob dep for `ref` (`putBlob` ⇒
    /// tokened) versus a tokenless evidence dep (`adoptEvidence` ⇒ tokenless)? False also when this
    /// build has no dep for the ref at all.
    bool depIsTokened(const BlobRef & ref) const;

    /// Record a TOKENLESS evidence blob dep directly from a `ManifestEntry` — no HEAD or backend
    /// call. Lets staging adopt sites record the dep by hash without asserting presence before
    /// precommit; the promote gate observes/resurrects it post-precommit. Inline entries record nothing.
    void adoptEvidence(const ManifestEntry & entry);

    /// Record a TOKENLESS pending blob dep by ref (without a HEAD) for a blob whose bytes are staged locally and
    /// will be putBlob'd post-precommit. putBlob later overwrites it with the tokened dep on upload.
    void recordPendingBlobDep(const BlobRef & ref, uint64_t size);

    /// Mint a root-local part `ManifestId`, write its body under
    /// `cas/manifests/<ns>/<writer_epoch>/<build_sequence>/000001.zst` via the pool's shared request
    /// controller. It uses budgeted attempts with resolve-before-reissue and performs no preliminary HEAD,
    /// because `manifest_ordinal` is monotone within this build. It enforces manifest-size caps before the body
    /// write returns and therefore before any owner transition is published. The body is not retained after a
    /// successful write; on retry the caller re-stages from source. Every call uses a fresh manifest ordinal.
    /// The id is recorded for best-effort `abandon` cleanup.
    ManifestId stageManifest(std::vector<ManifestEntry> entries);

    /// Add this transaction's precommit owner intent; there is no `_precommits` namespace. One
    /// `appendRefOps` call appending an OwnerTransition `RefOp` (new_binding = {Precommit,
    /// final_ref_name, id.ref}) to final_ref_name's ref-log entry, so the later promote is an atomic
    /// owner move over that same entry. Needs NO body-exists HEAD as a safety authority: GC and
    /// promotion handle a missing precommit manifest body by failing closed (a missing-body precommit
    /// is a non-activating, non-promotable intent).
    void precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id);

    /// Atomically promote the precommit to the committed ref with one `appendRefOps` call on the target ref's
    /// ref-log entry.
    ///  1. tokened leaves are already protected by the durable precommit edge, so no writer-side retired-view
    ///     refresh is needed;
    ///  2. stream-read the precommit manifest body; validate RefMatchesBody / ManifestNamespaceMatches;
    ///  3. the NON-tokened blob leaves (tokened leaves are edge-protected, not re-checked): a committed-source
    ///     adoptEvidence leaf is TRUSTED via the durable manifest edge — NO per-file HEAD/loadMeta probe (§4
    ///     manifest-trust: the live source pins the blob, in-degree >= 1); a genuinely
    ///     absent adopted blob is an invariant violation caught by fsck, not here;
    ///  4. a body-absent precommit or a lost owner-liveness ⇒ ABORTED; a non-tokened, non-adopted leaf (no
    ///     tokened dep and no committed-source adopt — a staging bug) ⇒ LOGICAL_ERROR (fail closed);
    ///  5. atomically replace precommit(build_id) owner with committed(final_ref_name) owner by appending
    ///     ONE pure-move `RefOp` (old_binding={Precommit,final_ref_name,T}, new_binding={Committed,final_ref_name,T},
    ///     same manifest_ref T) and setting refs[final_ref_name];
    ///  6. promotion never emits blob deltas. A missing-body precommit
    ///     is non-activating and was rejected at step 4 (the writer re-stages with a fresh ManifestId).
    ///
    /// PROCESS-RESTART INVARIANT: a `PartWriteTxn` is a plain in-memory C++ object owned by the wiring's
    /// `ContentAddressedTransaction` — it is NEVER persisted and NEVER resumed across a process
    /// restart. There is no "replay a precommit" code path anywhere in the core: `promote` is called
    /// synchronously, in-process, strictly AFTER every referenced blob's `putBlob` (which for S3
    /// staging drives `promoteStaged`'s conditional copy) has already returned successfully. If the
    /// process exits between `precommitAdd` and `promote` (e.g. between staging a blob and its
    /// server-side-copy promote completing), the `PartWriteTxn` object is simply lost with it: nothing ever
    /// "wakes up" that precommit and finishes promoting it. The precommit's owner binding is left as a
    /// dead intent in the ref log and is REMOVED (never promoted) by an exact precommit-removal ref-log
    /// transaction -- the current writer's own `PartWriteTxn::abandon` if it is still mounted, otherwise a
    /// fenced successor's stale-precommit sweep. `GC` folds the resulting
    /// `-1` manifest edge but never detects or removes a dead precommit itself. So the
    /// hazard — "promote a precommit whose copy did not complete" — has no code
    /// path to occur through: promotion is not a recoverable/resumable operation, only a synchronous
    /// one that either completes within the writing process or never happens at all.
    /// `allow_repoint` opts into an intended
    /// repoint of a committed ref that already names a DIFFERENT manifest -- a standalone write/remove
    /// on an already-committed part (the committed-publish machinery's missing piece; the promote guard's
    /// own error text already named it: "use republishRef for an intended repoint"). Default `false`
    /// preserves the existing unique-ref guard byte-for-byte: a committed ref naming a different manifest
    /// still throws ABORTED. With `true`, the guard is skipped and the old committed binding is retired
    /// in the SAME ref-log record as the ordinary precommit->committed promotion, plus a
    /// `CasEventType::RefRepoint` audit event -- every effective repoint is loud by construction.
    void promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id, bool allow_repoint = false);

    /// Retire the build sequence so the GC watermark floor can advance; staged manifest debris is best-effort
    /// cleaned, with the orphan sweep as the durable backstop.
    void abandon();

    /// Called by `Pool::dropNamespace` for every in-flight build once its namespace-removal transaction is
    /// durable. If
    /// this build's owning namespace equals `removed_ns`, mark it cancelled so every further operation
    /// fails closed at `requireAlive` (ABORTED); a build in any other namespace is left untouched.
    /// Cross-thread safe: reads only the immutable `info` (the owning namespace) and stores ONE atomic --
    /// it touches no other member, so the build's own thread may keep running concurrently. Staged debris
    /// is cleaned best-effort when the build's own thread later runs `abandon` (or via the GC backstop).
    void cancelForNamespaceRemoval(const RootNamespace & removed_ns);

    UInt128 buildId() const { return build_id; }
    /// The strictly increasing per-process sequence used by the active-build watermark.
    uint64_t buildSeq() const { return build_seq; }

private:
    /// One blob dependency recorded by this build. A token identifies an incarnation uploaded by this
    /// transaction and must be retained through promotion; a tokenless adopted entry relies on the durable
    /// source-manifest edge instead. `adopted` distinguishes that trusted source evidence from a pending
    /// upload that has not yet been completed by `putBlob`.
    struct DepEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        std::optional<Token> token;                       /// nullopt = live-source evidence
        uint64_t size = 0;
        /// True only for `adoptEvidence` — a committed-source evidence dep, trusted at promote. A
        /// tokenless dep with adopted=false is a PENDING upload (`recordPendingBlobDep`) that must be
        /// tokened by putBlob before promote; reaching promote un-tokened is a staging bug (fail closed).
        bool adopted = false;
    };
    /// Keyed on the full `BlobRef` pair (algorithm + digest), because a bare digest is not a blob identity.
    /// This remains an ordered `std::map` (not `unordered_map`): `BlobRef` already provides `operator<=>`, so
    /// no hasher is needed here. `BlobRefHash` is for unordered dedup-cache/set consumers elsewhere. The
    /// dependencies are blob-only, so `ObjectKind` is not part of the key.
    using DepKey = BlobRef;

    /// Apply the cold-reuse rule: HEAD the key; absent ⇒ FILE_DOESNT_EXIST;
    /// condemned-at-current-token ⇒ throw ABORTED (caller must re-upload from its own source bytes);
    /// else record the current token as the dep. Returns the admitted size.
    uint64_t observeAndAdmit(ObjectKind kind, const BlobRef & ref, const String & key);
    /// Overload for callers that already hold a fresh, present HeadResult for `key` (the putBlob
    /// HEAD-before-PUT path), avoiding a redundant second HEAD. `hr.exists` MUST be true.
    uint64_t observeAndAdmit(ObjectKind kind, const BlobRef & ref, const String & key, const HeadResult & hr);
    /// INV-1 (revival-from-source): revive a condemned or absent object by re-uploading from the writer's
    /// OWN re-readable source without reading the dying object (no backend().get). The source is STREAMED
    /// into the put sink (header + `source.write_payload`), never materialized into a full in-memory copy;
    /// `source.write_payload` may be re-invoked on each conditional-write attempt (it re-reads the staged
    /// temp file / re-emits the captured String), so it is taken by const ref and not consumed.
    void uploadFromSource(ObjectKind kind, const BlobRef & ref, const String & key, const BlobSource & source);

    /// The build's owning root namespace, derived from PartWriteInfo::intended_ref ("ns/ref" — the ref is the
    /// last `/`-segment; the namespace is everything before it). Sets a manifest body's root_namespace_id.
    RootNamespace manifestNamespace() const;

    /// Reject operations after `abandon`, namespace cancellation, or writer-epoch fencing. These checks happen
    /// before backend work so a stale transaction cannot publish a new owner or stage more debris.
    void requireAlive() const;
    /// Best-effort exact-token delete of THIS build's staged `_manifests` debris; the precommit body (if
    /// any) is SKIPPED -- left for GC's delete-after-sealed-decrements. Never throws (the namespace-scoped orphan
    /// sweep is the durable backstop). Shared by the normal and the namespace-removal-cancelled `abandon`
    /// paths; only ever called on the build's OWN thread.
    void cleanupStagedManifestDebrisBestEffort();

    /// A leaf is trusted at promote iff this build holds a TOKENLESS dep recorded by
    /// `adoptEvidence` (adopted=true) — a committed-source evidence adopt. The live source pins the blob
    /// (in-degree >= 1, not condemnable) and this build's precommit edge is durable, so the durable manifest
    /// edge is the liveness evidence: no HEAD, no loadMeta, no copy-forward. A tokened dep (edge-protected,
    /// handled by `depIsTokened`), a tokenless PENDING-upload dep (adopted=false), or NO dep at all (a
    /// staging bug) is NOT trusted — it fails closed. The single gate for the promote non-tokened leaf.
    bool isTrustedAdopt(const BlobRef & ref) const;

    PoolPtr store;
    UInt128 build_id{};
    uint64_t build_seq{};                                 /// per-process monotone sequence
    uint64_t epoch{};                                     /// owning Pool's process_epoch
    uint32_t next_manifest_ordinal = 1;                   /// per-build monotone manifest ordinal
    PartWriteInfo info;
    bool alive = true;
    /// Set by `cancelForNamespaceRemoval` from `Pool::dropNamespace`'s thread
    /// once this build's owning namespace is durably removed. Atomic because it is WRITTEN cross-thread
    /// and READ by `requireAlive` on the build's own thread. Once cancelled, every further op fails closed.
    std::atomic<bool> cancelled{false};
    bool precommitted = false;                            /// a create-precommit owner_transition was appended

    /// Set by precommitAdd so promote knows which shard to move ownership in.
    RootNamespace precommit_target_ns;
    String precommit_final_ref;
    ManifestRef precommit_manifest;

    std::vector<ManifestId> staged_manifests;             /// for best-effort abandon cleanup

    std::map<DepKey, DepEntry> deps;                      /// dependencies recorded by this build (blobs only)
};

}

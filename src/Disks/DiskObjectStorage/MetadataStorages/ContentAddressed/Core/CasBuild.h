#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestCodec.h>
#include <atomic>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace DB { class WriteBuffer; }

namespace DB::Cas
{

struct BlobSource
{
    uint64_t size = 0;
    std::function<void(WriteBuffer &)> write_payload;   /// must write exactly `size` bytes
    /// S3-native staging promote (spec 2026-07-11-cas-s3-native-staging §5/§8): when set, the blob's
    /// bytes already live in an S3 staging object with THIS key, and `putBlob` promotes it by a
    /// WRITE-ONCE conditional SERVER-SIDE COPY (`Backend::promoteStaged`) instead of streaming
    /// `write_payload` — and resurrects a condemned incarnation by an unconditional server-side copy
    /// from the SAME staging object (`Backend::resurrectStaged`), never a read of the condemned blob
    /// (`feedback_ca_resurrect_invariant`). Unset (the default, `StagingBackend::Local`) ⇒ the local
    /// streaming path is byte-for-byte unchanged and `write_payload` is the source.
    std::optional<String> server_side_copy_from;
    static BlobSource fromString(String bytes);         /// convenience for small content/tests
};

/// putBlob's return value: the `BlobRef` it was addressed by (the write mint's algo + digest pair)
/// plus the admitted logical size.
struct PutBlobResult
{
    BlobRef ref;
    uint64_t size = 0;
};

/// The ONE write mint (Phase 3 T2): hashes `payload` under `algo` and returns the full `BlobRef` pair
/// (algo travels WITH the digest — never a bare digest). Replaces the old digest-returning overload
/// (which took an explicit `digest_len`); the width now always follows `algo` via `blobHashLenFor`.
BlobRef poolContentHash(BlobHashAlgo algo, std::string_view payload);

/// The writer protocol (spec §5, CA GC root-local part-manifest redesign rev. 15) — the W-rules live
/// HERE, not in the wiring. One Build per written ref. Thread-compat: a Build is used by ONE thread (the
/// wiring's commit path); the per-hash freshness meta it point-reads (`CasBlobMeta.h`) is itself
/// thread-safe (a plain backend point-read/CAS, no shared in-memory state).
///
/// Write path: stageManifest (mint a ManifestId, stream-write the body) -> precommitAdd (append a
/// create-precommit RootOwnerEvent in the target root shard) -> putBlob (blob bodies) -> promote
/// (atomic single-shard owner move precommit->committed, fail-closed revalidation). Only blobs stay
/// content-addressed; a part is one immutable single-owner ManifestId.
class Build
{
public:
    Build(StorePtr store_, UInt128 build_id_,
          uint64_t build_seq_, uint64_t epoch_, BuildInfo info_);
    ~Build();

    /// W-FRESH-TAG: every upload attempt mints a fresh random incarnation_tag.
    /// New content: streaming PUT If-None-Match:*; on PreconditionFailed ⇒ the cold-reuse rule
    /// (observe current token; condemned ⇒ uploadFromSource — re-upload from the writer's source
    /// bytes; else adopt — free).
    /// ORDERING (EDGE-BEFORE-OBSERVE, spec 2026-07-09-cas-writer-gc-simplification): putBlob is always
    /// called AFTER precommitAdd (the wiring order stageManifest → precommitAdd → putBlob → promote). Its
    /// ADOPT paths observe an existing incarnation, so they are safe only under this build's durable
    /// precommit closure — asserted by `chassert(precommitted)` in observeAndAdmit. A FRESH upload before
    /// precommit is legal (newborn-debris watermark), but production never does it.
    PutBlobResult putBlob(const BlobRef & ref, BlobSource source);

    /// B156b discriminator: does this build hold a TOKENED Blob dep for `ref` (putBlob'd here ⇒
    /// tokened) versus a TOKENLESS W-EVIDENCE dep (adoptEvidence ⇒ tokenless)? False also when this
    /// build has no dep for the ref at all.
    bool depIsTokened(const BlobRef & ref) const;

    /// B188: record a TOKENLESS W-EVIDENCE blob dep directly from a ManifestEntry — NO HEAD, no backend
    /// call. Lets staging adopt sites record the dep by hash without asserting presence before
    /// precommit; the promote gate observes/resurrects it post-precommit. Inline entries record nothing.
    void adoptEvidence(const ManifestEntry & entry);

    /// B188: record a TOKENLESS Blob dep by ref (no HEAD) for a blob whose bytes are staged locally and
    /// will be putBlob'd post-precommit. putBlob later overwrites it with the tokened dep on upload.
    void recordPendingBlobDep(const BlobRef & ref, uint64_t size);

    /// Mint a root-local part ManifestId, stream-write its body under
    /// `cas/manifests/<ns>/<writer_epoch>/<build_sequence>/000001.proto` via putIfAbsentStream (NO preliminary HEAD —
    /// the manifest_ordinal is per-build monotone). Enforces the OQ7 caps fail-closed BEFORE the body write
    /// returns (and therefore before any owner transition is published). The body is not retained after a
    /// successful write; on retry the caller re-stages from source. NoManifestIdReuse: a fresh random
    /// manifest_ordinal per call. The id is recorded for best-effort `abandon` cleanup.
    ManifestId stageManifest(std::vector<ManifestEntry> entries);

    /// Build-intent owner add, written to the SAME root shard as the future committed ref (spec §Precommit
    /// Add) — there is no `_precommits` namespace. ONE root-shard CAS appending a RootOwnerEvent
    /// {old=none, new={Precommit, final_ref_name, build_id, id.ref}} to the single ordered journal; shard =
    /// store->shardOf(final_ref_name), so the later promote is an atomic owner move in this same shard.
    /// Needs NO body-exists HEAD as a safety authority: GC and promotion handle a missing precommit
    /// manifest body by failing closed (a missing-body precommit is a non-activating, non-promotable intent).
    void precommitAdd(const RootNamespace & target_ns, const String & final_ref_name, const ManifestId & id);

    /// Atomic commit promotion (spec §Promote Precommit): ONE root-shard CAS in shardOf(final_ref_name).
    ///  1. (no writer-side retire-view refresh — removed in the 2026-07-09 writer-GC simplification;
    ///     tokened leaves are edge-protected via EDGE-BEFORE-OBSERVE);
    ///  2. stream-read the precommit manifest body; validate RefMatchesBody / ManifestNamespaceMatches;
    ///  3. revalidate the NON-tokened blob leaves only (tokened leaves are edge-protected, not re-checked):
    ///     presence observation + per-hash `.meta` point-read, condemned ⇒ copy-forward-from-source (fail-closed);
    ///  4. body absent | a non-tokened blob absent | a blob condemned-and-not-recreatable ⇒ ABORTED;
    ///  5. atomically replace precommit(build_id) owner with committed(final_ref_name) owner by appending
    ///     ONE pure-move RootOwnerEvent (old={Precommit,final_ref_name,build_id,T}, new={Committed,final_ref_name,T},
    ///     same manifest_ref T) and setting refs[final_ref_name];
    ///  6. promotion NEVER emits blob deltas (spec rev. 15 §Promote Precommit). A missing-body precommit
    ///     is non-activating and was rejected at step 4 (the writer re-stages with a fresh ManifestId).
    ///
    /// CRASH-RECOVERY INVARIANT (S3-native staging plan Task 6, design §6 "crash between
    /// precommitAdd(edge) and copy"): a `Build` is a plain in-memory C++ object owned by the wiring's
    /// `ContentAddressedTransaction` — it is NEVER persisted and NEVER resumed across a process
    /// restart. There is no "replay a precommit" code path anywhere in the core: `promote` is called
    /// synchronously, in-process, strictly AFTER every referenced blob's `putBlob` (which for S3
    /// staging drives `promoteStaged`'s conditional copy) has already returned successfully. If the
    /// process exits between `precommitAdd` and `promote` (e.g. between staging a blob and its
    /// server-side-copy promote completing), the `Build` object is simply lost with it: nothing ever
    /// "wakes up" that precommit and finishes promoting it. The precommit's `RootOwnerEvent` is left as
    /// a dead intent, judged build-dead via the durable per-server watermark and REMOVED (never
    /// promoted) by `Gc::reclaimAbandonedPrecommit` (`CasGc.cpp`) once that floor passes it. So the
    /// hazard the design calls out — "promote a precommit whose copy did not complete" — has no code
    /// path to occur through: promotion is not a recoverable/resumable operation, only a synchronous
    /// one that either completes within the writing process or never happens at all.
    void promote(const RootNamespace & target_ns, const String & final_ref_name, UInt128 build_id, const ManifestId & id);

    /// Carry the mutable per-ref payload (txn_version.txt, ...) that promote writes into the ref's mutable files.
    void setPendingMutableFiles(std::map<String, String> files) { pending_mutable_files = std::move(files); }

    /// Retire seq so the GC watermark floor can advance; staged manifest debris is best-effort cleaned
    /// (the Phase-1d orphan sweep is the durable backstop).
    void abandon();

    /// spec §Namespace Removal ("After the transaction is durable, it ... cancels local builds"): called
    /// by `Store::dropNamespace` for every in-flight build ONCE its removal transaction is durable. If
    /// this build's owning namespace equals `removed_ns`, mark it cancelled so every further operation
    /// fails closed at `requireAlive` (ABORTED); a build in any other namespace is left untouched.
    /// Cross-thread safe: reads only the immutable `info` (the owning namespace) and stores ONE atomic --
    /// it touches no other member, so the build's own thread may keep running concurrently. Staged debris
    /// is cleaned best-effort when the build's own thread later runs `abandon` (or via the GC backstop).
    void cancelForNamespaceRemoval(const RootNamespace & removed_ns);

    UInt128 buildId() const { return build_id; }
    /// The strictly-increasing per-process build_seq (spec 2026-06-16).
    uint64_t buildSeq() const { return build_seq; }

private:
    struct DepEntry
    {
        ObjectKind kind = ObjectKind::Blob;
        std::optional<Token> token;                       /// nullopt = live-root evidence (W-EVIDENCE)
        uint64_t size = 0;
    };
    /// Phase 3 T2: keyed on the full `BlobRef` pair (algo + digest) — the blob identity, per spec's
    /// decree that a bare digest is never an identity. Stays an ordered `std::map` (not
    /// `unordered_map`): `BlobRef` already provides `operator<=>`, so no hasher is needed for THIS
    /// container (that's what `BlobRefHash` is for -- the unordered dedup-cache/set consumers
    /// elsewhere). The old `(kind, hash)` pair key is gone: `ObjectKind` is always `Blob` now (the
    /// standalone tree kind was excised 2026-07-03), so dropping it loses no information.
    using DepKey = BlobRef;

    /// The §5 step-2 cold-reuse rule: HEAD the key; absent ⇒ FILE_DOESNT_EXIST;
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

    /// Verified copy-forward (spec 2026-07-02-cas-copy-forward-condemned-evidence.md): the narrow,
    /// deliberate exception to INV-1's "never read the dying object" — allowed ONLY for tokenless
    /// W-EVIDENCE deps (`adoptEvidence`: every call site adopts from a COMMITTED source manifest, so
    /// the blob is referenced by a live committed owner and this is a reference transfer, not a
    /// resurrection). Reads the condemned-but-present incarnation IN FULL, verifies fail-closed
    /// (envelope decodes + recomputed payload hash == `ref`), re-wraps under a fresh envelope
    /// (fresh incarnation_tag, this build's build_id — W-FRESH-TAG), and displaces EXACTLY the
    /// observed incarnation via token-conditional putOverwrite. Every failure mode (absent, corrupt,
    /// lost delete race) throws ABORTED — never a blind PUT, never putIfAbsent after a lost race.
    /// Returns the fresh (or adopted-clean) token. O(blob) resident memory — recovery path only.
    Token copyForwardFromCondemned(const BlobRef & ref, const String & key, HeadResult hr);

    /// The build's owning root namespace, derived from BuildInfo::intended_ref ("ns/ref" — the ref is the
    /// last `/`-segment; the namespace is everything before it). Sets a manifest body's root_namespace_id.
    RootNamespace manifestNamespace() const;

    void requireAlive() const;                            /// throws LOGICAL_ERROR after abandon
    /// Best-effort exact-token delete of THIS build's staged `_manifests` debris; the precommit body (if
    /// any) is SKIPPED -- left for GC's delete-after-sealed-decrements. Never throws (the Phase-1d orphan
    /// sweep is the durable backstop). Shared by the normal and the namespace-removal-cancelled `abandon`
    /// paths; only ever called on the build's OWN thread.
    void cleanupStagedManifestDebrisBestEffort();

    /// A leaf is copy-forwardable iff this build holds a TOKENLESS W-EVIDENCE Blob dep for `hash`
    /// (adoptEvidence — in production always sourced from a committed manifest, which is the copy-forward
    /// exception's PROVENANCE; it is NOT a checked runtime guarantee here). The enforced runtime safety
    /// rests on the caller's owner-liveness check + fold barrier (see the in-closure gate in `promote`),
    /// exactly as the tokened resurrect did. A tokened dep, or NO dep at all (a staging bug — must fail
    /// closed), is NOT copy-forwardable. The single gate for the promote copy-forward site (D3).
    bool isCopyForwardableTokenless(const BlobRef & ref) const;

    StorePtr store;
    UInt128 build_id{};
    uint64_t build_seq{};                                 /// per-process monotone seq (spec 2026-06-16)
    uint64_t epoch{};                                     /// owning Store's process_epoch
    uint32_t next_manifest_ordinal = 1;                   /// per-build monotone manifest ordinal
    BuildInfo info;
    bool alive = true;
    /// spec §Namespace Removal: set by `cancelForNamespaceRemoval` (from `Store::dropNamespace`'s thread)
    /// once this build's owning namespace is durably removed. Atomic because it is WRITTEN cross-thread
    /// and READ by `requireAlive` on the build's own thread. Once cancelled, every further op fails closed.
    std::atomic<bool> cancelled{false};
    bool precommitted = false;                            /// a create-precommit owner_transition was appended

    /// Set by precommitAdd so promote knows which shard to move ownership in.
    RootNamespace precommit_target_ns;
    String precommit_final_ref;
    ManifestRef precommit_manifest;

    std::map<String, String> pending_mutable_files;       /// promote writes these into the ref's mutable files
    std::vector<ManifestId> staged_manifests;             /// for best-effort abandon cleanup

    std::map<DepKey, DepEntry> deps;                      /// the W-DEP-SET (blob-only now)
};

}

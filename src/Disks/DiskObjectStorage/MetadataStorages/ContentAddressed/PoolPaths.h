#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace DB::ContentAddressed
{

// Every key builder takes the object-storage common key prefix as its FIRST argument and prepends
// it to the bare key. An EMPTY prefix yields exactly the old bare key (no leading slash), so the
// fan-out / store layout below is unchanged when no prefix is configured. The single source of
// truth for the prefix is ContentAddressedMetadataStorage::storage_path_prefix, threaded through
// the metadata storage and its transaction so the read and write sides can never disagree.

// Content-addressed object keys with 2x2 hex prefix fan-out (S3 per-prefix limits). The builders
// take a typed bare identity (BlobHash / PartId) and return a typed full object key
// (BlobObjectKey / PartObjectKey) so a bare hash and a full key can never be confused.
BlobObjectKey blobKey(const std::string & key_prefix, const BlobHash & blob_hash);
PartObjectKey partKey(const std::string & key_prefix, const PartId & part_id);

// Pool roots for enumeration (GC scan). Each is the object-key prefix under which all objects of a
// given kind live: manifests under partsPrefix (<key_prefix>/parts), content blobs under blobsPrefix
// (<key_prefix>/blobs), and every server's/table's refs under refsRootPrefix (<key_prefix>/store/).
// They are empty-prefix-safe (an empty key_prefix yields the bare root) and consistent with the
// per-object key builders above, so listing under them enumerates exactly those keys.
std::string partsPrefix(const std::string & key_prefix);
std::string blobsPrefix(const std::string & key_prefix);
std::string refsRootPrefix(const std::string & key_prefix);

// Per-server/per-table ref object key: <key_prefix>/store/<server_id>/<table_uuid>/refs/<part_name>.
std::string refsPrefix(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid);
RefObjectKey refKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name);

// Per-ref sidecar object key: <key_prefix>/store/<server_id>/<table_uuid>/refs/<part_name>.meta. It
// holds the part's mutable per-part files (RefSidecar). It lives UNDER the same refs/ prefix the ref
// itself does, so removeRecursive's ref-scoped deletion (which lists+deletes everything under
// refsPrefix) already reclaims it — a crashed/aborted write leaves no orphan the reachability sweep
// (which scans only blobs/+parts/) would miss, because the sidecar is never reachable as a blob/part.
RefMetaObjectKey refMetaKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name);

// Per-mutable-file object key: <key_prefix>/store/<server_id>/<table_uuid>/refs/<part_name>.<file>.meta.
// Each mutable per-part file's bytes are stored verbatim in its OWN tiny object so the read path
// (getStorageObjects -> readObject) returns EXACTLY that file's bytes — the bundle .meta sidecar
// (refMetaKey) is the atomic per-part index used for listing / carry-forward, while these per-file
// objects back the byte reads. Both end in the .meta suffix and live under refsPrefix, so the
// ref-enumerators skip them (isRefMetaKey) and removeRecursive's ref-scoped deletion reclaims them.
RefMetaObjectKey refMutableFileKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & part_name, const std::string & file);

// FREEZE namespace. A frozen part is published as its OWN ref under the shadow MIRROR of the physical
// store tree: shadow/<backup>/store/<uuid[:3]>/<uuid>/refs/<part> (one ref per frozen part, unlike the
// shared "detached" ref). The ref objects therefore physically live under the same intermediate
// prefixes the enumeration walks (shadow/<backup>/store, shadow/<backup>/store/<uuid[:3]>), so the
// SAME generic child-derivation that lists a live table dir resolves those shadow levels. The shadow
// helpers key off the LITERAL shadow table dir (the dir under the disk root excluding the part/file,
// i.e. shadow/<backup>/store/<uuid[:3]>/<uuid>) rather than (backup, server, uuid): each replica's
// uuid already separates replicas, and a same-server re-freeze with the same backup name is an
// idempotent overwrite of byte-identical content (no server_id segment is needed).
//
// shadowRefsRootPrefix is an additional GC root the reachability scan walks alongside refsRootPrefix,
// so a frozen snapshot's blobs stay reachable even after the live part is merged/dropped — the whole
// point of FREEZE.
std::string shadowRefsRootPrefix(const std::string & key_prefix);
std::string shadowRefsPrefix(const std::string & key_prefix, const std::string & shadow_table_dir);
RefObjectKey shadowRefKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name);
RefMetaObjectKey shadowRefMetaKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name);
RefMetaObjectKey shadowRefMutableFileKey(const std::string & key_prefix, const std::string & shadow_table_dir, const std::string & part_name, const std::string & file);

// The literal first path component reserved for FREEZE snapshots (mirrors kDetachedDirName).
inline constexpr std::string_view kShadowDirName = "shadow";

// True iff the disk-relative path's FIRST component is the reserved FREEZE shadow root (kShadowDirName),
// i.e. the path lives in the shadow snapshot namespace (shadow/<backup>/store/<uuid[:3]>/<uuid>/…). Used
// to route shadow reads/lists/existence/removal to the shadow ref-set BEFORE the live-table-dir branch
// (a shadow table dir also satisfies parseTableUuid and would otherwise be mis-routed to the live refs
// prefix). Leading slashes are ignored.
bool isShadowPath(const std::string & path);

// The suffix that distinguishes a per-ref sidecar object from a ref object under the SAME refs/
// prefix. Every enumerator that treats a key under refsPrefix as a ref (the table-dir listing,
// existsDirectory, and the GC live-set scan) must skip keys ending in this suffix, since a sidecar
// is not a ref and its payload is a RefSidecar, not a part id.
inline constexpr std::string_view kRefMetaSuffix = ".meta";

// The MergeTree detached-parts namespace. A part-dir component equal to this name is not a real part
// but a container of detached part directories (detached/<detached_part>/<file>); the listing must
// yield the detached part directory names, not the files inside (B36). Mirrors
// MergeTreeData::DETACHED_DIR_NAME (kept here to avoid a Storages dependency from the disk layer).
inline constexpr std::string_view kDetachedDirName = "detached";

// The non-replicated deduplication-log namespace. A MergeTree with non_replicated_deduplication_window
// writes an on-disk log at <table>/deduplication_logs/deduplication_log_N.txt. In the Atomic layout the
// path <uuid>/deduplication_logs/<file> is structurally indistinguishable from a part file
// <uuid>/<part>/<file>, so this directory name is RESERVED: a component equal to it (directly under the
// table dir) is never a part dir — its contents are table-level verbatim files under the files/
// namespace. ClickHouse part names never take this form (they end in numeric min_max_level groups), so
// reserving the name cannot shadow a real part. Mirrors how kDetachedDirName reserves "detached".
inline constexpr std::string_view kDeduplicationLogsDirName = "deduplication_logs";

// True iff key (a key under some refsPrefix) is a per-ref sidecar rather than a ref object.
bool isRefMetaKey(const std::string & key);

// Per-server/per-table direct object key for non-part / table-level files (e.g.
// format_version.txt, later mutation_*.txt). These are stored verbatim (no content addressing,
// no ref, no manifest) under <key_prefix>/store/<server_id>/<table_uuid>/files/<tail>, where <tail>
// is the path beyond the table dir <uuid[:3]>/<uuid>/.
// TODO(phase4-gc): non-part objects are GC roots.
std::string tableFilesPrefix(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid);
std::string tableFileKey(const std::string & key_prefix, const std::string & server_id, const std::string & table_uuid, const std::string & tail);

// Pool-ownership marker object key: <key_prefix>/_pool_meta. A single small object at the pool root
// that records the on-disk pool format version and the owning server's identity (ServerUUID +
// timestamp). Read/validated on mount to fail closed on an un-owned/second-mounter pool (B11).
std::string poolMetaKey(const std::string & key_prefix);

// Per-mounter registry. Each live mounter registers itself by creating one small marker object under
// the mounters prefix, so the full set of mounters is listable from the bucket alone (the bucket is
// the single source of truth) — needed for the multi-mounter milestone. The registry lives UNDER the
// `_pool_meta.mounters/` prefix so it shares the pool-meta namespace but is a distinct keyspace from
// the single `_pool_meta` ownership marker (one is a key, the other a prefix; they never collide).
//   - poolMountersPrefix: <key_prefix>/_pool_meta.mounters/  (list this to enumerate mounters)
//   - poolMounterKey:      <key_prefix>/_pool_meta.mounters/<server_id>  (one per live mounter)
std::string poolMountersPrefix(const std::string & key_prefix);
std::string poolMounterKey(const std::string & key_prefix, const std::string & server_id);

// Cross-mounter write-session pins. While a part is being written, each in-flight transaction owns a
// uniquely-keyed WriteSession object under the sessions prefix that records the part's blob hashes
// BEFORE they are referenced by any published ref, so a GC sweep on ANOTHER mounter treats those
// hashes as reachable for the lifetime of the write (the cross-process generalization of the
// in-process pin). The owner rewrites only its OWN uniquely-keyed object (no CAS / no contention) and
// removes it once the ref is published (commit) or the transaction aborts. Sessions live under the
// `pool/sessions/` prefix, a distinct keyspace from blobs/parts/refs, so the reachability sweep
// enumerates them on their own and never confuses one with a blob/part/ref.
//   - sessionsPrefix: <key_prefix>/sessions/        (list this to enumerate live write sessions)
//   - sessionKey:      <key_prefix>/sessions/<session_id>  (one per in-flight transaction)
std::string sessionsPrefix(const std::string & key_prefix);
std::string sessionKey(const std::string & key_prefix, const std::string & session_id);

// GC-leader coordination, built entirely on the create-if-absent CAS primitive (the bucket is the
// single source of truth; no Keeper). Two keyspaces:
//   - the FENCE allocator: monotonically-increasing tokens at <key_prefix>/fence/<n> (n = 1,2,3...).
//     A token is allocated by cond-creating fence/<n> scanning n upward until one create SUCCEEDS;
//     because only one caller can create fence/<n> for a given n, no two callers ever get the same
//     token. The fence token is the real GC-safety authority (a later task re-checks the max fence at
//     delete time so a paused leader cannot delete).
//   - the GC LEADER lock: a single small record at <key_prefix>/gc.lock holding the current leader's
//     server id, lease deadline and fence token. It is a liveness/coordination HINT only — the fence,
//     not the lock object, is the safety backstop.
//   - fencePrefix: <key_prefix>/fence/     (list this to find the high-water token; the allocator scan)
//   - fenceKey:    <key_prefix>/fence/<n>  (one tiny object per allocated token)
//   - gcLockKey:   <key_prefix>/gc.lock    (the single leader-lock record)
std::string fencePrefix(const std::string & key_prefix);
std::string fenceKey(const std::string & key_prefix, uint64_t n);
std::string gcLockKey(const std::string & key_prefix);

// ==== CA GC S2: log-structured, streaming GC layout (spec §4, §5) ====
//
// The reverse index (blob -> referrers) is log-structured like an LSM tree: a sorted SNAPSHOT run plus
// a tail of small +/- DELTA objects appended on commit/drop, compacted per GC epoch. All three families
// — the snapshot, the log, and (later) the leadership lease — are keyed by HASH-PREFIX SHARD from day
// one, so a shard is the unit of leadership/epoch/compaction. The first implementation runs ONE worker
// across all shards; running N workers (one per shard) is then a configuration flip, NOT a layout change.
//
// `ShardId` is the small integer derived from a content hash's prefix (shardForHash). `kGcShardCount` is
// the SINGLE source of truth for the shard count — change it in one place and the whole layout follows.

using ShardId = uint32_t;

// The number of GC shards. A power of two so shardForHash can mask the top hash-prefix bits cheaply and
// the partition is uniform. This is the one knob that turns "1 worker over N shards" into "N workers".
inline constexpr ShardId kGcShardCount = 16;

// Derive the GC shard for a content hash from its hash prefix (the high bits of H0). The hash is the
// lowercase-hex content digest; the first hex nibbles are uniformly distributed, so mapping them onto
// [0, kGcShardCount) gives a uniform, stable partition. A hash too short to carry a prefix (only the
// short synthetic hashes used in unit tests) maps to shard 0 deterministically.
ShardId shardForHash(const BlobHash & blob_hash);

// Per-shard epoch counter object key: <key_prefix>/gc/current_epoch/<shard>. The epoch a writer
// currently appends deltas under (per-shard, not global — §5.1 rule 1). A writer reads it (default 0 if
// absent) to stamp its delta; the fenced shard leader closes the epoch by a plain fenced PUT of E+1
// before folding. Single-writer-per-shard (the GC leader), so no CAS is needed.
std::string gcCurrentEpochKey(const std::string & key_prefix, ShardId shard);

// Per-(epoch, shard) delta-log PREFIX: <key_prefix>/gc/log/<epoch>.<shard>/. List this to enumerate the
// epoch's coalesced delta objects for the fold. The epoch is NOT zero-padded here: the log prefix is
// LISTed by exact (epoch, shard), never scanned in lexical order, so numeric formatting is sufficient.
std::string gcLogPrefix(const std::string & key_prefix, uint64_t epoch, ShardId shard);

// A single coalesced delta-log object key: <key_prefix>/gc/log/<epoch>.<shard>/<event_id>. One object
// holds one group-commit batch of deltas; `event_id` is the batch's stable id (so a re-append into a
// later epoch lands an idempotent, dedup-able object — §5.1). Typed (GcLogObjectKey) so a log key can
// never be confused with a blob/part/ref key (B29).
GcLogObjectKey gcLogEventKey(const std::string & key_prefix, uint64_t epoch, ShardId shard, const std::string & event_id);

// Per-(epoch, shard) snapshot object key: <key_prefix>/gc/snap/<padded-epoch>.<shard>. The sorted
// (H)->count run for the shard at this epoch. The epoch is ZERO-PADDED to a fixed width so a LEXICAL
// LIST of the gc/snap prefix yields the snapshots in NUMERIC epoch order (the compaction picks the
// latest by listing — without padding, "10" would sort before "2"). Typed (GcSnapObjectKey) per B29.
GcSnapObjectKey gcSnapKey(const std::string & key_prefix, uint64_t epoch, ShardId shard);

// The LIST prefix for ALL snapshot runs of EVERY epoch and shard: <key_prefix>/gc/snap/. A lexical LIST
// returns the objects in (padded-epoch, shard) order — the compaction's rebuild lists this and filters to
// one shard's `.<shard>` suffix, taking the highest epoch as the latest snapshot for that shard.
std::string gcSnapPrefix(const std::string & key_prefix);

// The LIST prefix for ALL delta-log objects of EVERY epoch and shard: <key_prefix>/gc/log/. The rebuild
// lists this to discover which epochs still carry un-folded deltas for a shard (filtering the
// <epoch>.<shard>/ component). The per-epoch fold uses the tighter gcLogPrefix instead.
std::string gcLogRootPrefix(const std::string & key_prefix);

// Verbatim object key for a generic disk-level file: a path that is neither a part file nor a
// table-level file (e.g. the server's startup access-check probe clickhouse_access_check_<uuid>
// written at the disk root). Such files are stored verbatim at <key_prefix>/<path> (no content
// addressing, no ref, no manifest) using the same empty-prefix-safe join as every other key builder.
std::string diskFileKey(const std::string & key_prefix, const std::string & path);

struct PartFilePath
{
    std::string table_uuid;
    std::string part_name;
    std::string file; /// empty when the path is a part directory
    /// Set to the backup name when the path is a FREEZE target shadow/<backup_name>/…/<part>[/<file>].
    /// Empty for a normal live-part path. The frozen ref then lives in the shadow/ namespace
    /// (shadowRefKey) rather than the live store/.../refs/ location, so a freeze never clobbers the
    /// live part's ref (the shadow ref is also an independent GC root).
    std::string backup_name;
    /// Set (alongside backup_name) when the path is a FREEZE target: the LITERAL shadow table dir under
    /// the disk root excluding the part and file, i.e. the joined components [0 .. part_idx-1]
    /// (shadow/<backup>/store/<uuid[:3]>/<uuid>). The shadow ref-family keys mirror the physical store
    /// tree under this dir (shadowRefsPrefix), so the enumeration's intermediate levels resolve via the
    /// same generic child-derivation as the live table dir. Empty for a normal live-part path.
    std::string shadow_table_dir;
};

// Parse a disk-relative ClickHouse path <uuid[:3]>/<uuid>/<part>[/<file>].
// Returns nullopt if the path is the table dir or shallower (fewer than 3 components).
std::optional<PartFilePath> parsePartFilePath(const std::string & path);

// Returns the table_uuid iff path is exactly the table dir <uuid[:3]>/<uuid>[/] (2 components).
std::optional<std::string> parseTableUuid(const std::string & path);

// True iff the path's LAST two components form an Atomic-style <uuid[:3]>/<uuid> pair (the 3-char
// prefix component immediately followed by the matching uuid, with nothing after it). Unlike
// parseTableUuid this does NOT accept the non-Atomic fallback (any 2+-component dir): it is the strict
// "this dir IS a uuid-anchored table dir" predicate the shadow router uses to tell a shadow TABLE dir
// (shadow/<backup>/store/<uuid[:3]>/<uuid>) apart from a shadow INTERMEDIATE dir (shadow/<backup>,
// shadow/<backup>/store, shadow/<backup>/store/<uuid[:3]>), which parseTableUuid would mis-accept.
bool endsWithTableUuidPair(const std::string & path);

// True iff the path addresses a file inside a part dir, i.e. <uuid[:3]>/<uuid>/<part>/<file>
// (4+ components, non-empty file). These are content-addressed (ref + manifest + blob). Everything
// else handled by writeFile / file reads (e.g. <uuid[:3]>/<uuid>/format_version.txt, 3 components)
// is a non-part / table-level file handled by plain passthrough.
bool isPartFilePath(const std::string & path);

struct TableFilePath
{
    std::string table_uuid;
    std::string tail; /// path beyond the table dir <uuid[:3]>/<uuid>/
};

// Parse a non-part / table-level file path: <uuid[:3]>/<uuid>/<tail...> where <tail...> is not a
// single part-dir-shaped component path. Returns nullopt if the path is shallower than the table
// dir, or if it is a part-file path (use isPartFilePath / parsePartFilePath for those).
std::optional<TableFilePath> parseTableFilePath(const std::string & path);

}

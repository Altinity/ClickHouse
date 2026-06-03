#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Identifiers.h>
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
};

// Parse a disk-relative ClickHouse path <uuid[:3]>/<uuid>/<part>[/<file>].
// Returns nullopt if the path is the table dir or shallower (fewer than 3 components).
std::optional<PartFilePath> parsePartFilePath(const std::string & path);

// Returns the table_uuid iff path is exactly the table dir <uuid[:3]>/<uuid>[/] (2 components).
std::optional<std::string> parseTableUuid(const std::string & path);

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

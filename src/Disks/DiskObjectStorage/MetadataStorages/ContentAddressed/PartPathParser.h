#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DB::ContentAddressed
{

/// The wiring's ClickHouse-path classifier (M-W Task 1): the parsing half of the PoC's `PoolPaths`,
/// ported verbatim — this surface survived the full SQL suites (Atomic + non-Atomic layouts,
/// detached, FREEZE shadow, deduplication logs, projections, every tmp/operation part-dir prefix).
/// Key CONSTRUCTION does not live here (that is `Cas::Layout`); these functions are pure and
/// side-effect-free. Path semantics are wiring policy: the `Cas` core never sees ClickHouse paths.

/// The literal first path component reserved for FREEZE snapshots
/// (shadow/<backup>/store/<uuid[:3]>/<uuid>/<part>/...).
inline constexpr std::string_view kShadowDirName = "shadow";

/// The MergeTree detached-parts directory. parsePartFilePath reports a detached path with
/// part_name == kDetachedDirName and the real detached part dir as the FIRST component of `file`
/// (the PoC contract, B36); the transaction/read routing re-splits it.
inline constexpr std::string_view kDetachedDirName = "detached";

/// B181: detached parts live INSIDE the table's OWN archive namespace as refs keyed by this
/// prefix — `detached/PART` versus a live `PART`. One namespace per table; the live-vs-detached
/// name collision is impossible because the ref names differ. The routing prepends this to the
/// detached part name to form the ref, and the `TABLE/detached` container dir surfaces the
/// table's refs filtered to this prefix (stripped for display). No parallel detached namespace
/// exists anymore (the old `detachedNamespace` is gone).
inline constexpr std::string_view kDetachedRefPrefix = "detached/";

/// The content-addressing boundary marker: a SUFFIX on a table-dir segment (`…/<uuid>@cas@`), not a
/// path segment. It marks where the mirrored ClickHouse path ends and the content-addressed archive
/// begins — like a `.zip` extension (`foo.zip/inner/file`). `@` is S3-safe and never occurs in
/// ClickHouse uuids, part names, detached prefixes, projection names, or column files, so it cannot
/// collide with real path data. Shard discovery uses this suffix via the registry (`listNamespaces`)
/// + static `[0, root_shards)` fan-out — not key classification.
inline constexpr std::string_view kCasArchiveSuffix = "@cas@";

/// Compose the mirrored content-addressed archive path for a table identifier as the parser reports
/// it. Atomic tables report the bare `<uuid>` → reconstruct `store/<u3>/<uuid>@cas@` (u3 = first 3
/// chars, matching ClickHouse's store fanout). Non-Atomic tables report the full joined
/// `data/<db>/<tbl>` path → append `@cas@` to it verbatim. The `@cas@` suffix lands on the
/// table-dir (last) segment in both cases. Pure; no ClickHouse dependency.
std::string mirroredArchiveNamespace(const std::string & table_uuid);

/// Reserved table-level subdirectory: TABLE_DIR/deduplication_logs/FILE is structurally
/// indistinguishable from a part file in the Atomic layout, so the name is reserved — never a part
/// dir; its contents are table-level verbatim files. ClickHouse part names never take this form.
inline constexpr std::string_view kDeduplicationLogsDirName = "deduplication_logs";

/// The canonical set of MUTABLE per-part files: bytes differ between two parts that are otherwise
/// byte-identical in column data, so they MUST NOT contribute to part identity and MUST NOT be
/// shared through the content-addressed tree. The wiring stores exactly this set in the ref's
/// RefPayload.mutable_files (D-W3); the tree builder excludes exactly this set (B23: one constant
/// makes "excluded from identity" and "stored per-ref" the same concept by construction).
inline constexpr std::array<std::string_view, 3> kMutablePerPartFiles{
    "uuid.txt", "txn_version.txt", "metadata_version.txt"};

/// True iff file is a mutable per-part file (see kMutablePerPartFiles). Matches the LAST path
/// component (a detached part's files carry a <detached_part>/ prefix inside the shared detached
/// ref, B36/B46/B62) and treats the atomic-write sibling tmp (txn_version.txt.tmp,
/// VersionMetadataOnDisk) as mutable too — otherwise a standalone autocommit write of the tmp on a
/// committed part would republish the part with a one-file tree (data loss).
constexpr bool isMutablePerPartFile(std::string_view file)
{
    const auto slash = file.rfind('/');
    const std::string_view basename = (slash == std::string_view::npos) ? file : file.substr(slash + 1);
    std::string_view stem = basename;
    if (stem.ends_with(".tmp"))
        stem = stem.substr(0, stem.size() - 4);
    for (const auto & name : kMutablePerPartFiles)
        if (basename == name || stem == name)
            return true;
    return false;
}

struct PartFilePath
{
    std::string table_uuid;
    std::string part_name;
    std::string file; /// empty when the path is a part directory
    /// Set to the backup name when the path is a FREEZE target shadow/<backup_name>/.../<part>[/<file>].
    /// Empty for a normal live-part path.
    std::string backup_name;
    /// Set (alongside backup_name) for a FREEZE target: the LITERAL shadow table dir under the disk
    /// root excluding the part and file (shadow/<backup>/store/<uuid[:3]>/<uuid>). Empty otherwise.
    std::string shadow_table_dir;
};

/// Parse a disk-relative ClickHouse path to its (table, part, in-part file) split. Anchors on the
/// Atomic <uuid[:3]>/<uuid> pair anywhere in the path (robust to a leading store/ or shadow/
/// prefix); falls back to the RIGHTMOST part-dir-shaped component for non-Atomic layouts
/// (data/db/table/part/...). Returns nullopt for the table dir or shallower.
std::optional<PartFilePath> parsePartFilePath(const std::string & path);

/// Returns the table identifier iff path is exactly a table dir: the bare <uuid> for the Atomic
/// layout, the full joined data/db/table path for non-Atomic.
std::optional<std::string> parseTableUuid(const std::string & path);

/// True iff the path is an Atomic-layout INTERMEDIATE shard directory `store/<u3>`, where <u3> is a
/// 3-character uuid prefix (the only child it has on disk is a uuid-anchored `<u3>/<uuid>` table
/// dir). This shape is ambiguous with the non-Atomic `data/<db>` fallback of parseTableUuid, so the
/// metadata router must consult it FIRST and treat `store/<u3>` as a generic intermediate dir to be
/// enumerated by a mirrored LIST — never as a non-Atomic table id.
bool isAtomicShardDir(const std::string & path);

/// Strict "this dir IS a uuid-anchored table dir" predicate: the path's LAST two components form
/// an Atomic <uuid[:3]>/<uuid> pair. Unlike parseTableUuid it rejects the non-Atomic fallback —
/// the shadow router uses it to tell a shadow TABLE dir from a shadow INTERMEDIATE dir.
bool endsWithTableUuidPair(const std::string & path);

/// True iff the path addresses a file INSIDE a part dir (content-addressed). Table-level files
/// (format_version.txt, deduplication_logs/...) and generic disk files are excluded.
bool isPartFilePath(const std::string & path);

struct TableFilePath
{
    std::string table_uuid;
    std::string tail; /// path beyond the table dir, full sub-path preserved
};

/// Parse a non-part table-level file path. Returns nullopt for the bare table dir, shallower
/// paths, part files, and generic disk-root files (e.g. clickhouse_access_check_*).
std::optional<TableFilePath> parseTableFilePath(const std::string & path);

/// True iff the path's FIRST component is the reserved FREEZE shadow root. Routed BEFORE the
/// live-table branches (a shadow table dir also satisfies parseTableUuid).
bool isShadowPath(const std::string & path);

}

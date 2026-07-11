#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasManifestId.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>

using namespace DB::Cas;

namespace
{
/// A `BlobRef` at `algo` whose first bytes are `0x00, 0xaa, 0xbb` (the rest zero) -- for key-shape
/// tests that need a stable, recognizable hex prefix. `Layout` no longer captures an algo (Phase 3
/// T2/T3): every blob key is built from a `BlobRef` alone, so key-shape tests construct one directly.
BlobRef prefixedRef(BlobHashAlgo algo)
{
    BlobDigest d{};
    d.bytes[0] = 0x00; d.bytes[1] = 0xaa; d.bytes[2] = 0xbb;
    return BlobRef{algo, d};
}
}

TEST(CasLayout, KeyShapes)
{
    /// Per design §10 EVERY algo carries an explicit path segment: `blobs/ch128/...`, not the legacy
    /// `blobs/...`.
    Layout l{"p"};
    const BlobRef ref = prefixedRef(BlobHashAlgo::CityHash128);
    const String hex = codecFor(BlobHashAlgo::CityHash128).toHex(ref.digest);
    EXPECT_EQ(l.blobKey(ref), "p/blobs/ch128/" + hex.substr(0, 2) + "/" + hex);
    EXPECT_EQ(l.gcStateKey(), "p/gc/state");
    EXPECT_EQ(l.outcomesKey(4, 42, 7, 1), "p/gc/gen/4/attempt/42/outcomes/7/1");
    EXPECT_EQ(l.poolMetaKey(), "p/_pool_meta");
}

TEST(CasLayout, BlobKeyCarriesAlgoSegment)
{
    /// Every algo gets its own segment (design §3/§10), so two algos can never collide in the key
    /// space even after a config change on a fresh pool. `Layout` itself carries no algo anymore --
    /// the segment comes from the `BlobRef` passed to `blobKey`/`blobMetaKey`.
    const Layout l("p");

    const BlobRef ch128_ref = prefixedRef(BlobHashAlgo::CityHash128);
    const String ch128_hex = codecFor(BlobHashAlgo::CityHash128).toHex(ch128_ref.digest);
    EXPECT_EQ(l.blobKey(ch128_ref), "p/blobs/ch128/" + ch128_hex.substr(0, 2) + "/" + ch128_hex);
    EXPECT_EQ(l.blobMetaKey(ch128_ref), l.blobKey(ch128_ref) + ".meta");

    const BlobRef xxh3_ref = prefixedRef(BlobHashAlgo::XXH3_128);
    const String xxh3_hex = codecFor(BlobHashAlgo::XXH3_128).toHex(xxh3_ref.digest);
    EXPECT_EQ(l.blobKey(xxh3_ref), "p/blobs/xxh3/" + xxh3_hex.substr(0, 2) + "/" + xxh3_hex);
    EXPECT_EQ(l.blobMetaKey(xxh3_ref), l.blobKey(xxh3_ref) + ".meta");

    const BlobRef sha256_ref = prefixedRef(BlobHashAlgo::Sha256);
    const String sha256_hex = codecFor(BlobHashAlgo::Sha256).toHex(sha256_ref.digest);
    EXPECT_EQ(l.blobKey(sha256_ref), "p/blobs/sha256/" + sha256_hex.substr(0, 2) + "/" + sha256_hex);

    /// Trees/manifests/refs are UNCHANGED -- only blob-body keys gain the algo segment.
    EXPECT_EQ(l.blobsPrefix(), "p/blobs/");
}

TEST(CasLayout, RootNamespaceKeys)
{
    Layout l("p");
    RootNamespace ns{"srv1/3f2e-uuid"};
    /// Phase 1: ref shards relocated out of roots/ to cas/refs/; the namespace fan-out is unchanged.
    EXPECT_EQ(l.rootShardKey(ns, 3), "p/cas/refs/srv1/3f2e-uuid/3");
    /// Browse helpers (verbatim `_files` tree) stay under roots/.
    EXPECT_EQ(l.namespaceFileKey(ns, "format_version.txt"), "p/roots/srv1/3f2e-uuid/_files/format_version.txt");
    EXPECT_EQ(l.namespaceFilesPrefix(ns), "p/roots/srv1/3f2e-uuid/_files/");
}

TEST(CasLayout, RelocatedRefAndManifestKeys)
{
    Layout l("p");
    const RootNamespace ns{"srid/store/ab/uuid@cas@"};
    /// Ref shards: roots/ -> cas/refs/ (identity-preserving; only the base prefix moves).
    EXPECT_EQ(l.rootShardKey(ns, 3), "p/cas/refs/srid/store/ab/uuid@cas@/3");
    /// Pool-wide ref prefix (discovery LIST + strip base).
    EXPECT_EQ(l.casRefsPrefix(), "p/cas/refs/");
    /// All manifests of a namespace: cas/manifests/<ns>/ (replaces roots/<ns>/_manifests/).
    EXPECT_EQ(l.manifestNamespacePrefix(ns), "p/cas/manifests/srid/store/ab/uuid@cas@/");

    /// manifestKey: same writer/build/aa/inst fan-out, now under cas/manifests/<ns>/ (no /_manifests/ infix).
    ManifestId id;
    id.root_namespace = ns;
    id.ref.writer_epoch = 1;
    id.ref.build_sequence = 1042;
    id.ref.manifest_ordinal = 1;
    const String key = l.manifestKey(id);
    EXPECT_EQ(key, "p/cas/manifests/srid/store/ab/uuid@cas@/1/1042/000001.proto");
    EXPECT_EQ(key.find("/_manifests/"), String::npos) << key;
}

TEST(CasLayout, RootNamespaceValidation)
{
    Layout l("p");
    EXPECT_THROW(l.rootShardKey(RootNamespace{""}, 0), DB::Exception);
    EXPECT_THROW(l.rootShardKey(RootNamespace{"/lead"}, 0), DB::Exception);
    EXPECT_THROW(l.rootShardKey(RootNamespace{"trail/"}, 0), DB::Exception);
    /// File names may be NESTED relative paths (M-W T2: deduplication_logs/...); only unclean
    /// shapes are rejected (empty, leading/trailing '/', empty segments, '..' escapes).
    EXPECT_NO_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "a/b"));
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, ""), DB::Exception);
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "/lead"), DB::Exception);
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "trail/"), DB::Exception);
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "a//b"), DB::Exception);
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "../up"), DB::Exception);
    EXPECT_THROW(l.namespaceFileKey(RootNamespace{"ok"}, "a/../b"), DB::Exception);

    /// A middle empty segment ("a//b") is rejected (doubled '/').
    EXPECT_THROW(l.rootShardKey(RootNamespace{"a//b"}, 0), DB::Exception);
    /// A segment exactly equal to the reserved "_files" is rejected.
    EXPECT_THROW(l.rootShardKey(RootNamespace{"srv1/_files/x"}, 0), DB::Exception);
    /// But a segment that merely CONTAINS "_files" as a substring is legal (no false positive).
    EXPECT_NO_THROW(l.rootShardKey(RootNamespace{"my_files/tbl"}, 0));
}

TEST(CasLayout, GenerationAndRootsKeys)
{
    Layout l("p");
    /// rev. 15: gc/snap is gone; generations carry write-once seals + blob-target / cleanup runs.
    /// rev. 16: every per-round artifact is attempt-scoped under gc/gen/<gen>/attempt/<attempt>/.
    EXPECT_EQ(l.foldSealKey(12, 0), "p/gc/gen/12/attempt/0/fold_seal");
    EXPECT_EQ(l.blobTargetRunKey(12, 0, 0, 0), "p/gc/gen/12/attempt/0/blob_target/0/0");
    EXPECT_EQ(l.partManifestCleanupKey(12, 0, 0, 0), "p/gc/gen/12/attempt/0/part_manifest_cleanup/0/0");
    EXPECT_EQ(l.rootsPrefix(), "p/roots/");
}

TEST(CasLayout, AttemptScopedGenKeys)
{
    DB::Cas::Layout layout("p");
    EXPECT_EQ(layout.foldSealKey(4, 42), "p/gc/gen/4/attempt/42/fold_seal");
    EXPECT_EQ(layout.blobTargetRunKey(4, 42, 3, 0), "p/gc/gen/4/attempt/42/blob_target/3/0");
    EXPECT_EQ(layout.partManifestCleanupKey(4, 42, 0, 1), "p/gc/gen/4/attempt/42/part_manifest_cleanup/0/1");
    EXPECT_EQ(layout.outcomesKey(5, 42, 7, 3), "p/gc/gen/5/attempt/42/outcomes/7/3");
    EXPECT_EQ(layout.gcGenPrefix(4), "p/gc/gen/4/");
    EXPECT_EQ(layout.gcGenAttemptPrefix(4, 42), "p/gc/gen/4/attempt/42/");
}

TEST(CasLayout, RegistryDeletedGcDiscoveryViaList)
{
    /// Task 4: the namespace registry (`gc/registry`) is deleted; discovery authority moved to LIST.
    /// The `_registry` namespace segment is not reserved (it was only reserved while the registry lived
    /// under `roots/_registry`, which was already relocated to `gc/registry` before being deleted).
    Layout l("p");
    EXPECT_NO_THROW(l.rootShardKey(RootNamespace{"a/_registry@cas@"}, 0));
    /// `_files` and `_pool_meta`-style reservations are unaffected.
    EXPECT_THROW(l.rootShardKey(RootNamespace{"a/_files"}, 0), DB::Exception);
}

TEST(CasLayout, CasArchiveSuffixConstant)
{
    EXPECT_EQ(DB::ContentAddressed::kCasArchiveSuffix, "@cas@");
}

TEST(CasVfsPaths, MirroredArchiveNamespace)
{
    using DB::ContentAddressed::mirroredArchiveNamespace;
    /// Atomic: bare uuid -> store/<u3>/<uuid>@cas@
    EXPECT_EQ(mirroredArchiveNamespace("3f2a0000-0000-0000-0000-000000000001"),
              "store/3f2/3f2a0000-0000-0000-0000-000000000001@cas@");
    /// Non-Atomic: a full data/db/tbl path is used verbatim, @cas@ appended to the last segment.
    EXPECT_EQ(mirroredArchiveNamespace("data/mydb/events"),
              "data/mydb/events@cas@");
}

TEST(CasLayout, ManifestKeyShape)
{
    Layout l("p");
    ManifestId id;
    id.root_namespace = RootNamespace("srv-a/3f2e-uuid@cas@");
    id.ref.writer_epoch = 7;
    id.ref.build_sequence = 1042;
    id.ref.manifest_ordinal = 1;
    const String key = l.manifestKey(id);
    EXPECT_EQ(key,
        "p/cas/manifests/srv-a/3f2e-uuid@cas@/"
        "7/1042/000001.proto");
}

TEST(CasLayout, ManifestsSegmentReserved)
{
    Layout l("p");
    ManifestId bad;
    bad.root_namespace = RootNamespace("srv-a/_manifests/x");
    EXPECT_THROW(l.manifestKey(bad), DB::Exception);
    /// Also rejected as a generic namespace segment via rootShardKey.
    EXPECT_THROW(l.rootShardKey(RootNamespace{"srv-a/_manifests/tbl"}, 0), DB::Exception);
    /// A segment that merely CONTAINS "_manifests" as a substring is still legal (no false positive).
    EXPECT_NO_THROW(l.rootShardKey(RootNamespace{"my_manifests/tbl"}, 0));
}

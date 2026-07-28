#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h>

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
    EXPECT_EQ(l.outcomesKey(4, 42, 7, 1), "p/gc/gen/4/attempt/42/outcomes/7/1.zst");
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
    /// Phase 1: ref objects live under cas/refs/<ns>/; the namespace fan-out is unchanged.
    EXPECT_EQ(l.refsNamespacePrefix(ns), "p/cas/refs/srv1/3f2e-uuid/");
    /// Browse helpers (verbatim `_files` tree) stay under roots/.
    EXPECT_EQ(l.namespaceFileKey(ns, "format_version.txt"), "p/roots/srv1/3f2e-uuid/_files/format_version.txt");
    EXPECT_EQ(l.namespaceFilesPrefix(ns), "p/roots/srv1/3f2e-uuid/_files/");
}

TEST(CasLayout, RelocatedRefAndManifestKeys)
{
    Layout l("p");
    const RootNamespace ns{"srid/store/ab/uuid@cas@"};
    /// Ref objects: cas/refs/<ns>/ (identity-preserving namespace fan-out).
    EXPECT_EQ(l.refsNamespacePrefix(ns), "p/cas/refs/srid/store/ab/uuid@cas@/");
    /// Pool-wide ref prefix (discovery LIST + strip base).
    EXPECT_EQ(l.casRefsPrefix(), "p/cas/refs/");
    /// All manifests of a namespace: cas/manifests/<ns>/ (replaces roots/<ns>/_manifests/).
    EXPECT_EQ(l.manifestNamespacePrefix(ns), "p/cas/manifests/srid/store/ab/uuid@cas@/");

    /// manifestKey: canonical hex build directory, under cas/manifests/<ns>/ (no /_manifests/ infix).
    ManifestId id;
    id.root_namespace = ns;
    id.ref.writer_epoch = 1;
    id.ref.build_sequence = 1042;
    id.ref.manifest_ordinal = 1;
    const String key = l.manifestKey(id);
    EXPECT_EQ(key, "p/cas/manifests/srid/store/ab/uuid@cas@/"
        "0000000000000001-0000000000000412/000001.zst");
    EXPECT_EQ(key.find("/_manifests/"), String::npos) << key;
}

TEST(CasLayout, RootNamespaceValidation)
{
    Layout l("p");
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{""}), DB::Exception);
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"/lead"}), DB::Exception);
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"trail/"}), DB::Exception);
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
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"a//b"}), DB::Exception);
    /// A segment exactly equal to the reserved "_files" is rejected.
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"srv1/_files/x"}), DB::Exception);
    /// But a segment that merely CONTAINS "_files" as a substring is legal (no false positive).
    EXPECT_NO_THROW(l.refsNamespacePrefix(RootNamespace{"my_files/tbl"}));
}

TEST(CasLayout, GenerationAndRootsKeys)
{
    Layout l("p");
    /// rev. 15: gc/snap is gone; generations carry write-once seals + blob-target / cleanup runs.
    /// rev. 16: every per-round artifact is attempt-scoped under gc/gen/<gen>/attempt/<attempt>/.
    EXPECT_EQ(l.foldSealKey(12, 0), "p/gc/gen/12/attempt/0/fold_seal");
    EXPECT_EQ(l.blobTargetRunKey(12, 0, 0, 0), "p/gc/gen/12/attempt/0/blob_target/0/0");
    EXPECT_EQ(l.rootsPrefix(), "p/roots/");
}

TEST(CasLayout, AttemptScopedGenKeys)
{
    DB::Cas::Layout layout("p");
    EXPECT_EQ(layout.foldSealKey(4, 42), "p/gc/gen/4/attempt/42/fold_seal");
    EXPECT_EQ(layout.blobTargetRunKey(4, 42, 3, 0), "p/gc/gen/4/attempt/42/blob_target/3/0");
    EXPECT_EQ(layout.outcomesKey(5, 42, 7, 3), "p/gc/gen/5/attempt/42/outcomes/7/3.zst");
    EXPECT_EQ(layout.gcGenPrefix(4), "p/gc/gen/4/");
    EXPECT_EQ(layout.gcGenAttemptPrefix(4, 42), "p/gc/gen/4/attempt/42/");
}

TEST(CasLayout, RegistryDeletedGcDiscoveryViaList)
{
    /// Task 4: the namespace registry (`gc/registry`) is deleted; discovery authority moved to LIST.
    /// The `_registry` namespace segment is not reserved (it was only reserved while the registry lived
    /// under `roots/_registry`, which was already relocated to `gc/registry` before being deleted).
    Layout l("p");
    EXPECT_NO_THROW(l.refsNamespacePrefix(RootNamespace{"a/_registry@cas@"}));
    /// `_files` and `_pool_meta`-style reservations are unaffected.
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"a/_files"}), DB::Exception);
}

TEST(CasLayout, CasArchiveSuffixConstant)
{
    EXPECT_EQ(DB::Cas::kCasArchiveSuffix, "@cas@");
}

TEST(CasVfsPaths, MirroredArchiveNamespace)
{
    using DB::Cas::mirroredArchiveNamespace;
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
        "0000000000000007-0000000000000412/000001.zst");
}

TEST(CasLayout, ManifestsSegmentReserved)
{
    Layout l("p");
    ManifestId bad;
    bad.root_namespace = RootNamespace("srv-a/_manifests/x");
    EXPECT_THROW(l.manifestKey(bad), DB::Exception);
    /// Also rejected as a generic namespace segment via refsNamespacePrefix (the shared checkNamespace).
    EXPECT_THROW(l.refsNamespacePrefix(RootNamespace{"srv-a/_manifests/tbl"}), DB::Exception);
    /// A segment that merely CONTAINS "_manifests" as a substring is still legal (no false positive).
    EXPECT_NO_THROW(l.refsNamespacePrefix(RootNamespace{"my_manifests/tbl"}));
}

TEST(CasLayout, ManifestKeyHexRoundTrip)
{
    Layout l("p");
    ManifestId id;
    id.root_namespace = RootNamespace("srv-a/3f2e-uuid@cas@");
    id.ref.writer_epoch = 7;
    id.ref.build_sequence = 0x8e;
    id.ref.manifest_ordinal = 42;
    const String key = l.manifestKey(id);
    EXPECT_EQ(key,
        "p/cas/manifests/srv-a/3f2e-uuid@cas@/"
        "0000000000000007-000000000000008e/000042.zst");

    const auto parsed = l.parseManifestKey(key);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->root_namespace, id.root_namespace);
    EXPECT_EQ(parsed->ref, id.ref);

    /// The old two-directory decimal shape (`<writer_epoch>/<build_sequence>/<ordinal>.zst`) is no
    /// longer canonical: the segment right before the file is a plain decimal number, not two
    /// fixed-width hex fields joined by '-', so `parseRefTxnId` rejects it.
    EXPECT_FALSE(l.parseManifestKey("p/cas/manifests/srv-a/3f2e-uuid@cas@/7/142/000042.zst").has_value());
    /// Foreign prefix, missing build segment, non-registered-suffix file, and out-of-range ordinal
    /// are all rejected.
    EXPECT_FALSE(l.parseManifestKey("p/cas/refs/srv-a/3f2e-uuid@cas@/"
        "0000000000000007-000000000000008e/000042.zst").has_value());
    EXPECT_FALSE(l.parseManifestKey("p/cas/manifests/0000000000000007-000000000000008e/000042.zst").has_value());
    EXPECT_FALSE(l.parseManifestKey("p/cas/manifests/srv-a/3f2e-uuid@cas@/"
        "0000000000000007-000000000000008e/000042.bin").has_value());
    EXPECT_FALSE(l.parseManifestKey("p/cas/manifests/srv-a/3f2e-uuid@cas@/"
        "0000000000000007-000000000000008e/000000.zst").has_value());
    EXPECT_FALSE(l.parseManifestKey("p/cas/manifests/srv-a/3f2e-uuid@cas@/"
        "0000000000000007-000000000000008E/000042.zst").has_value());   /// uppercase hex
}

TEST(CasLayout, RefObjectKeyRoundTrips)
{
    Layout l("p");
    const RootNamespace ns{"srv1/tbl@cas@"};
    const RefTxnId id{7, 0x8e};

    const String log_key = l.refLogKey(ns, id);
    EXPECT_EQ(log_key, "p/cas/refs/srv1/tbl@cas@/_log/0000000000000007-000000000000008e.zst");
    const auto parsed_log = l.parseRefObjectKey(log_key);
    ASSERT_TRUE(parsed_log.has_value());
    EXPECT_EQ(parsed_log->ns, ns);
    EXPECT_EQ(parsed_log->kind, RefObjectKind::Log);
    EXPECT_EQ(parsed_log->txn_id, id);

    const String snap_key = l.refSnapshotKey(ns, id);
    EXPECT_EQ(snap_key, "p/cas/refs/srv1/tbl@cas@/_snap/0000000000000007-000000000000008e.zst");
    const auto parsed_snap = l.parseRefObjectKey(snap_key);
    ASSERT_TRUE(parsed_snap.has_value());
    EXPECT_EQ(parsed_snap->ns, ns);
    EXPECT_EQ(parsed_snap->kind, RefObjectKind::Snap);
    EXPECT_EQ(parsed_snap->txn_id, id);

    const String cleanup_key = l.refCleanupMarkerKey(ns, id);
    EXPECT_EQ(cleanup_key, "p/cas/refs/srv1/tbl@cas@/_cleanup/0000000000000007-000000000000008e");
    const auto parsed_cleanup = l.parseRefObjectKey(cleanup_key);
    ASSERT_TRUE(parsed_cleanup.has_value());
    EXPECT_EQ(parsed_cleanup->ns, ns);
    EXPECT_EQ(parsed_cleanup->kind, RefObjectKind::Cleanup);
    EXPECT_EQ(parsed_cleanup->txn_id, id);
}

TEST(CasLayout, RefObjectKeyLexicalOrder)
{
    Layout l("p");
    const RootNamespace ns{"srv1/tbl@cas@"};
    const RefTxnId id{7, 0x8e};
    /// spec §Object Layout: "`_cleanup` sorts before `_log` ... and takes no part in the `_log`-before-
    /// `_snap` recovery ordering". Asserted here on the actual generated keys, same namespace + id.
    EXPECT_LT(l.refCleanupMarkerKey(ns, id), l.refLogKey(ns, id));
    EXPECT_LT(l.refLogKey(ns, id), l.refSnapshotKey(ns, id));
}

TEST(CasLayout, ParseRefObjectKeyRejections)
{
    Layout l("p");
    const RootNamespace ns{"srv1/tbl@cas@"};
    const RefTxnId id{7, 0x8e};
    const String log_key = l.refLogKey(ns, id);
    const String snap_key = l.refSnapshotKey(ns, id);

    /// Foreign top-level prefix.
    EXPECT_FALSE(l.parseRefObjectKey("p/cas/manifests/srv1/tbl@cas@/_log/" + renderRefTxnId(id)).has_value());
    /// Unknown kind directory (also covers the removed numeric-shard ref-key shape, which has no kind dir).
    EXPECT_FALSE(l.parseRefObjectKey("p/cas/refs/srv1/tbl@cas@/_bogus/" + renderRefTxnId(id)).has_value());
    EXPECT_FALSE(l.parseRefObjectKey(l.refsNamespacePrefix(ns) + "3").has_value());
    /// Uppercase hex and a short id are non-canonical RefTxnId renders.
    EXPECT_FALSE(l.parseRefObjectKey("p/cas/refs/srv1/tbl@cas@/_log/"
        "0000000000000007-000000000000008E").has_value());
    EXPECT_FALSE(l.parseRefObjectKey("p/cas/refs/srv1/tbl@cas@/_log/7-8e").has_value());
    /// `_snap` without its stored suffix, and WITH a stray one, are both rejected. The suffix is taken
    /// from the registry rather than spelled out: it was `.proto` when this test was written and is
    /// `.zst` today, and stripping the wrong number of characters would have tested nothing.
    const String snap_suffix{storedSuffix(FormatId::RefSnapshot)};
    EXPECT_FALSE(l.parseRefObjectKey(snap_key.substr(0, snap_key.size() - snap_suffix.size())).has_value());
    EXPECT_FALSE(l.parseRefObjectKey(log_key + ".proto").has_value());
    /// `_cleanup`/`_log` ids never carry an extension.
    EXPECT_FALSE(l.parseRefObjectKey(l.refCleanupMarkerKey(ns, id) + ".bin").has_value());
    /// Trailing garbage after the id.
    EXPECT_FALSE(l.parseRefObjectKey(log_key + "/extra").has_value());
    EXPECT_FALSE(l.parseRefObjectKey(snap_key + "/extra").has_value());
    /// Missing namespace segment entirely.
    EXPECT_FALSE(l.parseRefObjectKey("p/cas/refs/_log/" + renderRefTxnId(id)).has_value());
    /// The `_ckpt` (spec INV-4) has no kind directory and no transaction id, so the id-bearing parser
    /// must not claim it. Every sweep over the ref prefix has to consult `parseRefCkptKey` as well --
    /// `groupRefKeys` treats a key neither parser recognizes as corruption that aborts ref folding.
    EXPECT_FALSE(l.parseRefObjectKey(l.refCkptKey(ns)).has_value());
}

/// Stage A task 5 (spec INV-4): `refCkptKey` and `parseRefCkptKey` are inverses, and the `_ckpt`
/// parser is exactly as strict as its id-bearing sibling -- it claims OUR checkpoint keys and nothing
/// else. It returns `std::nullopt` rather than throwing for the same reason `parseRefObjectKey` does:
/// classifying an untrusted listed key is an ordinary "is this ours" question.
TEST(CasLayout, RefCkptKeyRoundTripsAndRejectsEverythingElse)
{
    Layout l("p");
    const RootNamespace ns{"srv1/tbl@cas@"};
    const RefTxnId id{7, 0x8e};

    /// Stage A shape: the namespace prefix plus the bare leaf, with no compression suffix (the format
    /// is raw), so the key is exactly `<ns>/_ckpt`.
    EXPECT_EQ(l.refCkptKey(ns), l.refsNamespacePrefix(ns) + "_ckpt");
    EXPECT_EQ(l.parseRefCkptKey(l.refCkptKey(ns)), ns);
    /// A namespace with slashes in it round-trips whole: everything before the leaf is the namespace.
    EXPECT_EQ(l.parseRefCkptKey("p/cas/refs/a/b/c/_ckpt"), RootNamespace{"a/b/c"});

    /// Foreign pool prefix.
    EXPECT_FALSE(l.parseRefCkptKey("q/cas/refs/srv1/tbl@cas@/_ckpt").has_value());
    /// The three id-bearing kinds are not checkpoints.
    EXPECT_FALSE(l.parseRefCkptKey(l.refLogKey(ns, id)).has_value());
    EXPECT_FALSE(l.parseRefCkptKey(l.refSnapshotKey(ns, id)).has_value());
    EXPECT_FALSE(l.parseRefCkptKey(l.refCleanupMarkerKey(ns, id)).has_value());
    /// A suffix the registry does not put there, and trailing garbage.
    EXPECT_FALSE(l.parseRefCkptKey(l.refCkptKey(ns) + ".zst").has_value());
    EXPECT_FALSE(l.parseRefCkptKey(l.refCkptKey(ns) + "/extra").has_value());
    /// A near-miss leaf name.
    EXPECT_FALSE(l.parseRefCkptKey(l.refsNamespacePrefix(ns) + "_ckp").has_value());
    EXPECT_FALSE(l.parseRefCkptKey(l.refsNamespacePrefix(ns) + "_ckpt2").has_value());
    /// Missing namespace segment entirely.
    EXPECT_FALSE(l.parseRefCkptKey("p/cas/refs/_ckpt").has_value());
    /// The mirror of the rejection above: `_ckpt` is not a canonical `RefTxnId` render, so a key that
    /// puts it inside a kind directory is claimed by NEITHER parser.
    EXPECT_FALSE(l.parseRefObjectKey(l.refsNamespacePrefix(ns) + "_log/_ckpt").has_value());
    /// It IS claimed by this one, as a checkpoint of the namespace `srv1/tbl@cas@/_log`. That is not a
    /// hole: a namespace is an OPAQUE multi-segment string (the wiring composes "srv1/<uuid>",
    /// "shadow/<backup>/<uuid>"), so nothing in a key can distinguish a deeper real namespace from a
    /// shallower one with a stray segment. Reading it as a checkpoint of the longer name is the only
    /// answer available, and it is inert: the table it names has no logs and no snapshots, so the fold
    /// does nothing for it.
    EXPECT_EQ(l.parseRefCkptKey(l.refsNamespacePrefix(ns) + "_log/_ckpt"),
              RootNamespace{ns.string() + "/_log"});
}

/// C3: blobKey/parseBlobKey are inverses; pins the grammar before relocating the definitions
/// from CasPartWriteTxn.cpp to CasLayout.cpp (relocation must not change a single byte of output).
TEST(CasLayout, BlobKeyRoundTripsThroughParse)
{
    DB::Cas::Layout layout("pool0");
    const DB::Cas::BlobRef ref{DB::Cas::BlobHashAlgo::XXH3_128,
                               DB::Cas::codecFor(DB::Cas::BlobHashAlgo::XXH3_128).fromHex(std::string(32, 'a'))};
    const String body = layout.blobKey(ref);
    const String meta = layout.blobMetaKey(ref);
    EXPECT_EQ(meta, body + ".meta");

    auto parsed_body = layout.parseBlobKey(body);
    auto parsed_meta = layout.parseBlobKey(meta);   /// body and .meta parse to the SAME BlobRef
    ASSERT_TRUE(parsed_body.has_value());
    ASSERT_TRUE(parsed_meta.has_value());
    EXPECT_EQ(*parsed_body, ref);
    EXPECT_EQ(*parsed_meta, ref);
    EXPECT_FALSE(layout.parseBlobKey("pool0/blobs/unknown-algo/aa/aa00").has_value());  /// foreign => nullopt
}

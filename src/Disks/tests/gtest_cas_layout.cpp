#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>

using namespace DB::Cas;

TEST(CasLayout, KeyShapes)
{
    Layout l{"p"};
    EXPECT_EQ(l.blobKey(BlobId{"00aabb"}), "p/blobs/00/00aabb");
    EXPECT_EQ(l.treeKey(TreeId{"ffee01"}), "p/trees/ff/ffee01");
    EXPECT_EQ(l.gcStateKey(), "p/gc/state");
    EXPECT_EQ(l.retiredKey(4, 9, 1), "p/gc/retired/4.9/1");
    EXPECT_EQ(l.outcomesKey(4, 9, 1), "p/gc/outcomes/4.9/1");
    EXPECT_EQ(l.checkpointKey(12), "p/gc/checkpoint/12");
    EXPECT_EQ(l.buildHeartbeatKey("deadbeef00"), "p/builds/de/deadbeef00");
    EXPECT_EQ(l.poolMetaKey(), "p/_pool_meta");
}

TEST(CasLayout, RootNamespaceKeys)
{
    Layout l("p");
    RootNamespace ns{"srv1/3f2e-uuid"};
    EXPECT_EQ(l.rootShardKey(ns, 3), "p/roots/srv1/3f2e-uuid/3");
    EXPECT_EQ(l.rootNamespacePrefix(ns), "p/roots/srv1/3f2e-uuid/");
    EXPECT_EQ(l.namespaceFileKey(ns, "format_version.txt"), "p/roots/srv1/3f2e-uuid/_files/format_version.txt");
    EXPECT_EQ(l.namespaceFilesPrefix(ns), "p/roots/srv1/3f2e-uuid/_files/");
    EXPECT_EQ(l.gcRetiredPrefix(), "p/gc/retired/");
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

TEST(CasLayout, GcSnapAndRootsKeys)
{
    Layout l("p");
    EXPECT_EQ(l.gcSnapKey(/*generation*/ 12, /*snap_shard*/ 3), "p/gc/snap/12/3");
    EXPECT_EQ(l.gcSnapShardPrefix(12), "p/gc/snap/12/");
    EXPECT_EQ(l.rootsPrefix(), "p/roots/");
}

TEST(CasLayout, ShortIdThrows)
{
    Layout l{"p"};
    EXPECT_THROW(l.blobKey(BlobId{"x"}), DB::Exception);    // < 2 chars
    EXPECT_THROW(l.treeKey(TreeId{""}), DB::Exception);      // empty
    EXPECT_NO_THROW(l.blobKey(BlobId{"ab"}));                // exactly 2 chars is OK
}

TEST(CasLayout, RegistryKeyMovedToGc)
{
    Layout l("p");
    EXPECT_EQ(l.rootsRegistryKey(), "p/gc/registry");
    /// The registry no longer lives under roots/, so a `_registry` namespace segment is no longer
    /// reserved (design §5.3 bonus cleanup) — but it also never occurs in a real CH path.
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

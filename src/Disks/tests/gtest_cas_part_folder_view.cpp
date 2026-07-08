#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartRefKey.h>
#include <gtest/gtest.h>

using namespace DB;

TEST(CasPartRefKey, CacheKeyIsUnambiguous)
{
    /// Refs may contain '/' (the `detached/<part>` fold, B181); the '\0' join keeps
    /// (ns="a", ref="b/c") distinct from (ns="a/b", ref="c").
    const ContentAddressed::PartRefKey k1{Cas::RootNamespace{"a"}, "b/c"};
    const ContentAddressed::PartRefKey k2{Cas::RootNamespace{"a/b"}, "c"};
    EXPECT_NE(k1.cacheKey(), k2.cacheKey());
    EXPECT_FALSE(k1 == k2);
    EXPECT_TRUE((k1 == ContentAddressed::PartRefKey{Cas::RootNamespace{"a"}, "b/c"}));
}

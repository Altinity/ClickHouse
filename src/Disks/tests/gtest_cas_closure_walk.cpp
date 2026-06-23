#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.h>
#include <Disks/tests/cas_test_helpers.h>

#include <map>
#include <set>
#include <vector>

using DB::UInt128;
using DB::Cas::tests::u128Of;

TEST(CasClosureWalk, RecursesSubtreesDedupsAndYieldsAllObjects)
{
    using namespace DB::Cas;
    auto entry = [](Placement placement, const UInt128 & file_hash) -> TreeEntry
    {
        TreeEntry e;
        e.placement = placement;
        e.file_hash = file_hash;
        return e;
    };
    std::map<UInt128, std::vector<TreeEntry>> nodes = {
        { u128Of("T"), { entry(Placement::Blob, u128Of("B1")),
                         entry(Placement::Subtree, u128Of("S")) } },
        { u128Of("S"), { entry(Placement::Blob, u128Of("B2")),
                         entry(Placement::Blob, u128Of("B1")) } },
    };
    auto inlineSource = [&](const UInt128 & node) -> std::vector<TreeEntry> {
        auto it = nodes.find(node); return it == nodes.end() ? std::vector<TreeEntry>{} : it->second;
    };
    size_t edge_calls = 0;
    std::set<UInt128> visited_trees;
    closureWalk(u128Of("T"), inlineSource,
        /*on_tree=*/[&](const UInt128 & t){ visited_trees.insert(t); },
        /*on_edge=*/[&](const UInt128 & /*parent*/, const TreeEntry & /*e*/){ ++edge_calls; });
    EXPECT_EQ(visited_trees, (std::set<UInt128>{u128Of("T"), u128Of("S")}));   // both trees expanded once (dedup)
    EXPECT_EQ(edge_calls, 4u);                                                  // T->B1, T->S, S->B2, S->B1
}

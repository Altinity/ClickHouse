#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h>
#include <base/types.h>
#include <functional>
namespace DB::Cas
{
using EntriesOf = std::function<std::vector<TreeEntry>(const UInt128 & node)>;
using OnTree    = std::function<void(const UInt128 & tree)>;
/// Called once per non-Inline child entry; the full TreeEntry gives placement/file_hash/file_size.
using OnEdge    = std::function<void(const UInt128 & parent_tree, const TreeEntry & entry)>;
/// One closure traversal: DFS from `root`, dedup trees via a seen-set, recurse on Subtree, skip Inline.
void closureWalk(const UInt128 & root, const EntriesOf & entries_of, const OnTree & on_tree, const OnEdge & on_edge);
}

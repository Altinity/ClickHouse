#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasClosureWalk.h>

#include <set>

namespace DB::Cas
{

namespace
{
/// Recursive DFS carrying the shared `seen` set so a tree reachable via several paths is expanded
/// once. Matches CasFsck.cpp's placement handling: Blob/PackSlice yield an edge; Subtree yields an
/// edge AND recurses into the child tree; Inline carries its bytes in the payload — no object/edge.
void closureWalkImpl(const UInt128 & node, const EntriesOf & entries_of, const OnTree & on_tree,
                     const OnEdge & on_edge, std::set<UInt128> & seen)
{
    if (!seen.insert(node).second)
        return;
    on_tree(node);
    for (const TreeEntry & entry : entries_of(node))
    {
        switch (entry.placement)
        {
            case Placement::Blob:
            case Placement::PackSlice:
                on_edge(node, entry);
                break;
            case Placement::Subtree:
                on_edge(node, entry);
                closureWalkImpl(entry.file_hash, entries_of, on_tree, on_edge, seen);
                break;
            case Placement::Inline:
                break;   /// embedded bytes — no separate object, no edge
        }
    }
}
}

void closureWalk(const UInt128 & root, const EntriesOf & entries_of, const OnTree & on_tree, const OnEdge & on_edge)
{
    std::set<UInt128> seen;
    closureWalkImpl(root, entries_of, on_tree, on_edge, seen);
}

}

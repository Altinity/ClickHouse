#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowManifestSet.h>

namespace DB::Cas
{

bool RefCowManifestSet::contains(const ManifestRef & m) const
{
    const auto it = overlay.find(m);
    if (it != overlay.end())
        return it->second;
    return base->contains(m);
}

void RefCowManifestSet::insert(const ManifestRef & m)
{
    chassert(!contains(m));
    const auto it = overlay.find(m);
    if (it != overlay.end())
        it->second = true;   /// was a tombstone shadowing a base member -- revive it
    else
        overlay.emplace(m, true);
    ++net_delta;
}

void RefCowManifestSet::erase(const ManifestRef & m)
{
    chassert(contains(m));
    const auto it = overlay.find(m);
    if (it != overlay.end())
    {
        if (base->contains(m))
            it->second = false;   /// keep shadowing the base member
        else
            overlay.erase(it);    /// pure-overlay member: nothing left to shadow
    }
    else
    {
        overlay.emplace(m, false);   /// tombstone a base-only member
    }
    --net_delta;
}

void RefCowManifestSet::materialize()
{
    if (overlay.empty())
        return;
    auto merged = std::make_shared<Base>(*base);
    for (const auto & [m, present] : overlay)
    {
        if (present)
            merged->insert(m);
        else
            merged->erase(m);
    }
    base = std::move(merged);
    overlay.clear();
    net_delta = 0;
}

}

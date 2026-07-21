#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowManifestSet.h>
#include <Common/Exception.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

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
    /// Unconditional membership guard, in EVERY build (not a `chassert`): a duplicate insert means the
    /// index has drifted from `committed`/`precommits`, and if it silently bumped `net_delta` the index
    /// would report a manifest present that a single `erase` could then hide while another owner still
    /// names it -- corrupting the add-precommit uniqueness invariant and GC's `+1/-1` edge accounting.
    /// Fail closed instead. The caller's own uniqueness check is what enforces the invariant; this is the
    /// last line that turns a maintaining-code bug into a caught exception rather than silent drift.
    if (contains(m))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefCowManifestSet: inserting a manifest that already has an owner -- the owned-manifest "
            "index has drifted from committed/precommits (a bug in the maintaining code)");
    const auto it = overlay.find(m);
    if (it != overlay.end())
        it->second = true;   /// was a tombstone shadowing a base member -- revive it
    else
        overlay.emplace(m, true);
    ++net_delta;
}

void RefCowManifestSet::erase(const ManifestRef & m)
{
    /// Same fail-closed rationale as `insert`: erasing an absent manifest would drift `net_delta` and,
    /// worse, could shadow a still-live owner. Throw in every build rather than silently corrupting.
    if (!contains(m))
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefCowManifestSet: erasing a manifest with no current owner -- the owned-manifest index "
            "has drifted from committed/precommits (a bug in the maintaining code)");
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
    /// Uniquely-owned base (the production flush case): fold into it in place -- O(overlay), no O(N)
    /// `unordered_set` copy. Otherwise a copy still shares this base, so fold into a fresh one and
    /// swap, leaving the shared holder untouched. `target = base` in the sole-owner branch is a
    /// `shared_ptr` bump, not a `Base` copy; the `std::move` below hands it straight back. See the
    /// header for the full safety argument.
    std::shared_ptr<Base> target = (base.use_count() == 1) ? base : std::make_shared<Base>(*base);
    for (const auto & [m, present] : overlay)
    {
        if (present)
            target->insert(m);
        else
            target->erase(m);
    }
    base = std::move(target);
    overlay.clear();
    net_delta = 0;
}

}

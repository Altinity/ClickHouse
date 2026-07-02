#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>

#include <algorithm>
#include <mutex>

namespace DB::Cas
{

RetireView::RetireView(BackendPtr backend_, Layout layout_)
    : backend(std::move(backend_))
    , layout(std::move(layout_))
{
}

void RetireView::refresh()
{
    /// Build the new map + round in locals — no lock held during backend I/O; readers never see
    /// a half view.
    ///
    /// ORDERING (ack-floor redesign): read gc/state FIRST; the per-shard retired-list refs come out of
    /// that SAME body, so a set can never be older than the round it is installed for. This subsumes the
    /// old LIST-vs-state ordering hazard (a flat LIST could observe entries before or after the round it
    /// was labeled with); now the round and the refs that resolve its list are one atomic read.
    ///
    /// Two racing refreshes may install views out of round order — each installed view is internally
    /// coherent and honestly labeled with the round it observed, and a gate needing a newer round simply
    /// refreshes again; there is deliberately no monotonicity guard.
    uint64_t new_round = 0;
    std::map<uint64_t, String> retired_refs;
    if (auto state_object = backend->get(layout.gcStateKey()))
    {
        const GcState state = decodeGcState(state_object->bytes);
        new_round = state.round;
        retired_refs = state.retired_refs;
    }
    /// ABSENT gc/state => round 0, empty view: a pool GC never touched.

    std::map<std::pair<uint8_t, UInt128>, std::vector<Token>> new_condemned;

    /// GET each referenced retired-list object. An absent ref map or an absent object contributes
    /// nothing — NOT an error: a shard with no outstanding candidates simply has no ref (or a ref whose
    /// object GC has already emptied/removed). Skipping it means strictly LESS condemnation, safe for the
    /// WRITER because the publish gate re-observes under W-REVALIDATE.
    for (const auto & [shard, key] : retired_refs)
    {
        auto object = backend->get(key);
        if (!object)
            continue;

        RetiredSet set = decodeRetiredSet(object->bytes);
        for (auto & entry : set.entries)
            new_condemned[{static_cast<uint8_t>(entry.kind), entry.hash}].push_back(std::move(entry.token));
    }

    std::unique_lock lock(mutex);
    view_round = new_round;
    condemned = std::move(new_condemned);
}

uint64_t RetireView::round() const
{
    std::shared_lock lock(mutex);
    return view_round;
}

std::optional<std::vector<Token>> RetireView::findCondemned(ObjectKind kind, const UInt128 & hash) const
{
    std::shared_lock lock(mutex);
    const auto it = condemned.find({static_cast<uint8_t>(kind), hash});
    if (it == condemned.end())
        return std::nullopt;
    return it->second;
}

bool RetireView::isCondemnedToken(ObjectKind kind, const UInt128 & hash, const Token & token) const
{
    std::shared_lock lock(mutex);
    const auto it = condemned.find({static_cast<uint8_t>(kind), hash});
    if (it == condemned.end())
        return false;
    /// Token identity is Token::operator== — value AND type (pinned in the header comment).
    return std::find(it->second.begin(), it->second.end(), token) != it->second.end();
}

}

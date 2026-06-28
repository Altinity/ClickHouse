#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h>

#include <algorithm>
#include <mutex>

namespace DB::Cas
{

namespace
{

/// Refresh is rare by construction (Store::open and fence-advanced publish conflicts only), so a
/// large page keeps the LIST round-trips minimal; the cursor loop below still covers any size.
constexpr size_t LIST_PAGE_LIMIT = 1000;

}

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
    /// ORDERING: read gc/state FIRST, then the retired sets. The view then claims round R while
    /// its entries reflect storage AT-OR-AFTER R: entries GC drops after we read gc/state are
    /// handled by the gate's re-observation under W-REVALIDATE, and entries ADDED for round R+1
    /// are a bonus (strictly more conservative). Reading gc/state last could claim a NEWER round
    /// over entries observed BEFORE it — overstating how current the view is.
    ///
    /// Two racing refreshes may install views out of round order — each installed view is
    /// internally coherent and honestly labeled with the round it observed, and a gate needing a
    /// newer round simply refreshes again; there is deliberately no monotonicity guard.
    uint64_t new_round = 0;
    uint64_t snap_generation = 0;
    uint64_t snap_attempt = 0;
    if (auto state_object = backend->get(layout.gcStateKey()))
    {
        const GcState state = decodeGcState(state_object->bytes);
        new_round = state.round;
        snap_generation = state.snap_generation;
        snap_attempt = state.snap_attempt;
    }
    /// ABSENT gc/state => round 0: a pool GC never touched.

    std::map<std::pair<uint8_t, UInt128>, std::vector<Token>> new_condemned;

    /// Task 3: retired sets are attempt-scoped under the adopted (snap_generation, snap_attempt). LIST
    /// only that attempt's retired namespace so an unadopted (deposed-leader) retired set is invisible to
    /// the writer publish gate. (Task 7 refines round/generation coherence; minimal compile-correct here.)
    const String prefix = layout.gcGenAttemptRetiredPrefix(snap_generation, snap_attempt);
    String cursor;
    while (true)
    {
        ListPage page = backend->list(prefix, cursor, LIST_PAGE_LIMIT);
        for (const auto & listed : page.keys)
        {
            auto object = backend->get(listed.key);
            /// A retired object that disappears between list and get was rewritten/removed by GC —
            /// skipping it means strictly LESS condemnation, which is the safe direction for the
            /// WRITER only because the publish gate re-observes under W-REVALIDATE.
            if (!object)
                continue;

            RetiredSet set = decodeRetiredSet(object->bytes);
            for (auto & entry : set.entries)
                new_condemned[{static_cast<uint8_t>(entry.kind), entry.hash}].push_back(std::move(entry.token));
        }
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
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

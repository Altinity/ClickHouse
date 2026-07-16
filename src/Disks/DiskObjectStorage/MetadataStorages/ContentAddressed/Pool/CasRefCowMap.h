#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <base/types.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace DB::Cas
{

/// A value-semantic ordered map from `ref_name` to `RefCommittedRow`, drop-in for the
/// `std::map<String, RefCommittedRow>` `RefTableState::committed` used to hold (spec
/// docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md §Mechanism): copy-on-write
/// over an immutable shared base plus a small per-copy overlay, so the copy-then-mutate-then-swap
/// pattern `CasRefLedger`/`CasRefProtocol` already use (`working = rt->state`, `scratch = state`,
/// candidate snapshots, ...) costs O(touched rows), not O(all rows).
///
/// - Keyed reads (`find`/`contains`/`at`/`count`) check `overlay` first (a tombstone there means
///   "removed"; a present entry means "overridden"), falling back to `base`.
/// - Point writes (`emplace`/`insert_or_assign`/`erase`) only ever touch `overlay`.
/// - Ordered iteration (`begin`/`end`) merges `base` and `overlay` in sorted key order, applying
///   overlay overrides/tombstones -- used only by the table's cold full-scan paths (`snapshotOf`,
///   `CasRefLedger::listRefs`, `dropNamespace`, `CasFsck`/`CasGc` owner-set builders).
/// - `materialize()` folds `overlay` into a fresh immutable `base` (O(n), once); wired into
///   `CasRefLedger::flushRefBatch`'s state-install point so the map is back to "base + empty
///   overlay" before the next flush's trial copies begin.
///
/// The iterator this class hands out is read-only everywhere, even from a non-const map: no access
/// site mutates a row in place through a found iterator any more (`CasRefProtocol.cpp`'s
/// `applySetPayload` used to -- it now reads, copies, and writes the updated row back via
/// `insert_or_assign`, which is the only way an overlay write ever happens). This keeps `RefCowMap`
/// free of the mutable-reference-into-an-immutable-base problem entirely.
class RefCowMap
{
public:
    using Base = std::map<String, RefCommittedRow>;

private:
    using Overlay = std::map<String, std::optional<RefCommittedRow>>;

public:
    /// A read-only forward iterator over the merged (base (+) overlay) view, in sorted key order.
    /// `iterator` is simply an alias of `const_iterator` -- exactly like handing a `std::map`'s
    /// `const_iterator` to `std::map::erase` already works today.
    class const_iterator
    {
    public:
        const_iterator() = default;

        std::pair<const String &, const RefCommittedRow &> operator*() const;

        struct ArrowProxy
        {
            std::pair<const String &, const RefCommittedRow &> value;
            const std::pair<const String &, const RefCommittedRow &> * operator->() const { return &value; }
        };
        ArrowProxy operator->() const { return ArrowProxy{**this}; }

        const_iterator & operator++();

        bool operator==(const const_iterator & other) const
        {
            return base_it == other.base_it && overlay_it == other.overlay_it;
        }
        bool operator!=(const const_iterator & other) const { return !(*this == other); }

    private:
        friend class RefCowMap;
        void normalize();

        Base::const_iterator base_it{};
        Base::const_iterator base_end{};
        Overlay::const_iterator overlay_it{};
        Overlay::const_iterator overlay_end{};
        bool at_overlay = false;
    };
    using iterator = const_iterator;

    RefCowMap() = default;

    const_iterator begin() const;
    const_iterator end() const;
    const_iterator find(const String & key) const;

    bool contains(const String & key) const { return find(key) != end(); }
    size_t count(const String & key) const { return contains(key) ? 1 : 0; }
    const RefCommittedRow & at(const String & key) const;

    size_t size() const { return static_cast<size_t>(static_cast<int64_t>(base->size()) + net_delta); }
    bool empty() const { return size() == 0; }

    std::pair<iterator, bool> emplace(String key, RefCommittedRow row);
    std::pair<iterator, bool> insert_or_assign(String key, RefCommittedRow row);
    size_t erase(const String & key);
    iterator erase(const_iterator pos);

    bool operator==(const RefCowMap & other) const;

    /// Fold `overlay` into a fresh immutable `base` (O(current size)), leaving `overlay` empty.
    /// Called once per ref-log flush by `CasRefLedger::flushRefBatch` right after its state install
    /// (spec §Materialization) -- never per batch item.
    void materialize();

    /// Test-only: current overlay row count (0 right after `materialize()`).
    size_t overlayEntriesForTest() const { return overlay.size(); }
    /// Test-only: `base`'s `shared_ptr::use_count()` -- a copy that shares `base` (no per-row
    /// allocation) bumps this by exactly one.
    long baseUseCountForTest() const { return base.use_count(); }

private:
    void insertLive(const String & key, RefCommittedRow row);

    std::shared_ptr<const Base> base = std::make_shared<const Base>();
    Overlay overlay;
    /// size() = base->size() + net_delta, maintained in lock-step by every overlay-mutating op so
    /// size()/empty() stay O(1) (spec §Mechanism: "size/empty: tracked incrementally").
    int64_t net_delta = 0;
};

}

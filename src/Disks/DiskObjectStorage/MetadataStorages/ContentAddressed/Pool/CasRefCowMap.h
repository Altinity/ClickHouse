#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <base/types.h>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace DB::Cas
{

/// A value-semantic ordered map from `ref_name` to `RefCommittedRow`, designed as a drop-in
/// replacement for the `std::map<String, RefCommittedRow>` previously held directly by
/// `RefTableState::committed`. Copies share an immutable ordered base and copy only a per-copy
/// overlay, so the
/// copy-then-mutate-then-swap operations used by `CasRefLedger` and `CasRefProtocol` cost
/// O(touched rows), rather than copying every committed reference. The shared base is immutable,
/// but the map itself is not thread-safe; callers retain the same state lock or detached-copy
/// ownership rules as the state it contains.
///
/// - Keyed reads (`find`/`contains`/`at`/`count`) check `overlay` first (a tombstone there means
///   "removed"; a present entry means "overridden"), falling back to `base`.
/// - Point writes (`emplace`/`insert_or_assign`/`erase`) only ever touch `overlay`.
/// - Ordered iteration (`begin`/`end`) merges `base` and `overlay` in sorted key order, applying
///   overlay overrides/tombstones -- used only by the table's cold full-scan paths (`snapshotOf`,
///   `CasRefLedger::listRefs`, `dropNamespace`, `CasFsck`/`CasGc` owner-set builders) once the map
///   is integrated into the ref table. This merge preserves the canonical bytewise `ref_name`
///   order required by snapshot encoding.
/// - `materialize()` folds `overlay` into a fresh immutable `base` (O(n), once per flush), leaving
///   an empty overlay before the next flush's trial copies begin. Ref-table integration must perform
///   this at the state-install point, not once per batch item, so the hot copy path remains
///   proportional to the rows touched by the flush.
///
/// Iterators are read-only even when obtained from a non-const map. A caller that needs to change a
/// row must copy it and use `insert_or_assign`; exposing mutable references would allow a write to
/// bypass the overlay and modify neither the owning map's accounting nor its copy-on-write state.
class RefCowMap
{
public:
    /// Sorted by `.first` (bytewise `String` order, matching `std::map<String, ...>`'s default
    /// comparator exactly). Kept as a flat vector rather than a node-based tree so ordered/merged
    /// iteration walks contiguous memory and `materialize()` can rebuild it with a single linear
    /// merge pass instead of repeated tree insert/erase; keyed lookups pay `std::lower_bound`'s
    /// O(log n) comparisons (no pointer-chasing tree descent) in exchange for O(n) insert/erase,
    /// which is fine here because `base` is only ever rebuilt whole, in `materialize()` -- nothing
    /// mutates it row-by-row.
    using Base = std::vector<std::pair<String, RefCommittedRow>>;

private:
    using Overlay = std::map<String, std::optional<RefCommittedRow>>;

public:
    /// A read-only forward iterator over the merged base-and-overlay view, in sorted key order.
    /// `iterator` is an alias of `const_iterator`, so erasing through an iterator cannot expose a
    /// mutable reference into the immutable base.
    class const_iterator
    {
    public:
        const_iterator() = default;

        /// Returns the current key and row. The references remain valid while the source map's
        /// base and overlay entries used by this iterator are not erased or otherwise replaced.
        std::pair<const String &, const RefCommittedRow &> operator*() const;

        /// Temporary proxy that gives a merged pair-of-references iterator the usual `it->member`
        /// syntax without exposing mutable storage.
        struct ArrowProxy
        {
            std::pair<const String &, const RefCommittedRow &> value;
            const std::pair<const String &, const RefCommittedRow &> * operator->() const { return &value; }
        };
        ArrowProxy operator->() const { return ArrowProxy{**this}; }

        /// Advances to the next live entry, skipping tombstones and consuming shadowed base rows.
        const_iterator & operator++();

        bool operator==(const const_iterator & other) const
        {
            return base_it == other.base_it && overlay_it == other.overlay_it;
        }
        bool operator!=(const const_iterator & other) const { return !(*this == other); }

    private:
        friend class RefCowMap;

        /// Advances the two sorted source iterators past tombstones and selects the next source;
        /// overlay entries win when both sources contain the same key.
        void normalize();

        Base::const_iterator base_it{};
        Base::const_iterator base_end{};
        Overlay::const_iterator overlay_it{};
        Overlay::const_iterator overlay_end{};
        bool at_overlay = false;
    };
    using iterator = const_iterator;

    RefCowMap() = default;

    /// Returns an iterator to the first live entry in the merged view.
    const_iterator begin() const;

    /// Returns the past-the-end iterator for the merged view.
    const_iterator end() const;

    /// Looks up `key`, consulting overlay entries before the shared base. A tombstone is reported
    /// as absent, and a successful iterator refers to the overlay row when one overrides the base.
    const_iterator find(const String & key) const;

    bool contains(const String & key) const { return find(key) != end(); }   // NOLINT(readability-container-contains): this is the container's contains implementation.
    size_t count(const String & key) const { return contains(key) ? 1 : 0; }
    /// Returns the row for `key`, or throws `std::out_of_range` when the key is absent or tombstoned.
    const RefCommittedRow & at(const String & key) const;

    size_t size() const { return static_cast<size_t>(static_cast<int64_t>(base->size()) + net_delta); }
    bool empty() const { return size() == 0; }

    /// Inserts `row` only when `key` is absent from the merged view. The new row is stored in the
    /// overlay; the returned flag reports whether insertion happened.
    std::pair<iterator, bool> emplace(String key, RefCommittedRow row);

    /// Inserts or replaces `key` in the overlay. The returned flag is true only when the merged
    /// view did not already contain the key.
    std::pair<iterator, bool> insert_or_assign(String key, RefCommittedRow row);

    /// Removes `key` from the merged view. A base row is retained behind an overlay tombstone;
    /// an overlay-only row can be removed outright. Returns one when a live row was removed.
    size_t erase(const String & key);

    /// Removes the row referenced by `pos` and returns the following iterator. `pos` must belong to
    /// this map and be dereferenceable, as with the corresponding `std::map` operation; `end()` is
    /// accepted as a no-op for compatibility with existing callers.
    iterator erase(const_iterator pos);

    /// Compares the live merged views, including both keys and committed-row contents; the
    /// representation of base and overlay storage does not affect the result.
    bool operator==(const RefCowMap & other) const;

    /// Folds `overlay` into a fresh immutable `base` (O(current size)) and clears the overlay.
    /// Call this after installing a completed state, once per ref-log flush and never once per
    /// batch item. If the overlay is already empty, this is a no-op.
    void materialize();

    /// Test-only: current overlay row count (0 right after `materialize()`).
    size_t overlayEntriesForTest() const { return overlay.size(); }
    /// Test-only: `base`'s `shared_ptr::use_count()` -- a copy that shares `base` (no per-row
    /// allocation) bumps this by exactly one.
    int64_t baseUseCountForTest() const { return base.use_count(); }

private:
    /// Records a live overlay value and updates `net_delta` according to whether it replaces a
    /// tombstone, overrides the base, or introduces a key absent from both sources.
    void insertLive(const String & key, RefCommittedRow row);

    std::shared_ptr<const Base> base = std::make_shared<const Base>();
    Overlay overlay;
    /// size() = base->size() + net_delta, maintained in lock-step by every overlay-mutating op so
    /// size()/empty() stay O(1). `net_delta` counts live overlay changes relative to `base`.
    int64_t net_delta = 0;
};

}

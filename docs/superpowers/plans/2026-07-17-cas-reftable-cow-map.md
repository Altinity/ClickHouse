# CAS ref-table `committed` COW map — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `RefTableState::committed` (`std::map<String, RefCommittedRow>`, `Pool/CasRefProtocol.h:148`) with a value-semantic copy-on-write ordered map (`RefCowMap`) so the per-batch-item/per-op copies in `CasRefLedger::flushRefBatch` and `applyRefLogTxn`/`admits` cost O(touched rows), not O(all refs). Pure performance change: behavior stays byte-identical (same snapshot bytes, same admission decisions, same errors).

**Architecture:** `RefCowMap` holds an immutable shared `base` (`std::shared_ptr<const std::map<String, RefCommittedRow>>`) plus a small per-copy `overlay` (`std::map<String, std::optional<RefCommittedRow>>`, present = override, `nullopt` = tombstone). Copying a `RefCowMap` is an atomic refcount bump on `base` plus a small `overlay` copy — O(overlay size), not O(all rows). Keyed reads/writes hit the overlay first, falling back to `base`. Ordered iteration merges `base` and `overlay` in sorted key order (a standard two-sorted-range merge), which is what `snapshotOf`'s canonical bytewise-sorted output needs. `materialize()` folds `overlay` back into a fresh immutable `base` (O(n), once) — wired into `CasRefLedger::flushRefBatch`'s state-install point so the map returns to "base + empty overlay" before the next flush's trial copies begin.

**Tech Stack:** C++20 (ClickHouse `Disks`/CAS subsystem), gtest, `ninja`/`unit_tests_dbms`.

**Design spec (source of truth):** `docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md` (approved, commit `b55e75bc4d5`).

## Global Constraints

- Branch `cas-gc-rebuild`. No `git push`.
- No `rebase`/`amend` — add new commits.
- Every commit is pathspec-exact (`git add <exact files>`, never `-A`/`.`); before committing run `git diff --cached --stat` and confirm it contains ONLY the files this task touched (foreign-file check — this is a shared worktree).
- Wrap every `git` command in `flock /tmp/cas_git.lock -c '...'`.
- Wrap every build in `flock /tmp/cas_build.lock -c '...'`. Never pass `-j` to `ninja`, never use `nproc` — let it auto-detect. Build target: `unit_tests_dbms`. Always redirect build output to a log file under the build directory (e.g. `build/build_task1.log`); a subagent analyzes the log and reports a concise pass/fail summary back, not the raw log.
- Redirect every test run to a log file under the build directory too (unique name per task so parallel runs don't clobber each other); a subagent analyzes it.
- The `Ca*:Cas*` gtest battery must stay at 0 failures throughout (`build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*'`). Baseline going in: 905/905. **Note:** Phase 1 (Tasks 1–2) *adds* new tests under suite name `CasRefCowMap`, which matches the `Cas*` filter — the total count will grow past 905 as a direct, expected result of this plan; the invariant that must hold is "0 failures, and every one of the original 905 still passes," not that the total stays literally 905.
- Every commit trailer ends with exactly:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```
- Say "exception", never "crash" (release build never crashes on a logical error). Say "ASan", never "ASAN".
- Wrap literal ClickHouse SQL/class/function names in inline code (e.g. `` `RefCowMap` ``, `` `applySetPayload` ``); write function names as `f`, not `f()`, in prose/comments/commit messages.
- Allman braces (opening brace on its own line) in all new/modified C++.
- Never use `sleep` in C++ to paper over a race.
- **Non-goals (do not implement — see design spec §Non-goals):** a persistent/immutable ordered tree (spec's "approach A"); swapping the base container to `absl::btree_map`; touching `RefTableState::precommits` (stays `std::set`) or any other `RefTableState` field.

---

## Phase 1 — the `RefCowMap` type

### Task 1: `RefCowMap` core — keyed ops + ordered iteration

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.cpp`
- Test: `src/Disks/tests/gtest_cas_ref_cow_map.cpp` (new file)

**Interfaces:**
- Consumes: `DB::Cas::RefCommittedRow` (existing, `Formats/CasRefSnapshotFormat.h:34`, has `ref_name`/`manifest_ref`/`payload`/`published_at_ms`, `operator==` defaulted), `DB::Cas::ManifestRef` (existing, `Primitives/CasTypes.h:359`, has `operator==`/`operator<`).
- Produces: `DB::Cas::RefCowMap` with a `std::map<String, RefCommittedRow>`-compatible subset: `const_iterator`/`iterator` (the same read-only type), `begin()`/`end()`/`find()`/`contains()`/`count()`/`at()`/`size()`/`empty()`/`emplace(String, RefCommittedRow)`/`insert_or_assign(String, RefCommittedRow)`/`erase(const String&)`/`erase(const_iterator)`/`operator==`. Also `materialize()` and two test-only accessors (`overlayEntriesForTest()`, `baseUseCountForTest()`) consumed by Task 2's tests and Phase 3.

- [ ] **Step 1: Write the failing test file**

  Create `src/Disks/tests/gtest_cas_ref_cow_map.cpp`:

  ```cpp
  #include <gtest/gtest.h>
  #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
  #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>

  #include <stdexcept>
  #include <string>
  #include <utility>
  #include <vector>

  using namespace DB::Cas;

  namespace
  {

  RefCommittedRow row(uint64_t epoch, uint64_t seq, uint32_t ordinal, String payload = "")
  {
      RefCommittedRow r;
      r.manifest_ref = ManifestRef{epoch, seq, ordinal};
      r.payload = payload;
      return r;
  }

  }

  /// ===================================================================================
  /// Keyed ops (spec 2026-07-17-cas-reftable-cow-map-design.md §Mechanism)
  /// ===================================================================================

  TEST(CasRefCowMap, EmptyMapHasNoEntries)
  {
      RefCowMap m;
      EXPECT_TRUE(m.empty());
      EXPECT_EQ(m.size(), 0u);
      EXPECT_FALSE(m.contains("a"));
      EXPECT_TRUE(m.find("a") == m.end());
  }

  TEST(CasRefCowMap, EmplaceThenFind)
  {
      RefCowMap m;
      const auto [it, inserted] = m.emplace("a", row(1, 1, 1));
      EXPECT_TRUE(inserted);
      EXPECT_EQ(m.size(), 1u);
      ASSERT_TRUE(m.contains("a"));
      EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
      EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
  }

  TEST(CasRefCowMap, EmplaceDoesNotOverwriteExisting)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      const auto [it, inserted] = m.emplace("a", row(2, 2, 2));
      EXPECT_FALSE(inserted);
      EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
      EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{1, 1, 1}));   /// unchanged
  }

  TEST(CasRefCowMap, InsertOrAssignOverwritesExisting)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      const auto [it, inserted] = m.insert_or_assign("a", row(2, 2, 2));
      EXPECT_FALSE(inserted);
      EXPECT_EQ(it->second.manifest_ref, (ManifestRef{2, 2, 2}));
      EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{2, 2, 2}));
  }

  TEST(CasRefCowMap, InsertOrAssignInsertsWhenAbsent)
  {
      RefCowMap m;
      const auto [it, inserted] = m.insert_or_assign("a", row(1, 1, 1));
      EXPECT_TRUE(inserted);
      EXPECT_EQ(m.size(), 1u);
      EXPECT_EQ(it->second.manifest_ref, (ManifestRef{1, 1, 1}));
  }

  TEST(CasRefCowMap, EraseByKey)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      EXPECT_EQ(m.erase("a"), 1u);
      EXPECT_FALSE(m.contains("a"));
      EXPECT_EQ(m.size(), 0u);
      EXPECT_EQ(m.erase("a"), 0u);              /// already gone: no-op
      EXPECT_EQ(m.erase("nonexistent"), 0u);
  }

  TEST(CasRefCowMap, AtThrowsOnMissingKey)
  {
      RefCowMap m;
      EXPECT_THROW(m.at("missing"), std::out_of_range);
  }

  TEST(CasRefCowMap, CountMatchesContains)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      EXPECT_EQ(m.count("a"), 1u);
      EXPECT_EQ(m.count("b"), 0u);
  }

  /// ===================================================================================
  /// Ordered iteration -- overlay overrides/tombstones a materialized base (spec: "Ordered
  /// iteration: merge-iterate base and overlay ... a standard two-sorted-range merge").
  /// ===================================================================================

  TEST(CasRefCowMap, OrderedIterationOverAllBaseRowsIsSorted)
  {
      RefCowMap m;
      m.emplace("c", row(1, 3, 1));
      m.emplace("a", row(1, 1, 1));
      m.emplace("b", row(1, 2, 1));

      std::vector<String> names;
      for (const auto & [name, r] : m)
          names.push_back(name);
      EXPECT_EQ(names, (std::vector<String>{"a", "b", "c"}));
  }

  TEST(CasRefCowMap, MergedIterationAppliesTombstonesAndOverrides)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      m.emplace("b", row(1, 2, 1));
      m.emplace("c", row(1, 3, 1));
      m.materialize();   /// a, b, c now live in `base`

      m.insert_or_assign("b", row(9, 9, 9));   /// override b via the overlay
      m.erase("c");                             /// tombstone c via the overlay
      m.emplace("d", row(9, 9, 2));             /// pure-overlay addition (not in base)

      std::vector<std::pair<String, ManifestRef>> seen;
      for (const auto & [name, r] : m)
          seen.emplace_back(name, r.manifest_ref);

      const std::vector<std::pair<String, ManifestRef>> expected = {
          {"a", ManifestRef{1, 1, 1}},
          {"b", ManifestRef{9, 9, 9}},
          {"d", ManifestRef{9, 9, 2}},
      };
      EXPECT_EQ(seen, expected);
      EXPECT_EQ(m.size(), 3u);
  }

  TEST(CasRefCowMap, EraseByIteratorReturnsNextAndRemovesTheRow)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      m.emplace("b", row(1, 2, 1));
      m.emplace("c", row(1, 3, 1));

      auto it = m.find("b");
      ASSERT_TRUE(it != m.end());
      auto next = m.erase(it);
      ASSERT_TRUE(next != m.end());
      EXPECT_EQ(next->first, "c");
      EXPECT_FALSE(m.contains("b"));
      EXPECT_EQ(m.size(), 2u);
  }

  TEST(CasRefCowMap, EraseByIteratorOfLastElementReturnsEnd)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      auto it = m.find("a");
      auto next = m.erase(it);
      EXPECT_TRUE(next == m.end());
      EXPECT_TRUE(m.empty());
  }
  ```

- [ ] **Step 2: Confirm it fails to build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task1_red.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task1_red.log
  ```

  Expected: `NINJA_EXIT=` non-zero; the log's first error is that `Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h` does not exist. Have a subagent read `build/build_task1_red.log` and confirm this is the only error class present (report a one-line summary, not the raw log).

- [ ] **Step 3: Write `CasRefCowMap.h`**

  Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h`:

  ```cpp
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
  ```

- [ ] **Step 4: Write `CasRefCowMap.cpp`**

  Create `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.cpp`:

  ```cpp
  #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
  #include <stdexcept>

  namespace DB::Cas
  {

  std::pair<const String &, const RefCommittedRow &> RefCowMap::const_iterator::operator*() const
  {
      return at_overlay
          ? std::pair<const String &, const RefCommittedRow &>(overlay_it->first, *overlay_it->second)
          : std::pair<const String &, const RefCommittedRow &>(base_it->first, base_it->second);
  }

  void RefCowMap::const_iterator::normalize()
  {
      /// Drop overlay tombstones (and the base row each one shadows) until the next live overlay
      /// entry, or exhaustion.
      while (overlay_it != overlay_end && !overlay_it->second.has_value())
      {
          const String key = overlay_it->first;
          ++overlay_it;
          if (base_it != base_end && base_it->first == key)
              ++base_it;
      }
      /// Overlay wins ties: a live overlay entry at the same key as `base_it` is an override.
      at_overlay = (overlay_it != overlay_end) && (base_it == base_end || overlay_it->first <= base_it->first);
  }

  RefCowMap::const_iterator & RefCowMap::const_iterator::operator++()
  {
      if (at_overlay)
      {
          const String key = overlay_it->first;
          ++overlay_it;
          if (base_it != base_end && base_it->first == key)
              ++base_it;   /// this overlay entry shadowed a base row of the same key: consume it too
      }
      else
      {
          ++base_it;
      }
      normalize();
      return *this;
  }

  RefCowMap::const_iterator RefCowMap::begin() const
  {
      const_iterator it;
      it.base_it = base->begin();
      it.base_end = base->end();
      it.overlay_it = overlay.begin();
      it.overlay_end = overlay.end();
      it.normalize();
      return it;
  }

  RefCowMap::const_iterator RefCowMap::end() const
  {
      const_iterator it;
      it.base_it = base->end();
      it.base_end = base->end();
      it.overlay_it = overlay.end();
      it.overlay_end = overlay.end();
      it.at_overlay = false;
      return it;
  }

  RefCowMap::const_iterator RefCowMap::find(const String & key) const
  {
      const auto ov = overlay.find(key);
      if (ov != overlay.end())
      {
          if (!ov->second.has_value())
              return end();   /// tombstoned: not present
          const_iterator it;
          it.base_it = base->find(key);   /// may or may not also exist in base; overlay wins either way
          it.base_end = base->end();
          it.overlay_it = ov;
          it.overlay_end = overlay.end();
          it.at_overlay = true;
          return it;
      }
      const auto b = base->find(key);
      if (b == base->end())
          return end();
      const_iterator it;
      it.base_it = b;
      it.base_end = base->end();
      it.overlay_it = overlay.lower_bound(key);   /// first overlay key >= this one: keeps the iterator mergeable
      it.overlay_end = overlay.end();
      it.at_overlay = false;
      return it;
  }

  const RefCommittedRow & RefCowMap::at(const String & key) const
  {
      const auto it = find(key);
      if (it == end())
          throw std::out_of_range("RefCowMap::at: key not found: " + key);
      return it->second;
  }

  void RefCowMap::insertLive(const String & key, RefCommittedRow row)
  {
      const auto ov = overlay.find(key);
      if (ov != overlay.end())
      {
          if (!ov->second.has_value())
              ++net_delta;   /// tombstoned (dead) -> live again
          ov->second = std::move(row);
      }
      else
      {
          if (!base->contains(key))
              ++net_delta;   /// brand new key, absent from base too
          overlay.emplace(key, std::move(row));
      }
  }

  std::pair<RefCowMap::iterator, bool> RefCowMap::emplace(String key, RefCommittedRow row)
  {
      if (contains(key))
          return {find(key), false};
      insertLive(key, std::move(row));
      return {find(key), true};
  }

  std::pair<RefCowMap::iterator, bool> RefCowMap::insert_or_assign(String key, RefCommittedRow row)
  {
      const bool was_present = contains(key);
      insertLive(key, std::move(row));
      return {find(key), !was_present};
  }

  size_t RefCowMap::erase(const String & key)
  {
      const auto ov = overlay.find(key);
      if (ov != overlay.end())
      {
          if (!ov->second.has_value())
              return 0;   /// already tombstoned: no-op
          if (base->contains(key))
              ov->second.reset();   /// keep shadowing the base row
          else
              overlay.erase(ov);    /// pure-overlay key: nothing left to shadow
          --net_delta;
          return 1;
      }
      if (!base->contains(key))
          return 0;
      overlay.emplace(key, std::nullopt);   /// tombstone a base-only row
      --net_delta;
      return 1;
  }

  RefCowMap::iterator RefCowMap::erase(const_iterator pos)
  {
      if (pos == end())
          return end();
      const String key = pos->first;
      ++pos;
      erase(key);
      return pos;
  }

  bool RefCowMap::operator==(const RefCowMap & other) const
  {
      if (size() != other.size())
          return false;
      auto a = begin();
      auto b = other.begin();
      for (; a != end() && b != other.end(); ++a, ++b)
          if (a->first != b->first || !(a->second == b->second))
              return false;
      return a == end() && b == other.end();
  }

  void RefCowMap::materialize()
  {
      if (overlay.empty())
          return;
      auto merged = std::make_shared<Base>(*base);
      for (const auto & [key, maybe_row] : overlay)
      {
          if (maybe_row)
              (*merged)[key] = *maybe_row;
          else
              merged->erase(key);
      }
      base = std::move(merged);
      overlay.clear();
      net_delta = 0;
  }

  }
  ```

  No `CMakeLists.txt` change is needed: `src/CMakeLists.txt` already globs this directory (`add_headers_and_sources(dbms Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool)`, currently line 137), and `src/CMakeLists.txt`'s test globbing picks up any `gtest*.cpp` under `src/Disks/tests/` automatically (`file(GLOB_RECURSE ... "gtest*.cpp")`, currently line 883).

- [ ] **Step 5: Build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task1_green.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task1_green.log
  ```

  Have a subagent read `build/build_task1_green.log` and report `NINJA_EXIT` plus a one-line summary. Expected: `NINJA_EXIT=0`.

- [ ] **Step 6: Run the new tests**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='CasRefCowMap.*' > build/test_task1_cowmap.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task1_cowmap.log
  ```

  Have a subagent read `build/test_task1_cowmap.log` and confirm all listed tests pass (`TEST_EXIT=0`).

- [ ] **Step 7: Commit**

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.cpp \
          src/Disks/tests/gtest_cas_ref_cow_map.cpp
  git diff --cached --stat
  '
  ```

  Confirm the `--stat` output lists ONLY those three files (foreign-file check), then:

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git commit -m "$(cat <<"EOF"
  cas: add RefCowMap -- copy-on-write ordered map for the ref-table (Phase 1 Task 1)

  Value-semantic ordered map (immutable shared `base` + small per-copy `overlay`) that will
  replace `RefTableState::committed`'s `std::map<String, RefCommittedRow>` (spec
  docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md). This task lands the
  keyed-op + ordered-iteration core, tested standalone; the type is not wired into
  `RefTableState` yet (Phase 2).

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )"
  git status
  '
  ```

---

### Task 2: `materialize()` correctness + the property/fuzz test

**Files:**
- Modify: `src/Disks/tests/gtest_cas_ref_cow_map.cpp` (append)

**Interfaces:**
- Consumes: everything Task 1 produced (`RefCowMap`, `materialize()`, `overlayEntriesForTest()`, `baseUseCountForTest()`).
- Produces: nothing new for later tasks — this task is pure test coverage over Task 1's implementation (the design spec's "Correctness & testing" bullets: property/fuzz test, O(1)-copy assertion, copy-then-mutate isolation).

`materialize()` itself was already implemented in Task 1 (it has no callers yet, so Task 1's tests could not exercise it) — this task is where it actually gets tested, alongside the property/fuzz oracle test the design spec requires.

- [ ] **Step 1: Write the failing tests**

  Append to `src/Disks/tests/gtest_cas_ref_cow_map.cpp` (add `#include <map>` and `#include <random>` to the top-of-file includes first):

  ```cpp
  /// ===================================================================================
  /// materialize() (spec §Materialization)
  /// ===================================================================================

  TEST(CasRefCowMap, MaterializeFoldsOverlayIntoFreshBaseAndKeepsValuesUnchanged)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      m.emplace("b", row(1, 2, 1));
      m.erase("a");
      EXPECT_GT(m.overlayEntriesForTest(), 0u);

      m.materialize();
      EXPECT_EQ(m.overlayEntriesForTest(), 0u);
      EXPECT_FALSE(m.contains("a"));
      ASSERT_TRUE(m.contains("b"));
      EXPECT_EQ(m.at("b").manifest_ref, (ManifestRef{1, 2, 1}));
      EXPECT_EQ(m.size(), 1u);
  }

  TEST(CasRefCowMap, MaterializeOnAnEmptyOverlayIsANoOp)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      m.materialize();
      const long use_count_before = m.baseUseCountForTest();
      m.materialize();   /// overlay is already empty
      EXPECT_EQ(m.baseUseCountForTest(), use_count_before);
      EXPECT_TRUE(m.contains("a"));
  }

  TEST(CasRefCowMap, MaterializeDoesNotAffectACopyTakenBeforeIt)
  {
      RefCowMap m;
      m.emplace("a", row(1, 1, 1));
      RefCowMap snapshot_before = m;   /// copy shares m's pre-materialize base, owns its own overlay
      m.insert_or_assign("a", row(2, 2, 2));
      m.materialize();

      EXPECT_EQ(m.at("a").manifest_ref, (ManifestRef{2, 2, 2}));
      EXPECT_EQ(snapshot_before.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
  }

  TEST(CasRefCowMap, EqualityComparesEffectiveContentsNotInternalLayout)
  {
      RefCowMap a;
      a.emplace("x", row(1, 1, 1));
      a.materialize();   /// "x" lives in `base`

      RefCowMap b;
      b.emplace("x", row(1, 1, 1));   /// same logical content, but lives entirely in `overlay`

      EXPECT_EQ(a.overlayEntriesForTest(), 0u);
      EXPECT_GT(b.overlayEntriesForTest(), 0u);
      EXPECT_TRUE(a == b);
  }

  /// ===================================================================================
  /// Copy-on-write isolation + O(1)-copy assertion (spec §Correctness & testing)
  /// ===================================================================================

  TEST(CasRefCowMap, CopyIsIsolatedFromOriginal)
  {
      RefCowMap original;
      original.emplace("a", row(1, 1, 1));
      original.materialize();

      RefCowMap copy = original;
      copy.insert_or_assign("a", row(9, 9, 9));
      copy.emplace("b", row(9, 9, 9));

      EXPECT_EQ(original.at("a").manifest_ref, (ManifestRef{1, 1, 1}));
      EXPECT_FALSE(original.contains("b"));

      EXPECT_EQ(copy.at("a").manifest_ref, (ManifestRef{9, 9, 9}));
      EXPECT_TRUE(copy.contains("b"));
  }

  TEST(CasRefCowMap, CopySharesBaseUntilEitherSideMaterializesANewOne)
  {
      RefCowMap original;
      original.emplace("a", row(1, 1, 1));
      original.materialize();

      RefCowMap copy = original;
      /// A copy shares the SAME base object (refcount bump, no per-row allocation) until a write
      /// forces a new base into existence via `materialize()` (spec §Mechanism: "Copy = O(1)").
      EXPECT_EQ(original.baseUseCountForTest(), 2);
      EXPECT_EQ(copy.baseUseCountForTest(), 2);

      copy.insert_or_assign("a", row(2, 2, 2));   /// writes go to `copy`'s overlay; `base` is untouched
      EXPECT_EQ(original.baseUseCountForTest(), 2);
      EXPECT_EQ(copy.baseUseCountForTest(), 2);

      copy.materialize();   /// NOW `copy` points at a fresh base of its own
      EXPECT_EQ(original.baseUseCountForTest(), 1);
      EXPECT_EQ(copy.baseUseCountForTest(), 1);
  }

  /// ===================================================================================
  /// Randomized exactness property test: RefCowMap must behave IDENTICALLY to
  /// std::map<String, RefCommittedRow> across randomized op sequences (spec §Correctness &
  /// testing: "random op sequences ... including copy-then-mutate isolation ... and
  /// tombstone/override correctness on the merged iterator").
  /// ===================================================================================

  TEST(CasRefCowMap, PropertyMatchesStdMapOverRandomOps)
  {
      std::mt19937 rng(20260717);

      for (int trial = 0; trial < 50; ++trial)
      {
          RefCowMap actual;
          std::map<String, RefCommittedRow> oracle;

          for (int step = 0; step < 200; ++step)
          {
              const String key = "ref" + std::to_string(rng() % 12);
              const uint32_t action = rng() % 6;
              switch (action)
              {
                  case 0:   /// emplace
                  {
                      RefCommittedRow r = row(1, static_cast<uint64_t>(step) + 1, 1);
                      const bool oracle_inserted = oracle.emplace(key, r).second;
                      const bool actual_inserted = actual.emplace(key, r).second;
                      EXPECT_EQ(oracle_inserted, actual_inserted) << "trial " << trial << " step " << step;
                      break;
                  }
                  case 1:   /// insert_or_assign
                  {
                      RefCommittedRow r = row(2, static_cast<uint64_t>(step) + 1, 2);
                      oracle[key] = r;
                      actual.insert_or_assign(key, r);
                      break;
                  }
                  case 2:   /// erase by key
                  {
                      const size_t oracle_erased = oracle.erase(key);
                      const size_t actual_erased = actual.erase(key);
                      EXPECT_EQ(oracle_erased, actual_erased) << "trial " << trial << " step " << step;
                      break;
                  }
                  case 3:   /// find/contains/at (read-only)
                  {
                      EXPECT_EQ(oracle.contains(key), actual.contains(key)) << "trial " << trial << " step " << step;
                      if (oracle.contains(key))
                          EXPECT_EQ(oracle.at(key), actual.at(key)) << "trial " << trial << " step " << step;
                      break;
                  }
                  case 4:   /// erase via a found iterator
                  {
                      if (auto it = actual.find(key); it != actual.end())
                      {
                          oracle.erase(key);
                          actual.erase(it);
                      }
                      break;
                  }
                  case 5:   /// materialize -- must not change observable content
                  {
                      actual.materialize();
                      break;
                  }
              }

              ASSERT_EQ(oracle.size(), actual.size()) << "trial " << trial << " step " << step;

              auto oit = oracle.begin();
              auto ait = actual.begin();
              for (; oit != oracle.end() && ait != actual.end(); ++oit, ++ait)
              {
                  ASSERT_EQ(oit->first, ait->first) << "trial " << trial << " step " << step;
                  ASSERT_EQ(oit->second, ait->second) << "trial " << trial << " step " << step;
              }
              ASSERT_TRUE(oit == oracle.end()) << "trial " << trial << " step " << step;
              ASSERT_TRUE(ait == actual.end()) << "trial " << trial << " step " << step;
          }
      }
  }
  ```

- [ ] **Step 2: Confirm it fails to build or fails at runtime**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task2_red.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task2_red.log
  ```

  This is expected to build cleanly (Task 1 already implemented `materialize()`/`operator==`/copy semantics) — the point of "RED" here is running the new tests, not a compile failure:

  ```bash
  build/src/unit_tests_dbms --gtest_filter='CasRefCowMap.*' > build/test_task2_red.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task2_red.log
  ```

  Have a subagent read both logs and report whether anything is unexpectedly red. If Task 1's implementation is already fully correct, these new tests may pass immediately (Task 1 gave RefCowMap a real implementation, not a stub) — that is fine; log the result and proceed. If any test fails, fix `CasRefCowMap.cpp` (not the test) to match the spec, since the test encodes the spec's own contract.

- [ ] **Step 3: Re-run to confirm green**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='CasRefCowMap.*' > build/test_task2_green.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task2_green.log
  ```

  Have a subagent confirm `TEST_EXIT=0` and all tests pass.

- [ ] **Step 4: Run the full `Ca*:Cas*` battery**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > build/test_task2_battery.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task2_battery.log
  ```

  Have a subagent confirm 0 failures (count will be 905 + the new `CasRefCowMap.*` tests — see Global Constraints note).

- [ ] **Step 5: Commit**

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git add src/Disks/tests/gtest_cas_ref_cow_map.cpp
  git diff --cached --stat
  '
  ```

  Confirm only that one file is staged, then:

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git commit -m "$(cat <<"EOF"
  cas: RefCowMap property/fuzz test + materialize + O(1)-copy proof (Phase 1 Task 2)

  Randomized-op-sequence oracle test against std::map<String, RefCommittedRow> (insert,
  update, erase, find, size, ordered iteration, materialize interleaved), plus targeted
  tests for materialize() itself, copy-then-mutate isolation, and the O(1)-copy claim
  (shared_ptr use_count, no per-row allocation on copy) -- the design spec's full
  "Correctness & testing" list for RefCowMap in isolation, before it is wired into
  RefTableState (Phase 2).

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )"
  git status
  '
  ```

---

## Phase 2 — swap `RefTableState::committed` to `RefCowMap`

### Task 3: the type swap + `applySetPayload` rewrite

This is necessarily a single atomic change: `RefTableState::committed`'s type is defined once, in one header, and every translation unit that includes it (`CasRefLedger.cpp`, `CasPartWriteTxn.cpp`, `Tools/CasFsck.cpp`, `Gc/CasGc.cpp`, `Gc/CasOrphanManifestSweep.cpp`) sees the new type the moment the header changes — there is no way to stage it file-by-file without an intermediate broken build. Of the ~18 call sites across those files, **all but one** are pure keyed reads (`find`/`end`/`contains`/`count`/`at`) or ordered structured-binding iteration (`for (const auto & [name, row] : state.committed)`) — both already supported by `RefCowMap`'s API, so they compile unchanged. The **one** exception is `CasRefProtocol.cpp`'s `applySetPayload`, which mutates a row in place through a found iterator (`it->second.payload = op.payload;`) — `RefCowMap`'s iterator is read-only (Task 1), so this one site needs a real (behavior-preserving) rewrite.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp`

**Interfaces:**
- Consumes: `RefCowMap` (Task 1).
- Produces: `RefTableState::committed` is now `RefCowMap` — every later task and every existing caller in `CasRefLedger.cpp`/`CasPartWriteTxn.cpp`/`Tools/CasFsck.cpp`/`Gc/CasGc.cpp`/`Gc/CasOrphanManifestSweep.cpp` sees this type from here on.

- [ ] **Step 1: Change the field type in `CasRefProtocol.h`**

  Add the include (near the other `Formats`/`Primitives` includes at the top of the file, currently lines 2–11):

  ```cpp
  #include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCowMap.h>
  ```

  Change `RefTableState` (currently lines 148–156):

  ```cpp
  struct RefTableState
  {
      RefLifecycle lifecycle = RefLifecycle::Removed;   /// see representation note above
      std::optional<RefTxnId> remove_txn_id;
      RefTxnId greatest_applied{};                       /// {0, 0} = no transaction applied yet

      RefCowMap committed;                                              /// keyed by ref_name
      std::set<std::pair<String, ManifestRef>> precommits;             /// (ref_name, manifest_ref)
  };
  ```

  Update the `snapshotOf` doc comment's ordering claim (currently lines 216–220) to attribute sortedness to `RefCowMap` instead of `std::map` directly:

  ```cpp
  /// The canonical snapshot of `state` under `ns` (spec §Snapshot Format): `committed` sorted by
  /// bytewise `ref_name` (guaranteed by `RefCowMap`'s sorted merge-iteration order,
  /// `Pool/CasRefCowMap.h` -- the same ordering `std::map<String, ...>` gave before it, by design)
  /// and `precommits` sorted by `(ref_name, manifest_ref)` (guaranteed by
  /// `std::set<std::pair<String, ManifestRef>>`'s iteration order, since `ManifestRef::operator<`
  /// matches the tuple order `CasRefSnapshotCodec` itself sorts by). `snapshot_id` is
  /// `state.greatest_applied`; a `Removed` state produces zero rows plus `remove_txn_id`, per spec.
  /// Does not itself enforce that the result is encodable (a never-born state's `snapshot_id` is
  /// `{0, 0}`, which `encodeRefTableSnapshot` already rejects) -- that check already lives in the
  /// codec and need not be duplicated here.
  ```

- [ ] **Step 2: Rewrite `applySetPayload` in `CasRefProtocol.cpp`**

  Replace (currently lines 119–131):

  ```cpp
  void applySetPayload(RefTableState & state, const RefOp & op)
  {
      if (state.lifecycle != RefLifecycle::Live)
          throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: set_payload while namespace is not Live");

      const auto it = state.committed.find(op.ref_name);
      if (it == state.committed.end() || !(it->second.manifest_ref == op.expected_manifest_ref))
          throw Exception(ErrorCodes::CORRUPTED_DATA,
              "RefTableState: set_payload '{}' no longer names its expected_manifest_ref", op.ref_name);

      it->second.payload = op.payload;
      it->second.published_at_ms = op.published_at_ms;
  }
  ```

  with:

  ```cpp
  void applySetPayload(RefTableState & state, const RefOp & op)
  {
      if (state.lifecycle != RefLifecycle::Live)
          throw Exception(ErrorCodes::CORRUPTED_DATA, "RefTableState: set_payload while namespace is not Live");

      const auto it = state.committed.find(op.ref_name);
      if (it == state.committed.end() || !(it->second.manifest_ref == op.expected_manifest_ref))
          throw Exception(ErrorCodes::CORRUPTED_DATA,
              "RefTableState: set_payload '{}' no longer names its expected_manifest_ref", op.ref_name);

      /// `RefCowMap`'s iterator is read-only (Pool/CasRefCowMap.h): a write always goes through
      /// `insert_or_assign`, never through the found iterator in place. Copy the row, apply the same
      /// two field mutations the old in-place code did, and write the whole row back -- this IS the
      /// COW map's single-row copy-out (spec §Mechanism), not a whole-table one.
      RefCommittedRow updated = it->second;
      updated.payload = op.payload;
      updated.published_at_ms = op.published_at_ms;
      state.committed.insert_or_assign(op.ref_name, std::move(updated));
  }
  ```

- [ ] **Step 3: Update the `stateFromSnapshot` comment's `std::map::emplace` reference**

  In `stateFromSnapshot`'s doc comment (currently around line 209), change `` "would otherwise DROP the second row via `std::map::emplace` below" `` to `` "would otherwise DROP the second row via `RefCowMap::emplace` below (same no-overwrite-on-existing-key semantics as `std::map::emplace`)" ``. The loop body itself (`state.committed.emplace(row.ref_name, row);`, currently line 222) needs no code change — `RefCowMap::emplace` has the identical signature and semantics.

- [ ] **Step 4: Build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task3.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task3.log
  ```

  This single target pulls in `CasRefLedger.cpp`, `CasPartWriteTxn.cpp`, `Tools/CasFsck.cpp`, `Gc/CasGc.cpp`, and `Gc/CasOrphanManifestSweep.cpp` (all part of the `dbms` library `add_headers_and_sources` glob that `unit_tests_dbms` links), so it exercises every consumer of `RefTableState::committed` in the tree. Have a subagent read `build/build_task3.log` and report `NINJA_EXIT` plus, if non-zero, a list of every distinct compile error (file:line + message) — expected `NINJA_EXIT=0` given the read-only sites are already `RefCowMap`-compatible per this task's analysis. If anything else fails to compile, fix that call site (it means this plan's site inventory missed something) before proceeding, and note the miss in the commit body.

- [ ] **Step 5: Run the full `Ca*:Cas*` battery**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > build/test_task3.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task3.log
  ```

  Have a subagent confirm 0 failures across the whole filter (905 pre-existing + the `CasRefCowMap.*` tests from Phase 1). This is the empirical proof that `snapshotOf`'s byte-identical output and every state-machine invariant survived the swap unchanged (`gtest_cas_ref_statemachine.cpp`, `gtest_cas_ref_writer.cpp`, `gtest_cas_ref_gc.cpp`, `gtest_cas_ref_intake.cpp`, `gtest_cas_fsck.cpp` all exercise `RefTableState::committed` directly and are untouched by this task).

- [ ] **Step 6: Commit**

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp
  git diff --cached --stat
  '
  ```

  Confirm only those two files are staged, then:

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git commit -m "$(cat <<"EOF"
  cas: RefTableState::committed -> RefCowMap (Phase 2 Task 3)

  Swaps the field type from std::map<String, RefCommittedRow> to RefCowMap (spec
  docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md), so every copy-then-
  mutate-then-swap site in CasRefLedger/CasRefProtocol (working = rt->state, scratch =
  state, candidate_state = rt->state, ...) becomes O(touched rows) once materialize() is
  wired in (Phase 3). All keyed-read and ordered-iteration call sites across
  CasRefLedger.cpp/CasPartWriteTxn.cpp/Tools/CasFsck.cpp/Gc/CasGc.cpp/
  Gc/CasOrphanManifestSweep.cpp compile unchanged; the one site that mutated a row through
  a found iterator (applySetPayload) is rewritten to copy-then-insert_or_assign, since
  RefCowMap hands out a read-only iterator. Behavior is byte-identical: Ca*:Cas* battery
  stays green (905 pre-existing + new RefCowMap tests).

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )"
  git status
  '
  ```

---

### Task 4: full-tree rebuild + battery re-confirmation

Task 3's build only compiled `unit_tests_dbms` and its dependency graph. This task rebuilds the full `clickhouse` server binary to catch any external consumer of `RefTableState`/`RefCowMap` that `unit_tests_dbms` does not link (per the source-layout campaign's own prior finding that a renamed/retyped symbol can be stranded outside the unit-test target — see `docs/superpowers/worklogs/2026-07-16-unattended-codecs-txn-sourcelayout-part2.md`), and re-runs the full battery as a clean gate before Phase 3 touches behavior again.

**Files:** none (build + test only).

- [ ] **Step 1: Build the full server binary**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build clickhouse > build/build_task4_server.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task4_server.log
  ```

  Have a subagent read `build/build_task4_server.log` and report `NINJA_EXIT` plus, if non-zero, every distinct error. Expected `NINJA_EXIT=0`.

- [ ] **Step 2: Re-run the full `Ca*:Cas*` battery**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > build/test_task4.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task4.log
  ```

  Have a subagent confirm 0 failures. No commit for this task (no files change) — if the full-tree build surfaces a stranded consumer, fix it as part of Task 3 instead (amend that task's file list and re-run Steps 4–6 there with a new commit; do not amend the existing commit — add a follow-up one per the no-amend global constraint) before continuing to Phase 3.

---

## Phase 3 — materialize-on-install wiring

### Task 5: wire `materialize()` into `flushRefBatch`'s install point

This is the task that actually delivers the performance win: without it, `RefCowMap`'s overlay would keep growing, unmaterialized, across every successful flush forever, and every later flush's `working = rt->state` copy (`CasRefLedger.cpp:1006`) would degrade back toward O(all touched-ever rows) instead of staying O(this-flush's-touched rows).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
- Modify: `src/Disks/tests/gtest_cas_ref_writer.cpp`

**Interfaces:**
- Consumes: `RefCowMap::materialize()` and `RefCowMap::overlayEntriesForTest()` (Task 1).
- Produces: `CasRefLedger::committedOverlayEntriesForTest(const RootNamespace &)` and `Pool::committedOverlayEntriesForTest(const RootNamespace &)` (new test-only accessors, mirroring the existing `tailSinceSnapshotCountForTest` forwarding pattern) — consumed only by this task's own regression test.

- [ ] **Step 1: Write the failing test**

  In `src/Disks/tests/gtest_cas_ref_writer.cpp`, add right after `TEST(RefWriterAppendLane, WarmIsolatedMutationCostsOneCreateZeroReads)` (currently ends at line 576):

  ```cpp
  /// Phase 3 (spec 2026-07-17-cas-reftable-cow-map-design.md §Materialization): each of these N
  /// publishes is its own isolated (unbatched) flush touching exactly one NEW ref -- if
  /// `flushRefBatch` did not materialize `rt->state.committed` after installing each flush's
  /// transaction, the overlay would grow by ~1 entry per flush and this would read back ~N,
  /// defeating the whole point of the COW map for a long-running table.
  TEST(RefWriterAppendLane, MaterializeKeepsOverlaySmallAcrossManyIsolatedFlushes)
  {
      auto backend = std::make_shared<RefWriterTestBackend>();
      auto store = openPool(backend);
      const RootNamespace ns{"srv1/cowmap"};

      constexpr int kRefs = 20;
      for (int i = 0; i < kRefs; ++i)
          publishEmptyPart(store, ns, "ref" + std::to_string(i));

      EXPECT_LE(store->committedOverlayEntriesForTest(ns), 1u);
      EXPECT_EQ(store->listRefs(ns).size(), static_cast<size_t>(kRefs));   /// sanity: all N really committed
  }
  ```

- [ ] **Step 2: Confirm it fails to build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task5_red.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task5_red.log
  ```

  Expected: `NINJA_EXIT=` non-zero, error mentions `committedOverlayEntriesForTest` is not a member of `Pool`. Have a subagent confirm this is the only error.

- [ ] **Step 3: Add the test-only accessors**

  In `CasRefLedger.h`, add right after `tailSinceSnapshotCountForTest` (currently line 114):

  ```cpp
      size_t committedOverlayEntriesForTest(const RootNamespace & ns);
  ```

  In `CasRefLedger.cpp`, add right after `tailSinceSnapshotCountForTest`'s definition (currently lines 1423–1429):

  ```cpp
  size_t CasRefLedger::committedOverlayEntriesForTest(const RootNamespace & ns)
  {
      const auto rt = getRefTableRuntime(ns);
      ensureRefTableRecovered(ns, *rt);
      std::lock_guard lock(rt->state_mutex);
      return rt->state.committed.overlayEntriesForTest();
  }
  ```

  In `CasPool.h`, add right after the `tailSinceSnapshotCountForTest` declaration (currently line 667):

  ```cpp
      size_t committedOverlayEntriesForTest(const RootNamespace & ns);
  ```

  In `CasPool.cpp`, add right after `tailSinceSnapshotCountForTest`'s forwarding definition (currently lines 1179–1182):

  ```cpp
  size_t Pool::committedOverlayEntriesForTest(const RootNamespace & ns)
  {
      return ref_ledger.committedOverlayEntriesForTest(ns);
  }
  ```

- [ ] **Step 4: Wire `materialize()` into the install point**

  In `CasRefLedger.cpp`, inside `flushRefBatch`'s `case CasWriteOutcome::Committed:` block, add the `materialize()` call right after `applyRefLogTxn(rt->state, final_txn);` (currently around line 1215):

  ```cpp
          case CasWriteOutcome::Committed:
          {
              try
              {
                  std::lock_guard lock(rt->state_mutex);
                  applyRefLogTxn(rt->state, final_txn);
                  /// COW-map materialize (spec 2026-07-17-cas-reftable-cow-map-design.md
                  /// §Materialization): fold this flush's overlay into a fresh immutable base HERE,
                  /// under the SAME state_mutex critical section as the install above, so
                  /// `rt->state.committed` is back to "base + empty overlay" before the next flush's
                  /// trial copies (`working = rt->state`, CasRefLedger.cpp:1006) begin -- an O(n) fold
                  /// once per flush, replacing what used to be an implicit O(n) copy on every trial
                  /// anyway.
                  rt->state.committed.materialize();
                  /// rev.6 Task 10 (spec §publish-from-live): this commit's own txn joins the
                  /// applied-above-newest-snapshot tail counters -- the live `rt->state` just mutated
                  /// above IS the next publish candidate's body, so there is no per-entry log to retain.
                  rt->tail_count_since_snapshot.fetch_add(1, std::memory_order_relaxed);
                  rt->tail_bytes_since_snapshot.fetch_add(bytes.size(), std::memory_order_relaxed);
              }
  ```

  (Only the one new line plus its comment are added; the surrounding lines are unchanged context shown for anchoring the edit.)

- [ ] **Step 5: Build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task5_green.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task5_green.log
  ```

  Have a subagent confirm `NINJA_EXIT=0`.

- [ ] **Step 6: Run the new test**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='RefWriterAppendLane.MaterializeKeepsOverlaySmallAcrossManyIsolatedFlushes' > build/test_task5_new.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task5_new.log
  ```

  Have a subagent confirm `TEST_EXIT=0` and the test passes. If `committedOverlayEntriesForTest` reads back close to 20 instead of `<=1`, the `materialize()` call is either missing, misplaced (outside the `state_mutex` lock), or firing on the wrong object (`working.committed` instead of `rt->state.committed`) — fix `flushRefBatch`, not the test, since the test encodes the actual product requirement (spec §Materialization).

- [ ] **Step 7: Run the full `Ca*:Cas*` battery**

  ```bash
  build/src/unit_tests_dbms --gtest_filter='Ca*:Cas*' > build/test_task5_battery.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task5_battery.log
  ```

  Have a subagent confirm 0 failures.

- [ ] **Step 8: Rebuild the full server binary and confirm no stranded consumer**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build clickhouse > build/build_task5_server.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task5_server.log
  ```

  Have a subagent confirm `NINJA_EXIT=0`.

- [ ] **Step 9: Commit**

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h \
          src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp \
          src/Disks/tests/gtest_cas_ref_writer.cpp
  git diff --cached --stat
  '
  ```

  Confirm the `--stat` output lists ONLY those five files, then:

  ```bash
  flock /tmp/cas_git.lock -c '
  cd /home/mfilimonov/workspace/ClickHouse/master
  git commit -m "$(cat <<"EOF"
  cas: materialize the ref-table COW map once per flush (Phase 3 Task 5)

  Wires RefCowMap::materialize() into CasRefLedger::flushRefBatch's state-install point
  (right after applyRefLogTxn(rt->state, final_txn), under the same state_mutex critical
  section), completing the O(touched rows) copy cost the design spec targets
  (docs/superpowers/specs/2026-07-17-cas-reftable-cow-map-design.md). Without this, the
  overlay set up by Phase 2's type swap would grow unboundedly across successive flushes.
  New regression test (RefWriterAppendLane.MaterializeKeepsOverlaySmallAcrossManyIsolated
  Flushes) publishes 20 distinct refs as 20 isolated flushes and asserts the overlay stays
  <=1 entry, guarding against a future removal/misplacement of the materialize() call.
  Ca*:Cas* battery stays green; full server binary rebuilt clean.

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )"
  git status
  '
  ```

---

## Spec coverage checklist (self-review)

- §Mechanism (COW overlay, keyed/point-write/ordered-iteration cost profile, `size`/`empty` O(1)): Task 1.
- §Materialization (fold on install, once per flush): `RefCowMap::materialize()` in Task 1, wired at the real call site in Task 5.
- §Isolation (unchanged invariant — leader validates+PUTs without the lock, install/materialize under `state_mutex`, readers hold the lock for the whole read): unaffected by construction — Tasks 3/5 do not touch any locking, only the type held under the existing locks; Task 5 explicitly places `materialize()` inside the pre-existing `state_mutex` critical section.
- §Scope/ripple (`committed` only, ~15+ access sites drop-in, codec boundary unchanged): Task 3 (site inventory verified against the actual tree — turned up 18 sites across 5 files, not literally 15, all but one drop-in).
- §Correctness & testing (property/fuzz test, O(1)-copy assertion, `Ca*:Cas*` battery green, `snapshotOf` byte-identical): Task 2 (property test + O(1)-copy), Tasks 3/4/5 (battery gates after every behavior-affecting change).
- §Non-goals (persistent tree, `absl::btree_map`, `precommits`): explicitly out of scope in Global Constraints; not touched by any task.
- Commit-path evidence (soak / `trace_log` check that `__copy_construct_tree` is no longer a top stack, optional microbench): explicitly OUT OF SCOPE for this plan — it is a separate item already tracked in the team's own task list (R4: "Soak 20m on improved binary + trace_log (confirm ref-table copies gone)"), to run after this plan's Task 5 lands.

# CA Reduce S3 Op-Count Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the per-operation S3 round-trip count of content-addressed (CA) MergeTree — the measured root cause of node unresponsiveness under load — by (B) eliminating the per-access HEAD on the `resolveRef` read path, (A) making GC snap I/O proportional to churn, and (zstd) shrinking the snap blob.

**Architecture:** Three independent levers on the CA core (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), shipped **biggest-win-first**: Pillar B (`Store` decode-cache: single-flight coalescing + opt-in bounded-TTL), then Pillar A1 (`Gc` resident-snap read-cache + skip-unchanged-persist, then the optional decoupled periodic checkpoint), then zstd compression of the `gc/snap` blob. Each task group ships and is testable on its own. Delete safety (INV-NO-LOSS / INV-NO-RETURN) is preserved by construction; Pillar B keeps every strict-freshness caller force-fresh.

**Tech Stack:** C++ (ClickHouse, Allman braces), gtest (`unit_tests_dbms`), zstd via `ZstdDeflatingWriteBuffer`/`ZstdInflatingReadBuffer`, the CA in-memory test backend (`Cas::InMemoryBackend`), the Python soak harness (`utils/ca-soak/`).

**Spec:** `docs/superpowers/specs/2026-06-14-ca-reduce-s3-op-count-design.md` (B149/B148/B150). **Scope decisions (2026-06-14):** retire-without-HEAD (Pillar A §2.4) is **deferred** (only helps tree/pack, not blobs; revisit after soak). The decoupled periodic checkpoint (A1b, Task Group 4) carries an adversarial-review gate and may be deferred if Groups 1–3 + zstd already restore soak responsiveness — there is a decision gate before it.

**Build/run conventions (from CLAUDE.md):**
- Build: from the build dir, `ninja unit_tests_dbms` **without** `-j`/`nproc`, redirect to a log in the build dir, and have a **subagent** summarize the log (never cat it). Example: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`.
- Run gtests: `build/src/unit_tests_dbms --gtest_filter='<Filter>' > build/test_<name>.log 2>&1`, then a **subagent** summarizes the log.
- Allman braces everywhere. Say "exception" not "crash". Add commits per task; never amend/rebase; never commit to master (work on the current `cas-mergetree-poc` branch or a task branch).

---

## File Structure

**Pillar B (read path):**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` — add single-flight members, TTL cache-entry struct, `allow_stale` params.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp` — refactor `readShardDecoded` into coalescing wrapper + uncoalesced worker; add TTL fast-path; thread `allow_stale`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` (`PoolConfig`) — `shard_decode_cache_ttl_ms`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` — opt the audited hot-path callers into `allow_stale`.
- `src/Disks/tests/cas_test_helpers.h` — add a reusable op-counting backend (`CountingBackend`).
- `src/Disks/tests/gtest_cas_store.cpp` — single-flight + TTL tests.

**Pillar A1 (GC):**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h` — resident-snap members.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` — `fold` skip-unchanged-persist + resident read-cache; `runRegularRound` resident update; (A1b) decoupled checkpoint + recovery re-fold.
- `src/Disks/tests/gtest_cas_gc_round.cpp` — resident-cache / skip-persist / checkpoint tests.

**zstd:**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp` / `.h` — version bump + codec byte + zstd compress/decompress around the existing binary body.
- `src/Disks/tests/gtest_cas_gc_formats.cpp` — round-trip + size assertion.

**Soak re-validation:**
- `utils/ca-soak/` — rerun the aggressive config, compare S3 counters to the B148 baseline.

---

## TASK GROUP 1 — Pillar B: single-flight coalescing (always on, zero staleness)

`Store::readShardDecoded` (`CasStore.cpp:154`) does `head` → cache-check-by-token → `get`+decode. Under concurrent resolves of the same shard, every thread independently HEADs+GETs. Single-flight coalesces concurrent calls for one key into one HEAD (+ at most one GET); followers wait and receive the leader's result. Zero added staleness (all coalesced callers get the same fresh observation).

### Task 1: Reusable op-counting test backend

**Files:**
- Modify: `src/Disks/tests/cas_test_helpers.h`

- [ ] **Step 1: Add `CountingBackend` to the helper header**

Add this class to `src/Disks/tests/cas_test_helpers.h` (inside the existing `DB::Cas` test namespace, near the other helpers). It subclasses the non-`final` `InMemoryBackend` (`Core/CasInMemoryBackend.h`) and counts `head`/`get`/`putIfAbsent` per key, mirroring the existing `CountingGetBackend` style in `gtest_cas_gc_round.cpp:62`.

```cpp
/// Counts head/get/putIfAbsent per key for op-count assertions (Pillar B / A1 tests).
class CountingBackend : public InMemoryBackend
{
public:
    HeadResult head(const String & key) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++head_counts[key];
            ++head_total;
        }
        return InMemoryBackend::head(key);
    }

    std::optional<GetResult> get(const String & key, Range range = {}) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++get_counts[key];
            ++get_total;
        }
        return InMemoryBackend::get(key, range);
    }

    PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token = nullptr) override
    {
        {
            std::lock_guard lock(count_mutex);
            ++put_counts[key];
            ++put_total;
        }
        return InMemoryBackend::putIfAbsent(key, bytes, out_token);
    }

    uint64_t headCount(const String & key) const { return lookup(head_counts, key); }
    uint64_t getCount(const String & key) const { return lookup(get_counts, key); }
    uint64_t putCount(const String & key) const { return lookup(put_counts, key); }
    uint64_t headTotal() const { std::lock_guard lock(count_mutex); return head_total; }
    uint64_t getTotal() const { std::lock_guard lock(count_mutex); return get_total; }
    uint64_t putTotal() const { std::lock_guard lock(count_mutex); return put_total; }

    void resetCounts()
    {
        std::lock_guard lock(count_mutex);
        head_counts.clear();
        get_counts.clear();
        put_counts.clear();
        head_total = get_total = put_total = 0;
    }

private:
    uint64_t lookup(const std::map<String, uint64_t> & m, const String & key) const
    {
        std::lock_guard lock(count_mutex);
        const auto it = m.find(key);
        return it == m.end() ? 0 : it->second;
    }

    mutable std::mutex count_mutex;
    std::map<String, uint64_t> head_counts;
    std::map<String, uint64_t> get_counts;
    std::map<String, uint64_t> put_counts;
    uint64_t head_total = 0;
    uint64_t get_total = 0;
    uint64_t put_total = 0;
};
```

Ensure the header includes `<map>`, `<mutex>`, `<optional>` and the backend header `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h>` (check the top of the file; add any missing include).

- [ ] **Step 2: Build to verify it compiles**

Run (build dir): `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`
Then dispatch a subagent to summarize `build/build_unit_tests.log`.
Expected: builds clean (the new class is header-only and unused so far).

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/cas_test_helpers.h
git commit -m "CA test: add CountingBackend (per-key head/get/put op-count spy)"
```

### Task 2: Single-flight coalescing in `readShardDecoded`

**Files:**
- Modify: `src/Disks/.../Core/CasStore.h` (add members + private method decls)
- Modify: `src/Disks/.../Core/CasStore.cpp:154-199` (refactor)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

- [ ] **Step 1: Write the failing test (deterministic coalescing via a gated backend)**

Add to `src/Disks/tests/gtest_cas_store.cpp`. A gated backend blocks the leader inside `head` until the test releases it, so followers provably attach to the in-flight entry first. (The CV gate is test scaffolding for determinism, not a production race fix.)

```cpp
namespace
{
/// Blocks the FIRST head() call on a latch so followers attach to the single-flight entry
/// while the leader is still in-flight. Deterministic: no sleeps.
class GatedHeadBackend : public DB::Cas::InMemoryBackend
{
public:
    DB::Cas::HeadResult head(const DB::String & key) override
    {
        {
            std::unique_lock lock(m);
            ++head_calls;
            if (head_calls == 1)
            {
                leader_in_head = true;
                cv.notify_all();
                gate.wait(lock, [this] { return released; });
            }
        }
        return DB::Cas::InMemoryBackend::head(key);
    }

    void waitLeaderInHead()
    {
        std::unique_lock lock(m);
        cv.wait(lock, [this] { return leader_in_head; });
    }
    void release()
    {
        {
            std::lock_guard lock(m);
            released = true;
        }
        gate.notify_all();
    }
    uint64_t headCalls() const { std::lock_guard lock(m); return head_calls; }

private:
    mutable std::mutex m;
    std::condition_variable cv;
    std::condition_variable gate;
    uint64_t head_calls = 0;
    bool leader_in_head = false;
    bool released = false;
};
}

TEST(CasStoreSingleFlight, ConcurrentResolvesCoalesceToOneHead)
{
    using namespace DB::Cas;
    auto b = std::make_shared<GatedHeadBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});

    /// Publish one ref so resolveRef has a present shard to read.
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    constexpr int followers = 8;
    std::vector<std::thread> threads;
    std::vector<std::optional<Resolved>> results(followers + 1);

    /// Leader thread: enters head() and blocks on the gate.
    threads.emplace_back([&] { results[0] = s->resolveRef(ns, "part_1"); });
    b->waitLeaderInHead();

    /// Followers attach to the in-flight entry while the leader is parked in head().
    for (int i = 0; i < followers; ++i)
        threads.emplace_back([&, i] { results[i + 1] = s->resolveRef(ns, "part_1"); });

    /// Give followers a moment to queue, then release the leader.
    b->release();
    for (auto & t : threads)
        t.join();

    /// Exactly one head() call served all 1 + followers resolves.
    EXPECT_EQ(b->headCalls(), 1u);
    for (const auto & r : results)
    {
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->tree_size, results[0]->tree_size);
    }
}
```

Note: the "give followers a moment to queue" is inherent to any coalescing test; to keep it deterministic, the assertion tolerates the worst case — if a follower happens to arrive after `release()` it would just be a new leader. To make it strict, gate followers too: have the test backend's `head` count remain 1 because only the leader reaches `head` (followers never call `head` — they wait on the future). This is structurally guaranteed by single-flight, so `EXPECT_EQ(headCalls, 1)` holds regardless of timing **as long as at least the leader is in-flight when followers call** — which `waitLeaderInHead()` guarantees. Keep the assertion strict.

- [ ] **Step 2: Run the test to verify it fails**

Run: `build/src/unit_tests_dbms --gtest_filter='CasStoreSingleFlight.*' > build/test_singleflight.log 2>&1`
Dispatch a subagent to summarize `build/test_singleflight.log`.
Expected: FAIL — current code makes each of the 9 resolves call `head` independently, so `headCalls()` is 9, not 1 (after the leader is released, followers each run the full path).

- [ ] **Step 3: Add the single-flight members and private method decls to `CasStore.h`**

In `CasStore.h`, add `#include <future>` near the other includes. In the private section, near the decode-cache members (lines 166-168), add:

```cpp
    /// Single-flight coalescing for readShardDecoded: concurrent resolves of the SAME shard key
    /// share ONE head (+ at most one get). The leader publishes its decode to the followers'
    /// shared_future. Zero added staleness — all coalesced callers get the leader's fresh result.
    std::mutex shard_inflight_mutex;
    std::unordered_map<String, std::shared_future<std::shared_ptr<const RootShard>>> shard_inflight;
```

And declare the worker + wrapper (near the existing `readShardDecoded` decl at line 134):

```cpp
    std::shared_ptr<const RootShard> readShardDecoded(const RootNamespace & ns, uint64_t shard);
    /// The actual head+get+decode for one shard key (no coalescing).
    std::shared_ptr<const RootShard> loadShardDecoded(const String & key);
    /// Coalescing wrapper around loadShardDecoded.
    std::shared_ptr<const RootShard> coalescedReadShardDecoded(const String & key);
```

- [ ] **Step 4: Refactor `CasStore.cpp` — extract the worker, add the wrapper**

Replace the body of `readShardDecoded` (lines 154-199) so the existing HEAD+cache+GET+decode logic moves into `loadShardDecoded(const String & key)`, and `readShardDecoded` becomes a thin wrapper that computes the key and delegates to `coalescedReadShardDecoded`:

```cpp
std::shared_ptr<const RootShard> Store::readShardDecoded(const RootNamespace & ns, uint64_t shard)
{
    const String key = pool_layout.rootShardKey(ns, shard);
    return coalescedReadShardDecoded(key);
}

std::shared_ptr<const RootShard> Store::coalescedReadShardDecoded(const String & key)
{
    std::shared_ptr<std::promise<std::shared_ptr<const RootShard>>> promise;
    std::shared_future<std::shared_ptr<const RootShard>> future;
    {
        std::lock_guard lock(shard_inflight_mutex);
        auto it = shard_inflight.find(key);
        if (it != shard_inflight.end())
        {
            future = it->second;   /// follower: wait on the in-flight leader
        }
        else
        {
            promise = std::make_shared<std::promise<std::shared_ptr<const RootShard>>>();
            future = promise->get_future().share();
            shard_inflight.emplace(key, future);
        }
    }

    if (!promise)
        return future.get();   /// follower: leader's result (rethrows the leader's exception)

    /// Leader: do the real work, publish to followers whether it succeeds or throws.
    try
    {
        auto result = loadShardDecoded(key);
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_value(result);
        return result;
    }
    catch (...)
    {
        {
            std::lock_guard lock(shard_inflight_mutex);
            shard_inflight.erase(key);
        }
        promise->set_exception(std::current_exception());
        throw;
    }
}

std::shared_ptr<const RootShard> Store::loadShardDecoded(const String & key)
{
    /// Empty-manifest sentinel for the absent case — shared so callers can treat absent and present
    /// uniformly (no refs). Never mutated.
    static const std::shared_ptr<const RootShard> empty_shard = std::make_shared<const RootShard>();

    /// A `head` gets the current token without transferring/decoding the manifest body.
    const HeadResult h = pool_backend->head(key);
    if (!h.exists)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }

    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end() && it->second.first == h.token)
            return it->second.second;
    }

    std::optional<GetResult> object = pool_backend->get(key);
    if (!object)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        shard_decode_cache.erase(key);
        return empty_shard;
    }
    auto decoded = std::make_shared<const RootShard>(decodeRootShard(object->bytes));
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        if (shard_decode_cache.size() >= SHARD_DECODE_CACHE_MAX_ENTRIES)
            shard_decode_cache.clear();
        shard_decode_cache[key] = {object->token, decoded};
    }
    return decoded;
}
```

(The `loadShardDecoded` body is the original `readShardDecoded` body verbatim, with `const String key = pool_layout.rootShardKey(...)` removed since `key` is now the parameter.)

- [ ] **Step 5: Build**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Expected: clean build.

- [ ] **Step 6: Run the test to verify it passes**

Run: `build/src/unit_tests_dbms --gtest_filter='CasStoreSingleFlight.*:CasStore.*:CaWiring.*' > build/test_singleflight.log 2>&1`; subagent summarizes.
Expected: PASS — `headCalls() == 1`; all existing `CasStore`/`CaWiring` tests still green.

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA Pillar B: single-flight coalescing for readShardDecoded (one HEAD per concurrent resolve burst)"
```

---

## TASK GROUP 2 — Pillar B: opt-in bounded-TTL decode cache

A warm decode-cache hit still costs a HEAD (to fetch the current token). For **staleness-tolerant** callers, skip the HEAD when the entry was validated < TTL ago. **Opt-in per caller** (default force-fresh); strict-freshness callers keep the HEAD. Single-flight (Group 1) stays always on.

### Task 3: TTL cache entry + `allow_stale` plumbing + config

**Files:**
- Modify: `src/Disks/.../Core/CasStore.h` (`PoolConfig`, cache entry struct, signatures)
- Modify: `src/Disks/.../Core/CasStore.cpp` (TTL fast-path, validated_at stamping)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

- [ ] **Step 1: Write the failing test (TTL hit skips HEAD; force-fresh always HEADs)**

Add to `gtest_cas_store.cpp`, using the `CountingBackend` from Task 1 (`#include "cas_test_helpers.h"` if not already):

```cpp
TEST(CasStoreDecodeTtl, WarmHitWithinTtlSkipsHead)
{
    using namespace DB::Cas;
    auto b = std::make_shared<CountingBackend>();
    /// Large TTL so the second resolve is always within window.
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .shard_decode_cache_ttl_ms = 60000});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    /// First resolve (allow_stale): primes the cache, one HEAD + one GET.
    ASSERT_TRUE(s->resolveRef(ns, "part_1", /*allow_stale=*/true).has_value());
    const String shard_key = s->layout().rootShardKey(ns, /*shard=*/0);  // shardOf("part_1")
    b->resetCounts();

    /// Second resolve (allow_stale): warm hit within TTL — NO head, NO get.
    ASSERT_TRUE(s->resolveRef(ns, "part_1", /*allow_stale=*/true).has_value());
    EXPECT_EQ(b->headTotal(), 0u);
    EXPECT_EQ(b->getTotal(), 0u);
}

TEST(CasStoreDecodeTtl, ForceFreshAlwaysHeads)
{
    using namespace DB::Cas;
    auto b = std::make_shared<CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p", .shard_decode_cache_ttl_ms = 60000});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    const RootNamespace ns{"srv1/tbl"};
    ASSERT_TRUE(s->resolveRef(ns, "part_1", /*allow_stale=*/true).has_value());  // prime
    b->resetCounts();

    /// Default (force-fresh) caller: must HEAD even on a warm entry (today's semantics).
    ASSERT_TRUE(s->resolveRef(ns, "part_1").has_value());  // allow_stale defaults false
    EXPECT_GE(b->headTotal(), 1u);
}
```

Note: use the real shard for `"part_1"` — `shardOf` is internal; the key assertion is on `head`/`get` totals, not a specific key, so the `shard_key` local can be dropped if `shardOf` is not test-visible. Keep totals-based assertions.

- [ ] **Step 2: Run to verify failure**

Run: `build/src/unit_tests_dbms --gtest_filter='CasStoreDecodeTtl.*' > build/test_ttl.log 2>&1`; subagent summarizes.
Expected: FAIL to compile — `PoolConfig` has no `shard_decode_cache_ttl_ms`, `resolveRef` has no `allow_stale` param.

- [ ] **Step 3: Add config + TTL cache-entry struct to `CasStore.h`**

In `PoolConfig` (the struct with `pool_prefix`, `root_shards`, `blob_header_len`, `background_heartbeats`, `read_only`), add:

```cpp
    /// Pillar B bounded-TTL decode cache: a staleness-tolerant caller (allow_stale=true) may reuse a
    /// decode validated < this many ms ago WITHOUT a HEAD. 0 disables the TTL (all callers force-fresh).
    /// Strict-freshness callers always pass allow_stale=false and always HEAD, regardless of this value.
    uint64_t shard_decode_cache_ttl_ms = 200;
```

Replace the decode-cache value type (line 168) to carry a validation timestamp:

```cpp
    struct ShardDecodeCacheEntry
    {
        Token token;
        std::shared_ptr<const RootShard> shard;
        std::chrono::steady_clock::time_point validated_at;
    };
    std::unordered_map<String, ShardDecodeCacheEntry> shard_decode_cache;
```

Add `#include <chrono>` if absent. Update `readShardDecoded`/`loadShardDecoded`/`coalescedReadShardDecoded` decls to thread `allow_stale`:

```cpp
    std::shared_ptr<const RootShard> readShardDecoded(const RootNamespace & ns, uint64_t shard, bool allow_stale = false);
    std::shared_ptr<const RootShard> loadShardDecoded(const String & key);
    std::shared_ptr<const RootShard> coalescedReadShardDecoded(const String & key);
```

And the public `resolveRef` decl (line 79):

```cpp
    std::optional<Resolved> resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale = false);
```

- [ ] **Step 4: Implement the TTL fast-path in `CasStore.cpp`**

Update `readShardDecoded` to check the TTL fast-path BEFORE entering single-flight, and update the three cache call sites to the new struct + stamp `validated_at`.

```cpp
std::shared_ptr<const RootShard> Store::readShardDecoded(const RootNamespace & ns, uint64_t shard, bool allow_stale)
{
    const String key = pool_layout.rootShardKey(ns, shard);

    /// Pillar B TTL fast-path: a staleness-tolerant caller may reuse a recently-validated decode
    /// WITHOUT a HEAD. Only for PRESENT entries (absence is never TTL-cached — a just-created ref
    /// must be observable by force-fresh callers; staleness-tolerant callers re-validate on miss).
    if (allow_stale && config.shard_decode_cache_ttl_ms > 0)
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end())
        {
            const auto age = std::chrono::steady_clock::now() - it->second.validated_at;
            const auto ttl = std::chrono::milliseconds(config.shard_decode_cache_ttl_ms);
            if (age < ttl)
                return it->second.shard;
        }
    }

    return coalescedReadShardDecoded(key);
}
```

In `loadShardDecoded`, update the cache reads/writes for the new struct and stamp `validated_at` on EVERY successful HEAD-validation (both the warm-token-match return and the fresh-decode insert):

```cpp
    // ... after `const HeadResult h = pool_backend->head(key);` and the !h.exists branch ...
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        auto it = shard_decode_cache.find(key);
        if (it != shard_decode_cache.end() && it->second.token == h.token)
        {
            it->second.validated_at = std::chrono::steady_clock::now();   /// HEAD re-validated the entry
            return it->second.shard;
        }
    }
    // ... after decode ...
    {
        std::lock_guard lock(shard_decode_cache_mutex);
        if (shard_decode_cache.size() >= SHARD_DECODE_CACHE_MAX_ENTRIES)
            shard_decode_cache.clear();
        shard_decode_cache[key] = ShardDecodeCacheEntry{
            .token = object->token,
            .shard = decoded,
            .validated_at = std::chrono::steady_clock::now()};
    }
```

Update `resolveRef` (line 201) to thread `allow_stale`:

```cpp
std::optional<Resolved> Store::resolveRef(const RootNamespace & ns, const String & ref_name, bool allow_stale)
{
    const auto root = readShardDecoded(ns, shardOf(ref_name), allow_stale);
    // ... unchanged ...
}
```

Also update `listRefs` (line 294) to pass `allow_stale=true` (listing tolerates a point-in-time snapshot):

```cpp
        const auto root = readShardDecoded(ns, shard, /*allow_stale=*/true);
```

- [ ] **Step 5: Build, then run the TTL + regression tests**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Run: `build/src/unit_tests_dbms --gtest_filter='CasStoreDecodeTtl.*:CasStoreSingleFlight.*:CasStore.*:CaWiring.*' > build/test_ttl.log 2>&1`; subagent summarizes.
Expected: PASS — warm hit does 0 HEAD/0 GET; force-fresh does ≥1 HEAD; all existing tests green.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA Pillar B: opt-in bounded-TTL decode cache (warm hit skips HEAD; force-fresh default)"
```

### Task 4: Opt the audited hot-path callers into `allow_stale`

**Files:**
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.cpp`

The caller audit (from the spec §3 / B149). **Opt IN (staleness-tolerant):** `resolveRouted` (hot read path: part reads, system.parts), `existsDirectory` shadow + part-dir branches, `getLastModified`, `getPartTreeId`, `dropRefIfPresent`. **KEEP force-fresh (do NOT touch):** `existsFile` mutable-file branch, `tryGetInManifestBytes` mutable branch (MVCC `txn_version`), `commit` rollback tracking, `republishRef`, `createHardLink`.

- [ ] **Step 1: Write the failing test (a hot-path read does not HEAD twice in a row)**

Add to `gtest_cas_store.cpp` (or `gtest_ca_wiring.cpp` if a `ContentAddressedMetadataStorage` fixture is needed). This asserts the metadata-storage hot read path benefits from the TTL. If wiring a full `ContentAddressedMetadataStorage` is heavy, assert at the `Store` level that `resolveRef(..., allow_stale=true)` twice does one HEAD — already covered by Task 3 — and instead verify the call sites by code review + the existing `CaWiring` tests staying green. Mark this step as a code-review checkpoint:

> Code-review checkpoint: confirm each opted-in call site below passes `allow_stale=true` and each force-fresh site is unchanged. No new test if `ContentAddressedMetadataStorage` is not unit-constructable; rely on `CaWiring.*` regression + the soak.

- [ ] **Step 2: Edit the opted-in call sites**

In `ContentAddressedMetadataStorage.cpp`:
- `resolveRouted` (~line 314): `auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);`
- `existsDirectory` shadow branch (~line 361): `return store()->resolveRef(shadowNamespace(p->shadow_table_dir), p->part_name, /*allow_stale=*/true).has_value();`
- `existsDirectory` part-dir branch (~line 387): `return store()->resolveRef(r->ns, r->ref, /*allow_stale=*/true).has_value();`
- `getLastModified` resolve_stamp (~line 457): `auto resolved = store()->resolveRef(r.ns, r.ref, /*allow_stale=*/true);`
- `getPartTreeId` (~line 771): `auto resolved = store()->resolveRef(r->ns, r->ref, /*allow_stale=*/true);`

In `ContentAddressedTransaction.cpp`:
- `dropRefIfPresent` (~line 101): `if (!metadata_storage.store()->resolveRef(ns, ref, /*allow_stale=*/true))` (the comment already notes `dropRef` re-reads inside its own CAS loop).

**Leave unchanged (force-fresh):** `existsFile` mutable branch (~341), `tryGetInManifestBytes` mutable (~679), `commit` (~184), `republishRef` (~81), `createHardLink` (~549, ~581). Add a one-line comment at each force-fresh site, e.g.:
```cpp
        /// Force-fresh (Pillar B): MVCC txn_version read — must not serve a TTL-stale manifest.
```

- [ ] **Step 3: Build + regression**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring.*' > build/test_pillarb.log 2>&1`; subagent summarizes.
Expected: clean build; all CA tests green.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp
git commit -m "CA Pillar B: opt audited hot-path resolveRef callers into bounded-TTL; keep MVCC/publish/rename force-fresh"
```

---

## TASK GROUP 3 — Pillar A1-min: skip-unchanged snap persist + resident read-cache

`fold` (`CasGc.cpp:928`) **unconditionally** writes a new snap generation and advances `snap_generation` **every round** (lines 986-1029), even with zero churn — so it re-`loadSnap`s (GET) and re-`putIfAbsent`s the whole snap each round. Two contained changes: (1) when no journal records were folded, skip the snap re-write and the gc/state CAS (thread the incoming token through); (2) keep the decoded snap resident across rounds and skip `loadSnap` when the durable generation is unchanged (generation is write-once, so a generation match ⇒ identical snap — self-validating, no fragile invalidation).

### Task 5: `fold` skips persist when nothing was folded

**Files:**
- Modify: `src/Disks/.../Core/CasGc.cpp` (`fold`, lines 952-1030)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

- [ ] **Step 1: Write the failing test (a no-churn round writes no new snap)**

Add to `gtest_cas_gc_round.cpp` (uses `CountingBackend` from `cas_test_helpers.h`):

```cpp
TEST(CasGcFold, NoChurnRoundWritesNoNewSnap)
{
    using namespace DB::Cas;
    auto b = std::make_shared<CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    /// Round 1 folds the publish — snap generation advances (a snap PUT happens).
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st1 = readState(*b, *s);

    b->resetCounts();
    /// Round 2: no new journal records since round 1 — must NOT write a new snap generation.
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);
    const GcState st2 = readState(*b, *s);

    EXPECT_EQ(st2.snap_generation, st1.snap_generation);              /// generation unchanged
    EXPECT_EQ(b->putCount(s->layout().gcSnapKey(st1.snap_generation + 1, 0)), 0u);  /// no probe write
}
```

- [ ] **Step 2: Run to verify failure**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFold.NoChurnRoundWritesNoNewSnap' > build/test_fold_skip.log 2>&1`; subagent summarizes.
Expected: FAIL — current `fold` advances `snap_generation` every round and writes `gcSnapKey(gen+1, 0)`.

- [ ] **Step 3: Implement skip-unchanged in `fold`**

In `fold`, track whether any records were folded, and short-circuit before the probe-write loop. Modify the per-shard loop (lines 952-966) to set a flag, and add the early return before line 968's persist block:

```cpp
    bool folded_any = false;
    for (const auto & [ns, root_shard] : result.root_shards)
    {
        const auto [root, manifest_token] = store->readShard(ns, root_shard);
        const String cursor_key = ns.string() + "/" + std::to_string(root_shard);
        const auto cursor_it = state.folded_cursor.find(cursor_key);
        const uint64_t cursor = cursor_it != state.folded_cursor.end() ? cursor_it->second : 0;

        if (root.shard_version > cursor)
            folded_any = true;   /// new journal records exist in (cursor, shard_version]

        auto transitioned = foldShardRecords(result.snap, state, cursor_key, root, cursor, root.shard_version);
        result.transitioned.insert(result.transitioned.end(), transitioned.begin(), transitioned.end());

        if (root.shard_version > 0 || cursor > 0)
            state.folded_cursor[cursor_key] = root.shard_version;
    }

    /// No new records since the last fold: the snap is unchanged and already durable at
    /// state.snap_generation. Skip the whole-snap re-write AND the gc/state CAS — thread the incoming
    /// token (from acquireOrRenewLease / a prior round) through to retire unchanged. This is safe:
    /// retire's round CAS rides exactly this token, so any intervening lease steal Conflicts there
    /// (the same zombie-window protection runRegularRound documents for the post-fold path); and the
    /// folded_cursor was not advanced past any unpersisted records (none exist). state/state_token
    /// are returned unmodified.
    if (!folded_any)
        return result;
```

The existing probe-write loop + gc/state CAS (lines 984-1030) stays as the churned-round path.

- [ ] **Step 4: Build + run**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Run: `build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_fold_skip.log 2>&1`; subagent summarizes.
Expected: PASS — `NoChurnRoundWritesNoNewSnap` green; the full `CasGc*` battery (lease/fold/retire/fence/recheck/cascade/leak/truncate) stays green.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA Pillar A1: fold skips snap re-write + gc/state CAS when no records folded (idle rounds = zero snap PUT)"
```

### Task 6: Resident snap read-cache (skip `loadSnap` on generation match)

**Files:**
- Modify: `src/Disks/.../Core/CasGc.h` (resident members)
- Modify: `src/Disks/.../Core/CasGc.cpp` (`fold` reuse, `runRegularRound` update)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

- [ ] **Step 1: Write the failing test (a no-churn round does not GET the snap)**

Add to `gtest_cas_gc_round.cpp`:

```cpp
TEST(CasGcFold, NoChurnRoundReusesResidentSnapNoGet)
{
    using namespace DB::Cas;
    auto b = std::make_shared<CountingBackend>();
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p"});
    publishPart(s, "srv1/tbl", "part_1", "payload-1");

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 1: folds, snap now resident
    const GcState st1 = readState(*b, *s);

    b->resetCounts();
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// round 2: no churn — reuse resident snap
    /// loadSnap GET of the authoritative generation must NOT happen (served from memory).
    EXPECT_EQ(b->getCount(s->layout().gcSnapKey(st1.snap_generation, 0)), 0u);
}
```

- [ ] **Step 2: Run to verify failure**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcFold.NoChurnRoundReusesResidentSnapNoGet' > build/test_resident.log 2>&1`; subagent summarizes.
Expected: FAIL — `fold` calls `loadSnap(state)` every round, so the snap key is GET once in round 2.

- [ ] **Step 3: Add resident members to `CasGc.h`**

In the `Gc` private section (near the steal-observation members `has_observation` / `last_seen_owner` / `last_seen_seq`, lines 272-276), add:

```cpp
    /// Pillar A1 resident-snap read-cache. The Gc instance is long-lived (one per scheduler thread,
    /// CasGcScheduler::loop), so this survives across rounds. A snap generation is write-once
    /// (putIfAbsent + byte-equal adoption), so a matching generation guarantees identical bytes —
    /// reusing the resident copy when resident_generation == state.snap_generation needs NO HEAD/GET
    /// and is self-validating: if another leader advanced the generation while we did not hold the
    /// lease, the next state read shows a different snap_generation => mismatch => reload.
    std::optional<std::map<uint64_t, GcSnap>> resident_snap;
    uint64_t resident_generation = 0;
```

Add `#include <optional>` if absent.

- [ ] **Step 4: Use + refresh the resident snap in `CasGc.cpp`**

In `fold`, replace `result.snap = loadSnap(state);` (line 948) with:

```cpp
    /// Reuse the resident snap when the durable generation is unchanged (write-once ⇒ identical);
    /// otherwise load it. Avoids the per-round whole-snap GET on idle rounds.
    if (resident_snap && resident_generation == state.snap_generation)
        result.snap = *resident_snap;
    else
        result.snap = loadSnap(state);
```

In `runRegularRound`, after `cascadeAndPersist(...)` (line 94) and before `trim(...)` (or after `trim`), refresh the resident snap to the final post-round content + generation:

```cpp
    /// Pillar A1: keep the final post-round snap resident for the next round's fold.
    resident_snap = folded.snap;
    resident_generation = state.snap_generation;
```

(Place it after `trim(state, folded.root_shards);` since `trim` only reads `folded.root_shards`, not the snap. `folded.snap` holds the final content after `recheck`/`cascadeAndPersist` mutated it in place; `state.snap_generation` holds the final durable generation.)

No explicit invalidation is needed on the early-return / exception paths: the `resident_generation == state.snap_generation` gate is authoritative (a generation is immutable once written). On `!acquired_lease` we return before touching `fold`, leaving the stale resident in place; it will be re-validated by the generation check next time we hold the lease.

- [ ] **Step 5: Build + run the full GC battery**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Run: `build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_resident.log 2>&1`; subagent summarizes.
Expected: PASS — `NoChurnRoundReusesResidentSnapNoGet` green; full `CasGc*` battery green (multi-round lease/steal/fence/recheck/cascade/leak/truncate tests unaffected — the resident cache is transparent to them).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA Pillar A1: resident snap read-cache — skip loadSnap GET when durable generation unchanged"
```

---

## DECISION GATE (after Groups 1–3 + Group 5 zstd, before Group 4)

Before implementing the protocol-intricate decoupled checkpoint (A1b, Task Group 4), **re-run the soak (Task Group 6) with Groups 1–3 + zstd only** and compare to the B148 baseline. A1-min already removes the *idle-round* snap GET+PUT and Pillar B removes the `resolveRef` HEAD storm — if the soak shows `system.parts` bounded and the S3 HEAD-rate / read-error-rate dropped sharply, **A1b may be deferred** (record the deferral in the backlog). Proceed to A1b only if the *per-churned-round whole-snap PUT* (O(pool) writes) remains a measured bottleneck. This gate honors the risk-control stance (A1b changes the durable cursor/recovery protocol and needs the §6a scenario-battery re-confirmation + adversarial review).

---

## TASK GROUP 4 — Pillar A1b (GATED): decoupled periodic checkpoint + recovery re-fold

**Only if the decision gate says the per-churned-round whole-snap PUT is still a bottleneck.** Persist the whole snap only every K folded records or M rounds; between checkpoints, fold the delta into the resident snap in memory and keep the durable `folded_cursor` at the last checkpoint. A new/recovering leader loads the checkpoint snap and re-folds the journal delta from `folded_cursor` to now (bounded by K).

### Task 7: Decoupled checkpoint state + policy

**Files:**
- Modify: `src/Disks/.../Core/CasGc.h` (resident cursor + checkpoint counters)
- Modify: `src/Disks/.../Core/CasGc.cpp` (`fold` checkpoint policy; recovery re-fold)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

- [ ] **Step 1: Write the failing test (churned rounds below K do not write a new snap; checkpoint at K does)**

```cpp
TEST(CasGcCheckpoint, ChurnedRoundsBelowKDoNotPersistSnap)
{
    using namespace DB::Cas;
    auto b = std::make_shared<CountingBackend>();
    /// Checkpoint every 1000 records / 1000 rounds: small churn stays below the threshold.
    auto s = Store::open(b, PoolConfig{.pool_prefix = "p",
        .gc_checkpoint_records = 1000, .gc_checkpoint_rounds = 1000});

    Gc gc(s, hexToU128("00000000000000000000000000000001"));
    publishPart(s, "srv1/tbl", "part_1", "payload-1");
    EXPECT_TRUE(gc.runRegularRound().acquired_lease);   /// folds part_1, but below K => no checkpoint
    const GcState st1 = readState(*b, *s);

    /// The durable snap generation did NOT advance for a sub-threshold churned round.
    EXPECT_EQ(st1.snap_generation, 0u);
    /// But a recovering leader can reconstruct: load checkpoint (gen 0, empty) + re-fold from cursor.
    Gc fresh(s, hexToU128("00000000000000000000000000000002"));
    /// (Assert via previewDeletes or a follow-up round that reachability is correct — see Step 4.)
}
```

- [ ] **Step 2: Run to verify failure** (compile failure: no `gc_checkpoint_records`/`gc_checkpoint_rounds`).

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcCheckpoint.*' > build/test_ckpt.log 2>&1`; subagent summarizes.

- [ ] **Step 3: Add checkpoint config + resident cursor + counters**

`PoolConfig`: `uint64_t gc_checkpoint_records = 4096;` and `uint64_t gc_checkpoint_rounds = 64;` (K caps recovery re-fold; M caps staleness). `Gc` private: `std::map<String, uint64_t> resident_cursor;` (the in-memory folded cursor, ahead of durable `folded_cursor`), `uint64_t records_since_checkpoint = 0;`, `uint64_t rounds_since_checkpoint = 0;`.

- [ ] **Step 4: Implement the checkpoint policy in `fold`**

Replace the "always persist on churn" path with: fold the delta into the resident snap and advance `resident_cursor` in memory every round; persist (probe-write the whole snap at a new generation + advance durable `snap_generation` AND `folded_cursor` via the gc/state CAS) ONLY when `records_since_checkpoint >= gc_checkpoint_records || rounds_since_checkpoint >= gc_checkpoint_rounds`. On a non-checkpoint churned round, the gc/state CAS still advances `round`/`fence` (via retire/fence) but **must not** advance `folded_cursor` past the unpersisted records. Recovery (`acquireOrRenewLease` path / fresh `Gc`): after loading durable state, load the checkpoint snap (`loadSnap(state)`), then re-fold every shard's journal records in `(folded_cursor, shard_version]` into the in-memory snap before the round proceeds — exactly the existing `foldShardRecords` over the delta.

> **Protocol obligations (adversarial-review gate, spec §5/§6a):**
> - Durable `folded_cursor` must equal the last-checkpoint cursor at all times (recovery re-folds from it). Records between the checkpoint cursor and live `shard_version` MUST remain in the journal — i.e. `trim` may only remove records at or below the **durable** `folded_cursor`, never the resident cursor. Verify `trim` (lines 103-138) already keys on `state.folded_cursor` (durable) — it does; ensure A1b does not advance `state.folded_cursor` except at checkpoints.
> - Re-fold from `folded_cursor` must be deterministic + idempotent (set semantics) ⇒ byte-identical resident snap. Add a test: build resident snap via N incremental folds, then a fresh `Gc` re-folds from the checkpoint cursor ⇒ `encodeGcSnap` byte-identical.
> - `retire` derives candidates from the (in-memory, re-folded) snap; a crash-replayed round must re-derive the SAME set. The existing crash-resume test family must stay green.
> - Resident snap dropped on every leadership/generation discontinuity (lease loss / steal / `gc/state` CAS abort): on those, reset `resident_snap`, `resident_cursor`, and the counters so the next round reloads + re-folds from durable.

- [ ] **Step 5: Tests — byte-identical re-fold + crash-replay identical retire + green battery**

Add `CasGcCheckpoint.RecoveryRefoldIsByteIdentical` (incremental vs from-checkpoint re-fold ⇒ identical `encodeGcSnap`), `CasGcCheckpoint.CrashReplayIdenticalRetireDecisions` (kill+reload mid-round ⇒ same deletes), and confirm `CasGc*`, `CasGcLeak.*`, `CasTruncateReclaim.*` stay green. Run via `--gtest_filter='CasGc*:CasTruncate*'`, log + subagent summary.

- [ ] **Step 6: Adversarial review (subagent) + commit**

Dispatch the `ubrella-clickhose-review` skill (or a deep-audit subagent) on the A1b diff against INV-NO-LOSS / INV-NO-RETURN / INV-JOURNAL-COVERAGE before committing. Then:

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "CA Pillar A1b: decoupled periodic snap checkpoint + recovery re-fold (per-churn snap PUT ∝ K)"
```

---

## TASK GROUP 5 — zstd-compress the `gc/snap` blob

On top of the binary codec (B147 part-1, `GC_SNAP_VERSION = 2`), zstd-compress the encoded snap. Bump to version 3 with a codec byte (0 = raw, 1 = zstd). Old readers reject v3 with `NOT_IMPLEMENTED` (safe; pre-release, no on-disk compat needed).

### Task 8: zstd codec around the binary snap body

**Files:**
- Modify: `src/Disks/.../Core/CasGcSnap.cpp` (`encodeGcSnap`/`decodeGcSnap`, magic/version at lines 31-32, 242-369)
- Test: `src/Disks/tests/gtest_cas_gc_formats.cpp`

- [ ] **Step 1: Write the failing test (round-trip + size shrink)**

Add to `gtest_cas_gc_formats.cpp`:

```cpp
TEST(CasGcSnapCodec, ZstdRoundTripAndShrinks)
{
    using namespace DB::Cas;
    GcSnap snap;
    snap.snap_shard = 0;
    snap.generation = 7;
    /// Populate with many repetitive edges so compression has something to do.
    for (uint64_t i = 0; i < 2000; ++i)
    {
        EdgeRec rec;
        rec.edge_kind = EdgeKind::Root;
        rec.target_kind = ObjectKind::Tree;
        rec.target_hash = (static_cast<UInt128>(1) << 64) | i;
        rec.root_shard = "srv1/tbl/0";
        rec.part_name = "part_" + std::to_string(i);
        snap.addEdge(rec);   /// (use the snap's public edge-insertion API)
    }

    const String encoded = encodeGcSnap(snap);          /// now zstd (version 3)
    const GcSnap decoded = decodeGcSnap(encoded);        /// round-trips
    EXPECT_EQ(encodeGcSnap(decoded), encoded);           /// stable / canonical

    /// The compressed form is smaller than the raw binary body for repetitive data.
    /// (Compare against a raw encode behind a test-only flag, or assert encoded.size() is below a
    /// conservative bound derived from the uncompressed footprint.)
    EXPECT_LT(encoded.size(), 2000u * 40u);
}
```

(If `addEdge`/`EdgeRec` are not public, build the snap via the existing test helper used in `gtest_cas_gc_snap.cpp` — match that file's construction style.)

- [ ] **Step 2: Run to verify failure**

Run: `build/src/unit_tests_dbms --gtest_filter='CasGcSnapCodec.ZstdRoundTripAndShrinks' > build/test_zstd.log 2>&1`; subagent summarizes.
Expected: FAIL — current encode is raw binary (v2); size bound and/or version check fails.

- [ ] **Step 3: Implement zstd in the codec**

In `CasGcSnap.cpp`: bump `GC_SNAP_VERSION` to `3`; add `constexpr uint8_t GC_SNAP_CODEC_RAW = 0;` and `constexpr uint8_t GC_SNAP_CODEC_ZSTD = 1;`. In `encodeGcSnap`: build the existing binary body into a local `String body` (the current logic), then write the 6-byte header `magic(4) + version(1=3) + codec(1)` and append zstd-compressed `body`:

```cpp
#include <IO/WriteBufferFromString.h>
#include <IO/ReadBufferFromString.h>
#include <IO/ZstdDeflatingWriteBuffer.h>
#include <IO/ZstdInflatingReadBuffer.h>
#include <IO/ReadHelpers.h>   // readStringUntilEOF
#include <zstd.h>

// in encodeGcSnap, after producing the raw binary `body` (the current v2 payload sans header):
String compressed;
{
    auto sink = std::make_unique<WriteBufferFromString>(compressed);
    ZstdDeflatingWriteBuffer zstd(std::move(sink), /*compression_level=*/ZSTD_defaultCLevel());
    zstd.write(body.data(), body.size());
    zstd.finalize();
}
String out;
out.reserve(6 + compressed.size());
out.append(GC_SNAP_MAGIC, 4);
out.push_back(static_cast<char>(GC_SNAP_VERSION));     // 3
out.push_back(static_cast<char>(GC_SNAP_CODEC_ZSTD));  // 1
out.append(compressed);
return out;
```

In `decodeGcSnap`: read magic (must be `CAGS`), read version (`> GC_SNAP_VERSION` ⇒ `NOT_IMPLEMENTED`; `< 3` ⇒ `CORRUPTED_DATA` since pre-release), read codec byte; if `GC_SNAP_CODEC_ZSTD`, decompress the remainder into `body`, else `body` = remainder; then parse `body` with the existing field-reading logic:

```cpp
auto in = std::make_unique<ReadBufferFromString>(remainder_view);
ZstdInflatingReadBuffer zstd(std::move(in));
String body;
readStringUntilEOF(body, zstd);
// ... parse `body` exactly as the v2 logic parsed the post-header bytes ...
```

Refactor so the field-encode and field-parse are pulled into helpers operating on a `String`/`std::string_view` body, keeping the wire layout identical to v2 (only the framing + compression change).

- [ ] **Step 4: Build + run codec + GC battery**

Run: `cd build && ninja unit_tests_dbms > build_unit_tests.log 2>&1`; subagent summarizes.
Run: `build/src/unit_tests_dbms --gtest_filter='CasGcSnapCodec.*:CasGcFormats.*:CasGc*' > build/test_zstd.log 2>&1`; subagent summarizes.
Expected: PASS — round-trip + shrink; the GC round/leak/truncate battery (which persists + reloads snaps) stays green end-to-end through the new codec.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcSnap.h \
        src/Disks/tests/gtest_cas_gc_formats.cpp
git commit -m "CA: zstd-compress gc/snap blob (version 3 + codec byte; old readers reject safely)"
```

---

## TASK GROUP 6 — Soak re-validation

Rebuild the server and re-run the aggressive soak; compare S3 counters to the B148 baseline (16M HEADs, 66% read-error). Run this twice: once after Groups 1–3 + zstd (the decision gate), and once after A1b if it ships.

### Task 9: Rebuild server + run the aggressive soak + measure

**Files:**
- Use: `utils/ca-soak/`

- [ ] **Step 1: Build the server binary**

Run: `cd build && ninja clickhouse > build_clickhouse.log 2>&1`; subagent summarizes `build/build_clickhouse.log`.
Expected: clean build of `build/programs/clickhouse`.

- [ ] **Step 2: Run the aggressive soak (same config as the B148 baseline)**

From `utils/ca-soak/`, bring up the compose stack and run the phase-3 aggressive config (6 workers / 25 GB). Use the same seed for comparability:

```bash
cd utils/ca-soak
docker compose up -d
python3 -m soak.run --seed 20260614 --phase 3 --duration 7200 --workers 6 --max-pool-gb 25 \
    --metrics logs/revalidate_$(date +%Y%m%dT%H%M%S).db
```

(Use a bounded 2h duration for the re-validation, not the full 24h. Adjust to the harness's actual launcher entrypoint if it differs.)

- [ ] **Step 3: Measure the S3 counters and compare to baseline**

While running (and at the end), capture on both replicas:
```sql
SELECT event, value FROM system.events
WHERE event IN ('S3HeadObject','S3ReadRequestsErrors','S3GetObject','S3PutObject',
                'DiskS3ReadRequestsCount','S3ReadRequestsThrottling','S3WriteRequestsThrottling')
ORDER BY event;
SELECT name, value FROM system.errors WHERE value > 0 ORDER BY value DESC;
SELECT count() FROM system.parts WHERE active;   -- must stay bounded (responsiveness)
```
Record the deltas vs the B148 baseline: HEAD-rate, `S3ReadRequestsErrors` rate, whether `system.parts` stays responsive, and the GC snap PUT/GET counts (expect idle-round snap I/O ≈ 0).

- [ ] **Step 4: Record results in the backlog + progress report**

Append the measured op-count deltas (HEAD/s, read-error %, snap I/O per round, `system.parts` latency) to `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` (extend B149) and the progress report `docs/superpowers/reports/2026-06-13-unattended-progress.md`. State explicitly whether the decision gate defers A1b. Commit:

```bash
git add docs/superpowers/deferred_backlog/cas-mergetree-integration.md \
        docs/superpowers/reports/2026-06-13-unattended-progress.md
git commit -m "CA soak re-validation: op-count deltas vs B148 baseline; A1b gate decision"
```

- [ ] **Step 5: Tear down the soak**

```bash
cd utils/ca-soak && docker compose down -v
```

---

## Self-Review

**Spec coverage:**
- Pillar B single-flight (§3) → Task 2. ✓
- Pillar B bounded-TTL opt-in + fail-safe default + caller audit (§3, §8) → Tasks 3-4. ✓
- Pillar A1 resident snap + no per-round snap I/O on idle (§2.1-2.2) → Tasks 5-6. ✓
- Pillar A1 decoupled periodic checkpoint + recovery re-fold (§2.2-2.3) → Task Group 4 (gated). ✓
- retire-without-HEAD (§2.4) → **deferred** (recorded in spec §2 + §7). ✓ (intentional gap)
- zstd snap (§4) → Task 8. ✓
- Correctness obligations / adversarial review (§5, §6a) → Task 6 protocol-obligations block + Task Group 4 review gate. ✓
- Soak re-validation (§6) → Task Group 6. ✓
- Protocol/model retesting = scenario gtests (§6a) → byte-identical re-fold + crash-replay tests in Task Group 4. ✓

**Placeholder scan:** Task 4 Step 1 is a code-review checkpoint (not a code step) because `ContentAddressedMetadataStorage` may not be unit-constructable in isolation — this is called out explicitly, not a hidden TODO. Task Group 4 carries a decision gate by design. No "TBD"/"implement later" left.

**Type consistency:** `ShardDecodeCacheEntry{token, shard, validated_at}` is introduced in Task 3 and used consistently. `CountingBackend` (Task 1) is reused in Tasks 3, 5, 6. `resident_snap`/`resident_generation` (Task 6) match the `loadSnap`/generation types (`std::map<uint64_t, GcSnap>`, `uint64_t`). `allow_stale` default-`false` threads through `readShardDecoded`/`resolveRef` consistently. `GC_SNAP_VERSION = 3` + codec byte consistent across encode/decode (Task 8).

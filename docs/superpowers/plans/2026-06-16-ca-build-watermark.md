# CA per-server build watermark (B167) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the reverted per-build heartbeat existence guard with a per-server build *watermark*: the incremental GC refuses to condemn an `everEdged ∧ InDeg=0` blob whose owning build is still in-flight, judged by a per-server `min_active` floor read from S3 user-metadata — so a dedup-resurrected in-flight blob survives to publish (kills the B167 livelock) without any per-build object to leak.

**Architecture:** Each object carries an owner triple `(server_id, epoch, build_seq)` in S3 user-metadata. Each server publishes one `servers/<server_id>` watermark `{epoch, min_active, seq}`, renewed async ~2 s and anchored synchronously before the first PUT, with a farewell (`min_active = UINT64_MAX`) on graceful shutdown. GC reads the candidate's owner from the HEAD it already does, reads the owning server's watermark (cached per round), and protects iff `serverLive ∧ epoch == watermark.epoch ∧ build_seq ≥ min_active`. Crashed servers are detected by a frozen `seq` across K=2 GC passes (the B160 mechanism). The ETag stays the delete token; re-stamp stays a body-in-hand re-PUT (Part A, already committed).

**Tech Stack:** C++ (ClickHouse), gtest (`src/Disks/tests/gtest_cas_*`), TLA+ already done (`CaBuildWatermark`), ninja build to a log analyzed by a subagent.

**Spec:** `docs/superpowers/specs/2026-06-16-ca-build-watermark-design.md`
**Branch:** `cas-mergetree-poc` (do NOT branch off; add commits here).

---

## Conventions for every task

- **Build:** `ninja -C build unit_tests_dbms > build/build_watermark_<task>.log 2>&1` (no `-j`/`nproc`); a subagent reads the log and returns a one-paragraph summary.
- **Run a test:** `./build/src/unit_tests_dbms --gtest_filter='<Filter>' > build/test_watermark_<task>.log 2>&1`; a subagent summarizes the log.
- Allman braces; "exception" not "crash"; commit trailer `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- All paths are under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/` unless noted; abbreviated `Core/` below. Tests live in `src/Disks/tests/`.

## File structure (what each unit owns)

- `Core/CasBackend.h` — add `ObjectMeta` type, an optional `meta` arg on the write methods + `putIfAbsentStream`, and an `attributes` field on `HeadResult`. The single interface change all backends follow.
- `Core/CasInMemoryBackend.{h,cpp}` — store per-object metadata; return it on `head`/`get`. The test oracle.
- `Core/CasObjectStorageBackend.{h,cpp}` — pass metadata to `writeObject`; read it from `tryGetObjectMetadata().attributes`. The production wiring.
- `Core/CasWatermark.{h,cpp}` (**new**) — `ServerWatermark` struct + codec + `WatermarkKeeper` (per-server anchor/renew/farewell). Mirrors `CasHeartbeat`.
- `Core/CasLayout.h` — `serverWatermarkKey`.
- `Core/CasStore.{h,cpp}` — process `epoch`, monotone `build_seq` counter, in-memory active set, `minActive()`, own the `WatermarkKeeper`; allocate/register/deregister build_seq.
- `Core/CasBuild.{h,cpp}` — carry the build's `build_seq`; stamp the owner triple into metadata on every object write; (Part A re-stamp already done).
- `Core/CasGc.{h,cpp}` — owner-triple + watermark read in the `retire` observe loop; per-server watermark cache (per round) + `last_seen_server_seq` (K=2 frozen detection).
- Tests: `gtest_cas_backend.cpp`, `gtest_cas_codecs.cpp`, `gtest_cas_store.cpp`, `gtest_cas_build.cpp`, `gtest_cas_gc_round.cpp`.

---

## Phase 1 — Backend user-metadata capability

### Task 1: `ObjectMeta` on the Backend interface

**Files:**
- Modify: `Core/CasBackend.h`

- [ ] **Step 1: Add the type and extend the interface.** In `Core/CasBackend.h`, near the top of `namespace DB::Cas` (before `struct Range`), add:

```cpp
/// User metadata carried alongside an object (S3 x-amz-meta-*). The CA store uses exactly one entry,
/// "cas_owner" = "<server_id_hex>:<epoch>:<build_seq>" — the owner triple the GC watermark reads.
using ObjectMeta = std::map<String, String>;
```

Add `#include <map>` and `#include <string>` if not present. Add `ObjectMeta attributes;` as the last field of `HeadResult` (line ~29) and of `GetResult` (line ~22). Extend the write methods with a trailing defaulted arg (so existing call sites compile unchanged):

```cpp
    virtual PutOutcome putIfAbsent(const String & key, const String & bytes, Token * out_token = nullptr,
                                   const ObjectMeta & meta = {}) = 0;
    virtual WriteSinkPtr putIfAbsentStream(const String & key, const ObjectMeta & meta = {}) = 0;
    virtual PutOutcome putOverwrite(const String & key, const String & bytes, const Token & expected,
                                    Token * out_token = nullptr, const ObjectMeta & meta = {}) = 0;
    virtual CasOutcome casPut(const String & key, const String & bytes, const std::optional<Token> & expected,
                              Token * out_token = nullptr, const ObjectMeta & meta = {}) = 0;
```

`get`, `head`, `deleteExact`, `list` are unchanged (head/get now *return* `attributes`).

- [ ] **Step 2: Find every `Backend` subclass that must be updated.**

Run: `grep -rn "public.*Backend\b\|: public Cas::Backend\|: public Backend" src/Disks --include='*.h' --include='*.cpp'`
Expected: at least `InMemoryBackend` (`Core/CasInMemoryBackend.h`), `ObjectStorageBackend` (`Core/CasObjectStorageBackend.h`), and the test wrapper `WriteCountingBackend` (`src/Disks/tests/gtest_cas_store.cpp`). Note every match — each needs its overrides' signatures updated to match in Tasks 2–3 and a test-wrapper fix.

- [ ] **Step 3: Build to confirm only the expected override-signature mismatches.**

Run: `ninja -C build unit_tests_dbms > build/build_watermark_t1.log 2>&1` (subagent summarizes).
Expected: FAILS to compile with "cannot instantiate abstract class" / signature-mismatch errors precisely at the subclasses found in Step 2 — this enumerates the call sites Tasks 2–3 fix. (No commit yet; Task 1 commits with Task 2 since the interface alone doesn't build.)

### Task 2: In-memory backend stores and returns metadata

**Files:**
- Modify: `Core/CasInMemoryBackend.h` (Object struct + override signatures)
- Modify: `Core/CasInMemoryBackend.cpp` (all write/read paths)
- Modify: `src/Disks/tests/gtest_cas_store.cpp` (`WriteCountingBackend` delegation signatures)
- Test: `src/Disks/tests/gtest_cas_backend.cpp`

- [ ] **Step 1: Write the failing test.** In `gtest_cas_backend.cpp` add:

```cpp
TEST(CasInMemoryBackend, RoundTripsUserMetadata)
{
    DB::Cas::InMemoryBackend backend;
    DB::Cas::Token tok;
    const DB::Cas::ObjectMeta meta{{"cas_owner", "ab:7:42"}};
    ASSERT_EQ(backend.putIfAbsent("k/key", "body", &tok, meta), DB::Cas::PutOutcome::Done);

    const auto hr = backend.head("k/key");
    ASSERT_TRUE(hr.exists);
    ASSERT_EQ(hr.attributes.at("cas_owner"), "ab:7:42");

    const auto gr = backend.get("k/key");
    ASSERT_TRUE(gr.has_value());
    ASSERT_EQ(gr->attributes.at("cas_owner"), "ab:7:42");
}
```

- [ ] **Step 2: Run it — expect compile failure** (`putIfAbsent` 4th arg / `attributes` member not yet wired in the in-memory backend). Run the build; subagent confirms it fails at this test.

- [ ] **Step 3: Implement.** In `Core/CasInMemoryBackend.h`, add `ObjectMeta meta;` to `struct Object` (line ~64), and update every override signature to match Task 1 (add the `const ObjectMeta & meta = {}` arg to `putIfAbsent`, `putIfAbsentStream`, `putOverwrite`, `casPut`).

In `Core/CasInMemoryBackend.cpp`:
- `putIfAbsent`, `putOverwrite`, `casPut`: when writing the `Object`, set `obj.meta = meta;`.
- `head`: set `hr.attributes = it->second.meta;`.
- `get`: set `result.attributes = it->second.meta;`.
- The streaming sink (`InMemoryWriteSink` returned by `putIfAbsentStream`): thread `meta` into the sink so `finalize` stores it on the `Object`. Add a `ObjectMeta meta;` member to `InMemoryWriteSink`, set it from the `putIfAbsentStream(key, meta)` arg, and on `finalize` write it onto the stored `Object`.

In `gtest_cas_store.cpp`, update `WriteCountingBackend`'s overrides to forward the new arg (e.g. `return inner->putIfAbsent(key, bytes, out_token, meta);`).

- [ ] **Step 4: Build + run the test — expect PASS.** `ninja -C build unit_tests_dbms`; `--gtest_filter='CasInMemoryBackend.RoundTripsUserMetadata'`. Subagent summarizes both logs.

- [ ] **Step 5: Run the full backend + store suites — expect green.** `--gtest_filter='Cas*Backend*:CasStore.*'`.

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInMemoryBackend.cpp \
        src/Disks/tests/gtest_cas_backend.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "CA B167: ObjectMeta on the Backend interface + in-memory round-trip

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

### Task 3: S3 backend passes/reads metadata, with a round-trip verification

**Files:**
- Modify: `Core/CasObjectStorageBackend.h` (override signatures)
- Modify: `Core/CasObjectStorageBackend.cpp` (`nativeConditionalPut`, the stream sink, `nativeHead`)
- Possibly modify: `src/Disks/ObjectStorages/S3/S3ObjectStorage.cpp` (only if it does not already round-trip `attributes`)

- [ ] **Step 1: Verify whether `S3ObjectStorage` already round-trips `attributes`.**

Run: `grep -n "attributes\|SetMetadata\|GetMetadata\|x-amz-meta\|object_metadata" src/Disks/ObjectStorages/S3/S3ObjectStorage.cpp src/IO/WriteBufferFromS3.cpp`
Decision:
- If `writeObject`'s `attributes` reach `S3::PutObjectRequest::SetMetadata` (directly or via `WriteBufferFromS3` `object_metadata`) **and** `getObjectMetadata`/`tryGetObjectMetadata` populates `ObjectMetadata::attributes` from the HEAD response → no S3-layer change needed; go to Step 2.
- If either direction is missing, add it: on write, forward `attributes` into the `WriteBufferFromS3` request metadata; on head, copy the HEAD response's user metadata into `ObjectMetadata::attributes`. Keep this change minimal and S3-only. (RustFS empirically supports `x-amz-meta-*` on PUT + HEAD — verified 2026-06-16 — so only the ClickHouse plumbing is in question.)

- [ ] **Step 2: Pass metadata on write.** In `Core/CasObjectStorageBackend.cpp` `nativeConditionalPut`, replace the `/*attributes=*/std::nullopt` argument with the metadata:

```cpp
std::optional<ObjectAttributes> attrs;
if (!meta.empty())
    attrs.emplace(meta.begin(), meta.end());   // ObjectMeta is the same map type as ObjectAttributes
auto buf = object_storage->writeObject(StoredObject(key), WriteMode::Rewrite, attrs, DBMS_DEFAULT_BUFFER_SIZE, ws);
```

Thread `const ObjectMeta & meta` through `nativeConditionalPut` and the streaming sink path (`NativeStreamingSink` / the `putIfAbsentStream` override), and add the `meta` arg to every override signature in `CasObjectStorageBackend.h`. In the `EmulatedSingleProcess` branch, forward `meta` to the inner emulated backend's put.

- [ ] **Step 3: Read metadata on head.** In `nativeHead`, set `hr.attributes` from the metadata:

```cpp
hr.attributes = ObjectMeta(metadata->attributes.begin(), metadata->attributes.end());
```

(and similarly populate `GetResult::attributes` in the native `get` if the storage exposes it; if not, leave `get`'s attributes empty — GC reads via `head`, so `head` is the load-bearing path.)

- [ ] **Step 4: Build — expect clean compile** under `-Werror`. `ninja -C build unit_tests_dbms`; subagent summarizes.

- [ ] **Step 5: Run the backend suite — expect green** (`--gtest_filter='Cas*Backend*'`); the emulated `ObjectStorageBackend` path must still pass.

- [ ] **Step 6: Commit.**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp
# add S3ObjectStorage.cpp / WriteBufferFromS3.cpp only if Step 1 required changes
git commit -m "CA B167: S3 backend passes/reads object user-metadata

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Phase 2 — Watermark object, codec, and Store state

### Task 4: `ServerWatermark` struct + JSON codec

**Files:**
- Create: `Core/CasWatermark.h`, `Core/CasWatermark.cpp`
- Modify: `Core/CMakeLists.txt` or the directory's source glob (only if sources are listed explicitly — check how `CasHeartbeat.cpp` is built; if a glob, no change)
- Test: `src/Disks/tests/gtest_cas_codecs.cpp`

- [ ] **Step 1: Write the failing codec round-trip test.** In `gtest_cas_codecs.cpp`:

```cpp
TEST(CasWatermark, RoundTrips)
{
    const DB::Cas::ServerWatermark w{.server_id = DB::UInt128(0xABCD), .epoch = 7, .min_active = 42, .seq = 3};
    const DB::String body = DB::Cas::encodeServerWatermark(w);
    const DB::Cas::ServerWatermark r = DB::Cas::decodeServerWatermark(body);
    ASSERT_EQ(r.server_id, w.server_id);
    ASSERT_EQ(r.epoch, w.epoch);
    ASSERT_EQ(r.min_active, w.min_active);
    ASSERT_EQ(r.seq, w.seq);
}
```

- [ ] **Step 2: Run — expect compile failure** (no `CasWatermark.h`).

- [ ] **Step 3: Implement the struct + codec** mirroring `CasHeartbeat.{h,cpp}` exactly (strict JSON, fail-closed decode, `format="cas_server_watermark"`, `version=1`). `Core/CasWatermark.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <base/extended_types.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace DB::Cas
{

/// Per-server build watermark (spec 2026-06-16-ca-build-watermark): one object per server under
/// servers/<server_id>, renewed async ~2s off the write path, anchored synchronously before the first
/// object PUT. Strict JSON, fail-closed decode (mirrors CasHeartbeat encoding split).
struct ServerWatermark
{
    UInt128 server_id{};
    uint64_t epoch = 0;        /// random per process start; GC checks equality, never ordering
    uint64_t min_active = 0;   /// oldest in-flight build_seq; UINT64_MAX when retired (farewell)
    uint64_t seq = 0;          /// liveness counter, bumped each renewal (frozen-seq crash detection)
};

String encodeServerWatermark(const ServerWatermark & w);
ServerWatermark decodeServerWatermark(std::string_view data);

}
```

`Core/CasWatermark.cpp`: copy `encodeHeartbeat`/`decodeHeartbeat` from `CasHeartbeat.cpp`, swap `format` to `"cas_server_watermark"`, emit `server_id` (hex), `epoch`, `min_active`, `seq` as the four keys; fail-closed on wrong format / unknown key / missing key / wrong type / future version (same throw codes).

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasWatermark.RoundTrips'`.

- [ ] **Step 5: Commit.** (`CasWatermark.{h,cpp}`, `gtest_cas_codecs.cpp`, CMake if needed.)

### Task 5: `serverWatermarkKey` in the layout

**Files:**
- Modify: `Core/CasLayout.h`
- Test: `src/Disks/tests/gtest_cas_codecs.cpp` (or wherever layout helpers are tested; reuse the file with the existing layout tests)

- [ ] **Step 1: Write the failing test.**

```cpp
TEST(CasLayout, ServerWatermarkKey)
{
    DB::Cas::Layout layout("pool");
    const DB::String hex(32, 'a');   // 32-char u128 hex
    ASSERT_EQ(layout.serverWatermarkKey(hex), "pool/servers/aa/" + hex);
}
```

- [ ] **Step 2: Run — expect compile failure.**

- [ ] **Step 3: Implement** in `Core/CasLayout.h` next to `buildHeartbeatKey`:

```cpp
String serverWatermarkKey(const String & server_id_hex) const
{
    return shardedKey("servers", server_id_hex);
}
```

- [ ] **Step 4: Build + run — expect PASS.** **Step 5: Commit.**

### Task 6: `WatermarkKeeper` — anchor, renew, farewell

**Files:**
- Modify: `Core/CasWatermark.h`, `Core/CasWatermark.cpp`
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (sibling of the existing keeper tests)

- [ ] **Step 1: Write the failing tests.** In `gtest_cas_heartbeat.cpp`:

```cpp
TEST(CasWatermarkKeeper, AnchorIsDurableThenRenewBumpsSeq)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout layout("pool");
    const DB::UInt128 server_id(0x1234);
    uint64_t min_active_now = 5;
    DB::Cas::WatermarkKeeper keeper(backend, layout, server_id, /*epoch=*/9,
                                    [&]{ return min_active_now; });
    keeper.start();                                  // anchor durable
    auto hr = backend->head(layout.serverWatermarkKey(DB::Cas::u128ToHex(server_id)));
    ASSERT_TRUE(hr.exists);
    auto w = DB::Cas::decodeServerWatermark(backend->get(layout.serverWatermarkKey(DB::Cas::u128ToHex(server_id)))->bytes);
    ASSERT_EQ(w.epoch, 9u); ASSERT_EQ(w.min_active, 5u); ASSERT_EQ(w.seq, 1u);

    min_active_now = 8;
    keeper.renewOnce();
    w = DB::Cas::decodeServerWatermark(backend->get(layout.serverWatermarkKey(DB::Cas::u128ToHex(server_id)))->bytes);
    ASSERT_EQ(w.min_active, 8u); ASSERT_EQ(w.seq, 2u);
}

TEST(CasWatermarkKeeper, FarewellRetiresEpoch)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::Layout layout("pool");
    DB::Cas::WatermarkKeeper keeper(backend, layout, DB::UInt128(0x1234), 9, []{ return 5; });
    keeper.start();
    keeper.farewell();
    auto w = DB::Cas::decodeServerWatermark(backend->get(layout.serverWatermarkKey(DB::Cas::u128ToHex(DB::UInt128(0x1234))))->bytes);
    ASSERT_EQ(w.min_active, std::numeric_limits<uint64_t>::max());
}
```

- [ ] **Step 2: Run — expect compile failure** (no `WatermarkKeeper`).

- [ ] **Step 3: Implement `WatermarkKeeper`** in `CasWatermark.{h,cpp}`, structurally a copy of `HeartbeatKeeper` with these differences:
  - Constructor: `WatermarkKeeper(BackendPtr, const Layout &, UInt128 server_id, uint64_t epoch, std::function<uint64_t()> min_active_fn)`. Key = `layout.serverWatermarkKey(u128ToHex(server_id))`.
  - `start()`: `putIfAbsent` (or `putOverwrite` if it already exists from a prior crashed process — see note) of `{server_id, epoch, min_active = min_active_fn(), seq = 1}`. **W-ANCHOR: durable before `start()` returns.** If the key already exists (prior incarnation), `putOverwrite` against its current token to claim it for the new `epoch` — a fresh process legitimately overwrites its own server slot (single writer per `server_id`). Record `last_token`.
  - `renewOnce()`: `putOverwrite` against `last_token` with `seq+1` and `min_active = min_active_fn()`. (Same single-writer fail-closed contract as `HeartbeatKeeper::renewOnce`.)
  - `farewell()`: `putOverwrite` against `last_token` with `min_active = UINT64_MAX` (keep `seq`+1, same epoch). Stops the background thread first.
  - `startBackground(period)` / `stopBackground()` / `backgroundLoop` — identical to `HeartbeatKeeper`, calling `renewOnce()` each tick.

  Note on `start()` overwrite: `HeartbeatKeeper::start` throws if the key exists (build_id is unique). For the per-server slot, existence is expected across restarts, so `start()` must claim it: HEAD → if absent `putIfAbsent`, else `putOverwrite(currentToken)`.

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasWatermarkKeeper.*'`. **Step 5: Commit.**

### Task 7: `Store` owns epoch, build_seq, active set, and the keeper

**Files:**
- Modify: `Core/CasStore.h` (fields + methods), `Core/CasStore.cpp` (`open`, `startBuild`, new methods)
- Modify: `Core/CasBuild.h` / `Core/CasBuild.cpp` (carry `build_seq`; deregister on publish/abandon/dtor)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

- [ ] **Step 1: Write the failing test.** In `gtest_cas_store.cpp`:

```cpp
TEST(CasStore, MinActiveTracksInFlightBuilds)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg; cfg.pool_prefix = "pool"; cfg.server_id = DB::UInt128(1);
    cfg.background_heartbeats = false;
    auto store = DB::Cas::Store::open(backend, cfg);

    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());   // no builds: floor == next seq
    auto b1 = store->startBuild({});                            // seq 1
    auto b2 = store->startBuild({});                            // seq 2
    ASSERT_EQ(store->minActive(), 1u);
    b1->abandon();                                              // finishes seq 1
    ASSERT_EQ(store->minActive(), 2u);                         // floor advances
    b2->abandon();
    ASSERT_EQ(store->minActive(), store->peekNextBuildSeq());  // empty again
}

TEST(CasStore, BuildSeqIsStrictlyMonotone)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg; cfg.pool_prefix = "pool"; cfg.server_id = DB::UInt128(1);
    cfg.background_heartbeats = false;
    auto store = DB::Cas::Store::open(backend, cfg);
    auto a = store->startBuild({}); auto sa = a->buildSeq();
    a->abandon();
    auto b = store->startBuild({});
    ASSERT_GT(b->buildSeq(), sa);                              // never reused, never lower
}
```

- [ ] **Step 2: Run — expect compile failure** (`minActive`, `peekNextBuildSeq`, `Build::buildSeq` missing).

- [ ] **Step 3: Implement.** In `Core/CasStore.h` add private fields and public methods:

```cpp
    uint64_t process_epoch = 0;                 // random per Store (process) at open
    std::mutex builds_mutex;
    uint64_t next_build_seq = 1;                // strictly-increasing counter (monotonicity is load-bearing)
    std::set<uint64_t> active_build_seqs;       // in-flight builds' seqs
    std::unique_ptr<WatermarkKeeper> watermark; // per-server watermark renewer

public:
    uint64_t epoch() const { return process_epoch; }
    uint64_t minActive();                       // oldest active seq, or next_build_seq if none
    uint64_t peekNextBuildSeq();                // for tests/assertions
private:
    uint64_t allocateBuildSeq();                // ++next_build_seq under lock; inserts into active set
    void retireBuildSeq(uint64_t seq);          // erase from active set (idempotent)
```

In `Core/CasStore.cpp`:
- `Store::open` (or the constructor): set `process_epoch` from a random draw (two `thread_local_rng()` folded to u64; nonzero); construct the `WatermarkKeeper` with `config.server_id`, `process_epoch`, and `[this]{ return minActive(); }`; call `watermark->start()` **before any object PUT** (W-ANCHOR); if `config.background_heartbeats`, `watermark->startBackground(config.heartbeat_period)`.
- `minActive()`: `std::lock_guard lk(builds_mutex); return active_build_seqs.empty() ? next_build_seq : *active_build_seqs.begin();`
- `peekNextBuildSeq()`: `std::lock_guard lk(builds_mutex); return next_build_seq;`
- `allocateBuildSeq()`: `std::lock_guard lk(builds_mutex); uint64_t s = next_build_seq++; active_build_seqs.insert(s); return s;`
- `retireBuildSeq(seq)`: `std::lock_guard lk(builds_mutex); active_build_seqs.erase(seq);`
- `startBuild`: replace the random-u128 `build_id` minting *for watermark purposes* with `const uint64_t seq = allocateBuildSeq();` and pass `seq` (and `process_epoch`) into the `Build`. **Keep** creating the existing per-build `HeartbeatKeeper` only if other code still depends on it; otherwise drop it (the watermark replaces it). Decision: drop the per-build `HeartbeatKeeper` creation here — the watermark supersedes it — unless `grep -rn "buildHeartbeatKey\|HeartbeatKeeper" Core/ src/Disks/tests` shows a load-bearing consumer; if it does, keep it for now and remove in a follow-up.

In `Core/CasBuild.h`: add `uint64_t build_seq{};` and `uint64_t epoch{};` members and a `uint64_t buildSeq() const { return build_seq; }`; extend the constructor to take them. In `Core/CasBuild.cpp`: in `publish()` (on success) and `abandon()` and the destructor, call `store->retireBuildSeq(build_seq);` (idempotent — safe to call from more than one of these).

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasStore.MinActiveTracksInFlightBuilds:CasStore.BuildSeqIsStrictlyMonotone'`.

- [ ] **Step 5: Full store/build suites — expect green.** `--gtest_filter='CasStore.*:CasBuild*'`.

- [ ] **Step 6: Commit.**

---

## Phase 3 — Writers stamp the owner triple

### Task 8: stamp `(server_id, epoch, build_seq)` into object metadata

**Files:**
- Modify: `Core/CasBuild.cpp` (`putBlob`, `putTree`, `recreateTree`, `resurrect`)
- Modify: `Core/CasBuild.h` (a small helper)
- Test: `src/Disks/tests/gtest_cas_build.cpp`

- [ ] **Step 1: Write the failing test.** In `gtest_cas_build.cpp` (use the existing fixture that builds a `Store` over `InMemoryBackend`):

```cpp
TEST(CasBuild, BlobCarriesOwnerTripleInMetadata)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg; cfg.pool_prefix = "pool"; cfg.server_id = DB::UInt128(0xAB);
    cfg.background_heartbeats = false;
    auto store = DB::Cas::Store::open(backend, cfg);
    auto build = store->startBuild({});
    const auto id = build->putBlob(/* small BlobSource with known bytes — mirror existing putBlob tests */);

    const auto hr = backend->head(store->layout().blobKey(id /* -> hash hex */));
    ASSERT_TRUE(hr.exists);
    const DB::String expected = DB::Cas::u128ToHex(cfg.server_id) + ":" + std::to_string(store->epoch())
                                + ":" + std::to_string(build->buildSeq());
    ASSERT_EQ(hr.attributes.at("cas_owner"), expected);
}
```

(Match the exact `putBlob`/`blobKey` usage from a neighbouring test in the same file.)

- [ ] **Step 2: Run — expect FAIL** (`attributes` has no `cas_owner` — writers don't stamp it yet).

- [ ] **Step 3: Implement.** Add a helper to `Core/CasBuild.h`/`.cpp`:

```cpp
ObjectMeta Build::ownerMeta() const
{
    return ObjectMeta{{"cas_owner",
        u128ToHex(store->poolConfig().server_id) + ":" + std::to_string(epoch) + ":" + std::to_string(build_seq)}};
}
```

In `putBlob`, `putTree`, `recreateTree`, and `resurrect`, pass `ownerMeta()` as the `meta` argument of the corresponding backend write (`putIfAbsentStream(key, ownerMeta())`, `putIfAbsent(..., &tok, ownerMeta())`, `putOverwrite(..., &tok, ownerMeta())`). Also write the forensic copy into the body header: keep `header.build_id` but set it to `(static_cast<UInt128>(epoch) << 64) | build_seq` so fsck can recover the triple from the body too (server_id already in `Provenance`).

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasBuild.BlobCarriesOwnerTripleInMetadata'`.

- [ ] **Step 5: Full build suite — expect green.** **Step 6: Commit.**

---

## Phase 4 — GC watermark oracle

### Task 9: per-server watermark cache + frozen-seq liveness in `Gc`

**Files:**
- Modify: `Core/CasGc.h` (fields + a `serverLive`/`watermarkOf` helper), `Core/CasGc.cpp`
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

- [ ] **Step 1: Write the failing unit test for the helper.** In `gtest_cas_gc_round.cpp`:

```cpp
TEST(CasGcWatermark, ParsesOwnerAndProtectsLiveBuild)
{
    auto backend = std::make_shared<DB::Cas::InMemoryBackend>();
    DB::Cas::PoolConfig cfg; cfg.pool_prefix = "pool"; cfg.server_id = DB::UInt128(0xAB);
    auto store = DB::Cas::Store::open(backend, cfg);
    DB::Cas::Gc gc(store, DB::UInt128(7));

    // server AB, epoch 9, min_active 5
    backend->putOverwrite(store->layout().serverWatermarkKey(DB::Cas::u128ToHex(DB::UInt128(0xAB))),
        DB::Cas::encodeServerWatermark({DB::UInt128(0xAB), 9, 5, 1}), /*expected*/{}, nullptr);

    DB::Cas::ObjectMeta live{{"cas_owner", DB::Cas::u128ToHex(DB::UInt128(0xAB)) + ":9:7"}};   // seq 7 >= 5
    DB::Cas::ObjectMeta done{{"cas_owner", DB::Cas::u128ToHex(DB::UInt128(0xAB)) + ":9:3"}};   // seq 3 < 5
    DB::Cas::ObjectMeta stale{{"cas_owner", DB::Cas::u128ToHex(DB::UInt128(0xAB)) + ":1:7"}};  // wrong epoch

    gc.beginWatermarkRound();
    ASSERT_TRUE (gc.protectedByLiveBuild(live));
    ASSERT_FALSE(gc.protectedByLiveBuild(done));
    ASSERT_FALSE(gc.protectedByLiveBuild(stale));
}
```

- [ ] **Step 2: Run — expect compile failure** (`beginWatermarkRound`, `protectedByLiveBuild` missing).

- [ ] **Step 3: Implement.** In `Core/CasGc.h` add:

```cpp
    std::map<UInt128, ServerWatermark> watermark_cache;       // per-round, cleared at beginWatermarkRound
    std::map<UInt128, uint64_t> last_seen_server_seq;         // across rounds (K=2 frozen detection)
    std::map<UInt128, uint64_t> server_frozen_rounds;         // consecutive rounds seq unchanged

public:
    void beginWatermarkRound();                               // clear watermark_cache; sample seqs for K=2
    bool protectedByLiveBuild(const ObjectMeta & meta);       // the condemn guard's 4th clause
```

In `Core/CasGc.cpp`:
- `beginWatermarkRound()`: `watermark_cache.clear();`
- A private `const ServerWatermark * watermarkOf(UInt128 server_id)`: if not cached, `head`+`get` `serverWatermarkKey`, decode, cache; return `nullptr` if absent.
- `serverLive(server_id, w)`: compare `w.seq` to `last_seen_server_seq[server_id]`. If changed → live, reset `server_frozen_rounds[server_id]=0`, update last_seen. If unchanged → `++server_frozen_rounds[server_id]`; **live iff `server_frozen_rounds[server_id] < 2`** (K=2: needs 2 consecutive frozen rounds to declare dead). Update `last_seen_server_seq`.
- `protectedByLiveBuild(meta)`: parse `meta.at("cas_owner")` → `(server_hex, epoch, build_seq)`; if missing/malformed → `return false` (unprotected, pre-watermark default, spec §guard); `w = watermarkOf(server)`; if `!w` → false; `return serverLive(server, *w) && w->epoch == epoch && build_seq >= w->min_active && w->min_active != UINT64_MAX;`

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasGcWatermark.*'`. **Step 5: Commit.**

### Task 10: wire the guard into `Gc::retire`

**Files:**
- Modify: `Core/CasGc.cpp` (`retire` observe loop ~626–644; call `beginWatermarkRound` at round start)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`

- [ ] **Step 1: Write the failing scenario test.** A live build's `everEdged ∧ InDeg=0` blob is NOT condemned; once its server's `min_active` passes it (build finished), it IS condemned. Model it by writing the blob with `cas_owner` metadata and a matching server watermark, running a retire round, and asserting the retire set excludes/includes it. Mirror the construction in the existing `CasGcRetire.*` tests:

```cpp
TEST(CasGcRetire, SkipsBlobOwnedByLiveBuildThenCondemnsWhenFloorPasses)
{
    // ... set up a pool where hash H is everEdged and InDeg=0 (referenced then dropped),
    //     blob written with cas_owner = AB:9:7, server AB watermark {epoch 9, min_active 5} ...
    // round 1: H must NOT appear in the retired set (protected: 7 >= 5)
    // raise the watermark to min_active = 8, run another round:
    // H MUST now appear in the retired set (7 < 8 -> unprotected)
}
```

(Fill the setup by copying the nearest `CasGcRetire` fixture; the assertion is on the returned `RetiredSet` containing/!containing `H`.)

- [ ] **Step 2: Run — expect FAIL** (guard not wired; H is condemned in round 1).

- [ ] **Step 3: Implement.** In `Gc::retire`, call `beginWatermarkRound()` once before the observe loop. In the observe loop, after `if (!observed.exists) continue;`, add:

```cpp
            if (protectedByLiveBuild(observed.attributes))
                continue;   // owned by a live build -> skip this round (non-destructive deferral, spec guard)
```

- [ ] **Step 4: Build + run — expect PASS.** `--gtest_filter='CasGcRetire.SkipsBlobOwnedByLiveBuildThenCondemnsWhenFloorPasses'`.

- [ ] **Step 5: Regression — the previously-fragile test must be green.** `--gtest_filter='CasGcRetire.*:CasGcRecheck.*'`. Expected: all pass — there is no per-build heartbeat object to linger, so `DeletedCandidateDoesNotReappear` passes (blobs written without `cas_owner`, or whose server has no live watermark, are unprotected by default).

- [ ] **Step 6: Commit.**

### Task 11: convergence test — the B167 livelock is gone

**Files:**
- Test: `src/Disks/tests/gtest_cas_build.cpp` (or `gtest_cas_protocol_scenarios.cpp` if present)

- [ ] **Step 1: Write the convergence test.** Adversarial GC that exact-token-deletes any condemned blob *not* protected by a live watermark; a build dedup-hits a condemned blob, re-streams a fresh incarnation stamped with its live triple, and **publishes in bounded steps** (previously: livelock). Assert the publish succeeds and the part reads back.

```cpp
TEST(CasBuild, ResurrectConvergesUnderProductiveGc)
{
    // condemned blob for hash H; build with live watermark covering its build_seq.
    // loop: GC tries to condemn+delete H whenever unprotected; build re-streams + publishes.
    // ASSERT: publish() returns success within a bounded number of iterations (e.g. <= 8),
    //         and the published part's blob resolves.
}
```

- [ ] **Step 2: Run — expect PASS** (Part A re-stream + Task 10 guard together converge). If it FAILS/livelocks, that is a real defect — stop and diagnose (do not loosen the test).

- [ ] **Step 3: Commit.**

---

## Phase 5 — documentation + integration

### Task 12: update the protocol-spec line and the `CasHeartbeat` comment

**Files:**
- Modify: `Core/CasHeartbeat.h` (the §5 comment that says heartbeats gate only full-GC debris)
- Modify: `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md` (the §5 line *"the publish gate, not the heartbeat, is the safety mechanism"*)

- [ ] **Step 1:** Update both to state that incremental GC now also honors a **per-server build watermark** (co-liveness mechanism), with the publish gate remaining the safety backstop. Cross-reference the watermark spec. No code/test change.

- [ ] **Step 2: Commit.**

### Task 13: build everything + full CA suite green

- [ ] **Step 1:** `ninja -C build unit_tests_dbms > build/build_watermark_final.log 2>&1` (subagent summary). Expect clean under `-Werror`.

- [ ] **Step 2:** `./build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*' > build/test_watermark_final.log 2>&1` (subagent summary). Expect green except the pre-existing B140 `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak`.

- [ ] **Step 3: Commit** any final fixups.

### Task 14: B160+B167 soak (headline validation)

- [ ] **Step 1:** Rebuild the soak server binary; run the two-replica soak (`utils/ca-soak`) with productive GC for ≥15 min.

- [ ] **Step 2:** Assert **0 broken detached parts** and `fsck` `dangling=0` (per `utils/ca-soak/soak/fsck.py`). Capture metrics. This is the real-world confirmation that the watermark closes B167 under load.

---

## Self-review

**Spec coverage:** identity triple in S3 metadata (Tasks 1–3, 8); watermark object `{epoch,min_active,seq}` + codec (4); `serverWatermarkKey` (5); startup anchor + async renew + farewell (6–7); monotone `build_seq` + in-memory active set + `min_active` (7, with the load-bearing monotonicity from `CaBuildWatermarkNum`); GC guard `serverLive ∧ epoch== ∧ build_seq≥min_active` (9–10); K=2 frozen-seq crash detection (9); body forensic copy (8); publish-gate backstop (unchanged — convergence test 11); spec/comment revision (12); soak (14). ETag-as-token and Part A re-stamp are pre-existing/committed. No server-side copy (correctly absent).

**Placeholder scan:** the two genuinely code-dependent investigations are explicit decision steps with exact greps (Task 3 Step 1 S3 round-trip; Task 7 Step 3 whether to drop the per-build `HeartbeatKeeper`), not vague TODOs. Test bodies that must mirror existing fixtures (Tasks 8, 10, 11) name the fixture to copy and the exact assertion.

**Type consistency:** `ObjectMeta = std::map<String,String>` used everywhere; metadata key `"cas_owner"`, value `"<server_hex>:<epoch>:<build_seq>"` consistent across Tasks 8/9/10; `ServerWatermark{server_id,epoch,min_active,seq}` consistent across 4/6/9; `minActive()`/`peekNextBuildSeq()`/`buildSeq()`/`epoch()`/`protectedByLiveBuild()`/`beginWatermarkRound()` names consistent across 7/9/10.

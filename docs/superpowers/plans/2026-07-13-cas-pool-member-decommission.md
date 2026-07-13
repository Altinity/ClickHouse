# CAS Pool-Member Decommission Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement dead-replica removal for CAS pools: `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<srid>' FROM DISK '<disk>'` + `clickhouse-disks ca-drop-member`, over one shared core `Cas::decommissionPoolMember`.

**Architecture:** The command becomes a temporary writer for the victim server root: it claims the victim's mount slot with a token-guarded no-wait `claimMount` (the liveness gate), then drives ONLY existing writer machinery — `Store::dropNamespace` per namespace, the orphan-manifest sweep for debris, scoped backend LIST+delete for staging and roots — and retires the slot strictly last (the resume anchor). GC never gains new powers; blob bytes are reclaimed by subsequent normal GC rounds.

**Tech Stack:** ClickHouse C++ (CAS core under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`), gtest (`unit_tests_dbms`), SQL parser/interpreter (`src/Parsers`, `src/Interpreters`), `programs/disks`, integration tests via praktika.

**Spec:** `docs/superpowers/specs/2026-07-13-cas-pool-member-decommission-design.md` — read it first.

## Global Constraints

- Allman braces (style check enforces this). Never `sleep` in C++ to fix a race.
- Fail-close everywhere: propagate errors; the ONLY tolerated-and-continue class is per-object transient failures inside the drain sweeps, which record a warning and SKIP slot deletion (spec §core step "Fail-close").
- The invariant "`GC` never invents a ref transition" must stay true — decommission acts as a WRITER (it holds the victim mount), never as GC.
- CAS is pre-release: no compatibility scaffolding, no config flags to keep old behavior.
- Wrap literal names (`MergeTree`, `claimMount`, log excerpts) in backticks in comments/docs/commit messages; write function names as `f` not `f()`; say "exception" not "crash" for logical errors.
- Docs edits under `docs/`: every header keeps/gets an explicit `{#kebab-case-anchor}`.
- Builds: `ninja -C <build_dir> <target> > <build_dir>/<log> 2>&1` — never `-j`, never `nproc`; analyze the log with a subagent. Tests: redirect output to a unique log file in the build directory; analyze with a subagent.
- New stateless tests via `./tests/queries/0_stateless/add-test <name>`; no `no-*` tags unless strictly necessary.
- Commit after every task (no rebase/amend; new commits on `cas-gc-rebuild`). End commit messages with the `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` trailer.
- The gtest binary is `unit_tests_dbms`; run scoped: `<build_dir>/src/unit_tests_dbms --gtest_filter='CasDecommission*'`.
- Before starting, verify the branch is `cas-gc-rebuild` and the tree builds: `git branch --show-current`.

## File Structure

| File | Role |
|---|---|
| Create `Core/CasDecommission.h` | `DecommissionReport` + `decommissionPoolMember` declaration |
| Create `Core/CasDecommission.cpp` | The orchestrator (7 spec steps) |
| Modify `Core/CasStore.h` / `CasStore.cpp` | `Store::openForDecommission` factory (extracted writable-mount tail, claim-policy param); `dropNamespace` returns stats; `poolBackendPtr` accessor |
| Modify `Core/CasServerRoot.h` / `.cpp` | `readOwnerUuid` helper (extracted from `claimOwnerOrThrow` read path) |
| Modify `Core/CasEvent.h` / `.cpp` | `MemberDecommission` event type |
| Modify `Core/CasOrphanManifestSweep.h` / `.cpp` | `sweepNamespace` returns deleted count |
| Create `src/Disks/tests/gtest_cas_decommission.cpp` | Core gtests |
| Modify `src/Parsers/ASTSystemQuery.h` / `.cpp`, `src/Parsers/ParserSystemQuery.cpp` | Grammar |
| Modify `src/Access/Common/AccessType.h` + `tests/queries/0_stateless/01271_show_privileges.reference` | Privilege |
| Modify `src/Interpreters/InterpreterSystemQuery.cpp` | Dispatch + result block |
| Create `programs/disks/CommandCaDropMember.cpp`; modify `programs/disks/DisksApp.cpp`, `programs/disks/CMakeLists.txt` | Disks facade |
| Create `tests/queries/0_stateless/0NNNN_system_content_addressed_drop_pool_member.sql` (+ `.reference`) | Grammar/dispatch stateless test |
| Create `tests/integration/test_content_addressed_drop_pool_member/` | End-to-end multi-node test |
| Modify `docs/superpowers/cas/04-gc-protocol.md`, `05-formats-and-backend.md`, `BACKLOG.md` | Doc updates |

All `Core/…` paths abbreviate `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/…`.

---

### Task 1: `Store::openForDecommission` — the admin claim gate

**Files:**
- Modify: `Core/CasStore.h` (public statics near `open`, `CasStore.h:332`)
- Modify: `Core/CasStore.cpp` (refactor `Store::open`, `CasStore.cpp:216-460`)
- Modify: `Core/CasServerRoot.h` / `Core/CasServerRoot.cpp` (`readOwnerUuid`)
- Test: `src/Disks/tests/gtest_cas_decommission.cpp` (new file)

**Interfaces:**
- Consumes: `claimMount` (`CasServerRoot.h:190`), `claimOwnerOrThrow` / `allocateWriterEpoch` (used at `CasStore.cpp:301,310`), `MountLeaseKeeper`, `openStoreForTest` (`cas_test_helpers.h:566`).
- Produces (later tasks rely on these exact signatures):
  ```cpp
  /// CasServerRoot.h — read the owner anchor without claiming. nullopt = absent.
  std::optional<UInt128> readOwnerUuid(Backend & b, const Layout & l, const String & server_root_id);

  /// CasStore.h — admin writer mount of the VICTIM srid. Throws ABORTED when the member is alive
  /// (LiveDoubleStart) and BAD_ARGUMENTS when there is nothing to decommission (no owner anchor,
  /// no mount body). Impersonates the victim uuid; bumps writer_epoch; starts the lease keeper.
  static StorePtr openForDecommission(BackendPtr backend, PoolConfig config, const String & victim_srid);
  ```

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_decommission.cpp`:

```cpp
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>

using namespace DB;
using namespace DB::Cas;
using namespace DB::Cas::tests;

namespace
{

/// Open a store for the VICTIM srid over `backend` (the pool's future dead member).
StorePtr openVictim(std::shared_ptr<InMemoryBackend> backend)
{
    return Store::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
}

}

TEST(CasDecommission, RefusesLiveMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto victim = openVictim(backend);   /// keeps renewing — the member is alive

    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    });
}

TEST(CasDecommission, ClaimsDeadMemberAndBumpsEpoch)
{
    auto backend = std::make_shared<InMemoryBackend>();
    uint64_t victim_epoch = 0;
    {
        auto victim = openVictim(backend);
        victim_epoch = victim->writerEpochForTest();   /// see Step 3 — add this accessor if absent
    }   /// graceful close: lease stamped already-expired + farewell — the slot is claimable

    auto admin = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    ASSERT_TRUE(admin != nullptr);
    EXPECT_GT(admin->writerEpochForTest(), victim_epoch);
    /// The admin store IS the victim server root now (impersonation).
    EXPECT_EQ(admin->poolConfig().server_root_id, "victim");
}

TEST(CasDecommission, RefusesUnknownMember)
{
    auto backend = std::make_shared<InMemoryBackend>();
    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "never_existed");
    });
}

TEST(CasDecommission, SecondConcurrentDecommissionRefused)
{
    auto backend = std::make_shared<InMemoryBackend>();
    { auto victim = openVictim(backend); }

    auto first = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    expectThrowsCode(ErrorCodes::ABORTED, [&]
    {
        Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin2"}, "victim");
    });
}
```

Register the file in `src/Disks/tests/CMakeLists.txt` if test sources are listed explicitly (check how `gtest_cas_store.cpp` is registered; most CH gtests are glob-collected — if globbed, no edit needed).

Notes for the implementer:
- If `Store` has no test accessor for the writer epoch, add `uint64_t writerEpochForTest() const { return process_epoch.load(std::memory_order_relaxed); }` next to the other `*ForTest` seams (`CasStore.h:530` area).
- `expectThrowsCode` is `cas_test_helpers.h:47`.
- `ErrorCodes` used in tests must be the ones the implementation throws — keep them in sync.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t1.log 2>&1; tail -5 build_debug/build_cas_decommission_t1.log
```
Expected: compile FAILURE — `openForDecommission` does not exist. (Use a subagent to read the log if it is long.)

- [ ] **Step 3: Implement**

3a. `Core/CasServerRoot.h` — declare next to `claimMount` (`CasServerRoot.h:190`):

```cpp
/// Read the owner anchor (`<prefix>/gc/server-roots/<srid>/owner`) WITHOUT claiming or validating
/// identity. Decommission uses it to impersonate the victim uuid (design
/// 2026-07-13-cas-pool-member-decommission §core). nullopt = anchor absent.
std::optional<UInt128> readOwnerUuid(Backend & b, const Layout & l, const String & server_root_id);
```

Implement in `CasServerRoot.cpp` by extracting the existing GET+decode from `claimOwnerOrThrow`'s read path (find `ownerKey` usage there); `claimOwnerOrThrow` then calls `readOwnerUuid` itself — one decode site, no behavior change.

3b. `Core/CasStore.cpp` — refactor `Store::open`:

- Add a private claim-policy enum and extract the writable-mount tail. In `CasStore.h` (private section):

```cpp
enum class MountClaimPolicy : uint8_t
{
    WaitForExpiry,   /// normal server open — waits out a stale self-lease (S13)
    NoWait,          /// decommission gate — a live lease is an immediate ABORTED refusal
};

/// The writable-mount startup tail extracted VERBATIM from `open` (owner claim → writer_epoch →
/// mount claim (+fence-recovery loop) → MountLeaseKeeper start → watermark anchor → the rest of
/// the writable-open initialization). `our_uuid` is the identity to mount as — `config.server_id`
/// for a normal open, the victim's owner uuid for decommission.
static void mountWritable(StorePtr & store, UInt128 our_uuid, MountClaimPolicy policy);
```

- Move `CasStore.cpp:287` (the `/// === Mount-safety startup protocol` block) through the END of the writable-open initialization (everything guarded by `if (!store->config.read_only)`) into `mountWritable`, replacing `store->config.server_id` with the `our_uuid` parameter. Inside the fence-recovery loop replace the claim call with the policy switch:

```cpp
MountClaimResult claim;
if (policy == MountClaimPolicy::WaitForExpiry)
{
    claim = claimMountAwaitingExpiry(
        *store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
        [&now_ms]() { return now_ms(); }, ttl_ms, poll_interval_ms, margin_ms, sleep_ms, on_wait_start,
        emit_mount_event);
}
else
{
    claim = claimMount(*store->pool_backend, store->pool_layout, srid, our_uuid, writer_epoch,
                       now_ms(), ttl_ms, emit_mount_event);
}
if (claim.kind != MountClaimResult::Claimed && policy == MountClaimPolicy::NoWait
    && claim.kind != MountClaimResult::FencedSelf)
    throw Exception(ErrorCodes::ABORTED,
        "CAS decommission '{}': pool member is alive or contended — mount lease held by uuid={} "
        "epoch={} pid={} hostname={} (expires_at_ms={}). Refusing (no FORCE variant exists; stop the "
        "server or wait for its lease to lapse).",
        srid, u128ToHex(claim.body.server_uuid), claim.body.writer_epoch, claim.body.pid,
        claim.body.hostname, claim.body.expires_at_ms);
```

The existing `FencedSelf` recovery ("a fence costs an epoch": re-`allocateWriterEpoch`, retry, bounded by `max_fence_recoveries`) and the `LiveDoubleStart`/`ForeignOwner` throw stay shared — the `NoWait` branch above merely converts what `WaitForExpiry` reports after its bounded wait into an immediate refusal. `Store::open` becomes: probe + `PoolMeta` + ctor + `mountWritable(store, store->config.server_id, MountClaimPolicy::WaitForExpiry)`.

3c. `Store::openForDecommission` (public static, `CasStore.cpp`, right after `open`):

```cpp
StorePtr Store::openForDecommission(BackendPtr backend, PoolConfig config, const String & victim_srid)
{
    backend = std::make_shared<InstrumentedBackend>(std::move(backend));
    validateServerRootId(victim_srid);

    config.server_root_id = victim_srid;
    config.read_only = false;
    config.skip_access_check = true;   /// the pool exists (the calling disk validated it); no probe writes

    Layout layout(config.pool_prefix);

    /// Impersonate the victim: decommission acts as "the next incarnation of that server". The gate
    /// below is then EXACTLY the S13 reclaim semantics: expired/terminated/fenced lease → reclaim;
    /// live lease → refuse. Owner anchor absent + mount absent = nothing to decommission.
    std::optional<UInt128> victim_uuid = readOwnerUuid(*backend, layout, victim_srid);
    if (!victim_uuid)
    {
        if (const auto mount = backend->get(layout.mountKey(victim_srid)))
            victim_uuid = decodeMountLease(mount->bytes).server_uuid;   /// partial hand-cleanup: adopt from the lease
        else
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "CAS decommission '{}': unknown pool member (no owner anchor and no mount lease). "
                "Nothing to decommission; if victim objects linger without a slot, run ca-fsck.",
                victim_srid);
    }
    config.server_id = *victim_uuid;

    backend->checkConditionalWriteSingleAttemptSupport();
    PoolMeta meta = PoolMeta::createOrValidate(
        *backend, layout, config.blob_header_len, config.blob_hash_algo, config.blob_hash_allow_new);

    StorePtr store(new Store(std::move(backend), std::move(config), std::move(meta)));
    mountWritable(store, *victim_uuid, MountClaimPolicy::NoWait);
    return store;
}
```

Mirror the `process_epoch` random-mint prologue from `open` (`CasStore.cpp:274-279`) before `mountWritable` if `mountWritable` does not itself overwrite it via `allocateWriterEpoch` (it does at the extracted `CasStore.cpp:310-311` — verify after the move; if covered, skip the mint).

- [ ] **Step 4: Run the tests + the full Cas suite (refactor safety net)**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t1.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission*' > build_debug/test_cas_decommission_t1.log 2>&1; tail -3 build_debug/test_cas_decommission_t1.log
build_debug/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build_debug/test_cas_all_t1.log 2>&1; tail -3 build_debug/test_cas_all_t1.log
```
Expected: all `CasDecommission*` PASS; the full `Cas*/Ca*` suite stays green (the `open` refactor must not change behavior). Analyze logs with a subagent.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.cpp \
        src/Disks/tests/gtest_cas_decommission.cpp
git commit -m "cas: \`Store::openForDecommission\` — admin claim gate for pool-member decommission

Extracts the writable-mount tail of \`Store::open\` into \`mountWritable\` with a claim policy
(\`WaitForExpiry\` for normal opens, \`NoWait\` for decommission), adds \`readOwnerUuid\`, and the
victim-impersonation factory per docs/superpowers/specs/2026-07-13-cas-pool-member-decommission-design.md.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `decommissionPoolMember` — namespace erasure, report, events

**Files:**
- Create: `Core/CasDecommission.h`, `Core/CasDecommission.cpp`
- Modify: `Core/CasEvent.h:17-29` (enum), `Core/CasEvent.cpp:55` area (name map)
- Modify: `Core/CasStore.h:409` / `CasStore.cpp` (`dropNamespace` returns stats)
- Test: `src/Disks/tests/gtest_cas_decommission.cpp`

**Interfaces:**
- Consumes: `Store::openForDecommission` (Task 1), `Store::listNamespaces` (`CasStore.h:393`), `Store::namespaceIsRemoved` (`CasStore.h:420`), `Store::listRefs` (`CasStore.h:392`), `Layout::refsNamespacePrefix` (`CasLayout.h:106`).
- Produces:
  ```cpp
  /// CasStore.h — dropNamespace now reports what it removed (existing callers may ignore it).
  struct DropNamespaceStats { uint64_t committed_refs = 0; uint64_t precommits = 0; };
  DropNamespaceStats dropNamespace(const RootNamespace & ns);

  /// CasDecommission.h
  namespace DB::Cas
  {
  struct DecommissionReport
  {
      String srid;
      uint64_t namespaces_removed = 0;
      uint64_t namespaces_already_removed = 0;
      uint64_t committed_refs_removed = 0;
      uint64_t precommits_removed = 0;
      uint64_t edge_deltas_emitted = 0;        /// == committed + precommit removals
      uint64_t manifest_debris_removed = 0;    /// Task 3
      uint64_t staging_objects_removed = 0;    /// Task 3
      uint64_t mountpoint_objects_removed = 0; /// Task 3
      bool slot_removed = false;               /// Task 4
      std::vector<String> warnings;            /// non-empty ⇒ slot kept (Task 4)
  };
  DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                            const String & victim_srid, const CasEventSink & sink = {});
  }
  ```
- New event: `CasEventType::MemberDecommission`, string `member_decommission`.

- [ ] **Step 1: Write the failing test**

Append to `gtest_cas_decommission.cpp`. Build the victim fixture with the raw ref-table helpers from `cas_test_helpers.h` (`registerNamespaceRaw` `:139`, `publishCommittedTransition` `:252`, `appendRefLogSeed` `:198` — copy the exact fixture idiom from `gtest_cas_ref_writer.cpp`, which creates namespaces + committed refs + precommit bindings against an `InMemoryBackend`):

```cpp
TEST(CasDecommission, ErasesAllVictimNamespaces)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        /// Two tables: ns "victim/db/t1" with 2 committed refs, ns "victim/db/t2" with 1 committed
        /// ref + 1 stale precommit (fixture idiom of gtest_cas_ref_writer.cpp).
        makeTableWithRefs(*victim, "victim/db/t1", /*committed=*/2, /*precommits=*/0);
        makeTableWithRefs(*victim, "victim/db/t2", /*committed=*/1, /*precommits=*/1);
    }

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.srid, "victim");
    EXPECT_EQ(report.namespaces_removed, 2u);
    EXPECT_EQ(report.namespaces_already_removed, 0u);
    EXPECT_EQ(report.committed_refs_removed, 3u);
    EXPECT_EQ(report.precommits_removed, 1u);
    EXPECT_EQ(report.edge_deltas_emitted, 4u);

    /// The namespaces are durably Removed — visible to a fresh admin store.
    auto check = Store::openForDecommission(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "chk"}, "victim");
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t1")));
    EXPECT_TRUE(check->namespaceIsRemoved(RootNamespace("victim/db/t2")));
}

TEST(CasDecommission, RerunCountsAlreadyRemoved)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    (void)decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    /// Task 4 will delete the slot on success and make a full re-run BAD_ARGUMENTS; until then a
    /// re-run must skip the Removed namespace idempotently.
    const auto second = decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a2"}, "victim");
    EXPECT_EQ(second.namespaces_removed, 0u);
    EXPECT_EQ(second.namespaces_already_removed, 1u);
}
```

Write the `makeTableWithRefs` fixture helper at the top of the test file using the raw helpers above; assert in it that `listRefs` sees the expected committed count before returning (self-checking fixture).

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t2.log 2>&1; tail -5 build_debug/build_cas_decommission_t2.log
```
Expected: compile FAILURE — `decommissionPoolMember` undefined.

- [ ] **Step 3: Implement**

3a. `CasEvent.h`: add `MemberDecommission` to the enum (`CasEvent.h:26` row, next to `MountClaim`); `CasEvent.cpp`: `case CasEventType::MemberDecommission: return "member_decommission";`.

3b. `dropNamespace` stats: change the signature at `CasStore.h:409` to return `DropNamespaceStats` (struct declared just above it). Inside the implementation the removal transaction already enumerates every committed owner and precommit binding — count them where the exact `owner_transition(old, none)` ops are built and return the counts. Compile-fix callers (`ContentAddressedMetadataStorage.cpp`, `CachedPartFolderAccess.cpp`, tests) by ignoring the return value — no behavior change.

3c. `Core/CasDecommission.h` — the header exactly as in **Interfaces** above (include `CasStore.h` types; document each field with `///`).

3d. `Core/CasDecommission.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
#include <Common/logger_useful.h>

namespace DB::Cas
{

DecommissionReport decommissionPoolMember(BackendPtr backend, PoolConfig config,
                                          const String & victim_srid, const CasEventSink & sink)
{
    DecommissionReport report;
    report.srid = victim_srid;

    StorePtr admin = Store::openForDecommission(std::move(backend), std::move(config), victim_srid);
    if (sink)
        admin->setEventSink(sink);   /// verify the setter name at the ContentAddressedMetadataStorage wiring site

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "begin";
        e.reason = "operator decommission of pool member";
        e.detail = {{"srid", victim_srid}};
    });

    /// Phase: namespace erasure. `listNamespaces` unions `cas/refs/` and `roots/`; only entries with
    /// a present ref table are droppable tables — a roots-only entry is mountpoint debris handled by
    /// the roots sweep (Task 3).
    for (const String & ns_str : admin->listNamespaces(victim_srid))
    {
        const RootNamespace ns(ns_str);
        if (admin->namespaceIsRemoved(ns))
        {
            ++report.namespaces_already_removed;
            continue;
        }
        const auto ref_objects = admin->backend().list(admin->layout().refsNamespacePrefix(ns), /*cursor=*/"", /*limit=*/1);
        if (ref_objects.entries.empty())
            continue;   /// not a table (roots-only listing entry)

        const auto stats = admin->dropNamespace(ns);
        ++report.namespaces_removed;
        report.committed_refs_removed += stats.committed_refs;
        report.precommits_removed += stats.precommits;
        report.edge_deltas_emitted += stats.committed_refs + stats.precommits;

        EventEmitter{*admin}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::MemberDecommission;
            e.outcome = "namespace_removed";
            e.reason = "decommission dropped a victim namespace";
            e.detail = {{"srid", victim_srid}, {"namespace", ns_str},
                        {"committed", std::to_string(stats.committed_refs)},
                        {"precommits", std::to_string(stats.precommits)}};
        });
    }

    EventEmitter{*admin}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::MemberDecommission;
        e.outcome = "end";
        e.reason = "decommission finished";
        e.detail = {{"srid", victim_srid},
                    {"namespaces_removed", std::to_string(report.namespaces_removed)},
                    {"warnings", std::to_string(report.warnings.size())}};
    });
    return report;
}

}
```

Adjust `Backend::list` usage to the real `ListPage` shape (`CasBackend.h:177`) — field names may differ (`entries`/`items`); follow an existing caller (GC discovery in `CasGc.cpp`).

- [ ] **Step 4: Run tests**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t2.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission*' > build_debug/test_cas_decommission_t2.log 2>&1; tail -3 build_debug/test_cas_decommission_t2.log
```
Expected: PASS (including Task 1 tests).

- [ ] **Step 5: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core src/Disks/tests/gtest_cas_decommission.cpp
git commit -m "cas: \`decommissionPoolMember\` core — namespace erasure with report + \`member_decommission\` events

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: drain sweeps — manifest debris, staging, roots

**Files:**
- Modify: `Core/CasDecommission.cpp` (three phases inserted after namespace erasure, before the `end` event)
- Modify: `Core/CasOrphanManifestSweep.h` / `.cpp` (`sweepNamespace` returns `uint64_t` deleted count; existing callers ignore it)
- Test: `src/Disks/tests/gtest_cas_decommission.cpp`

**Interfaces:**
- Consumes: `sweepNamespace(Store &, const RootNamespace &, const BuildPrefix &)` (`CasOrphanManifestSweep.h`), `Layout::parseManifestKey` (`CasLayout.h:251`), `Layout::mountpointObjectKey` (`CasLayout.h:312`), staging prefix shape `<pool_prefix>/staging/<srid>/` (`ContentAddressedMetadataStorage.cpp:541-548`).
- Produces: the three `DecommissionReport` counters `manifest_debris_removed`, `staging_objects_removed`, `mountpoint_objects_removed` filled.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CasDecommission, DrainsDebrisStagingAndRoots)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
        /// Pre-precommit manifest debris: a staged manifest body never named by any owner. Reuse the
        /// orphan-sweep fixture idiom (see the gtest that covers `sweepNamespace` — seed via the same
        /// manifest-body writer it uses, under ns "victim/db/t1", the victim's writer_epoch, a build
        /// sequence with no owner event).
        seedOrphanManifestBody(*victim, "victim/db/t1");
    }
    /// Foreign staging + mountpoint objects, written raw (no writer machinery needed):
    backend->putIfAbsent("p/staging/victim/upload1.tmp", "x");
    backend->putIfAbsent("p/staging/victim/upload2.tmp", "x");
    backend->putIfAbsent("p/roots/victim/clickhouse_access_check_abc", "x");

    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");

    EXPECT_EQ(report.manifest_debris_removed, 1u);
    EXPECT_EQ(report.staging_objects_removed, 2u);
    EXPECT_EQ(report.mountpoint_objects_removed, 1u);
    EXPECT_TRUE(report.warnings.empty());

    /// Nothing of the victim remains under staging/ or roots/ (scoped LISTs are empty).
    EXPECT_TRUE(backend->list("p/staging/victim/", "", 10).entries.empty());
    EXPECT_TRUE(backend->list("p/roots/victim/", "", 10).entries.empty());
}
```

Write `seedOrphanManifestBody` by copying the manifest-body seeding used by the existing orphan-sweep gtest (find it: `grep -l sweepNamespace src/Disks/tests/*.cpp`); assert inside the helper that the body exists after seeding. A no-owner survivor is what the sweep deletes — do NOT precommit it.

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t3.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission.DrainsDebrisStagingAndRoots' > build_debug/test_cas_decommission_t3.log 2>&1; tail -3 build_debug/test_cas_decommission_t3.log
```
Expected: FAIL — counters are 0, objects still listed.

- [ ] **Step 3: Implement the three phases** (insert into `decommissionPoolMember` after the namespace loop)

```cpp
    /// Phase: manifest-debris drain. MUST precede slot deletion: deleting the mount body destroys the
    /// watermark authority (`floorForNamespace` → nullopt → "not eligible") and would strand this
    /// debris forever (spec §core step 4). We are the epoch authority here — every old-epoch prefix
    /// is eligible (`prefix.writer_epoch < w.writer_epoch`, CasOrphanManifestSweep.cpp:156).
    {
        const String debris_prefix = admin->layout().casManifestsPrefix() + victim_srid;
        std::set<std::pair<String, BuildPrefix>> groups;
        forEachListedKey(admin->backend(), debris_prefix, [&](const String & key, const Token &)
        {
            if (const auto parsed = admin->layout().parseManifestKey(key))
                groups.emplace(parsed->root_namespace.string(),
                               BuildPrefix{parsed->ref.writer_epoch, parsed->ref.build_sequence});
        });
        for (const auto & [ns_str, prefix] : groups)
            report.manifest_debris_removed += sweepNamespace(*admin, RootNamespace(ns_str), prefix);
    }

    /// Phase: staging sweep. Backend-level scoped LIST+delete of `<pool>/staging/<srid>/` — the same
    /// prefix `sweepOwnMountStaging` owns (`ContentAddressedMetadataStorage.cpp:541`); that helper
    /// takes `IObjectStorage`, unavailable at this layer, and the victim's writers are fenced, so a
    /// plain exact-token delete of every listed object is race-free.
    report.staging_objects_removed += deleteListedPrefix(
        admin->backend(), admin->poolConfig().pool_prefix + "/staging/" + victim_srid + "/", report.warnings);

    /// Phase: roots sweep. Mountpoint objects `roots/<srid>/…` (loose, token-less semantics; listed
    /// tokens still gate the delete against nothing-in-particular — misses are counted as warnings).
    report.mountpoint_objects_removed += deleteListedPrefix(
        admin->backend(), admin->poolConfig().pool_prefix + "/roots/" + victim_srid + "/", report.warnings);
```

Helpers (file-local, `CasDecommission.cpp`):

```cpp
namespace
{

/// Paged LIST over `prefix`, invoking `fn(key, token)` per object.
template <typename F>
void forEachListedKey(Backend & backend, const String & prefix, F && fn)
{
    String cursor;
    for (;;)
    {
        auto page = backend.list(prefix, cursor, /*limit=*/1000);
        for (const auto & entry : page.entries)
            fn(entry.key, entry.token);
        if (page.next_cursor.empty())
            break;
        cursor = page.next_cursor;
    }
}

/// Delete every object under `prefix` by its listed token. A per-object failure (token mismatch,
/// transient error) is recorded as a warning and the sweep continues — the caller keeps the slot on
/// non-empty warnings (fail-close, spec §core). Returns the count actually deleted.
uint64_t deleteListedPrefix(Backend & backend, const String & prefix, std::vector<String> & warnings)
{
    uint64_t deleted = 0;
    forEachListedKey(backend, prefix, [&](const String & key, const Token & token)
    {
        try
        {
            backend.deleteExact(key, token);
            ++deleted;
        }
        catch (...)
        {
            warnings.push_back("delete failed: " + key + ": " + getCurrentExceptionMessage(false));
        }
    });
    return deleted;
}

}
```

Adapt `ListPage` field names (`entries`, `next_cursor`, `entry.key`, `entry.token`) to the real `CasBackend.h` shape and the `deleteExact` outcome type (it returns `DeleteOutcome` — treat `TokenMismatch`/`NotFound` outcomes as warnings too, not only exceptions). Also change `sweepNamespace` to return its deleted-body count (`CasOrphanManifestSweep.h/.cpp`; existing caller `CasGc.cpp::orphanPartManifestSweep` ignores the return).

- [ ] **Step 4: Run tests**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t3.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission*' > build_debug/test_cas_decommission_t3.log 2>&1; tail -3 build_debug/test_cas_decommission_t3.log
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core src/Disks/tests/gtest_cas_decommission.cpp
git commit -m "cas: decommission drain phases — manifest debris, staging, roots sweeps

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: slot retirement, fail-close, resume

**Files:**
- Modify: `Core/CasDecommission.cpp`
- Test: `src/Disks/tests/gtest_cas_decommission.cpp`

**Interfaces:**
- Consumes: `Layout::mountKey/ownerKey/epochKey` (`CasLayout.h:415-431`); the admin store's graceful close (farewell stamp — `CasServerRoot.cpp:845`).
- Produces: `report.slot_removed`; the final semantics "warnings non-empty ⇒ slot kept terminated"; a completed decommission makes a re-run throw `BAD_ARGUMENTS` (unknown member).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(CasDecommission, RemovesSlotAndMakesRerunUnknown)
{
    auto backend = std::make_shared<InMemoryBackend>();
    {
        auto victim = openVictim(backend);
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    const auto report = decommissionPoolMember(
        backend, PoolConfig{.pool_prefix = "p", .server_root_id = "admin"}, "victim");
    EXPECT_TRUE(report.slot_removed);
    EXPECT_FALSE(backend->get("p/gc/server-roots/victim/mount").has_value());
    EXPECT_FALSE(backend->get("p/gc/server-roots/victim/owner").has_value());
    EXPECT_FALSE(backend->get("p/gc/server-roots/victim/epoch").has_value());

    expectThrowsCode(ErrorCodes::BAD_ARGUMENTS, [&]
    {
        decommissionPoolMember(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "a2"}, "victim");
    });
}

/// Backend wrapper failing every delete under one prefix — drives the fail-close path.
class FailDeletesUnderPrefixBackend : public /* delegate-to-InMemoryBackend decorator */
{
    /// Delegate every Backend method to the wrapped InMemoryBackend; deleteExact throws an
    /// ErrorCodes::S3_ERROR-class exception when key starts with `fail_prefix` and `armed` is true.
    /// `void disarm()` clears `armed` (the resume half of the test).
};

TEST(CasDecommission, FailedDrainKeepsSlotThenResumes)
{
    auto inner = std::make_shared<InMemoryBackend>();
    {
        auto victim = Store::open(inner, PoolConfig{.pool_prefix = "p", .server_root_id = "victim"});
        makeTableWithRefs(*victim, "victim/db/t1", 1, 0);
    }
    inner->putIfAbsent("p/roots/victim/loose_file", "x");

    auto failing = std::make_shared<FailDeletesUnderPrefixBackend>(inner, "p/roots/victim/");
    const auto first = decommissionPoolMember(
        failing, PoolConfig{.pool_prefix = "p", .server_root_id = "a1"}, "victim");
    EXPECT_FALSE(first.warnings.empty());
    EXPECT_FALSE(first.slot_removed);
    EXPECT_TRUE(inner->get("p/gc/server-roots/victim/mount").has_value());   /// slot kept — resume anchor

    failing->disarm();
    const auto second = decommissionPoolMember(
        failing, PoolConfig{.pool_prefix = "p", .server_root_id = "a2"}, "victim");
    EXPECT_TRUE(second.warnings.empty());
    EXPECT_TRUE(second.slot_removed);
    EXPECT_EQ(second.namespaces_already_removed, 1u);
    EXPECT_EQ(second.mountpoint_objects_removed, 1u);
}
```

Write `FailDeletesUnderPrefixBackend` fully in the test file: a `Backend` subclass holding `std::shared_ptr<InMemoryBackend> inner`, forwarding every pure-virtual member (copy the list from `CasBackend.h`), with `deleteExact` throwing when armed and the key matches.

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t4.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission.RemovesSlot*:CasDecommission.FailedDrain*' > build_debug/test_cas_decommission_t4.log 2>&1; tail -3 build_debug/test_cas_decommission_t4.log
```
Expected: FAIL — `slot_removed` is false / mount object still present.

- [ ] **Step 3: Implement slot retirement** (in `decommissionPoolMember`, after the sweeps, before the `end` event)

```cpp
    /// Phase: slot retirement — STRICTLY LAST, and only over a clean drain (fail-close: an
    /// unconfirmed drain keeps the resume anchor; the operator re-runs).
    const Layout layout = admin->layout();
    Backend & raw_backend = admin->backend();
    if (report.warnings.empty())
    {
        /// Graceful close of the admin store: the keeper stamps the lease already-expired + the
        /// watermark farewell (`min_active = UINT64_MAX`) — the `terminated` state.
        admin.reset();

        /// Delete the control objects; the mount body LAST (it is the claim/resume anchor).
        std::vector<String> slot_keys = {layout.epochKey(victim_srid), layout.ownerKey(victim_srid),
                                         layout.mountKey(victim_srid)};
        bool all_deleted = true;
        for (const String & key : slot_keys)
        {
            if (const auto got = raw_backend.get(key))
            {
                try
                {
                    raw_backend.deleteExact(key, got->token);
                }
                catch (...)
                {
                    report.warnings.push_back("slot delete failed: " + key + ": "
                                              + getCurrentExceptionMessage(false));
                    all_deleted = false;
                    break;   /// keep the remaining keys — mount survives ⇒ resume works
                }
            }
        }
        report.slot_removed = all_deleted;
    }
    else
    {
        report.slot_removed = false;
        LOG_WARNING(getLogger("CasDecommission"),
            "CAS decommission '{}': drain incomplete ({} warnings) — mount slot kept (terminated); "
            "re-run the command to finish", victim_srid, report.warnings.size());
        admin.reset();   /// graceful close still stamps the farewell — the slot reads `terminated`
    }
```

CAUTION — the `end` event uses `EventEmitter{*admin}` but `admin` is reset above: restructure so the `end` event is emitted BEFORE `admin.reset()` (move the counters emission up, or emit via the sink directly). `raw_backend` must remain valid after `admin.reset()` — take the `BackendPtr` (`admin->poolBackendPtr()`, add the accessor: `BackendPtr poolBackendPtr() const { return pool_backend; }`) BEFORE resetting, not a reference into the dead store.

- [ ] **Step 4: Run the full decommission suite**

```bash
ninja -C build_debug unit_tests_dbms > build_debug/build_cas_decommission_t4.log 2>&1 && \
build_debug/src/unit_tests_dbms --gtest_filter='CasDecommission*' > build_debug/test_cas_decommission_t4.log 2>&1; tail -3 build_debug/test_cas_decommission_t4.log
build_debug/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build_debug/test_cas_all_t4.log 2>&1; tail -3 build_debug/test_cas_all_t4.log
```
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core src/Disks/tests/gtest_cas_decommission.cpp
git commit -m "cas: decommission slot retirement — farewell + control-object deletion, fail-close resume

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: SQL surface — `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER`

**Files:**
- Modify: `src/Parsers/ASTSystemQuery.h` (Type enum + formatImpl), `src/Parsers/ASTSystemQuery.cpp`
- Modify: `src/Parsers/ParserSystemQuery.cpp`
- Modify: `src/Access/Common/AccessType.h:335` area
- Modify: `tests/queries/0_stateless/01271_show_privileges.reference`
- Modify: `src/Interpreters/InterpreterSystemQuery.cpp`
- Test: `tests/queries/0_stateless/0NNNN_system_content_addressed_drop_pool_member.sql`

**Interfaces:**
- Consumes: `Cas::decommissionPoolMember` (Task 2 signature); `ContentAddressedMetadataStorage::store()`; `Store::poolBackendPtr` + `Store::poolConfig` (Task 4 / existing).
- Produces: `ASTSystemQuery::Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER`; srid carried in the existing `query.replica` field, disk in `query.disk`.

- [ ] **Step 1: Write the failing stateless test**

```bash
./tests/queries/0_stateless/add-test system_content_addressed_drop_pool_member
```

`.sql`:

```sql
-- Grammar + dispatch only: execution needs a CA disk (covered by the integration test).
SYSTEM CONTENT ADDRESSED DROP POOL MEMBER 'srv1' FROM DISK 'no_such_disk'; -- { serverError UNKNOWN_DISK }
```

`.reference`: empty.

- [ ] **Step 2: Run to verify failure**

```bash
python3 -m ci.praktika run "stateless" --test 0NNNN_system_content_addressed_drop_pool_member > build_debug/test_stateless_capm.log 2>&1; tail -5 build_debug/test_stateless_capm.log
```
Expected: FAIL with `SYNTAX_ERROR` (the grammar does not exist yet). If no server binary is wired for praktika (`ci/tmp/clickhouse` symlink — see `docs/superpowers` praktika notes / memory `reference_praktika_local_runs`), build `clickhouse` first: `ninja -C build_debug clickhouse > build_debug/build_ch_t5.log 2>&1`.

- [ ] **Step 3: Implement**

3a. `ASTSystemQuery.h`: add `CONTENT_ADDRESSED_DROP_POOL_MEMBER,` to the `Type` enum (near `JEMALLOC_FLUSH_PROFILE`, `ASTSystemQuery.h:68`). magic_enum auto-derives the keyword sequence `CONTENT ADDRESSED DROP POOL MEMBER` (the `SYNC FILESYSTEM CACHE` / `JEMALLOC …` mechanism, `ASTSystemQuery.cpp:22-40`).

3b. `ASTSystemQuery.cpp` `formatImpl`: add a branch printing the arguments back (round-trip formatting):

```cpp
    else if (type == Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER)
    {
        ostr << ' ' << quoteString(replica) << ' ';
        print_keyword("FROM DISK");
        ostr << ' ' << quoteString(disk);
    }
```

Match the surrounding style (some branches use `print_identifier`/`print_keyword` helpers — follow the `DROP_REPLICA` branch as the model).

3c. `ParserSystemQuery.cpp`: add a case (next to `Type::SYNC_FILESYSTEM_CACHE`, `ParserSystemQuery.cpp:735`):

```cpp
        case Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER:
        {
            ASTPtr ast;
            if (!ParserStringLiteral{}.parse(pos, ast, expected))
                return false;
            res->replica = ast->as<ASTLiteral &>().value.safeGet<String>();
            if (!ParserKeyword{Keyword::FROM}.ignore(pos, expected))
                return false;
            if (!ParserKeyword{Keyword::DISK}.ignore(pos, expected))
                return false;
            if (!ParserStringLiteral{}.parse(pos, ast, expected))
                return false;
            res->disk = ast->as<ASTLiteral &>().value.safeGet<String>();
            break;
        }
```

(`Keyword::FROM` and `Keyword::DISK` exist in `Parsers/CommonParsers.h`; verify and use `ParserKeyword{"FROM DISK"}`-style single keyword only if the two-keyword form fails to resolve.)

3d. `AccessType.h` (`:335` area, inside the SYSTEM group):

```cpp
    M(SYSTEM_CONTENT_ADDRESSED_DROP_POOL_MEMBER, "CONTENT ADDRESSED DROP POOL MEMBER", GLOBAL, SYSTEM) \
```

Update `tests/queries/0_stateless/01271_show_privileges.reference` — add the matching row (run the test locally once to get the exact expected line: it lists `privilege`, aliases, level, parent).

3e. `InterpreterSystemQuery.cpp`: add the case (model: `CLEAR_DISK_METADATA_CACHE` at `:660` for disk resolution + the `SYNC_FILESYSTEM_CACHE` result-block pattern at `:657`):

```cpp
        case Type::CONTENT_ADDRESSED_DROP_POOL_MEMBER:
        {
            getContext()->checkAccess(AccessType::SYSTEM_CONTENT_ADDRESSED_DROP_POOL_MEMBER);
            auto disk = getContext()->getDisk(query.disk);
            auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
            auto * ca = dos ? dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get()) : nullptr;
            if (!ca)
                throw Exception(ErrorCodes::BAD_ARGUMENTS,
                    "SYSTEM CONTENT ADDRESSED DROP POOL MEMBER: disk '{}' is not content-addressed", query.disk);

            const auto host_store = ca->store();
            const auto report = Cas::decommissionPoolMember(
                host_store->poolBackendPtr(), host_store->poolConfig(), query.replica);

            /// One-row summary result set (precedent: SYNC_FILESYSTEM_CACHE / JEMALLOC_FLUSH_PROFILE).
            Block block{
                ColumnWithTypeAndName(DataTypeString().createColumnConst(1, report.srid)->convertToFullColumnIfConst(), std::make_shared<DataTypeString>(), "srid"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.namespaces_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "namespaces_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.namespaces_already_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "namespaces_already_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.committed_refs_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "committed_refs_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.precommits_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "precommits_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.manifest_debris_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "manifest_debris_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.staging_objects_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "staging_objects_removed"),
                ColumnWithTypeAndName(DataTypeUInt64().createColumnConst(1, report.mountpoint_objects_removed)->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt64>(), "mountpoint_objects_removed"),
                ColumnWithTypeAndName(DataTypeUInt8().createColumnConst(1, static_cast<UInt64>(report.slot_removed))->convertToFullColumnIfConst(), std::make_shared<DataTypeUInt8>(), "slot_removed"),
                ColumnWithTypeAndName(DataTypeString().createColumnConst(1, boost::algorithm::join(report.warnings, "; "))->convertToFullColumnIfConst(), std::make_shared<DataTypeString>(), "warnings"),
            };
            auto source = std::make_shared<SourceFromSingleChunk>(std::make_shared<const Block>(block.cloneEmpty()), Chunk(block.getColumns(), 1));
            result.pipeline = QueryPipeline(std::move(source));
            break;
        }
```

Copy the exact `SourceFromSingleChunk` construction idiom from `:657` (it may differ in Block/Chunk handling — the idiom there compiles, ours must match it). Add the case to `getRequiredAccessForDDLOnCluster` (the access-listing switch near `:2555`) returning `AccessType::SYSTEM_CONTENT_ADDRESSED_DROP_POOL_MEMBER`. Includes: `CasDecommission.h`, `DiskObjectStorage.h`, `ContentAddressedMetadataStorage.h` (guard consistency with how `CommandCaGcDryRun.cpp` includes them).

- [ ] **Step 4: Run tests**

```bash
ninja -C build_debug clickhouse > build_debug/build_ch_t5.log 2>&1 && \
python3 -m ci.praktika run "stateless" --test "0NNNN_system_content_addressed_drop_pool_member 01271_show_privileges" > build_debug/test_stateless_capm.log 2>&1; tail -5 build_debug/test_stateless_capm.log
```
Expected: both PASS (analyze log with a subagent).

- [ ] **Step 5: Commit**

```bash
git add src/Parsers src/Access src/Interpreters tests/queries/0_stateless/0NNNN_system_content_addressed_drop_pool_member.* tests/queries/0_stateless/01271_show_privileges.reference
git commit -m "cas: \`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER\` — grammar, privilege, interpreter with one-row report

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: disks facade — `ca-drop-member`

**Files:**
- Create: `programs/disks/CommandCaDropMember.cpp`
- Modify: `programs/disks/DisksApp.cpp:341-343` area (registration), `programs/disks/CMakeLists.txt:19-21` area
- Test: manual smoke via the local disks binary (no CI job for disks commands; the integration test in Task 7 covers the shared core end-to-end)

**Interfaces:**
- Consumes: `Cas::decommissionPoolMember`, `ContentAddressedMetadataStorage::store()` — same call shape as Task 5.

- [ ] **Step 1: Implement** (pattern: `CommandCaGcDryRun.cpp`, argument handling: copy the positional-argument idiom from `CommandCaInspect.cpp`)

```cpp
#include <Disks/DiskObjectStorage/DiskObjectStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasDecommission.h>
#include <Common/Exception.h>
#include <ICommand.h>

#include <iostream>

namespace DB
{

namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
}

class CommandCaDropMember final : public ICommand
{
public:
    CommandCaDropMember() : ICommand("CommandCaDropMember")
    {
        command_name = "ca-drop-member";
        description = "Decommission a DEAD pool member: erase its namespaces, debris, staging, roots "
                      "objects and mount slot. Refuses a live member. Open the CA disk read-only "
                      "(the admin claim is made internally).";
        options_description.add_options()("member", po::value<String>(), "server_root_id of the dead member");
        positional_options_description.add("member", 1);
    }

    void executeImpl(const CommandLineOptions & options, DisksClient & client) override
    {
        const String srid = getValueFromCommandLineOptionsThrow<String>(options, "member");
        auto disk = client.getCurrentDiskWithPath().getDisk();

        auto * dos = dynamic_cast<DiskObjectStorage *>(disk.get());
        auto * ca = dos ? dynamic_cast<ContentAddressedMetadataStorage *>(dos->getMetadataStorage().get()) : nullptr;
        if (!ca)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-drop-member: disk '{}' is not content-addressed", disk->getName());
        if (!ca->isReadOnly())
            throw Exception(ErrorCodes::BAD_ARGUMENTS,
                "ca-drop-member: open the CA disk read-only (a writable open would claim this tool's "
                "own srid; the decommission claim happens internally)");

        const auto host_store = ca->store();
        const auto report = Cas::decommissionPoolMember(
            host_store->poolBackendPtr(), host_store->poolConfig(), srid);

        std::cout << "srid=" << report.srid << "\n"
                  << "namespaces_removed=" << report.namespaces_removed << "\n"
                  << "namespaces_already_removed=" << report.namespaces_already_removed << "\n"
                  << "committed_refs_removed=" << report.committed_refs_removed << "\n"
                  << "precommits_removed=" << report.precommits_removed << "\n"
                  << "manifest_debris_removed=" << report.manifest_debris_removed << "\n"
                  << "staging_objects_removed=" << report.staging_objects_removed << "\n"
                  << "mountpoint_objects_removed=" << report.mountpoint_objects_removed << "\n"
                  << "slot_removed=" << (report.slot_removed ? "true" : "false") << "\n";
        for (const auto & w : report.warnings)
            std::cout << "warning=" << w << "\n";
    }
};

CommandPtr makeCommandCaDropMember()
{
    return std::make_shared<DB::CommandCaDropMember>();
}

}
```

Verify the exact options/positional idiom against `CommandCaInspect.cpp` (option helpers differ across commands; copy a compiling shape). Register: `command_descriptions.emplace("ca-drop-member", makeCommandCaDropMember());` in `DisksApp.cpp:343` area + `CommandCaDropMember.cpp` in `programs/disks/CMakeLists.txt`.

- [ ] **Step 2: Build + smoke**

```bash
ninja -C build_debug clickhouse > build_debug/build_ch_t6.log 2>&1 && \
build_debug/programs/clickhouse disks --help 2>&1 | grep ca-drop-member
```
Expected: the command listed with its description. (A functional pool run happens in Task 7.)

- [ ] **Step 3: Commit**

```bash
git add programs/disks/CommandCaDropMember.cpp programs/disks/DisksApp.cpp programs/disks/CMakeLists.txt
git commit -m "cas: \`clickhouse-disks ca-drop-member\` — disks facade for pool-member decommission

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: integration test — kill a replica, drop it, verify the pool heals

**Files:**
- Create: `tests/integration/test_content_addressed_drop_pool_member/__init__.py` (empty), `configs/` (storage config), `test.py`

**Interfaces:**
- Consumes: the SQL command (Task 5). Model EVERYTHING (cluster fixture, `with_rustfs`, CA storage configs, GC driving, fsck helpers) on `tests/integration/test_content_addressed_shared_pool/` — same two-node shape, same config style. RustFS, not MinIO (memory: MinIO cannot serve CA pools).

- [ ] **Step 1: Write the test**

`test.py` skeleton (adapt fixture names to the model test verbatim — cluster setup code must be copied from `test_content_addressed_shared_pool/test.py`, not invented):

```python
# Flow:
# 1. Two nodes (node1, node2), each with a CA disk on the SAME rustfs pool, distinct server_root_id
#    (srid1, srid2) — exactly the shared-pool model test's topology.
# 2. On node2: CREATE TABLE t2 (a MergeTree on the CA disk), INSERT enough rows for several parts.
# 3. On node1: CREATE TABLE t1 likewise, INSERT (this data must SURVIVE).
# 4. Hard-kill node2 (cluster.stop_node / kill -9 container semantics — no graceful farewell).
# 5. On node1, wait until system.content_addressed_mounts shows node2's srid as expired/fenced
#    (assert_eq_with_retry over SELECT state FROM system.content_addressed_mounts WHERE srid = srid2).
# 6. Run: SYSTEM CONTENT ADDRESSED DROP POOL MEMBER '<srid2>' FROM DISK '<ca_disk>' on node1.
#    Assert the one-row result: namespaces_removed >= 1, slot_removed = 1, warnings = ''.
# 7. Assert node1's t1 SELECT count unchanged.
# 8. Assert srid2 is GONE from system.content_addressed_mounts.
# 9. Drive GC to reclaim node2's blobs: the model test has the idiom for forcing/awaiting GC rounds;
#    then run the fsck check the ca-soak/fsck pattern uses (clickhouse-disks -C fsck-only config) and
#    assert 0 dangling / 0 unaccounted for the pool.
# 10. Re-run the same SYSTEM command → expect BAD_ARGUMENTS "unknown pool member".
```

Every helper referenced (retry-assert, GC forcing, fsck invocation) must be copied from the model test / `utils/ca-soak` fsck pattern — if the model test lacks a GC-forcing idiom, drop step 9's fsck gate to "blob count under `blobs/` decreases after GC rounds" using the rustfs LIST helper the model test uses.

- [ ] **Step 2: Run**

```bash
python3 -m ci.praktika run "integration" --test test_content_addressed_drop_pool_member > build_debug/test_integration_capm.log 2>&1; tail -20 build_debug/test_integration_capm.log
```
Expected: PASS (analyze the log with a subagent; on infra flakes re-run once before debugging).

- [ ] **Step 3: Commit**

```bash
git add tests/integration/test_content_addressed_drop_pool_member
git commit -m "cas: integration test — kill a replica, \`SYSTEM CONTENT ADDRESSED DROP POOL MEMBER\`, pool heals

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: documentation + backlog closure

**Files:**
- Modify: `docs/superpowers/cas/04-gc-protocol.md` (offline-replica/decommission subsection)
- Modify: `docs/superpowers/cas/05-formats-and-backend.md` (B200 entry)
- Modify: `docs/superpowers/cas/BACKLOG.md` (B200 status)
- Modify: `docs/superpowers/specs/2026-07-13-cas-pool-member-decommission-design.md` (Status → implemented)

**Steps:**

- [ ] **Step 1:** In `04-gc-protocol.md`, add a short subsection (with `{#anchor}`) near the heartbeat-fence section: a dead member is fenced (liveness) but its footprint (frozen watermark, stale precommits, staging, roots, slot) is erased only by the deliberate `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER` decommission — link the spec. State explicitly that decommission is a WRITER-role operation (`GC` still never invents a ref transition).
- [ ] **Step 2:** In `05-formats-and-backend.md`, update the two B200 mentions (`:609`, `:647`): implemented WITHOUT the roster; grammar renamed to `SYSTEM CONTENT ADDRESSED DROP POOL MEMBER`; the roster forward-hook (also remove the member's roster entry) noted for Part IV.
- [ ] **Step 3:** Update `BACKLOG.md`: B200 → DONE (pointer to spec + this plan) and add ONE follow-up entry — "ca-soak scenario card: decommission under load + chaos variant (kill the command mid-run, resume)" per spec §testing. Stamp the spec's `**Status:**` line implemented with the landing commit hashes.
- [ ] **Step 4: Commit**

```bash
git add docs/superpowers
git commit -m "cas: docs — pool-member decommission landed (B200 without roster)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

## Verification (after all tasks)

1. `build_debug/src/unit_tests_dbms --gtest_filter='Cas*:Ca*'` — full CAS gtest suite green.
2. Stateless: the new test + `01271_show_privileges` green.
3. Integration: `test_content_addressed_drop_pool_member` green.
4. Grep hygiene: `grep -rn "FORCE" Core/CasDecommission.cpp` — no FORCE path exists; `grep -n "invent" Core/CasGc.h` — the GC-never-invents comment untouched.

## Known verification points for the implementer (not placeholders — check-and-adapt sites)

- `Backend::ListPage` field names and `DeleteOutcome` handling — follow existing callers in `CasGc.cpp`.
- `Store::setEventSink` — exact name at the `ContentAddressedMetadataStorage` wiring site (`CasStore.cpp:353` comment names it).
- `ParserKeyword{Keyword::DISK}` availability — else use `ParserKeyword{"DISK"}` / a combined `"FROM DISK"` keyword, matching how other cases spell multi-word keywords.
- The `SourceFromSingleChunk` construction — copy the compiling idiom from `InterpreterSystemQuery.cpp:657` verbatim.
- `positional_options_description` idiom in disks commands — copy from `CommandCaInspect.cpp`.

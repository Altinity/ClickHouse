# CAS forced relink on fetch — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `ReplicatedMergeTree` fetch whose sender holds the part on a content-addressed pool that the receiving table's storage policy also has a (non-read-only) disk for always relinks onto that disk, ahead of the policy's volume order and of TTL move rules.

**Architecture:** The receiver advertises the SET of its policy's pool ids in the existing `cas_pool_uuid` request parameter (same `", "` list form as the zero-copy `remote_fs_metadata` capability list); the sender matches its part's pool against the set and names the match in a `cas_pool_uuid` response cookie; the receiver resolves the forced disk from that cookie BEFORE reserving space, reserves on it directly, and the existing relink block runs unchanged. All routing decisions are pure functions in a new small pair `DataPartsExchangeCasRouting.{h,cpp}` with gtest vectors; the sender's confirm routing uses the same pair to deduplicate aliases of one mount. No protocol version bump, no setting, no new interface method. A pool disk that is not live is still the forced target and fails closed (the fetch throws, the queue retries).

**Tech Stack:** C++23 (ClickHouse tree, Allman braces), gtest (`unit_tests_dbms`, suite names MUST start with `CAS`), ClickHouse integration tests (pytest, `tests/integration/test_cas_replicated_relink`, two nodes over RustFS + ZooKeeper), praktika local runner.

**Spec:** `docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md` (revision 3, commit `8d514d74a03`). Read it first; every task below cites its sections.

## Global Constraints

- Branch: `cas-gc-rebuild`, a SHARED checkout used by other sessions concurrently. NEVER `git add X && git commit`. Always: `git diff --cached --stat` (must show nothing foreign), then `git add <your new files>` if any, then `git commit -m "..." -- <exact paths>`, then `git branch --show-current && git log -1 --stat`. No rebase, no amend, no push.
- Commit message trailer (every commit):
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc
  ```
- C++: Allman braces (opening brace on its own line). No `sleep` to fix races. `chassert` is NOT a release fail-close — every defensive exit that must hold in release is a real branch with a log line.
- Comments and docs: functions written as `f`, not `f()`, when naming the function itself; literal names in backticks; comments keep the REASON and carry no plan/backlog/spec provenance ("Task 3", "rev.2", "CAS-134" do not belong in code comments).
- gtest suites for content-addressed code MUST be named `CAS…` — the gate filter is exactly `--gtest_filter='CAS*'`; never widen it.
- Build: `cd build && ninja clickhouse unit_tests_dbms > ninja_<task>.log 2>&1` — no `-j`, no `nproc`; output always redirected into the build dir; have a subagent (cheap model, medium effort) read the log and return only the summary. Confirm the binary is the one you built before trusting a green test (`ls -la --time-style=full-iso build/programs/clickhouse`).
- Tests: every run redirected to `build/test_<name>.log`; a subagent summarizes. Integration: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_replicated_relink > build/test_integration_<task>.log 2>&1` from the repo root (`ci/tmp/clickhouse` → `build/programs/clickhouse`, already symlinked; `--test` takes ONE space-separated list). Unit: `build/src/unit_tests_dbms --gtest_filter='CASRelink*' > build/test_gtest_<task>.log 2>&1`.
- Docs under `docs/`: every header carries an explicit `{#kebab-anchor}`; new files need the frontmatter block.
- No new MergeTree/server setting. No change to `IContentAddressedExchange`. No liveness predicate (spec §decisions, last two rows).
- Log lines that existing tests grep for MUST keep their text: `Sending part {} by relink`, `Relink of part {} onto disk {} finished (no bytes transferred).`, `Download of part {} onto disk {} finished.`, `Failpoint cas_relink_receiver_force_mechanism_failure: abandoning the relink of part {}`.

---

## File structure

| File | Responsibility |
|---|---|
| `src/Storages/MergeTree/DataPartsExchangeCasRouting.h` (new) | Pure routing decisions of the relink exchange: advertise encode/decode, the receiver's forced-candidate resolution, the sender's confirm routing. No disk/HTTP/storage includes. |
| `src/Storages/MergeTree/DataPartsExchangeCasRouting.cpp` (new) | Their implementation. |
| `src/Storages/tests/gtest_cas_relink_pool_routing.cpp` (new) | Unit vectors for the above (suites `CASRelinkPoolAdvertise`, `CASRelinkConfirmRouting`). Picked up by the `gtest*.cpp` glob in `src/CMakeLists.txt:903-908`. |
| `src/Storages/MergeTree/DataPartsExchange.cpp` | Sender gate (`:404-434`) and confirm routing (`:231-244`); receiver advertise (`:693-725`), forced-disk resolution after the cookie reads (`:786-787`), reservation (`:803-855`), relink block post-check (`:926-933`). Two new failpoints. |
| `src/Common/FailPoint.cpp` | Register `cas_relink_sender_omit_pool_cookie` and `cas_relink_receiver_drop_forced_disk`. |
| `tests/integration/test_cas_replicated_relink/configs/storage_conf_tiered.xml` (new) | Policies `cas_tiered` and `cas_two_pools` for node2. |
| `tests/integration/test_cas_replicated_relink/test.py` | Seven new tests (spec §tests), one new helper `part_disk`. |
| `docs/en/antalya/cas/architecture/replication.md` | Gate 1, diagram line, new section "Where a relinked part lands". |
| `docs/superpowers/cas/BACKLOG/replication.md`, `docs/superpowers/cas/BACKLOG/formats-and-storage.md` | Close CAS-134; answer `[mixed-ca-tiered-topology]` for the fetch path. |

---

### Task 1: Pure routing helpers with unit vectors

**Files:**
- Create: `src/Storages/MergeTree/DataPartsExchangeCasRouting.h`
- Create: `src/Storages/MergeTree/DataPartsExchangeCasRouting.cpp`
- Test: `src/Storages/tests/gtest_cas_relink_pool_routing.cpp`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces (used verbatim by Tasks 2 and 3), all in `namespace DB::DataPartsExchange`:
  - `String encodeCasPoolAdvertise(Strings pool_uuids)`
  - `Strings decodeCasPoolAdvertise(const String & text)`
  - `String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie)`
  - `struct CasRelinkCandidate { String disk_name; String pool_uuid; bool read_only = false; };`
  - `std::optional<size_t> resolveForcedCaCandidate(const std::vector<CasRelinkCandidate> &, const Strings & advertised_pools, const String & offered_pool_cookie)`
  - `struct CasConfirmRoutingCandidate { const void * exchange_identity = nullptr; String pool_uuid; bool owns_namespace = false; };`
  - `std::optional<size_t> resolveConfirmRoutingCandidate(const std::vector<CasConfirmRoutingCandidate> &, const String & pool_uuid)`

- [ ] **Step 1: Write the failing gtest**

Create `src/Storages/tests/gtest_cas_relink_pool_routing.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Storages/MergeTree/DataPartsExchangeCasRouting.h>

using namespace DB::DataPartsExchange;
using DB::Strings;

/// ---- the advertise: sort, unique, ", " ----

TEST(CASRelinkPoolAdvertise, EmptySetIsEmptyStringBothWays)
{
    EXPECT_EQ(encodeCasPoolAdvertise({}), "");
    EXPECT_TRUE(decodeCasPoolAdvertise("").empty());
}

TEST(CASRelinkPoolAdvertise, SingleIdIsVerbatim)
{
    /// The one-element wire form must be byte-for-byte the pre-set-advertise form: a sender that compares
    /// the whole parameter with its own pool id must still match.
    EXPECT_EQ(encodeCasPoolAdvertise({"0123abcd"}), "0123abcd");
    EXPECT_EQ(decodeCasPoolAdvertise("0123abcd"), Strings{"0123abcd"});
}

TEST(CASRelinkPoolAdvertise, SortsAndDeduplicates)
{
    EXPECT_EQ(encodeCasPoolAdvertise({"bb", "aa", "bb", "aa"}), "aa, bb");
    EXPECT_EQ(decodeCasPoolAdvertise("aa, bb"), (Strings{"aa", "bb"}));
}

TEST(CASRelinkPoolAdvertise, DropsEmptyIds)
{
    EXPECT_EQ(encodeCasPoolAdvertise({"", "aa", ""}), "aa");
    EXPECT_EQ(encodeCasPoolAdvertise({""}), "");
}

TEST(CASRelinkPoolAdvertise, RoundTripsThreeIds)
{
    const Strings ids{"cc", "aa", "bb"};
    EXPECT_EQ(decodeCasPoolAdvertise(encodeCasPoolAdvertise(ids)), (Strings{"aa", "bb", "cc"}));
}

/// ---- which pool the offer is for ----

TEST(CASRelinkPoolAdvertise, OfferedPoolIsTheCookieWhenPresent)
{
    EXPECT_EQ(resolveOfferedCasPool({"aa", "bb"}, "bb"), "bb");
    EXPECT_EQ(resolveOfferedCasPool({"aa"}, "zz"), "zz");
}

TEST(CASRelinkPoolAdvertise, AbsentCookieMeansTheSingleAdvertisedPool)
{
    EXPECT_EQ(resolveOfferedCasPool({"aa"}, ""), "aa");
}

TEST(CASRelinkPoolAdvertise, AbsentCookieWithSeveralPoolsIsNoPool)
{
    EXPECT_EQ(resolveOfferedCasPool({"aa", "bb"}, ""), "");
    EXPECT_EQ(resolveOfferedCasPool({}, ""), "");
}

/// ---- the receiver's forced candidate ----

static std::vector<CasRelinkCandidate> twoPools()
{
    return {
        {"disk_other", "other", false},
        {"disk_shared", "shared", false},
    };
}

TEST(CASRelinkPoolAdvertise, CookieSelectsTheCandidateOnThatPool)
{
    EXPECT_EQ(resolveForcedCaCandidate(twoPools(), {"other", "shared"}, "shared"), std::optional<size_t>{1});
    EXPECT_EQ(resolveForcedCaCandidate(twoPools(), {"other", "shared"}, "other"), std::optional<size_t>{0});
}

TEST(CASRelinkPoolAdvertise, AbsentCookieWithOneAdvertisedPoolSelectsIt)
{
    const std::vector<CasRelinkCandidate> one{{"disk_shared", "shared", false}};
    EXPECT_EQ(resolveForcedCaCandidate(one, {"shared"}, ""), std::optional<size_t>{0});
}

TEST(CASRelinkPoolAdvertise, AbsentCookieWithTwoAdvertisedPoolsSelectsNothing)
{
    EXPECT_EQ(resolveForcedCaCandidate(twoPools(), {"other", "shared"}, ""), std::nullopt);
}

TEST(CASRelinkPoolAdvertise, UnknownPoolSelectsNothing)
{
    EXPECT_EQ(resolveForcedCaCandidate(twoPools(), {"other", "shared"}, "zz"), std::nullopt);
}

TEST(CASRelinkPoolAdvertise, ReadOnlyCandidateIsSkipped)
{
    const std::vector<CasRelinkCandidate> ro{{"disk_shared_ro", "shared", true}};
    EXPECT_EQ(resolveForcedCaCandidate(ro, {"shared"}, "shared"), std::nullopt);

    const std::vector<CasRelinkCandidate> ro_then_rw{{"disk_shared_ro", "shared", true}, {"disk_shared", "shared", false}};
    EXPECT_EQ(resolveForcedCaCandidate(ro_then_rw, {"shared"}, "shared"), std::optional<size_t>{1});
}

TEST(CASRelinkPoolAdvertise, EmptyPoolIdNeverMatches)
{
    const std::vector<CasRelinkCandidate> not_started{{"disk_cold", "", false}};
    EXPECT_EQ(resolveForcedCaCandidate(not_started, {}, ""), std::nullopt);
    EXPECT_EQ(resolveForcedCaCandidate(not_started, {""}, ""), std::nullopt);
}

TEST(CASRelinkPoolAdvertise, TwoCandidatesOnOnePoolTakeTheFirst)
{
    const std::vector<CasRelinkCandidate> two{{"disk_a", "shared", false}, {"disk_b", "shared", false}};
    EXPECT_EQ(resolveForcedCaCandidate(two, {"shared"}, "shared"), std::optional<size_t>{0});
}

/// ---- the sender's confirm routing ----

static const void * const MOUNT_A = reinterpret_cast<const void *>(0x10);
static const void * const MOUNT_B = reinterpret_cast<const void *>(0x20);

TEST(CASRelinkConfirmRouting, OneOwnerAnswers)
{
    const std::vector<CasConfirmRoutingCandidate> c{{MOUNT_A, "shared", true}};
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, "shared"), std::optional<size_t>{0});
}

TEST(CASRelinkConfirmRouting, NoOwnerIsNoAnswer)
{
    const std::vector<CasConfirmRoutingCandidate> c{{MOUNT_A, "shared", false}, {MOUNT_B, "other", true}};
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, "shared"), std::nullopt);
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, ""), std::nullopt);
    EXPECT_EQ(resolveConfirmRoutingCandidate({}, "shared"), std::nullopt);
}

TEST(CASRelinkConfirmRouting, TwoDistinctOwnersAreAmbiguous)
{
    const std::vector<CasConfirmRoutingCandidate> c{{MOUNT_A, "shared", true}, {MOUNT_B, "shared", true}};
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, "shared"), std::nullopt);
}

TEST(CASRelinkConfirmRouting, AliasesOfOneMountCountOnce)
{
    /// A base disk and its cache wrapper share one exchange object: one mount, two disk names.
    const std::vector<CasConfirmRoutingCandidate> c{{MOUNT_A, "shared", true}, {MOUNT_A, "shared", true}};
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, "shared"), std::optional<size_t>{0});
}

TEST(CASRelinkConfirmRouting, NonOwnerOnThePoolIsIgnored)
{
    const std::vector<CasConfirmRoutingCandidate> c{{MOUNT_A, "shared", false}, {MOUNT_B, "shared", true}};
    EXPECT_EQ(resolveConfirmRoutingCandidate(c, "shared"), std::optional<size_t>{1});
}
```

- [ ] **Step 2: Build to verify it fails to compile (header absent)**

Run: `cd build && ninja unit_tests_dbms > ninja_task1_red.log 2>&1; echo NINJA_EXIT=$? >> ninja_task1_red.log`
Expected: `NINJA_EXIT=1`, the log names `DataPartsExchangeCasRouting.h: No such file`.

- [ ] **Step 3: Write the header**

Create `src/Storages/MergeTree/DataPartsExchangeCasRouting.h`:

```cpp
#pragma once

#include <Core/Types_fwd.h>

#include <optional>
#include <vector>

namespace DB::DataPartsExchange
{

/// The receiver's content-addressed pool advertise as it goes on the wire (the `cas_pool_uuid` request
/// parameter): the pool ids of every disk of its storage policy that could take a relink — sorted,
/// deduplicated, joined with ", ". The list form and the ", " splitter are the ones the zero-copy
/// `remote_fs_metadata` capability list already uses, so the exchange keeps one list convention. A
/// single id is written verbatim: a receiver with one pool puts on the wire exactly the string that a
/// sender comparing the whole value with its own pool id matches. Empty ids are dropped (a storage that
/// never started has no pool id and nothing to advertise).
String encodeCasPoolAdvertise(Strings pool_uuids);
Strings decodeCasPoolAdvertise(const String & text);

/// Which pool a relink offer is for. The sender names it in the `cas_pool_uuid` response cookie; a
/// sender that predates the cookie can only have matched a one-element advertise, so an absent cookie
/// means that single pool. Several advertised pools and no cookie is not a state an honest sender can
/// produce, and the answer is "no pool" — the receiver never guesses.
String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie);

/// One content-addressed disk of the RECEIVING table's storage policy, in policy order.
struct CasRelinkCandidate
{
    String disk_name;
    String pool_uuid;        /// empty: the storage never started; never a candidate
    bool read_only = false;  /// a static property of the disk's configuration; the one exclusion
};

/// Which candidate receives the offered relink: the index of the first candidate on the offered pool
/// (`resolveOfferedCasPool`) that is not read-only. `nullopt` means no disk of this policy may take
/// the offer, which the caller turns into a byte fetch. Whether the pool is LIVE is deliberately not
/// part of this decision — a not-live pool disk is still the target, the relink's own write gate
/// refuses it, and the fetch fails and is retried rather than landing on another disk.
std::optional<size_t> resolveForcedCaCandidate(
    const std::vector<CasRelinkCandidate> & candidates,
    const Strings & advertised_pools,
    const String & offered_pool_cookie);

/// One content-addressed disk of the SENDING table's storage policy, as the confirm routing sees it.
struct CasConfirmRoutingCandidate
{
    const void * exchange_identity = nullptr;  /// the `IContentAddressedExchange` behind the disk
    String pool_uuid;
    bool owns_namespace = false;
};

/// Which candidate answers a relink confirm for `pool_uuid`: EXACTLY one distinct mount that owns the
/// namespace, else `nullopt` — zero owners, or two distinct mounts, are both ambiguous and `Unknown`
/// is the only honest answer. Disks that alias one mount (a base disk and its cache wrapper share the
/// exchange object) count once, as the first of them.
std::optional<size_t> resolveConfirmRoutingCandidate(
    const std::vector<CasConfirmRoutingCandidate> & candidates,
    const String & pool_uuid);

}
```

- [ ] **Step 4: Write the implementation**

Create `src/Storages/MergeTree/DataPartsExchangeCasRouting.cpp`:

```cpp
#include <Storages/MergeTree/DataPartsExchangeCasRouting.h>

#include <base/sort.h>

#include <algorithm>

#include <boost/algorithm/string/join.hpp>

namespace DB::DataPartsExchange
{

namespace
{
const String CAS_POOL_ADVERTISE_DELIMITER = ", ";
}

String encodeCasPoolAdvertise(Strings pool_uuids)
{
    std::erase_if(pool_uuids, [](const String & id) { return id.empty(); });
    ::sort(pool_uuids.begin(), pool_uuids.end());
    pool_uuids.erase(std::unique(pool_uuids.begin(), pool_uuids.end()), pool_uuids.end());
    return boost::algorithm::join(pool_uuids, CAS_POOL_ADVERTISE_DELIMITER);
}

Strings decodeCasPoolAdvertise(const String & text)
{
    Strings pools;
    if (text.empty())
        return pools;

    size_t pos_start = 0;
    while (true)
    {
        const size_t pos_end = text.find(CAS_POOL_ADVERTISE_DELIMITER, pos_start);
        if (pos_end == String::npos)
        {
            pools.push_back(text.substr(pos_start));
            return pools;
        }
        pools.push_back(text.substr(pos_start, pos_end - pos_start));
        pos_start = pos_end + CAS_POOL_ADVERTISE_DELIMITER.size();
    }
}

String resolveOfferedCasPool(const Strings & advertised_pools, const String & offered_pool_cookie)
{
    if (!offered_pool_cookie.empty())
        return offered_pool_cookie;
    if (advertised_pools.size() == 1)
        return advertised_pools.front();
    return {};
}

std::optional<size_t> resolveForcedCaCandidate(
    const std::vector<CasRelinkCandidate> & candidates,
    const Strings & advertised_pools,
    const String & offered_pool_cookie)
{
    const String offered_pool = resolveOfferedCasPool(advertised_pools, offered_pool_cookie);
    if (offered_pool.empty())
        return std::nullopt;

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto & candidate = candidates[i];
        if (!candidate.read_only && !candidate.pool_uuid.empty() && candidate.pool_uuid == offered_pool)
            return i;
    }
    return std::nullopt;
}

std::optional<size_t> resolveConfirmRoutingCandidate(
    const std::vector<CasConfirmRoutingCandidate> & candidates,
    const String & pool_uuid)
{
    if (pool_uuid.empty())
        return std::nullopt;

    std::optional<size_t> matched;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        const auto & candidate = candidates[i];
        if (candidate.pool_uuid != pool_uuid || !candidate.owns_namespace)
            continue;
        if (!matched)
        {
            matched = i;
            continue;
        }
        /// A second DISTINCT mount owning the namespace: ambiguous. An alias of the first is not.
        if (candidates[*matched].exchange_identity != candidate.exchange_identity)
            return std::nullopt;
    }
    return matched;
}

}
```

- [ ] **Step 5: Build and run the vectors**

Run: `cd build && ninja unit_tests_dbms > ninja_task1.log 2>&1; echo NINJA_EXIT=$? >> ninja_task1.log` — a subagent reads the log; expected `NINJA_EXIT=0`, zero warnings in the two new files.
Run: `build/src/unit_tests_dbms --gtest_filter='CASRelink*' > build/test_gtest_task1.log 2>&1; echo EXIT=$? >> build/test_gtest_task1.log`
Expected: `[  PASSED  ] 20 tests.`, `EXIT=0`.

- [ ] **Step 6: Commit**

```bash
git diff --cached --stat          # must be empty
git add src/Storages/MergeTree/DataPartsExchangeCasRouting.h src/Storages/MergeTree/DataPartsExchangeCasRouting.cpp src/Storages/tests/gtest_cas_relink_pool_routing.cpp
git commit -m "ca-relink: the exchange's routing decisions as pure functions with unit vectors

Advertise encode/decode (the zero-copy list form), the receiver's forced
candidate and the sender's confirm routing, in DataPartsExchangeCasRouting.
Nothing calls them yet.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc" -- src/Storages/MergeTree/DataPartsExchangeCasRouting.h src/Storages/MergeTree/DataPartsExchangeCasRouting.cpp src/Storages/tests/gtest_cas_relink_pool_routing.cpp
git branch --show-current && git log -1 --stat
```

---

### Task 2: Sender — match against the advertised set, name the match, deduplicate confirm routing

**Files:**
- Modify: `src/Storages/MergeTree/DataPartsExchange.cpp:9` (include), `:52-57` (failpoint externs), `:231-244` (confirm routing), `:391-434` (offer gate)
- Modify: `src/Common/FailPoint.cpp:234-235` (register one failpoint)
- Test: existing `tests/integration/test_cas_replicated_relink` (the receiver still advertises one pool after this task, so the sender's behaviour is byte-for-byte unchanged on the wire except for one extra cookie the current receiver ignores)

**Interfaces:**
- Consumes from Task 1: `decodeCasPoolAdvertise`, `CasConfirmRoutingCandidate`, `resolveConfirmRoutingCandidate`.
- Produces for Task 3: the response cookie `cas_pool_uuid` (constant `CA_POOL_UUID_PARAM`, value = the matched pool id) on every relink offer; failpoint `cas_relink_sender_omit_pool_cookie` that suppresses that cookie.

- [ ] **Step 1: Register the sender failpoint**

In `src/Common/FailPoint.cpp`, the block ending at line 235 currently reads:

```cpp
    REGULAR(cas_relink_receiver_force_mechanism_failure) \
    PAUSEABLE_ONCE(cas_relink_receiver_pause_before_confirm)
```

Change it to:

```cpp
    REGULAR(cas_relink_receiver_force_mechanism_failure) \
    PAUSEABLE_ONCE(cas_relink_receiver_pause_before_confirm) \
    REGULAR(cas_relink_sender_omit_pool_cookie) \
    REGULAR(cas_relink_receiver_drop_forced_disk)
```

(Both failpoints are registered here; the receiver one is used in Task 3.)

- [ ] **Step 2: Declare the failpoints and the include in `DataPartsExchange.cpp`**

After line 9 (`#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedExchange.h>`) add:

```cpp
#include <Storages/MergeTree/DataPartsExchangeCasRouting.h>
```

In the `FailPoints` namespace (lines 52-57), after `extern const char cas_relink_receiver_pause_before_confirm[];` add:

```cpp
    /// Stands in for a sender that predates the `cas_pool_uuid` response cookie: the offer is made
    /// without naming the pool, and the receiver has to fall back on "the single advertised pool".
    extern const char cas_relink_sender_omit_pool_cookie[];
    /// Stands in for an offer this policy has no disk for: the receiver forgets the forced disk it
    /// resolved and must take the ordinary placement and a byte fetch.
    extern const char cas_relink_receiver_drop_forced_disk[];
```

- [ ] **Step 3: Replace the offer gate**

Lines 404-434 currently read (keep the comment block at 391-403 above them, but replace its second sentence — "the receiver advertised a `cas_pool_uuid` equal to THIS server's own pool_uuid (same shared pool)" — with "the pool of the disk this part sits on is among the pools the receiver advertised in `cas_pool_uuid`"):

```cpp
        if (client_protocol_version >= REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM
            && part->getDataPartStorage().isContentAddressed())
        {
            const String receiver_pool_uuid = parse<String>(params.get(CA_POOL_UUID_PARAM, ""));
            DiskPtr part_disk = data.getStoragePolicy()->tryGetDiskByName(part->getDataPartStorage().getDiskName());
            auto * ca_meta = tryGetContentAddressedExchange(part_disk);
            if (ca_meta && !receiver_pool_uuid.empty() && receiver_pool_uuid == ca_meta->getPoolUUID())
            {
                auto offer = ca_meta->getRelinkOffer(part->getDataPartStorage().getRelativePath());
                if (offer)
                {
                    LOG_DEBUG(log, "Sending part {} by relink (content-addressed, shared pool {}), manifest payload {} bytes",
                        part_name, receiver_pool_uuid, offer->manifest_bytes.size());
                    response.addCookie({CA_RELINK_COOKIE, CA_RELINK_COOKIE_VALUE});
                    /// The source token for the confirm request the receiver makes before it promotes
                    /// (spec §wire-protocol). It always accompanies the offer, and its ABSENCE is what
                    /// tells a confirm-capable receiver that this sender predates the handshake.
                    response.addCookie({CA_CONFIRM_TOKEN_COOKIE, offer->confirm_token});
                    /// The relink payload (B7 part_manifest_v2, all-tree task 7): the opaque encoded
                    /// PartManifest body (the receiver decodes it, ignores the sender identity, and
                    /// stages its OWN local manifest over the shared-pool blobs; the legacy part_id wire
                    /// field carries it). Self-contained: uuid.txt/metadata_version.txt are ordinary
                    /// manifest entries now (task 6), so no separate mutable-header field is sent.
                    writeStringBinary(offer->manifest_bytes, out);
                    data.addLastSentPart(part->info);
                    return;
                }
                /// No offer (no committed ref for this part here, or no mintable token) — fall through
                /// to the byte path.
            }
        }
```

Replace with:

```cpp
        if (client_protocol_version >= REPLICATION_PROTOCOL_VERSION_WITH_CA_CONFIRM
            && part->getDataPartStorage().isContentAddressed())
        {
            /// The receiver advertises every pool its storage policy has a writable content-addressed
            /// disk for, as one list; this server's decision stays local — is the pool of the disk THIS
            /// part sits on among them. The matched pool goes back as a cookie so a receiver with several
            /// pools can place the part on that pool's disk instead of guessing which disk the offer is for.
            const Strings receiver_pools = decodeCasPoolAdvertise(parse<String>(params.get(CA_POOL_UUID_PARAM, "")));
            DiskPtr part_disk = data.getStoragePolicy()->tryGetDiskByName(part->getDataPartStorage().getDiskName());
            auto * ca_meta = tryGetContentAddressedExchange(part_disk);
            const String matched_pool = ca_meta ? ca_meta->getPoolUUID() : String{};
            if (ca_meta && !matched_pool.empty()
                && std::find(receiver_pools.begin(), receiver_pools.end(), matched_pool) != receiver_pools.end())
            {
                auto offer = ca_meta->getRelinkOffer(part->getDataPartStorage().getRelativePath());
                if (offer)
                {
                    LOG_DEBUG(log, "Sending part {} by relink (content-addressed, shared pool {}), manifest payload {} bytes",
                        part_name, matched_pool, offer->manifest_bytes.size());
                    response.addCookie({CA_RELINK_COOKIE, CA_RELINK_COOKIE_VALUE});
                    /// The source token for the confirm request the receiver makes before it promotes
                    /// (spec §wire-protocol). It always accompanies the offer, and its ABSENCE is what
                    /// tells a confirm-capable receiver that this sender predates the handshake.
                    response.addCookie({CA_CONFIRM_TOKEN_COOKIE, offer->confirm_token});
                    /// Which of the advertised pools this offer is for. A receiver with one pool does not
                    /// need it (an offer can only be for that pool); the failpoint stands in for a sender
                    /// that predates the cookie.
                    bool omit_pool_cookie = false;
                    fiu_do_on(FailPoints::cas_relink_sender_omit_pool_cookie, { omit_pool_cookie = true; });
                    if (!omit_pool_cookie)
                        response.addCookie({CA_POOL_UUID_PARAM, matched_pool});
                    /// The relink payload (B7 part_manifest_v2, all-tree task 7): the opaque encoded
                    /// PartManifest body (the receiver decodes it, ignores the sender identity, and
                    /// stages its OWN local manifest over the shared-pool blobs; the legacy part_id wire
                    /// field carries it). Self-contained: uuid.txt/metadata_version.txt are ordinary
                    /// manifest entries now (task 6), so no separate mutable-header field is sent.
                    writeStringBinary(offer->manifest_bytes, out);
                    data.addLastSentPart(part->info);
                    return;
                }
                /// No offer (no committed ref for this part here, or no mintable token) — fall through
                /// to the byte path.
            }
        }
```

Also update the constant's comment at lines 115-117 to:

```cpp
/// CAS replication 2b. The receiver advertises the pool ids of its candidate content-addressed disks
/// under this request param (one id, or several joined with ", ", see `encodeCasPoolAdvertise`) so the
/// sender can decide whether a fetch-by-relink (same pool) is possible. On the offer the same name is a
/// response cookie naming the pool the sender matched.
constexpr auto CA_POOL_UUID_PARAM = "cas_pool_uuid";
```

- [ ] **Step 4: Replace the confirm routing loop**

Lines 231-244 currently read:

```cpp
    const IContentAddressedExchange * matched = nullptr;
    DiskPtr matched_disk;
    for (const auto & disk : data.getDisks())
    {
        const auto * ca_meta = tryGetContentAddressedExchange(disk);
        if (!ca_meta || ca_meta->getPoolUUID() != pool_uuid || !ca_meta->ownsNamespace(server_root_id, root_namespace))
            continue;
        if (matched)
            return CasConfirmAnswer::Unknown;
        matched = ca_meta;
        matched_disk = disk;
    }
    if (!matched)
        return CasConfirmAnswer::Unknown;
```

Replace with (the comment above it, lines 227-230, gets one added sentence: "A cache disk over a content-addressed disk shares the base disk's exchange object, so the two are one mount and count once."):

```cpp
    std::vector<CasConfirmRoutingCandidate> routing;
    Disks routing_disks;
    for (const auto & disk : data.getDisks())
    {
        const auto * ca_meta = tryGetContentAddressedExchange(disk);
        if (!ca_meta)
            continue;
        routing.push_back({ca_meta, ca_meta->getPoolUUID(), ca_meta->ownsNamespace(server_root_id, root_namespace)});
        routing_disks.push_back(disk);
    }
    const auto routed = resolveConfirmRoutingCandidate(routing, pool_uuid);
    if (!routed)
        return CasConfirmAnswer::Unknown;
    const IContentAddressedExchange * matched = tryGetContentAddressedExchange(routing_disks[*routed]);
    DiskPtr matched_disk = routing_disks[*routed];
```

(`matched` and `matched_disk` keep their names; lines 246-269 use them unchanged.)

- [ ] **Step 5: Build**

Run: `cd build && ninja clickhouse unit_tests_dbms > ninja_task2.log 2>&1; echo NINJA_EXIT=$? >> ninja_task2.log` — subagent summary; expected `NINJA_EXIT=0`, no new warnings.

- [ ] **Step 6: Run the existing relink suite — behaviour must be unchanged**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_replicated_relink > build/test_integration_task2.log 2>&1; echo EXIT=$? >> build/test_integration_task2.log`
Expected (subagent reads the log): all 11 existing tests `PASSED`, `EXIT=0`. In particular `test_replicated_fetch_by_relink` and `test_confirm_refuses_when_source_dropped_in_window` (the confirm routing path) are green.

- [ ] **Step 7: Commit**

```bash
git diff --cached --stat          # must be empty
git commit -m "ca-relink: the sender matches the part's pool against the advertised set and names the match

The relink gate decodes cas_pool_uuid as a list and offers when the part's
disk pool is in it; the offer carries the matched pool back as the
cas_pool_uuid cookie. The confirm routing goes through
resolveConfirmRoutingCandidate, so a base disk and its cache wrapper — one
mount under two names — no longer make the answer ambiguous. Two failpoints
registered: cas_relink_sender_omit_pool_cookie (used here) and
cas_relink_receiver_drop_forced_disk (for the receiver).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc" -- src/Storages/MergeTree/DataPartsExchange.cpp src/Common/FailPoint.cpp
git branch --show-current && git log -1 --stat
```

---

### Task 3: Receiver — advertise every pool, resolve the forced disk before reserving, and the integration tests that prove it

**Files:**
- Create: `tests/integration/test_cas_replicated_relink/configs/storage_conf_tiered.xml`
- Modify: `tests/integration/test_cas_replicated_relink/test.py` (fixture `main_configs` for node2 at lines 67-76; one helper after `active_part_names`; six new tests appended after `test_stalled_publish_protects_source_blobs_and_commits_nothing`)
- Modify: `src/Storages/MergeTree/DataPartsExchange.cpp:693-725` (advertise), `:786-790` (after the cookie reads), `:803-808` (reservation), `:881-933` (relink block head and post-check)
- Modify: `src/Storages/MergeTree/DataPartsExchange.h:120-131` (the `dest_disk`/`allow_ca_relink` comment)

**Interfaces:**
- Consumes from Task 1: `encodeCasPoolAdvertise`, `decodeCasPoolAdvertise`, `resolveOfferedCasPool`, `CasRelinkCandidate`, `resolveForcedCaCandidate`. From Task 2: the `cas_pool_uuid` response cookie; failpoints `cas_relink_sender_omit_pool_cookie`, `cas_relink_receiver_drop_forced_disk`.
- Produces: the receiver log line `Part {} is offered by relink for content-addressed pool {}; placing it on disk {} ahead of the storage policy's volume order and TTL rules` (DEBUG) and `Failpoint cas_relink_receiver_drop_forced_disk: forgetting the forced disk for part {}` (INFO); policies `cas_tiered` and `cas_two_pools` on node2 (Task 4 reuses `cas_tiered`).

- [ ] **Step 1: Add the node2 policies**

Create `tests/integration/test_cas_replicated_relink/configs/storage_conf_tiered.xml`:

```xml
<clickhouse>
    <!-- Loaded by node2 only. Two policies over disks defined elsewhere (`default` is the image's local
         disk; `disk_cas_shared` / `disk_cas_other` come from storage_conf.xml / storage_conf_other_pool.xml).
         Both exist to put the pool's disk where the storage policy would NOT choose it on its own:
         `cas_tiered` puts the local volume FIRST, so an ordinary reservation lands on `default`;
         `cas_two_pools` puts the OTHER pool first, so a first-CA-disk advertise names the wrong pool.
         A relink onto `disk_cas_shared` under either policy is therefore a forced placement. -->
    <storage_configuration>
        <policies>
            <cas_tiered>
                <volumes>
                    <hot>
                        <disk>default</disk>
                    </hot>
                    <cold>
                        <disk>disk_cas_shared</disk>
                    </cold>
                </volumes>
            </cas_tiered>
            <cas_two_pools>
                <volumes>
                    <first>
                        <disk>disk_cas_other</disk>
                    </first>
                    <second>
                        <disk>disk_cas_shared</disk>
                    </second>
                </volumes>
            </cas_two_pools>
        </policies>
    </storage_configuration>
</clickhouse>
```

In `test.py`, the node2 instance (lines 67-76) gets the file:

```python
    cluster.add_instance(
        "node2",
        main_configs=[
            "configs/storage_conf.xml",
            "configs/server_root_id_node2.xml",
            "configs/storage_conf_other_pool.xml",
            "configs/storage_conf_tiered.xml",
        ],
        macros={"replica": "node2"},
        with_rustfs=True,
        with_zookeeper=True,
        stay_alive=True,
    )
```

Add the policy names next to the existing constants (after line 21, `OTHER_CA_DISK = "disk_cas_other"`):

```python
# node2-only policies (configs/storage_conf_tiered.xml). `cas_tiered` = [local `default`] then
# [disk_cas_shared]: an ordinary reservation lands on `default`, so a relink onto the pool's disk is a
# forced placement. `cas_two_pools` = [disk_cas_other] then [disk_cas_shared]: the first content-addressed
# disk is the WRONG pool, so a relink onto disk_cas_shared proves the whole pool set was advertised.
TIERED_STORAGE_POLICY = "cas_tiered"
TWO_POOLS_STORAGE_POLICY = "cas_two_pools"
LOCAL_DISK = "default"
```

And one helper after `active_part_names` (line 206):

```python
def part_disk(node, table, part):
    """The disk the ACTIVE part of this name sits on, from `system.parts`."""
    return node.query(
        "SELECT disk_name FROM system.parts WHERE database = 'default' AND table = '{}' "
        "AND name = '{}' AND active".format(table, part)
    ).strip()


def detached_part_disk(node, table, part):
    return node.query(
        "SELECT disk FROM system.detached_parts WHERE database = 'default' AND table = '{}' "
        "AND name = '{}'".format(table, part)
    ).strip()
```

- [ ] **Step 2: Write the six failing integration tests**

Append to `test.py`:

```python
# ----------------------------------------------------------------------------------------------------
# FORCED PLACEMENT: a relink lands on the pool's disk even when the storage policy would put the part
# elsewhere. Every test here asserts the relink line AND `system.parts.disk_name`, because the first
# alone would also hold for a relink that then got moved, and the second alone would hold for a byte
# fetch that happened to be reserved on the pool's disk.
# ----------------------------------------------------------------------------------------------------


def _fetch_via_queue(node1, node2, table, node2_policy, create_sql=None):
    """INSERT on node1 while node2's fetches are stopped, then let node2 fetch exactly that one part.

    Returns the part name. `create_sql` overrides the table DDL (it must contain `{policy}` and `{zk}`).
    """
    drop_everywhere(table)
    if create_sql is None:
        create_replicated(node1, table)
        create_replicated(node2, table, policy=node2_policy)
    else:
        node1.query(create_sql.format(policy=STORAGE_POLICY, zk="/clickhouse/tables/" + table))
        node2.query(create_sql.format(policy=node2_policy, zk="/clickhouse/tables/" + table))
    node2.query("SYSTEM STOP FETCHES {}".format(table))
    insert_rows(node1, table, 0)
    part = active_part_names(node1, table)[0]
    node2.query("SYSTEM START FETCHES {}".format(table))
    node2.query("SYSTEM SYNC REPLICA {}".format(table), timeout=90)
    return part


def test_tiered_policy_relinks_onto_cas_over_volume_order():
    """`[default] then [disk_cas_shared]`: the policy's own placement is the local volume, and before the
    forced placement the fetch reserved there, failed the pool post-check and downloaded bytes onto
    `default`. Now the offer decides the disk."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "tiered_order"
    blobs_before = blob_keys()

    part = _fetch_via_queue(node1, node2, table, TIERED_STORAGE_POLICY)

    assert_relinked(node2, table, part)
    assert part_disk(node2, table, part) == CA_DISK
    assert_no_new_blobs(blobs_before)
    assert int(node2.query("SELECT count() FROM {}".format(table))) == NUM_ROWS
    drop_everywhere(table)


def test_relink_carries_projection_under_tiered_policy():
    """A projection-bearing part relinks like any other (the projection is loaded from the published
    manifest), and the forced placement does not disturb that."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "tiered_projection"
    create_sql = (
        "CREATE TABLE " + table + " (id Int64, v UInt64, s String, "
        "PROJECTION p_by_s (SELECT s, sum(v) GROUP BY s)) "
        "ENGINE = ReplicatedMergeTree('{zk}', '{{replica}}') ORDER BY id "
        "SETTINGS storage_policy = '{policy}'"
    )

    part = _fetch_via_queue(node1, node2, table, TIERED_STORAGE_POLICY, create_sql=create_sql)

    assert_relinked(node2, table, part)
    assert part_disk(node2, table, part) == CA_DISK
    assert int(node2.query(
        "SELECT count() FROM system.projection_parts WHERE database = 'default' AND table = '{}' "
        "AND parent_name = '{}' AND name = 'p_by_s' AND active".format(table, part)
    )) == 1
    assert int(node2.query("SELECT sum(v) FROM {} WHERE s = '7'".format(table))) == 70
    drop_everywhere(table)


def test_two_pool_policy_relinks_into_second_pool():
    """`[disk_cas_other] then [disk_cas_shared]`: the first content-addressed disk is the WRONG pool.
    A single-pool advertise names `other`, the sender declines, and the bytes land on `disk_cas_other`;
    advertising the whole set lets the sender match `shared` and the receiver place it there."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "two_pools"
    blobs_before = blob_keys()

    part = _fetch_via_queue(node1, node2, table, TWO_POOLS_STORAGE_POLICY)

    assert_relinked(node2, table, part)
    assert part_disk(node2, table, part) == CA_DISK
    assert_no_new_blobs(blobs_before)
    assert not log_lines(node2, download_finished_pattern(table, part, disk=OTHER_CA_DISK))
    drop_everywhere(table)


def test_mechanism_failure_falls_back_to_bytes_on_forced_disk():
    """A relink that fails for a mechanism reason re-requests the bytes on the SAME forced disk — the
    placement decision outlives the relink. (The one-offer recursion bound is proven by
    `test_recursion_brake_bounds_relink_to_one_attempt`; this test proves only the destination.)"""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "tiered_fallback"
    drop_everywhere(table)
    create_replicated(node1, table)
    create_replicated(node2, table, policy=TIERED_STORAGE_POLICY)
    node2.query("SYSTEM STOP FETCHES {}".format(table))
    insert_rows(node1, table, 0)
    part = active_part_names(node1, table)[0]

    node2.query("SYSTEM ENABLE FAILPOINT cas_relink_receiver_force_mechanism_failure")
    try:
        node2.query("SYSTEM START FETCHES {}".format(table))
        node2.query("SYSTEM SYNC REPLICA {}".format(table), timeout=90)
        assert_byte_downloaded(node2, table, part, disk=CA_DISK)
        assert part_disk(node2, table, part) == CA_DISK
        assert not log_lines(node2, download_finished_pattern(table, part, disk=LOCAL_DISK))
    finally:
        node2.query("SYSTEM DISABLE FAILPOINT cas_relink_receiver_force_mechanism_failure")
    drop_everywhere(table)


def test_detached_fetch_relinks_onto_cas_under_tiered_policy():
    """`ALTER TABLE ... FETCH PART` into `detached/` under the tiered policy: the forced placement
    applies to detached fetches too, and the detached part is on the pool's disk."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    src, dst = "tiered_det_src", "tiered_det_dst"
    drop_everywhere(src)
    drop_everywhere(dst)
    create_replicated(node1, src)
    create_replicated(node2, dst, policy=TIERED_STORAGE_POLICY)
    insert_rows(node1, src, 0)
    part = active_part_names(node1, src)[0]

    node2.query(
        "ALTER TABLE {dst} FETCH PART '{part}' FROM '/clickhouse/tables/{src}'".format(
            dst=dst, part=part, src=src
        )
    )

    assert_relinked(node2, dst, part)
    assert detached_part_disk(node2, dst, part) == CA_DISK
    node2.query("ALTER TABLE {} ATTACH PART '{}'".format(dst, part))
    assert part_disk(node2, dst, part) == CA_DISK
    assert int(node2.query("SELECT count() FROM {}".format(dst))) == NUM_ROWS
    drop_everywhere(src)
    drop_everywhere(dst)


def test_offer_for_unavailable_pool_falls_back_to_ordinary_placement():
    """The receiver resolved a forced disk and then lost it (the failpoint stands in for an offer this
    policy has no disk for): the ordinary reservation runs, the relink block sees a disk outside the
    offered pool, and the bytes go where the policy says — `default` under the tiered policy. Exactly
    one offer is made: the byte re-request carries no advertise."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "tiered_dropped"
    drop_everywhere(table)
    create_replicated(node1, table)
    create_replicated(node2, table, policy=TIERED_STORAGE_POLICY)
    node2.query("SYSTEM STOP FETCHES {}".format(table))
    insert_rows(node1, table, 0)
    part = active_part_names(node1, table)[0]

    node2.query("SYSTEM ENABLE FAILPOINT cas_relink_receiver_drop_forced_disk")
    try:
        node2.query("SYSTEM START FETCHES {}".format(table))
        node2.query("SYSTEM SYNC REPLICA {}".format(table), timeout=90)
        assert log_lines(
            node2,
            r"Failpoint cas_relink_receiver_drop_forced_disk: forgetting the forced disk for part {}".format(
                re.escape(part)
            ),
        )
        assert_byte_downloaded(node2, table, part, disk=LOCAL_DISK)
        assert part_disk(node2, table, part) == LOCAL_DISK
        assert len(log_lines(node1, relink_offer_pattern(table, part))) == 1
    finally:
        node2.query("SYSTEM DISABLE FAILPOINT cas_relink_receiver_drop_forced_disk")
    drop_everywhere(table)


def test_offer_without_pool_cookie_resolves_to_single_advertised_pool():
    """The old-sender shape: an offer with no `cas_pool_uuid` cookie. With ONE advertised pool that is
    the pool, and the relink is forced as usual; with TWO advertised pools the receiver refuses to guess,
    the ordinary reservation lands on the first volume (`disk_cas_other`), and the bytes go there."""
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    node1.query("SYSTEM ENABLE FAILPOINT cas_relink_sender_omit_pool_cookie")
    try:
        one_pool = "omit_cookie_one"
        part = _fetch_via_queue(node1, node2, one_pool, TIERED_STORAGE_POLICY)
        assert_relinked(node2, one_pool, part)
        assert part_disk(node2, one_pool, part) == CA_DISK
        drop_everywhere(one_pool)

        two_pools = "omit_cookie_two"
        part = _fetch_via_queue(node1, node2, two_pools, TWO_POOLS_STORAGE_POLICY)
        assert_byte_downloaded(node2, two_pools, part, disk=OTHER_CA_DISK)
        assert part_disk(node2, two_pools, part) == OTHER_CA_DISK
        assert len(log_lines(node1, relink_offer_pattern(two_pools, part))) == 1
        drop_everywhere(two_pools)
    finally:
        node1.query("SYSTEM DISABLE FAILPOINT cas_relink_sender_omit_pool_cookie")
```

- [ ] **Step 3: Run the new tests to verify they fail against the Task 2 binary**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test "test_cas_replicated_relink/test.py::test_tiered_policy_relinks_onto_cas_over_volume_order test_cas_replicated_relink/test.py::test_two_pool_policy_relinks_into_second_pool" > build/test_integration_task3_red.log 2>&1; echo EXIT=$? >> build/test_integration_task3_red.log`
Expected: both FAIL — the tiered one on `assert_relinked` (the log shows `Download of part ... onto disk default finished`), the two-pool one likewise with `disk_cas_other`. If praktika's `--test` does not accept `::` selectors, run the whole directory and read the two names in the summary.

- [ ] **Step 4: Replace the receiver's advertise block**

Lines 693-725 currently read:

```cpp
    /// CAS replication 2b — fetch-by-relink (spec §4). Advertise this replica's target content-addressed
    /// pool identity so a same-pool sender can relink instead of streaming bytes. The target disk is the
    /// provided one if it is CA, else the first CA disk among the table's disks. A non-CA fetch adds
    /// nothing here and is byte-for-byte unchanged.
    /// Gated on `allow_ca_relink` alone (B66b). That flag is the RECURSION BRAKE and nothing else: not
    /// advertising is what makes the sender stream bytes, so every same-sender byte re-request below
    /// clears it, and a persistent relink-mechanism failure therefore costs exactly one relink attempt.
    /// The gate used to be `try_zero_copy && !to_detached`, and BOTH halves were accidents of that same
    /// brake — `try_zero_copy` because the fallback re-requests with it false, and `!to_detached`
    /// because the relink path staged at the ACTIVE part path and ignored `to_detached`. `to_detached`
    /// is now a parameter of `relinkPartToDisk` (it stages under the `detached/` parent), and
    /// `try_zero_copy` goes back to meaning real zero-copy only.
    String advertised_pool_uuid;
    if (allow_ca_relink)
    {
        if (auto * ca_meta = tryGetContentAddressedExchange(disk))
        {
            advertised_pool_uuid = ca_meta->getPoolUUID();
            uri.addQueryParameter(CA_POOL_UUID_PARAM, advertised_pool_uuid);
        }
        else if (!disk)
        {
            for (const auto & data_disk : data.getDisks())
            {
                if (auto * ca_disk_meta = tryGetContentAddressedExchange(data_disk))
                {
                    advertised_pool_uuid = ca_disk_meta->getPoolUUID();
                    uri.addQueryParameter(CA_POOL_UUID_PARAM, advertised_pool_uuid);
                    break;
                }
            }
        }
    }
```

Replace with:

```cpp
    /// CAS fetch-by-relink: advertise the content-addressed pools this fetch may land in, so a sender
    /// holding the part in one of them relinks instead of streaming bytes. With a caller-supplied disk
    /// that is its pool alone (the disk is the caller's contract and is never overridden); otherwise it
    /// is every content-addressed disk of the table's storage policy that is not read-only, in policy
    /// order. The sender names the pool it matched in a response cookie, and the reservation below then
    /// goes to THAT pool's disk — ahead of the policy's volume order and of any TTL move rule, because a
    /// part that is already in the pool must never travel as bytes merely because the policy would have
    /// put it elsewhere (the mover carries it to a TTL destination afterwards). A pool disk that is not
    /// live is still the target: the relink's own write gate refuses it, the fetch fails and the queue
    /// retries — never a quiet landing on another disk. A non-CA fetch adds nothing here.
    /// Gated on `allow_ca_relink` alone. That flag is the RECURSION BRAKE and nothing else: not
    /// advertising is what makes the sender stream bytes, so every same-sender byte re-request below
    /// clears it, and a persistent relink-mechanism failure therefore costs exactly one relink attempt.
    /// The gate used to be `try_zero_copy && !to_detached`, and BOTH halves were accidents of that same
    /// brake — `try_zero_copy` because the fallback re-requests with it false, and `!to_detached`
    /// because the relink path staged at the ACTIVE part path and ignored `to_detached`. `to_detached`
    /// is now a parameter of `relinkPartToDisk` (it stages under the `detached/` parent), and
    /// `try_zero_copy` goes back to meaning real zero-copy only.
    Strings advertised_pools;
    std::vector<CasRelinkCandidate> ca_candidates;
    Disks ca_candidate_disks;
    if (allow_ca_relink)
    {
        if (disk)
        {
            if (auto * ca_meta = tryGetContentAddressedExchange(disk))
                advertised_pools.push_back(ca_meta->getPoolUUID());
        }
        else
        {
            for (const auto & data_disk : data.getDisks())
            {
                auto * ca_disk_meta = tryGetContentAddressedExchange(data_disk);
                if (!ca_disk_meta)
                    continue;
                ca_candidates.push_back({data_disk->getName(), ca_disk_meta->getPoolUUID(), data_disk->isReadOnly()});
                ca_candidate_disks.push_back(data_disk);
                if (!data_disk->isReadOnly())
                    advertised_pools.push_back(ca_disk_meta->getPoolUUID());
            }
        }
        const String advertise = encodeCasPoolAdvertise(advertised_pools);
        if (!advertise.empty())
            uri.addQueryParameter(CA_POOL_UUID_PARAM, advertise);
        /// The deduplicated form is what "the single advertised pool" is measured against below.
        advertised_pools = decodeCasPoolAdvertise(advertise);
    }
```

- [ ] **Step 5: Resolve the forced disk right after the cookie reads**

Lines 786-787 currently read:

```cpp
    int server_protocol_version = parse<int>(in->getResponseCookie("server_protocol_version", "0"));
    String remote_fs_metadata = parse<String>(in->getResponseCookie("remote_fs_metadata", ""));
```

Directly after them insert:

```cpp
    /// The relink offer, if any, is already visible: response cookies arrive with the headers, before a
    /// single body byte is read. Resolve the forced disk NOW, so the reservation below goes to it and the
    /// body reads keep their order. `offered_pool` is what the relink block later checks the chosen disk
    /// against; with a caller-supplied disk there is nothing to force and that check is all there is.
    const String ca_relink = parse<String>(in->getResponseCookie(CA_RELINK_COOKIE, ""));
    String offered_pool;
    DiskPtr forced_ca_disk;
    if (!ca_relink.empty())
    {
        const String offered_pool_cookie = parse<String>(in->getResponseCookie(CA_POOL_UUID_PARAM, ""));
        offered_pool = resolveOfferedCasPool(advertised_pools, offered_pool_cookie);
        if (!disk)
        {
            auto chosen = resolveForcedCaCandidate(ca_candidates, advertised_pools, offered_pool_cookie);
            fiu_do_on(FailPoints::cas_relink_receiver_drop_forced_disk,
            {
                LOG_INFO(log, "Failpoint cas_relink_receiver_drop_forced_disk: forgetting the forced disk for part {}", part_name);
                chosen.reset();
            });
            if (chosen)
            {
                forced_ca_disk = ca_candidate_disks[*chosen];
                LOG_DEBUG(log, "Part {} is offered by relink for content-addressed pool {}; placing it on disk {} "
                    "ahead of the storage policy's volume order and TTL rules", part_name, offered_pool, forced_ca_disk->getName());
                /// From here on the target is decided: every `!disk` reservation branch below is skipped.
                disk = forced_ca_disk;
            }
        }
    }
```

- [ ] **Step 6: Reserve on the forced disk**

Line 803-807 currently read:

```cpp
    ReservationPtr reservation;
    size_t sum_files_size = 0;
    if (server_protocol_version >= REPLICATION_PROTOCOL_VERSION_WITH_PARTS_SIZE)
    {
        readBinary(sum_files_size, *in);
```

Directly after `readBinary(sum_files_size, *in);` insert:

```cpp
        if (forced_ca_disk)
        {
            /// An object-storage disk reports no capacity, so this cannot decline for space; if it ever
            /// does, the loud NOT_ENOUGH_SPACE is the right outcome — the part is not re-placed elsewhere.
            reservation = MergeTreeData::reserveSpace(sum_files_size, forced_ca_disk);
        }
```

(The rest of the block is untouched: every `if (!disk)` branch is skipped because `disk` is set, and the trailing `if (!disk) disk = reservation->getDisk(); else LOG_TEST(...)` takes the `else` arm.)

- [ ] **Step 7: Update the relink block head and its post-check**

Lines 881-890 currently begin the block with a comment and `String ca_relink = parse<String>(in->getResponseCookie(CA_RELINK_COOKIE, ""));`. Delete that declaration line (`ca_relink` is now declared in Step 5) and replace the comment's first sentence — "The sender chose to relink: it sent only the part's encoded PartManifest body, no file bytes." — with "The sender chose to relink: it sent only the part's encoded PartManifest body, no file bytes, and the reservation above already went to the offered pool's disk."

Lines 926-933 currently read:

```cpp
        auto * chosen_ca = tryGetContentAddressedExchange(disk);
        if (!chosen_ca || chosen_ca->getPoolUUID() != advertised_pool_uuid)
        {
            LOG_INFO(log, "Part {} was offered by relink for content-addressed pool '{}', but reservation landed "
                "outside the advertised pool on disk {} (chosen pool: '{}'); falling back to a byte fetch",
                part_name, advertised_pool_uuid, disk->getName(), chosen_ca ? chosen_ca->getPoolUUID() : "<none>");
            return fall_back_to_byte_fetch();
        }
```

Replace with:

```cpp
        /// The disk is the forced one, so this holds by construction; it stays a real exit rather than
        /// an assertion because it is also how an offer for a pool this policy has no disk for (no forced
        /// disk, ordinary reservation) and a caller-supplied disk outside the pool leave the relink path.
        auto * chosen_ca = tryGetContentAddressedExchange(disk);
        if (!chosen_ca || offered_pool.empty() || chosen_ca->getPoolUUID() != offered_pool)
        {
            LOG_INFO(log, "Part {} was offered by relink for content-addressed pool '{}', but no disk of this table's "
                "storage policy takes it (chosen disk {}, pool '{}'); falling back to a byte fetch",
                part_name, offered_pool, disk->getName(), chosen_ca ? chosen_ca->getPoolUUID() : "<none>");
            return fall_back_to_byte_fetch();
        }
```

Also in the comment at lines 895-906 (inside the block, about the brake) replace the parenthetical "(The reservation-outside-the-pool exit below is bounded twice over — it re-requests with the non-CA disk it resolved, which cannot advertise anything either way — so do not read that one as evidence that the brake is redundant.)" with "(The no-disk-takes-it exit below is bounded twice over — it re-requests with the disk the ordinary reservation resolved, which is outside the pool and cannot advertise it — so do not read that one as evidence that the brake is redundant.)".

- [ ] **Step 8: Update the header comment on `dest_disk`**

In `src/Storages/MergeTree/DataPartsExchange.h`, the parameter `DiskPtr dest_disk = nullptr,` (line 120) gets a comment above it:

```cpp
        /// The target disk when the CALLER has already decided it (zero-copy `MOVE` re-fetching a shared
        /// part onto the move's destination); never overridden. When absent, a content-addressed relink
        /// offer decides the disk — the policy disk on the sender's pool — ahead of the storage policy's
        /// own placement; otherwise the ordinary reservation does.
        DiskPtr dest_disk = nullptr,
```

- [ ] **Step 9: Build**

Run: `cd build && ninja clickhouse > ninja_task3.log 2>&1; echo NINJA_EXIT=$? >> ninja_task3.log` — subagent summary; expected `NINJA_EXIT=0`, no new warnings (an unused-variable warning for `remote_fs_metadata` or `advertised_pool_uuid` means a leftover from Step 4/7).

- [ ] **Step 10: Run the whole relink suite — the six new tests and the eleven old ones**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_replicated_relink > build/test_integration_task3.log 2>&1; echo EXIT=$? >> build/test_integration_task3.log`
Expected (subagent): 17 `PASSED`, `EXIT=0`. If `test_relink_carries_projection_under_tiered_policy` alone fails, that is a pre-existing projection-relink defect, not this task's — report it, do not paper over it, and continue with the other sixteen green.

- [ ] **Step 11: Commit**

```bash
git diff --cached --stat          # must be empty
git add tests/integration/test_cas_replicated_relink/configs/storage_conf_tiered.xml
git commit -m "ca-relink: the receiver advertises every pool of its policy and places the part on the offered pool's disk

Before this the receiver advertised one guessed pool and reserved the target
disk independently; the offer survived only when the two coincided. Now the
sender's cas_pool_uuid cookie names the pool, the forced disk is resolved
from the response headers before the reservation, and the reservation goes
to it — ahead of volume order, JBOD balancing and TTL move rules. A pool
disk that is not live is still the target and fails closed. Six integration
tests: tiered policy, projection, two pools (closes CAS-134), mechanism
fallback on the forced disk, detached fetch, and the E1/absent-cookie arms.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc" -- src/Storages/MergeTree/DataPartsExchange.cpp src/Storages/MergeTree/DataPartsExchange.h tests/integration/test_cas_replicated_relink/test.py tests/integration/test_cas_replicated_relink/configs/storage_conf_tiered.xml
git branch --show-current && git log -1 --stat
```

---

### Task 4: TTL convergence — relink wins, the mover carries the part to the TTL destination

**Files:**
- Modify: `tests/integration/test_cas_replicated_relink/test.py` (one test appended)

**Interfaces:**
- Consumes from Task 3: policy `cas_tiered` on node2, helper `part_disk`, the forced placement.
- Produces: nothing new.

- [ ] **Step 1: Write the test**

Append to `test.py`:

```python
def test_relink_wins_over_ttl_then_mover_converges():
    """A `TTL ... TO DISK` rule that names the LOCAL disk for this (already expired) part does not stop the
    relink: the part lands on the pool's disk at zero byte cost, and the background mover — which sees a
    part that is not in its TTL destination — carries it to `default` afterwards. Moves are stopped on
    node2 around the fetch so the intermediate placement is observable, exactly as `test_ttl_move` does.

    `IF EXISTS` precedes the disk name in the grammar. It is there for node1, whose policy has no
    `default` disk: without it every reservation on node1 would log a warning about the missing disk.
    """
    node1 = cluster.instances["node1"]
    node2 = cluster.instances["node2"]
    table = "tiered_ttl"
    drop_everywhere(table)
    create_sql = (
        "CREATE TABLE " + table + " (id Int64, v UInt64, s String, ts DateTime) "
        "ENGINE = ReplicatedMergeTree('/clickhouse/tables/" + table + "', '{{replica}}') ORDER BY id "
        "TTL ts TO DISK IF EXISTS 'default' "
        "SETTINGS storage_policy = '{policy}'"
    )
    node1.query(create_sql.format(policy=STORAGE_POLICY))
    node2.query(create_sql.format(policy=TIERED_STORAGE_POLICY))

    node2.query("SYSTEM STOP MOVES {}".format(table))
    node2.query("SYSTEM STOP FETCHES {}".format(table))
    try:
        node1.query(
            "INSERT INTO {table} SELECT number, number * 10, toString(number), now() - INTERVAL 1 DAY "
            "FROM numbers({rows})".format(table=table, rows=NUM_ROWS)
        )
        part = active_part_names(node1, table)[0]
        assert part_disk(node1, table, part) == CA_DISK  # the sender holds it in the pool

        node2.query("SYSTEM START FETCHES {}".format(table))
        node2.query("SYSTEM SYNC REPLICA {}".format(table), timeout=90)

        # The TTL rule says `default`; the relink put it on the pool's disk anyway, and moves are stopped.
        assert_relinked(node2, table, part)
        assert part_disk(node2, table, part) == CA_DISK
        rows_before = node2.query("SELECT count(), sum(v) FROM {}".format(table))

        node2.query("SYSTEM START MOVES {}".format(table))
        wait_until(
            lambda: part_disk(node2, table, part) == LOCAL_DISK,
            timeout=120,
            what="the background mover carrying {} to {}".format(part, LOCAL_DISK),
        )
        assert node2.query("SELECT count(), sum(v) FROM {}".format(table)) == rows_before
        assert node2.query("SELECT count(), sum(v) FROM {}".format(table)) == node1.query(
            "SELECT count(), sum(v) FROM {}".format(table)
        )
    finally:
        node2.query("SYSTEM START MOVES {}".format(table))
    drop_everywhere(table)
```

- [ ] **Step 2: Run it**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_replicated_relink > build/test_integration_task4.log 2>&1; echo EXIT=$? >> build/test_integration_task4.log`
Expected (subagent): 18 `PASSED`, `EXIT=0`. If the move never happens within 120 s, read node2's log for `Would like to reserve space on disk 'default'` (a reservation problem on the local disk) or `moving` errors before touching the timeout.

- [ ] **Step 3: Commit**

```bash
git diff --cached --stat          # must be empty
git commit -m "ca-relink: TTL convergence test — the relink wins and the mover carries the part to its TTL disk

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc" -- tests/integration/test_cas_replicated_relink/test.py
git branch --show-current && git log -1 --stat
```

---

### Task 5: Documentation and backlog

**Files:**
- Modify: `docs/en/antalya/cas/architecture/replication.md:34` (diagram line), `:66` (gate 1 row), new section after the paragraph ending at line 79 ("...only the zero-byte-move property for that one fetch.")
- Modify: `docs/superpowers/cas/BACKLOG/replication.md` (the CAS-134 section at the end)
- Modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md:85` (`[mixed-ca-tiered-topology]`)

**Interfaces:** none.

- [ ] **Step 1: The user-facing architecture doc**

In `docs/en/antalya/cas/architecture/replication.md`:

Line 34, `    R->>Snd: GET part, cas_pool_uuid = R's pool uuid, client_protocol_version = 11`, becomes:

```
    R->>Snd: GET part, cas_pool_uuid = every pool of R's policy, client_protocol_version = 11
```

Line 38, `    Snd-->>R: cookie cas_relink = part_manifest_v2, cookie cas_source_token = ..., body = manifest bytes`, becomes:

```
    Snd-->>R: cookies cas_relink = part_manifest_v2, cas_source_token = ..., cas_pool_uuid = the matched pool; body = manifest bytes
```

Gate 1 row (line 66) becomes:

```
| 1 | Pool identity | The receiver advertises `cas_pool_uuid` — the pool uuids of every content-addressed disk of its storage policy that is not read-only, as one list — and the sender offers relink only if its own disk's pool uuid is **in** it, naming that uuid in a `cas_pool_uuid` response cookie. Matching by endpoint and prefix was tried and rejected — a minted pool uuid is the identity |
```

After the paragraph that ends "...only the zero-byte-move property for that one fetch." (line 79) insert:

```markdown
## Where a relinked part lands {#relink-placement}

The offer decides the disk. Once the sender has named the pool, the receiver places the part on the
first disk of its storage policy that belongs to that pool, and reserves space there directly — ahead
of everything the policy would otherwise consult: volume order, JBOD balancing,
`max_data_part_size_bytes`, and `TTL ... TO DISK|VOLUME` move rules. A part that is already in the
pool never travels as bytes merely because the policy would have put it somewhere else.

A TTL rule is not ignored, it is deferred: the background mover sees a part that is not in its TTL
destination and moves it there afterwards. The bytes then travel once, as a read from the pool on the
receiver, and the sender is never loaded.

Two things do not bend to the offer. A disk the caller supplied (zero-copy `MOVE` re-fetching a shared
part onto the move's destination) is never overridden — a content-addressed disk cannot reach that path
at all, since it does not support zero-copy replication. And a pool disk that is read-only is not a
candidate and is not advertised, so the sender streams bytes and the ordinary placement applies.

A pool disk that is not live — its mount lease lost, its identity lost, or the storage shut down — is
still the target. The relink's own write gate refuses it, the fetch fails, and the replication queue
retries; the part is never quietly placed on another disk instead. This is the behaviour a single-disk
content-addressed policy always had, and a mixed policy now shares it.

The byte-fetch fallback after a relink that failed for a mechanism reason (an undecodable manifest, a
body-absent precommit, a ref conflict) re-requests the bytes on the same pool disk, where they
content-address and deduplicate against the pool — the placement outlives the relink.
```

- [ ] **Step 2: Close CAS-134 in the backlog**

In `docs/superpowers/cas/BACKLOG/replication.md`, the section header
`## With no target disk supplied, a fetch advertises only the FIRST CA pool of the policy, so a reservation on a second CA pool loses relink (2031-triage CAS-134) {#relink-advertises-only-first-ca-pool}`
gets, as its first paragraph (before "P3, performance only — ..."):

```markdown
**CLOSED 2026-09-03 by the forced-relink-on-fetch design
(`docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`): the receiver advertises the
set of its policy's pools, the sender names the match, and the reservation goes to that pool's disk.
`test_two_pool_policy_relinks_into_second_pool` is the proof. Kept for provenance; the text below
describes the pre-fix code.**
```

Also in the two items recorded earlier in that file, `[move-to-ca-relink-from-replica]` and
`[zero-copy-parity-audit]`, replace "the forced-relink-on-fetch design of 2026-09-03" (two occurrences) with
"the forced-relink-on-fetch design (`docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`)".

- [ ] **Step 3: Answer `[mixed-ca-tiered-topology]` for the fetch path**

In `docs/superpowers/cas/BACKLOG/formats-and-storage.md`, line 85 currently reads:

```
- **[mixed-ca-tiered-topology] mixed CA + non-CA tiered hot/cold storage policy support is unverified** — DESIRABLE — Governs the severity of a relink pool-UUID mis-advertise bug; no test or doc confirms this topology is supported.
```

Replace with:

```
- **[mixed-ca-tiered-topology] mixed CA + non-CA tiered hot/cold storage policy support is unverified** — DESIRABLE, PARTIALLY ANSWERED 2026-09-03 — The FETCH path of the mixed topology is now designed and tested: under a `[local] then [cas]` policy a same-pool fetch relinks onto the CA disk and a `TTL ... TO DISK` rule pointing at the local disk is honoured afterwards by the mover (`test_tiered_policy_relinks_onto_cas_over_volume_order`, `test_relink_wins_over_ttl_then_mover_converges`; spec `docs/superpowers/specs/2026-09-03-cas-fetch-forced-relink-design.md`). Still open: the move-OUT leg onto a plain S3 disk on the same endpoint (`{#move-out-copies-envelope-bytes}`, CAS-020) — a policy `[cas, plain-s3 same endpoint]` with a TTL rule at the plain disk now reaches that failure on the mover leg, where the fetch used to stream bytes straight there. Declaring the topology supported needs CAS-020 first.
```

- [ ] **Step 4: Check the docs build rules**

Run: `grep -n "^## \|^### " docs/en/antalya/cas/architecture/replication.md | grep -v "{#"` — expected: no output (every header has an anchor).

- [ ] **Step 5: Commit**

```bash
git diff --cached --stat          # must be empty
git commit -m "ca-docs: where a relinked part lands — gate 1 advertises the pool set, placement overrides policy and TTL

Closes CAS-134 in the backlog and answers the fetch half of
[mixed-ca-tiered-topology]; the move-out leg (CAS-020) stays open.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01HNX5tL6Q5fYf9iCxBRNHtc" -- docs/en/antalya/cas/architecture/replication.md docs/superpowers/cas/BACKLOG/replication.md docs/superpowers/cas/BACKLOG/formats-and-storage.md
git branch --show-current && git log -1 --stat
```

---

### Task 6: Gates

**Files:** none modified unless a gate is red.

- [ ] **Step 1: The content-addressed unit gate**

Run: `build/src/unit_tests_dbms --gtest_filter='CAS*' > build/test_gtest_task6.log 2>&1; echo EXIT=$? >> build/test_gtest_task6.log` — subagent: the `PASSED` count, any `FAILED`, `EXIT=0`. The two new suites appear in the run (`CASRelinkPoolAdvertise`, `CASRelinkConfirmRouting`); if they do not, the suite names escaped the `CAS*` filter — rename, never widen.

- [ ] **Step 2: The relink integration suite, whole**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test test_cas_replicated_relink > build/test_integration_task6.log 2>&1; echo EXIT=$? >> build/test_integration_task6.log` — expected 18 `PASSED`.

- [ ] **Step 3: Neighbouring relink consumers**

Run: `python3 -m ci.praktika run "Integration tests (amd_binary, 1/5)" --test "test_cas_gcs_relink_liveness test_cas_drop_pool_member" > build/test_integration_task6_neighbours.log 2>&1; echo EXIT=$? >> build/test_integration_task6_neighbours.log` — both suites relink over a single-disk policy; the one-element advertise is byte-for-byte the old one, so both must stay green. (`test_cas_gcs_relink_liveness` runs against the fake GCS server bundled with the test; if the harness cannot start it locally, say so in the report rather than skipping silently.)

- [ ] **Step 4: Report**

Any red is a root cause or a tracked return item, never "pre-existing". Write the run summary (which log, which counts, the binary's timestamp) into the final report.

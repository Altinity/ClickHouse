# CAS observability — audit-log completion + `ca-inspect` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the manifest/precommit lifecycle fully auditable in `system.content_addressed_log` and add a `clickhouse-disks ca-inspect` command that decodes any CA bucket object to human-readable JSON.

**Architecture:** Part A adds two event emissions (`ManifestPut` in `Build::stageManifest`, `PrecommitRemoved` in `Build::abandon`), deletes three dead enum entries, and fixes two blob-retire audit findings in `closeBlob`/the retire-merge. Part B adds a read-only `clickhouse-disks` command that dispatches a bucket key to the existing decoders. No protocol/invariant change, so no TLA+ gate.

**Tech Stack:** C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`), `programs/disks` (clickhouse-disks), GoogleTest (`unit_tests_dbms`).

## Global Constraints

- Design spec: `docs/superpowers/specs/2026-07-08-cas-observability-audit-and-inspect-design.md`.
- Allman braces; enforced by CI.
- Build into `build/` **in the FOREGROUND**: `ninja -C build <target> > build/<log> 2>&1`, WAIT for exit in the same step, never background it. No `-j`/`nproc`. Summarize the log.
- Unit test binary: `build/src/unit_tests_dbms`. `clickhouse-disks` is built as part of the `clickhouse` target (`programs/disks`).
- **No `LOGICAL_ERROR` for runtime errors.** `ca-inspect` argument/decoding errors use `ErrorCodes::BAD_ARGUMENTS` (as the other `ca-*` commands do). Event emission never throws (best-effort sink).
- Commit on `cas-gc-rebuild` (never master); add new commits; `git add` only the specific files each task lists. Trailers, exactly:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk
  ```
- Wrap literal SQL/class/function names in `code` in commit messages/comments; write a function as `f`, not `f()`.

---

## File Structure

- `CasEvent.h` / `CasEvent.cpp` — delete dead `CasEventType` entries + their string-map lines (Task 1).
- `CasBuild.cpp` — emit `ManifestPut` (`stageManifest`), `PrecommitRemoved` (`abandon`) (Task 1).
- `CasBlobInDegree.{h,cpp}` — side-effect-free supersede peek + carry `old_token` in `RetiredMergeResult.replaced` (Task 2).
- `CasGc.cpp` — pass the side-effect-free peek callback; add `old_token` to the `blob_retire_replaced` emit; fix the stale `~L550` comment (Task 2).
- `programs/disks/CommandCaInspect.cpp` (new) + `programs/disks/DisksApp.cpp` — the CLI command (Task 3).
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.{h,cpp}` (new) — the unit-testable `caInspectToJson` dispatch function (Task 3).
- `src/Disks/tests/gtest_cas_observability.cpp` (new) — tests for Parts A and B (Tasks 1-3).
- `utils/ca-soak/scenarios/BACKLOG.md` — resolve INTROSPECTION-1/2 (Task 4).

---

## Task 1: Part A — emit `ManifestPut` + `PrecommitRemoved`, delete dead enums

**Files:**
- Modify: `.../Core/CasEvent.h` (`CasEventType`), `.../Core/CasEvent.cpp` (string map)
- Modify: `.../Core/CasBuild.cpp` (`Build::stageManifest`, `Build::abandon`)
- Test: `src/Disks/tests/gtest_cas_observability.cpp` (new)

**Interfaces:**
- Consumes: `EventEmitter{*store}.emit([&](CasEvent & e){ … })`; `manifestRefDebugString(const ManifestRef &)`; `CasEvent` fields (`type`, `namespace_`, `ref_name`, `object_kind`, `object_hash`, `token`, `reason`, `detail`); `store->setEventSink(CasEventSink)`.
- Produces: `manifest_put` and `precommit_removed` rows in the event stream.

- [ ] **Step 1: Write the failing tests** (`gtest_cas_observability.cpp`). Capture events via `setEventSink` (the `gtest_cas_event_log.cpp:38` pattern):

```cpp
#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h>
using namespace DB::Cas;

namespace { StorePtr openStore(std::shared_ptr<InMemoryBackend> & b)
{ b = std::make_shared<InMemoryBackend>(); return Store::open(b, PoolConfig{.pool_prefix="p", .server_root_id="test"}); } }

TEST(CasObservability, StageManifestEmitsManifestPut)
{
    std::shared_ptr<InMemoryBackend> b; auto s = openStore(b);
    std::vector<CasEvent> seen; s->setEventSink([&](const CasEvent & e){ seen.push_back(e); });
    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string()+"/all_0_0_0", .intended_namespace = ns});
    ManifestEntry e; e.path="f"; e.placement=EntryPlacement::Inline; e.inline_bytes="AAA";   // verify field name vs CasManifestCodec.h
    build->stageManifest({e});
    s->setEventSink(nullptr);
    EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::ManifestPut; }), 1);
}

TEST(CasObservability, AbandonEmitsPrecommitRemoved)
{
    std::shared_ptr<InMemoryBackend> b; auto s = openStore(b);
    const RootNamespace ns{"srv/tbl@cas@"};
    auto build = s->startBuild(BuildInfo{.intended_ref = ns.string()+"/all_0_0_0", .intended_namespace = ns});
    ManifestEntry e; e.path="f"; e.placement=EntryPlacement::Inline; e.inline_bytes="AAA";
    const ManifestId id = build->stageManifest({e});
    build->precommitAdd(ns, "all_0_0_0", id);
    std::vector<CasEvent> seen; s->setEventSink([&](const CasEvent & x){ seen.push_back(x); });
    build->abandon();
    s->setEventSink(nullptr);
    EXPECT_EQ(std::count_if(seen.begin(), seen.end(),
        [](const CasEvent & x){ return x.type == CasEventType::PrecommitRemoved; }), 1);
}
```

(Verify `ManifestEntry`'s inline field name, `BuildInfo`/`startBuild`/`stageManifest`/`precommitAdd`/`abandon` signatures, and `setEventSink` against the headers — the RED tests fail to compile/assert until Step 3.)

- [ ] **Step 2: Run to verify failure**

`ninja -C build unit_tests_dbms > build/build_obs_t1red.log 2>&1` (foreground) then
`build/src/unit_tests_dbms --gtest_filter='CasObservability.StageManifestEmitsManifestPut:CasObservability.AbandonEmitsPrecommitRemoved' > build/test_obs_t1red.log 2>&1`
Expected: both FAIL (no such events emitted yet).

- [ ] **Step 3: Emit `ManifestPut`** in `Build::stageManifest`, after the successful `sink->finalize()` (`res.outcome == PutOutcome::Done`) and before `staged_manifests.push_back(id)`:

```cpp
    EventEmitter{*store}.emit([&](CasEvent & e)
    {
        e.type = CasEventType::ManifestPut;
        e.namespace_ = owning_ns.string();
        e.object_kind = CasEventObjectKind::Manifest;
        e.object_hash = manifestRefDebugString(id.ref);
        e.token = res.token.value;   // the written body's token (verify PutResult carries `token`; else a post-write HEAD)
        e.reason = "stageManifest: part-manifest body written";
    });
```

- [ ] **Step 4: Emit `PrecommitRemoved`** in `Build::abandon`, right after the precommit-removal `mutateShard` returns (inside the `if (precommitted)` block, after `precommitted = false;`), using the captured `precommit_*` members:

```cpp
        EventEmitter{*store}.emit([&](CasEvent & e)
        {
            e.type = CasEventType::PrecommitRemoved;
            e.namespace_ = precommit_target_ns.string();
            e.ref_name = precommit_final_ref;
            e.object_kind = CasEventObjectKind::Root;
            e.object_hash = manifestRefDebugString(precommit_manifest);
            e.reason = "abandon: precommit binding removed";
        });
```

- [ ] **Step 5: Delete the dead enum entries.** In `CasEvent.h` remove `ManifestExpand, ManifestRetire, ManifestStrip` from the `CasEventType` list (keep `ManifestPut`, `ManifestDelete`). In `CasEvent.cpp` remove the three `case CasEventType::ManifestExpand/ManifestRetire/ManifestStrip:` string-map lines (28, 29, 31). No other reference exists except the stale `CasGc.cpp` comment (fixed in Task 2).

- [ ] **Step 6: Build + run — GREEN**

`ninja -C build unit_tests_dbms > build/build_obs_t1.log 2>&1` (foreground); `build/src/unit_tests_dbms --gtest_filter='CasObservability.StageManifest*:CasObservability.Abandon*' > build/test_obs_t1.log 2>&1` — both PASS. Regression: `build/src/unit_tests_dbms --gtest_filter='CasEventLog*:CasGcLog*:CasBuild*' >> build/test_obs_t1.log 2>&1` — all PASS (deleting unused enum entries + the `default:`-free switch still compiles; confirm the string-map switch has no `default` that now warns, or is exhaustive).

- [ ] **Step 7: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/tests/gtest_cas_observability.cpp
git commit  # "CAS audit: emit `manifest_put` + `precommit_removed`; drop dead manifest event types" + trailers
```

---

## Task 2: Part A — blob-retire audit fixes (side-effect-free supersede peek + `old_token`)

**Files:**
- Modify: `.../Core/CasBlobInDegree.h` (`RetiredMergeResult`), `.../Core/CasBlobInDegree.cpp` (`closeBlob` supersede + `foldDeltasIntoGeneration` signature)
- Modify: `.../Core/CasGc.cpp` (the `head_blob`/peek callbacks it passes; the `blob_retire_replaced` emit loop; the `~L550` comment)
- Test: `src/Disks/tests/gtest_cas_observability.cpp`

**Interfaces:**
- Consumes: `Backend::head(key)` → `HeadResult{exists, token, size}`; `Layout::blobKey(BlobId)`; `RetiredMergeResult::replaced`.
- Produces: exactly one `blob_retire_replaced` per resurrect supersede, carrying `detail["superseded_token"]`, and a single `CasGcRetireReplaced` increment (no accompanying `blob_retire`, no `CasGcRetiredCondemned` double-count).

**Context:** Today `closeBlob`'s supersede branch (`CasBlobInDegree.cpp:240-251`) peeks the current token via the `head_blob` lambda — which is the *fresh-condemn* observation hook (`CasGc.cpp` ~L624-666: emits `BlobRetire` + increments `CasGcRetiredCondemned`/`report.condemned`). So a supersede emits BOTH `blob_retire` and `blob_retire_replaced` and double-counts.

- [ ] **Step 1: Write the failing test** (append). Reuse the resurrect-supersede setup from `gtest_cas_gc_leak.cpp` (`ResurrectReplaced*`): drive a resurrect that supersedes a condemned token, capture events + counters:

```cpp
TEST(CasObservability, ResurrectSupersedeEmitsOnlyRetireReplacedWithOldToken)
{
    // ... reuse the ResurrectReplacedIncarnationReclaimed setup (gtest_cas_gc_leak.cpp): publish+drop P
    //     (condemn token A), resurrect (re-upload token B), drop B, then run the fold round that supersedes ...
    // Capture events for the supersede round via setEventSink; snapshot CasGcRetireReplaced / CasGcRetiredCondemned.
    // After the supersede fold round:
    EXPECT_EQ(count(events, CasEventType::BlobRetireReplaced), 1);
    EXPECT_EQ(count(events, CasEventType::BlobRetire), 0)  << "supersede must not also emit blob_retire";
    // the one blob_retire_replaced carries the superseded (old) token:
    EXPECT_FALSE(replacedEvent.detail.at("superseded_token").empty());
    EXPECT_EQ(CasGcRetireReplaced_delta, 1);
    EXPECT_EQ(CasGcRetiredCondemned_delta, 0) << "supersede peek must not fresh-condemn";
}
```

(The implementer adapts the exact resurrect setup from `gtest_cas_gc_leak.cpp` and reads counters the way that file does. If capturing the specific supersede round's events is awkward, filter the sink by `object_hash == u128ToHex(P)`.)

- [ ] **Step 2: Run to verify failure** (foreground build + focused run). Expected: FAIL — today a `blob_retire` accompanies the `blob_retire_replaced`, `CasGcRetiredCondemned` bumps, and `superseded_token` is absent.

- [ ] **Step 3: Add a side-effect-free peek callback** to `foldDeltasIntoGeneration`. In `CasBlobInDegree.cpp` add a parameter `const std::function<std::optional<HeadResult>(const UInt128 &)> & peek_head` alongside the existing `head_blob` (line 149). In the supersede branch (line 242), replace `head_blob(cur_blob)` with `peek_head(cur_blob)`. `head_blob` (fresh-condemn) stays for the fresh-condemn branch only. Update the `foldDeltasIntoGeneration` declaration in `CasBlobInDegree.h`.

- [ ] **Step 4: Provide the peek callback from the caller** (`CasGc.cpp`, where `foldDeltasIntoGeneration` is invoked ~L956/L1618): pass a side-effect-free lambda:

```cpp
        /*peek_head=*/ [&](const UInt128 & h) -> std::optional<HeadResult>
        {
            const HeadResult hr = backend.head(layout.blobKey(BlobId(u128ToHex(h))));
            return hr.exists ? std::optional<HeadResult>(hr) : std::nullopt;
        },
```

(No `IndegZero`/`GcRetireObserve`/`BlobRetire` emit, no counter increment — a pure HEAD.)

- [ ] **Step 5: Carry `old_token` into the replaced entry.** In `CasBlobInDegree.h`, change `RetiredMergeResult::replaced` from `std::vector<RetiredEntry>` to `std::vector<ReplacedEntry>` with `struct ReplacedEntry { RetiredEntry fresh; Token old_token; };` (or add a `Token superseded_token;` field to the pushed entry). In `closeBlob`'s supersede branch set `old_token = prior_retired[ri].token` when pushing. Update the graduation/still-retired handling that consumes `rmr.replaced` to use `.fresh`.

- [ ] **Step 6: Emit `old_token` in `blob_retire_replaced`.** In `CasGc.cpp`'s `merge.replaced` emit loop (the `EventEmitter … CasEventType::BlobRetireReplaced` loop), add `e.detail["superseded_token"] = entry.old_token.value;` (and use `entry.fresh.*` for the hash/new token/size). Also fix the stale comment ~L550 that name-drops `ManifestExpand` (describe it as the fold's `RootAdd`/`RootRemove` blob-edge events).

- [ ] **Step 7: Build + run — GREEN**

Foreground build; `build/src/unit_tests_dbms --gtest_filter='CasObservability.ResurrectSupersede*:CasGcLeak*:CasBlobIndegree*' > build/test_obs_t2.log 2>&1` — the new test PASSES and the existing `CasGcLeak.*` (resurrect fix) + `CasBlobIndegree*` stay GREEN (the re-condemn behavior is unchanged; only the peek's side effects + the event detail changed).

- [ ] **Step 8: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/tests/gtest_cas_observability.cpp
git commit  # "CAS gc: resurrect supersede uses a side-effect-free peek + records superseded_token (audit fix)" + trailers
```

---

## Task 3: Part B — `clickhouse-disks ca-inspect`

**Files:**
- Create: `.../Core/CasInspect.h`, `.../Core/CasInspect.cpp` (the unit-testable dispatch)
- Create: `programs/disks/CommandCaInspect.cpp`
- Modify: `programs/disks/DisksApp.cpp` (register)
- Test: `src/Disks/tests/gtest_cas_observability.cpp`

**Interfaces:**
- Consumes: `Layout` prefix accessors (`casRefsPrefix()`, `casManifestsPrefix()`, `gcStateKey()`, `mountKey()`, blobs prefix); decoders `decodeRootShard`, `decodePartManifest`, `decodeMountLease`, `decodeGcState`, `decodeFoldSeal`, `decodeRetiredSet`; `CasEnvelope` header parse.
- Produces: `String caInspectToJson(const Layout & layout, const String & key, std::string_view bytes)`.

- [ ] **Step 1: Write the failing test** (append). Encode synthetic objects and assert the JSON contains expected fields:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
// + CasManifestCodec.h, CasServerRoot.h, CasGcFormats.h as needed

TEST(CasObservability, CaInspectDecodesRootShardToJson)
{
    Layout layout("p");
    RootShard root; root.shard_version = 7;
    root.refs["all_0_0_0"] = RootRef{.ref_name="all_0_0_0", .manifest_ref=ManifestRef{.writer_epoch=1,.build_sequence=2,.manifest_ordinal=1}};
    const String key = layout.rootShardKey(RootNamespace{"srv/tbl@cas@"}, 0);
    const String json = caInspectToJson(layout, key, encodeRootShard(root));
    EXPECT_NE(json.find("\"shard_version\""), String::npos);
    EXPECT_NE(json.find("all_0_0_0"), String::npos);
}

TEST(CasObservability, CaInspectUnknownKeyThrows)
{
    Layout layout("p");
    EXPECT_THROW(caInspectToJson(layout, "p/not/a/ca/object", "xxxx"), DB::Exception);   // BAD_ARGUMENTS
}
```

(Add analogous assertions for a manifest key (`decodePartManifest`), a mount key (`decodeMountLease`), and `gc/state` (`decodeGcState`) — encode via the matching encoder, inspect the JSON. Verify encoder/decoder + `Layout` accessor names against the headers.)

- [ ] **Step 2: Run to verify failure** — the test file references `CasInspect.h`/`caInspectToJson`, which don't exist yet → build fails / link error.

- [ ] **Step 3: Implement `caInspectToJson`** (`CasInspect.cpp`): dispatch by `key` against the `Layout` prefixes, decode, and render JSON (use ClickHouse's `WriteBufferFromOwnString` + JSON escaping, or a small hand-rolled JSON matching the fields). Order the checks most-specific first; unknown key → `throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: unrecognized key layout '{}' (recognized: cas/refs, cas/manifests, gc/server-roots/*/mount, gc/state, gc/gen/*/fold_seal, retired, blobs)", key)`:

```cpp
String caInspectToJson(const Layout & layout, const String & key, std::string_view bytes)
{
    if (key.starts_with(layout.casManifestsPrefix()) && key.ends_with(".proto"))
        return renderPartManifest(decodePartManifest(bytes));
    if (key.starts_with(layout.casRefsPrefix()))
        return renderRootShard(decodeRootShard(bytes));
    if (key == layout.gcStateKey())
        return renderGcState(decodeGcState(bytes));
    if (key.ends_with("/mount"))         /// gc/server-roots/<srid>/mount
        return renderMountLease(decodeMountLease(bytes));
    if (key.ends_with("/fold_seal"))
        return renderFoldSeal(decodeFoldSeal(bytes));
    // retired-set keys; blobs/ envelope header; else:
    throw Exception(ErrorCodes::BAD_ARGUMENTS, "ca-inspect: unrecognized key layout '{}' ...", key);
}
```

Each `render*` prints the struct's fields with `u128`/token/hash as lowercase hex. (Match the real decoder return types + field names; the render helpers live in `CasInspect.cpp`.)

- [ ] **Step 4: Add the CLI command** `programs/disks/CommandCaInspect.cpp` modeled on `CommandCaGcDryRun.cpp`: require an object-storage + content-addressed + read-only disk (same three checks, error prefix `ca-inspect:`), read the key argument, fetch its bytes from the disk's object storage, call `caInspectToJson(ca->store()->layout(), key, bytes)`, print to `std::cout`. Register in `programs/disks/DisksApp.cpp` next to `ca-gc-dryrun`: `command_descriptions.emplace("ca-inspect", makeCommandCaInspect());` (+ the `makeCommandCaInspect()` decl alongside the others).

- [ ] **Step 5: Build + run**

`ninja -C build unit_tests_dbms > build/build_obs_t3.log 2>&1` (foreground) — `build/src/unit_tests_dbms --gtest_filter='CasObservability.CaInspect*' > build/test_obs_t3.log 2>&1` PASS. Then `ninja -C build clickhouse > build/build_clickhouse_obs.log 2>&1` (foreground) to confirm the CLI wiring compiles/links.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.cpp \
        programs/disks/CommandCaInspect.cpp programs/disks/DisksApp.cpp \
        src/Disks/tests/gtest_cas_observability.cpp
git commit  # "clickhouse-disks: add `ca-inspect` — decode a CA bucket object to JSON (read-only)" + trailers
```

---

## Task 4: Docs + finalize

**Files:**
- Modify: `utils/ca-soak/scenarios/BACKLOG.md`

- [ ] **Step 1: Manual `ca-inspect` smoke** (optional but recommended): against a real ca-soak pool object (bring the stand up if needed, or point at an existing object), run `clickhouse-disks ... ca-inspect <key>` for a root-shard and a manifest key; confirm readable JSON. Record the output snippet.

- [ ] **Step 2: Resolve the BACKLOG entries** — mark `INTROSPECTION-1` and `INTROSPECTION-2` RESOLVED with the commit trail (the `manifest_put`/`precommit_removed` events, the dead-enum deletion, the two blob-retire audit fixes, and `ca-inspect`).

- [ ] **Step 3: Commit**

```bash
git add utils/ca-soak/scenarios/BACKLOG.md
git commit  # "docs(cas): INTROSPECTION-1/2 resolved — manifest/precommit audit events + ca-inspect landed" + trailers
```

---

## Self-Review

**Spec coverage:** ManifestPut → Task 1 Step 3; PrecommitRemoved → Task 1 Step 4; delete dead enums → Task 1 Step 5; blob-audit (a) side-effect-free peek → Task 2 Steps 3-4; blob-audit (b) old_token → Task 2 Steps 5-6; ca-inspect + dispatch function → Task 3; docs → Task 4. All covered. No TLA+ task (no protocol/invariant change — correct).

**Placeholder scan:** open specifics are all "verify field name/signature against <named header>" (ManifestEntry inline field, PutResult token, encoder/decoder/Layout accessor names) and the resurrect-setup reuse from `gtest_cas_gc_leak.cpp` — each names a concrete existing source; no invented APIs, no TODO/TBD.

**Type consistency:** `caInspectToJson(const Layout &, const String &, std::string_view) -> String` defined in Task 3 and used in the same task's CLI + test. `RetiredMergeResult::replaced` becomes `std::vector<ReplacedEntry>{fresh, old_token}` in Task 2 Step 5 and consumed as `entry.fresh`/`entry.old_token` in Step 6 and the graduation path. `CasEvent` fields match `CasEvent.h`. `peek_head` callback signature matches `head_blob`'s.

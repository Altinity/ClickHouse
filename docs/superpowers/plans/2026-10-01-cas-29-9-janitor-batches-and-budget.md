# CAS-29.9 Janitor Batches, Pipeline and Time Budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On a folding round the namespace janitor drains dead-life `_log`/`_snap` keys page after page under a best-effort 20 s budget, deleting each page's keys as storage-sized batch jobs (or one-key jobs on a store without batch delete) on the GC I/O pool while it lists the next page.

**Architecture:** `NamespaceJanitor::runOnePage` becomes `NamespaceJanitor::run(suppress, JanitorRunContext, NamespaceJanitorResult &)`: the round thread lists, reads the catalog, classifies and runs exact-token `_ckpt`/`_files` deletes; dead canonical stream keys go to jobs of `batch_keys` keys on `io_pool`, at most `gc_io_concurrency` outstanding; one final wait; one publication of the longest complete prefix. The batch size comes from a new `IObjectStorage::batchDeleteKeyLimit` (S3: `objects_chunk_size_to_delete`, 1 once batch delete is known unsupported), forwarded through every `Backend` decorator. No failed request is ever re-sent key by key.

**Tech Stack:** C++23 (ClickHouse, Allman braces), gtest (`unit_tests_dbms`), AWS SDK S3 adapter, pytest integration (`ci.praktika`), docker ca-soak stand (RustFS + Keeper), Python 3 A/B drivers.

**Spec:** `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-09-30-cas-gc-janitor-batches-and-budget-design.md` (r10, approved). Owner decisions: `/home/mfilimonov/workspace/ClickHouse/master/tmp/cas299/owner_decisions.md`. Read both before any task.

**Worktree:** code tasks run in `/home/mfilimonov/workspace/ClickHouse/cas-83`, branch `fix/antalya-26.6/cas-gc-janitor-batches`, based on `e2dd0f5d343` (antalya-26.6). Release build dir `build/`. The A/B task (Task 9) runs its stand from `/home/mfilimonov/workspace/ClickHouse/master` and writes its report there.

## Global Constraints

- Allman braces in all C++ (CI style check).
- Build: `cd /home/mfilimonov/workspace/ClickHouse/cas-83 && ninja -C build unit_tests_dbms > build/<log> 2>&1; echo NINJA_EXIT=$? >> build/<log>`. No `-j`, no `nproc`. Read the last line for `NINJA_EXIT=0`; on failure `grep -a -n "error:" build/<log> | head -40`.
- New `gtest_*.cpp` files are globbed at configure time: run `cmake -S /home/mfilimonov/workspace/ClickHouse/cas-83 -B /home/mfilimonov/workspace/ClickHouse/cas-83/build > /home/mfilimonov/workspace/ClickHouse/cas-83/build/cmake_<task>.log 2>&1` after adding one, before building.
- Every gtest run: `cd /home/mfilimonov/workspace/ClickHouse/cas-83 && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='<suite>*' > build/<log> 2>&1; echo GATE_EXIT=$? >> build/<log> )`. Read `GATE_EXIT=` and `grep -a "FAILED\|PASSED" build/<log> | tail -20`.
- The gate filter is EXACTLY `CAS*`. Every new suite name starts with `CAS`. Non-`CAS` suites that a task touches (`S3BulkDeleteFallback`) are run explicitly in that task, in addition to the gate.
- Before claiming a gate result, confirm the binary is newer than the last source edit: `stat -c %Y build/src/unit_tests_dbms` vs `git -C /home/mfilimonov/workspace/ClickHouse/cas-83 log -1 --format=%ct` and the edited files' mtimes.
- `LOGICAL_ERROR` expectations are death tests whose child exits through `std::_Exit`. This plan adds no new `LOGICAL_ERROR`; if an implementer adds one, its test is an `EXPECT_DEATH` split from the throwing tests.
- No `sleep` for races, in code or tests: order with backend hooks, `ManualBarrier`, `CountdownForTest` and test seams. Engine retry backoff under an injected `FakeClock` is not a race fix.
- No fallback paths: a failed batch never triggers per-key deletes, anywhere.
- No new count setting. The budget is time: constant `kJanitorPhaseBudgetMs = 20'000` with the test seam `PoolConfig::gc_janitor_phase_budget_ms_for_test`.
- Shared object-storage code changes stay minimal: the `IObjectStorage::batchDeleteKeyLimit` virtual (default 1) plus the `S3ObjectStorage` override, and the three `S3Exception` throw sites (two in `S3ObjectStorage.cpp`, one in `deleteFileFromS3.cpp`). Nothing else under `src/IO` or `ObjectStorages/`, except comments that name the removed CAS per-key fallback.
- Commits: `git -C /home/mfilimonov/workspace/ClickHouse/cas-83 add -- <exact files>` then `git -C /home/mfilimonov/workspace/ClickHouse/cas-83 commit -s -m "<msg>"`. Exact file paths only, never a directory. No rebase, no amend, NO push.
- Tests fail first: every task writes its tests, builds, and records the red run before implementing.
- Prose (comments, docs, commit messages) follows `~/.claude/skills/tech-prose/STYLE.md`. Comments give the reason, never history, plan, spec, backlog, review or task references. Names of SQL identifiers, classes and functions go in backticks; functions written as `f`, not `f()`.
- Hooks capturing test locals: declare the captured locals BEFORE the `Pool`/backend that stores the hook (pool teardown can call hooks after the test body).
- A/B measurement (M1, M3, M4) is Task 9 only, using `utils/ca-soak/gc-ab` and `tmp/gc-cuts-stand/value` in the master worktree on local RustFS, `tc netem` 20 ms where a cell says so, and process-wide `system.events` deltas.

## Review Focus

1. **Teardown while jobs are in flight.** `Pool::beginTeardown` during a phase must leave no job touching freed state and must leak nothing: jobs not yet admitted hold, the phase waits for every job. Test: `TeardownDuringPhaseHoldsWithoutLeaking` (Task 6).
2. **Empty namespace or cursor at the end of the pass.** A LIST that returns no keys ends the phase after one page and publishes the empty cursor, with no job. Test: `EmptyNamespaceTakesOnePage` (Task 4).
3. **Catalog read failure on the first page.** It must propagate, publish nothing and submit no job; only page ≥ 2 read failures publish the prefix. Test: `FirstPageCatalogReadFailureThrowsAndPublishesNothing` (Task 4).
4. **Phase clock going backwards.** Elapsed time must saturate at 0, never wrap to a huge value that ends the phase after one page. Test: `ClockGoingBackwardsDoesNotExhaustTheBudget` (Task 4).
5. **`gc_io_concurrency` 0 with a pool present.** `PoolConfig` is not validated like the disk setting; a slot window of 0 would deadlock the round thread. Expected: jobs run inline. Test: `ZeroConcurrencyRunsInline` (Task 6).

## File Structure

| File | Responsibility | Tasks |
|---|---|---|
| `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` | `batchDeleteKeyLimit` virtual, default 1 | 1 |
| `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.{h,cpp}` | S3 override; canonical error name at two throw sites | 1, 3 |
| `src/IO/S3/deleteFileFromS3.cpp` | canonical error name at the one-key throw site | 3 |
| `.../ContentAddressed/Backend/CasBackend.h` | `Backend::bulkDeleteKeyLimit`, default 1 | 1 |
| `.../Backend/CasInMemoryBackend.{h,cpp}` | configurable limit, default `kBulkDeleteMaxKeys` | 1 |
| `.../Backend/CasObjectStorageBackend.{h,cpp}` | Native: storage limit; Emulated: `kBulkDeleteMaxKeys` | 1 |
| `.../Backend/CasInstrumentedBackend.h`, `.../Backend/CasThrottlingBackend.h` | forward the limit | 1 |
| `.../Gc/CasGc.{h,cpp}` | `bulkDeleteChunkKeys`, `removeCohortWriteOnce`, containment in two families, `runNamespaceJanitor`, atomic authority | 1, 2, 4, 6, 7 |
| `.../Gc/CasNamespaceJanitor.{h,cpp}` | the phase: page loop, jobs, stop rules, cursor | 4, 5, 6 |
| `.../Formats/CasLayout.h` | `writeOnceStreamKey` | 4 |
| `.../Pool/CasPool.h` | two `_for_test` seams | 4, 6 |
| `src/Common/setThreadName.h` | `CAS_GC_JANITOR` | 6 |
| `src/Common/ProfileEvents.cpp` | two new events | 7 |
| `.../ContentAddressed/ContentAddressedSettings.cpp` | two setting descriptions | 7 |
| `src/Disks/tests/cas_test_helpers.h` | `BatchCapabilityBackend` | 1 |
| `src/Disks/tests/cas_scripted_s3_server.h` (new) | scripted S3 server and storage factory, shared by S3 tests | 1 |
| `src/Disks/tests/cas_namespace_janitor_test_helpers.h` (new) | janitor fixtures, `JanitorBackend`, `CountdownForTest` | 4 |
| `src/Disks/tests/gtest_cas_namespace_janitor.cpp` | existing tests, migrated to `run` | 4 |
| `src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp` (new) | sequential batching, budget, cursor, failure rules | 4, 5 |
| `src/Disks/tests/gtest_cas_namespace_janitor_pipeline.cpp` (new) | fan-out, slots, stop flag, latch, refresh | 6 |
| `src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp` (new) | janitor tests that need `S3Exception` or the S3 adapter | 5, 6 |
| `src/Disks/tests/gtest_cas_gc_bulk_delete_fallback.cpp` | `removeCohortWriteOnce` unit tests | 2 |
| `src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp`, `gtest_cas_ref_gc.cpp` | family containment and request cut | 2 |
| `src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp` | adapter tests | 1, 3 |
| `src/Disks/tests/gtest_cas_bulk_delete_backend.cpp`, `gtest_cas_write_once_key.cpp` | limit forwarding; canonical stream key | 1, 4 |
| `tests/integration/test_cas_janitor_drain/` (new) | RustFS drain test, three disks | 8 |
| `docs/en/antalya/cas/architecture/garbage-collection.md`, `docs/en/operations/system-tables/cas_gc_log.md`, `docs/en/antalya/cas/configuration.md` | docs | 7 |

`.../` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed`.

## Spec-to-task map

| Spec section | Task |
|---|---|
| Batch size, capability (`batchDeleteKeyLimit`, forwarding, `bulkDeleteChunkKeys`) | 1 |
| Other families, cohort and request, "no per-key recovery" | 2 |
| S3 adapter: keep the error name | 3 |
| Pipeline (sequential model), stop rules, cursor, budget, deferred rounds, code shape | 4 |
| Batch failure (leak and advance; hold classes), capability rejection in the janitor | 5 |
| Jobs on `io_pool`, slots, stop flag, authority (atomic, latch, refresh), schedule refusal | 6 |
| Observability (events), settings descriptions, docs | 7 (row fields land in 4) |
| Integration test | 8 |
| Value proof M1–M4 | 9 |

The spec's job-level table rows are split across Tasks 4 to 6 so each task has its own test cycle. Task 4's jobs hold on every failure; Task 5 replaces that with the spec's classification. The budget lands in Task 4 with the page loop, so no commit runs an unbounded loop.

---

### Task 1: Storage batch-delete limit and its forwarding

**Files:**
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/IObjectStorage.h` (after `removeObjectsIfExistUnderProfile`, ~:380)
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h` (next to `removeObjectsIfExistUnderProfile`, ~:137), `S3ObjectStorage.cpp` (after `removeObjectsIfExistImpl`, ~:771)
- Modify: `.../Backend/CasBackend.h` (after `removeManyWriteOnce`, ~:202)
- Modify: `.../Backend/CasInMemoryBackend.h`, `.../Backend/CasInMemoryBackend.cpp`
- Modify: `.../Backend/CasObjectStorageBackend.h` (~:103), `.../Backend/CasObjectStorageBackend.cpp`
- Modify: `.../Backend/CasInstrumentedBackend.h:160-164`, `.../Backend/CasThrottlingBackend.h:125-128`
- Modify: `.../Gc/CasGc.h`, `.../Gc/CasGc.cpp` (new public `bulkDeleteChunkKeys`)
- Modify: `src/Disks/tests/cas_test_helpers.h` (add `BatchCapabilityBackend` after `CountingBackend`)
- Create: `src/Disks/tests/cas_scripted_s3_server.h`
- Modify: `src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp` (use the new header)
- Test: `src/Disks/tests/gtest_cas_bulk_delete_backend.cpp`, `src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp`

**Interfaces:**
- Produces:
  - `virtual size_t IObjectStorage::batchDeleteKeyLimit() const` — most objects one `removeObjectsIfExistUnderProfile` call sends as one request; ≥ 1; default 1.
  - `size_t S3ObjectStorage::batchDeleteKeyLimit() const override`.
  - `virtual size_t Backend::bulkDeleteKeyLimit() const` — default 1; `InMemoryBackend` answers `setBulkDeleteKeyLimit(size_t)`'s value, default `kBulkDeleteMaxKeys`; `ObjectStorageBackend` Native mode answers the storage's limit, Emulated mode `kBulkDeleteMaxKeys`; `InstrumentedBackend` and `ThrottlingBackend` forward.
  - `size_t Gc::bulkDeleteChunkKeys() const` (public) = `std::clamp(min(gc_bulk_delete_chunk_keys, backend limit), 1, kBulkDeleteMaxKeys)`.
  - Test helper `DB::Cas::tests::BatchCapabilityBackend : CountingBackend` with `setBatchDeleteSupported(std::optional<bool>)`, `setStoreRejectsBatches(bool)`, `setStorageLimit(size_t)`, `requestSizes()`, `callSizes()`, `rejectedBatches()`.
  - Test header `cas_scripted_s3_server.h`, namespace `DB::Cas::tests::s3`: `ScriptedS3Server` (responder gets the request body), `ScriptedDeletes`, `keyOfDeleteObject`, `keysOfDeleteObjects`, `sendXml`, `sendDeleteObjectsResult`, `sendBatchSuccessWithErrors`, `sendBatchNotImplemented`, `sendBatchInternalError`, `sendDeleteObjectSuccess`, `sendSingleDeleteError`, `StorageOptions`, `makeClientForTest`, `makeStorageForTest`, `contextForTest`.

- [ ] **Step 0: Save the base binary for Task 9**

The A/B needs a server binary built from `e2dd0f5d343`, and `build/` moves with the branch after this task.

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
git rev-parse HEAD            # expect e2dd0f5d343...
git status --short -- src     # expect no output
ninja -C build clickhouse > build/t1_base_clickhouse.log 2>&1; echo NINJA_EXIT=$? >> build/t1_base_clickhouse.log
tail -1 build/t1_base_clickhouse.log      # NINJA_EXIT=0
mkdir -p tmp/ab && cp build/programs/clickhouse tmp/ab/clickhouse-base-e2dd0f5d343
```

- [ ] **Step 1: Move the scripted S3 server into a shared header**

Create `src/Disks/tests/cas_scripted_s3_server.h` with the helpers now in the anonymous namespace of `gtest_cas_s3_bulk_delete_fallback.cpp` (lines ~55–245), changed as below. The responder now receives the request body, which the handler reads in full (that also keeps the keep-alive framing correct).

```cpp
#pragma once

#include "config.h"

#if USE_AWS_S3

#include <Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h>
#include <Disks/DiskObjectStorage/ObjectStorages/StoredObject.h>
#include <IO/S3/Client.h>
#include <IO/S3/S3Capabilities.h>
#include <IO/S3/URI.h>
#include <IO/S3Common.h>
#include <IO/S3Settings.h>
#include <Common/RemoteHostFilter.h>
#include <Common/tests/gtest_global_context.h>

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/SharedPtr.h>
#include <Poco/StreamCopier.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace DB::S3RequestSetting
{
    extern const S3RequestSettingsUInt64 objects_chunk_size_to_delete;
}

namespace DB::Cas::tests::s3
{

/// A local HTTP server standing in for S3. `DeleteObjects` arrives as a POST to the bucket root; a one-key
/// `DeleteObject` arrives as an HTTP DELETE of the key's path.
class ScriptedS3Server
{
public:
    using Responder = std::function<void(
        const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse &)>;

private:
    class Handler : public Poco::Net::HTTPRequestHandler
    {
        ScriptedS3Server & owner;

    public:
        explicit Handler(ScriptedS3Server & owner_) : owner(owner_) { }

        void handleRequest(Poco::Net::HTTPServerRequest & request, Poco::Net::HTTPServerResponse & response) override
        {
            {
                std::lock_guard lock(owner.mutex);
                owner.methods_seen.push_back(request.getMethod());
            }
            /// An unread body on a keep-alive connection would be parsed as the start of the next request.
            std::string body;
            Poco::StreamCopier::copyToString(request.stream(), body);
            owner.responder(request, body, response);
        }
    };

    class Factory : public Poco::Net::HTTPRequestHandlerFactory
    {
        ScriptedS3Server & owner;

        Poco::Net::HTTPRequestHandler * createRequestHandler(const Poco::Net::HTTPServerRequest &) override
        {
            return new Handler(owner);
        }

    public:
        explicit Factory(ScriptedS3Server & owner_) : owner(owner_) { }
    };

    std::unique_ptr<Poco::Net::ServerSocket> server_socket;
    Poco::SharedPtr<Factory> handler_factory;
    Poco::AutoPtr<Poco::Net::HTTPServerParams> server_params;
    std::unique_ptr<Poco::Net::HTTPServer> server;
    Responder responder;
    mutable std::mutex mutex;
    std::vector<std::string> methods_seen;

public:
    explicit ScriptedS3Server(Responder responder_)
        : server_socket(std::make_unique<Poco::Net::ServerSocket>(0))
        , handler_factory(new Factory(*this))
        , server_params(new Poco::Net::HTTPServerParams())
        , server(std::make_unique<Poco::Net::HTTPServer>(handler_factory, *server_socket, server_params))
        , responder(std::move(responder_))
    {
        server->start();
    }

    ~ScriptedS3Server() { server->stopAll(true); }

    std::string getUrl() const { return "http://" + server_socket->address().toString(); }

    size_t countMethod(const std::string & method) const
    {
        std::lock_guard lock(mutex);
        return static_cast<size_t>(std::count(methods_seen.begin(), methods_seen.end(), method));
    }
};

/// The keys a scripted server removed, so a test double can mirror them into its own store.
class ScriptedDeletes
{
public:
    void add(const std::string & key)
    {
        std::lock_guard lock(mutex);
        keys.insert(key);
    }

    bool contains(const std::string & key) const
    {
        std::lock_guard lock(mutex);
        return keys.contains(key);
    }

private:
    mutable std::mutex mutex;
    std::set<std::string> keys;
};

/// The object key of a one-key `DeleteObject`: its path after the bucket, without the query string.
inline std::string keyOfDeleteObject(const Poco::Net::HTTPServerRequest & request)
{
    const std::string prefix = "/test-bucket/";
    const std::string & uri = request.getURI();
    const std::string path = uri.substr(0, uri.find('?'));
    return path.starts_with(prefix) ? path.substr(prefix.size()) : path;
}

/// The keys a `DeleteObjects` body names, in body order.
inline std::vector<std::string> keysOfDeleteObjects(const std::string & body)
{
    std::vector<std::string> keys;
    for (size_t open = body.find("<Key>"); open != std::string::npos; open = body.find("<Key>", open))
    {
        open += 5;
        const size_t close = body.find("</Key>", open);
        keys.push_back(body.substr(open, close - open));
    }
    return keys;
}

inline void sendXml(Poco::Net::HTTPServerResponse & response, Poco::Net::HTTPResponse::HTTPStatus status, const std::string & body)
{
    response.setContentType("application/xml");
    response.setContentLength(body.size());
    response.setStatus(status);
    auto & out = response.send();
    out << body;
    out.flush();
}

/// A quiet-mode `DeleteObjects` answer (HTTP 200) listing only `errors`, as `(key, code)`, in order.
inline void sendDeleteObjectsResult(
    Poco::Net::HTTPServerResponse & response, const std::vector<std::pair<std::string, std::string>> & errors)
{
    std::string body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                       "<DeleteResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    for (const auto & [key, code] : errors)
        body += "<Error><Key>" + key + "</Key><Code>" + code + "</Code><Message>" + code + "</Message></Error>";
    body += "</DeleteResult>";
    sendXml(response, Poco::Net::HTTPResponse::HTTP_OK, body);
}
```

Then move, unchanged apart from being `inline`: `sendBatchSuccessWithErrors`, `sendBatchNotImplemented`, `sendBatchInternalError`, `sendDeleteObjectSuccess`, `sendSingleDeleteError`, `contextForTest`. Replace `makeStorageForTest` with:

```cpp
struct StorageOptions
{
    std::optional<uint64_t> objects_chunk_size_to_delete;
    /// Empty: the storage keeps its default callback, which returns no client, so nothing is reissued.
    DB::S3ObjectStorage::S3CredentialsRefreshCallback credentials_refresh_callback;
};

inline std::unique_ptr<DB::S3::Client> makeClientForTest(const std::string & endpoint)
{
    /// Body of the old `makeStorageForTest` up to `ClientFactory::instance().create(...)`, returning the client.
}

inline std::shared_ptr<DB::S3ObjectStorage> makeStorageForTest(
    const std::string & endpoint, const DB::S3Capabilities & capabilities, const StorageOptions & options = {})
{
    auto settings = std::make_unique<DB::S3Settings>();
    if (options.objects_chunk_size_to_delete)
        settings->request_settings[DB::S3RequestSetting::objects_chunk_size_to_delete] = *options.objects_chunk_size_to_delete;
    const DB::S3::URI uri(endpoint + "/test-bucket/");
    if (options.credentials_refresh_callback)
        return std::make_shared<DB::S3ObjectStorage>(
            makeClientForTest(endpoint), std::move(settings), uri, capabilities, DB::ObjectStorageKeyGeneratorPtr{},
            "disk", /*for_disk_s3=*/ true, options.credentials_refresh_callback);
    return std::make_shared<DB::S3ObjectStorage>(
        makeClientForTest(endpoint), std::move(settings), uri, capabilities, DB::ObjectStorageKeyGeneratorPtr{}, "disk");
}

}

#endif
```

If `~ScriptedS3Server` with `stopAll(true)` does not compile against this Poco (check `Poco/Net/HTTPServer.h` / `TCPServer.h`), drop the destructor: the old class had none.

In `gtest_cas_s3_bulk_delete_fallback.cpp`: delete the moved code, `#include <Disks/tests/cas_scripted_s3_server.h>` inside the `#if USE_AWS_S3` block, add `using namespace DB::Cas::tests::s3;`, and change every responder lambda to take `(const Poco::Net::HTTPServerRequest & request, const std::string &, Poco::Net::HTTPServerResponse & response)` (drop the parameter name where unused). Keep `expectNotImplemented` in the file.

- [ ] **Step 2: Write the failing tests**

Add `BatchCapabilityBackend` to `src/Disks/tests/cas_test_helpers.h`, right after `class CountingBackend` ends, inside `namespace DB::Cas::tests`. Add `extern const int NOT_IMPLEMENTED;` to the header's existing `namespace DB::ErrorCodes` block.

```cpp
/// The in-memory store with `S3ObjectStorage`'s batch-delete capability. A `removeManyWriteOnce` of more
/// than one key is refused locally with `NOT_IMPLEMENTED` once the capability is false; a store that rejects
/// `DeleteObjects` turns an unknown capability false on its first multi-key request. One key always goes.
class BatchCapabilityBackend : public CountingBackend
{
public:
    size_t bulkDeleteKeyLimit() const override
    {
        std::lock_guard lock(capability_mutex);
        return batch_supported == std::optional<bool>{false} ? 1 : storage_limit;
    }

    void removeManyWriteOnce(const std::vector<DB::Cas::WriteOnceKey> & keys, DB::Cas::TransportAccess & access) override
    {
        {
            std::lock_guard lock(capability_mutex);
            call_sizes.push_back(keys.size());
            if (keys.size() > 1 && batch_supported == std::optional<bool>{false})
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "batch delete is known unsupported");
            request_sizes.push_back(keys.size());
            if (keys.size() > 1 && !batch_supported && rejects_batches)
            {
                batch_supported = false;
                ++rejected_batches;
                throw DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "the store rejected DeleteObjects");
            }
        }
        CountingBackend::removeManyWriteOnce(keys, access);
    }

    void setBatchDeleteSupported(std::optional<bool> value)
    {
        std::lock_guard lock(capability_mutex);
        batch_supported = value;
    }

    void setStoreRejectsBatches(bool value)
    {
        std::lock_guard lock(capability_mutex);
        rejects_batches = value;
    }

    void setStorageLimit(size_t value)
    {
        std::lock_guard lock(capability_mutex);
        storage_limit = value;
    }

    /// Every `removeManyWriteOnce` that reached this backend, locally refused ones included.
    std::vector<size_t> callSizes() const
    {
        std::lock_guard lock(capability_mutex);
        return call_sizes;
    }

    /// Requests the modeled store received: calls minus local refusals.
    std::vector<size_t> requestSizes() const
    {
        std::lock_guard lock(capability_mutex);
        return request_sizes;
    }

    size_t rejectedBatches() const
    {
        std::lock_guard lock(capability_mutex);
        return rejected_batches;
    }

private:
    mutable std::mutex capability_mutex;
    std::optional<bool> batch_supported;
    bool rejects_batches = false;
    size_t storage_limit = DB::Cas::kBulkDeleteMaxKeys;
    size_t rejected_batches = 0;
    std::vector<size_t> call_sizes;
    std::vector<size_t> request_sizes;
};
```

Append to `src/Disks/tests/gtest_cas_bulk_delete_backend.cpp` (it already includes `CasInstrumentedBackend.h`, `CasThrottlingBackend.h`, `cas_test_helpers.h`; add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>` and `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>`):

```cpp
TEST(CASBulkDeleteBackend, InMemoryStorageLimitDefaultsToTheCasMaximum)
{
    InMemoryBackend backend;
    EXPECT_EQ(backend.bulkDeleteKeyLimit(), kBulkDeleteMaxKeys);
    backend.setBulkDeleteKeyLimit(250);
    EXPECT_EQ(backend.bulkDeleteKeyLimit(), 250u);
}

TEST(CASBulkDeleteBackend, PoolWrappedBackendForwardsStorageLimit)
{
    auto backend = std::make_shared<DB::Cas::tests::BatchCapabilityBackend>();
    backend->setStorageLimit(250);
    auto store = DB::Cas::tests::openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/ 0);
    ASSERT_NE(dynamic_cast<InstrumentedBackend *>(store->poolBackendPtr().get()), nullptr)
        << "the pool must wrap its backend, or this test proves nothing about the decorator";
    EXPECT_EQ(store->poolBackendPtr()->bulkDeleteKeyLimit(), 250u);
    Gc gc(store, DB::UInt128{1});
    EXPECT_EQ(gc.bulkDeleteChunkKeys(), 250u);
}

TEST(CASBulkDeleteBackend, GcChunkIsTheSmallerOfSettingAndStorageLimit)
{
    auto backend = std::make_shared<DB::Cas::tests::BatchCapabilityBackend>();
    backend->setStorageLimit(700);
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_bulk_delete_chunk_keys = 300});
    EXPECT_EQ(Gc(store, DB::UInt128{1}).bulkDeleteChunkKeys(), 300u);
    backend->setBatchDeleteSupported(false);
    EXPECT_EQ(Gc(store, DB::UInt128{1}).bulkDeleteChunkKeys(), 1u);
}

TEST(CASBulkDeleteBackend, ZeroStorageLimitClampsGcChunkToOneKey)
{
    auto backend = std::make_shared<DB::Cas::tests::BatchCapabilityBackend>();
    backend->setStorageLimit(0);
    auto store = DB::Cas::tests::openPoolForTest(backend, 0);
    EXPECT_EQ(Gc(store, DB::UInt128{1}).bulkDeleteChunkKeys(), 1u);
}
```

Inside the file's existing `#if USE_AWS_S3` region (the one holding `ThrottlingRefusesTheChunkOnceAndTheEngineReissuesIt`; if that test is not guarded, put this next to it unguarded):

```cpp
TEST(CASBulkDeleteBackend, ThrottlingBackendForwardsStorageLimit)
{
    auto inner = std::make_shared<InMemoryBackend>();
    inner->setBulkDeleteKeyLimit(250);
    ThrottlingBackend backend(inner, ThrottlingBackend::Mode::FirstPerKey, 1, 429);
    EXPECT_EQ(backend.bulkDeleteKeyLimit(), 250u);
}
```

If `PoolConfig`'s designated-initializer order rejects `.gc_bulk_delete_chunk_keys` after `.server_root_id`, build the config by assignment instead: `PoolConfig config{.pool_prefix = "p", .server_root_id = "test"}; config.gc_bulk_delete_chunk_keys = 300;`.

Append to `gtest_cas_s3_bulk_delete_fallback.cpp`, inside `#if USE_AWS_S3`:

```cpp
TEST(CASS3BatchDelete, S3BatchDeleteKeyLimitFollowsChunkSetting)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendBatchNotImplemented(response);
    });

    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{})->batchDeleteKeyLimit(), 1000u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 500})->batchDeleteKeyLimit(), 500u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 0})->batchDeleteKeyLimit(), 1u);
    EXPECT_EQ(makeStorageForTest(server.getUrl(), DB::S3Capabilities{false})->batchDeleteKeyLimit(), 1u);

    auto learning = makeStorageForTest(server.getUrl(), DB::S3Capabilities{}, {.objects_chunk_size_to_delete = 500});
    expectNotImplemented([&]
    {
        learning->removeObjectsIfExistUnderProfile({DB::StoredObject("key-a"), DB::StoredObject("key-b")}, DB::ObjectStorageControlRequest{});
    });
    EXPECT_EQ(learning->batchDeleteKeyLimit(), 1u) << "a learned false capability caps every later GC request at one key";
}
```

- [ ] **Step 3: Run the build to verify the tests fail**

Run: `cd /home/mfilimonov/workspace/ClickHouse/cas-83 && ninja -C build unit_tests_dbms > build/t1_red.log 2>&1; echo NINJA_EXIT=$? >> build/t1_red.log; tail -1 build/t1_red.log; grep -a -n "error:" build/t1_red.log | head`
Expected: `NINJA_EXIT=1`, errors naming `bulkDeleteKeyLimit`, `setBulkDeleteKeyLimit`, `bulkDeleteChunkKeys`, `batchDeleteKeyLimit`.

- [ ] **Step 4: Implement**

`IObjectStorage.h`, right after the `removeObjectsIfExistUnderProfile` declaration:

```cpp
    /// The most objects one `removeObjectsIfExistUnderProfile` call sends as one request; at least 1.
    /// A content-addressed caller cuts its requests to this size, so one admitted request stays one
    /// storage request. The default suits a storage without a batch delete.
    virtual size_t batchDeleteKeyLimit() const { return 1; }
```

`S3ObjectStorage.h`, after `removeObjectsIfExistUnderProfile`:

```cpp
    /// `objects_chunk_size_to_delete`, at least 1, while `DeleteObjects` is not known unsupported; 1 after.
    size_t batchDeleteKeyLimit() const override;
```

`S3ObjectStorage.cpp`, after `removeObjectsIfExistImpl`:

```cpp
size_t S3ObjectStorage::batchDeleteKeyLimit() const
{
    if (const auto supported = s3_capabilities.isBatchDeleteSupported(); supported.has_value() && !*supported)
        return 1;
    return std::max<size_t>(1, s3_settings.get()->request_settings[S3RequestSetting::objects_chunk_size_to_delete]);
}
```

`CasBackend.h`, after the `removeManyWriteOnce` declaration:

```cpp
    /// The most keys one `removeManyWriteOnce` sends to the storage as one request; at least 1. A decorator
    /// must forward it: the default of 1 would turn every batch below it into one-key requests.
    virtual size_t bulkDeleteKeyLimit() const { return 1; }
```

`CasInMemoryBackend.h`: add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>` (for `kBulkDeleteMaxKeys`; confirm `CasRequests.h` does not include `CasInMemoryBackend.h`) and `#include <atomic>`; next to `bulkRemoveCalls`:

```cpp
    /// `kBulkDeleteMaxKeys` unless a test sets another limit.
    size_t bulkDeleteKeyLimit() const override { return bulk_delete_key_limit.load(); }
    void setBulkDeleteKeyLimit(size_t limit) { bulk_delete_key_limit.store(limit); }
```

and a private member `std::atomic<size_t> bulk_delete_key_limit{kBulkDeleteMaxKeys};`.

`CasObjectStorageBackend.h`, next to `removeManyWriteOnce`:

```cpp
    /// Native: the object storage's own limit. Emulated: `kBulkDeleteMaxKeys`, since this backend deletes
    /// the keys one by one under its emulation lock.
    size_t bulkDeleteKeyLimit() const override;
```

`CasObjectStorageBackend.cpp` (include `CasRequests.h` if not yet):

```cpp
size_t ObjectStorageBackend::bulkDeleteKeyLimit() const
{
    return mode == Mode::Native ? object_storage->batchDeleteKeyLimit() : kBulkDeleteMaxKeys;
}
```

`CasInstrumentedBackend.h` and `CasThrottlingBackend.h`, next to `attemptEnvelopeMs`:

```cpp
    size_t bulkDeleteKeyLimit() const override { return inner->bulkDeleteKeyLimit(); }
```

`CasGc.h`, public section of `class Gc` (next to `setPhaseSink`):

```cpp
    /// Keys per GC write-once delete request: `gc_bulk_delete_chunk_keys` capped by the storage's batch
    /// limit, clamped to [1, `kBulkDeleteMaxKeys`] because an empty request would never advance.
    size_t bulkDeleteChunkKeys() const;
```

`CasGc.cpp`:

```cpp
size_t Gc::bulkDeleteChunkKeys() const
{
    const uint64_t configured = store->poolConfig().gc_bulk_delete_chunk_keys;
    const uint64_t storage_limit = store->poolBackendPtr()->bulkDeleteKeyLimit();
    return std::clamp<size_t>(std::min(configured, storage_limit), 1, kBulkDeleteMaxKeys);
}
```

- [ ] **Step 5: Build and run the tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build unit_tests_dbms > build/t1_build.log 2>&1; echo NINJA_EXIT=$? >> build/t1_build.log; tail -1 build/t1_build.log
( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CASBulkDeleteBackend*:CASS3BatchDelete*:S3BulkDeleteFallback*' > build/t1_green.log 2>&1; echo GATE_EXIT=$? >> build/t1_green.log )
tail -3 build/t1_green.log
```
Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`, all listed tests PASSED (the six existing `S3BulkDeleteFallback` tests included).

- [ ] **Step 6: Gate**

Run: `cd /home/mfilimonov/workspace/ClickHouse/cas-83 && ( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CAS*' > build/t1_gate.log 2>&1; echo GATE_EXIT=$? >> build/t1_gate.log ); tail -3 build/t1_gate.log`
Expected: `GATE_EXIT=0`. Nothing calls `bulkDeleteChunkKeys` from GC yet, so no behaviour changes.

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
F=src/Disks/DiskObjectStorage
C=$F/MetadataStorages/ContentAddressed
git add -- $F/ObjectStorages/IObjectStorage.h $F/ObjectStorages/S3/S3ObjectStorage.h $F/ObjectStorages/S3/S3ObjectStorage.cpp \
  $C/Backend/CasBackend.h $C/Backend/CasInMemoryBackend.h $C/Backend/CasObjectStorageBackend.h $C/Backend/CasObjectStorageBackend.cpp \
  $C/Backend/CasInstrumentedBackend.h $C/Backend/CasThrottlingBackend.h $C/Gc/CasGc.h $C/Gc/CasGc.cpp \
  src/Disks/tests/cas_test_helpers.h src/Disks/tests/cas_scripted_s3_server.h \
  src/Disks/tests/gtest_cas_bulk_delete_backend.cpp src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp
git commit -s -m "cas: size GC delete requests by the storage's batch-delete limit"
```

Add `$C/Backend/CasInMemoryBackend.cpp` only if Step 4 touched it.

---

### Task 2: One request per storage batch in the ref and manifest cleanups, no per-key fallback

**Files:**
- Modify: `.../Gc/CasGc.h:75-96` (replace `removeChunkWriteOnceOrOneByOne` with `removeCohortWriteOnce`), `:874` (`cleanupRefObjects` returns `bool`)
- Modify: `.../Gc/CasGc.cpp:393-408` (helper), `:1252-1300` (manifest cleanup), `:1336-1345` (ref cleanup phase), `:3706-3860` (`cleanupRefObjects`)
- Modify comments naming the removed fallback: `.../Backend/CasObjectStorageBackend.cpp:~1017`, `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:~674` and `~:686-692`, `S3ObjectStorage.h:~131-136`, `src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp` file comment and `expectNotImplemented` comment
- Test: `src/Disks/tests/gtest_cas_gc_bulk_delete_fallback.cpp` (rewrite), `src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp`, `src/Disks/tests/gtest_cas_ref_gc.cpp`

**Interfaces:**
- Consumes: `Gc::bulkDeleteChunkKeys()`, `BatchCapabilityBackend` (Task 1).
- Produces:
  - `void removeCohortWriteOnce(CasOperation & op, const std::vector<WriteOnceKey> & cohort, size_t request_keys, const Retry & policy, const std::function<void(size_t begin, size_t end)> & on_request_done)` (free function in `DB::Cas`, declared in `CasGc.h`).
  - `bool Gc::cleanupRefObjects(...)`: true when the storage rejected a batch delete.
  - Phase rows: `manifest_deletes` gains `unsent`, `capability_learned`; `ref_object_cleanup` gains `capability_learned`.

- [ ] **Step 1: Write the failing tests**

Rewrite `src/Disks/tests/gtest_cas_gc_bulk_delete_fallback.cpp` (keep its includes, `manifestKey`, `manifestKeys`, `openPlainPool`; delete all five old tests and the file comment):

```cpp
/// `removeCohortWriteOnce` (CasGc.h) cuts a cohort into requests of the storage's batch size and never
/// re-sends a rejected request in smaller pieces.

namespace
{

/// Creates `keys`, one committed body each.
void seedBodies(CasOperation & op, const std::vector<WriteOnceKey> & keys)
{
    for (const WriteOnceKey & key : keys)
        ASSERT_TRUE(std::holds_alternative<Committed>(op.create(key.str(), "b", Retry::once())));
}

}

TEST(CASGCCohortDelete, RequestsAreCutAtTheRequestSize)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    CasOperation op = store->openRequests().admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(5);
    seedBodies(op, keys);

    std::vector<std::pair<size_t, size_t>> requests;
    removeCohortWriteOnce(op, keys, 2, Retry::once(), [&](size_t begin, size_t end) { requests.emplace_back(begin, end); });

    EXPECT_EQ(requests, (std::vector<std::pair<size_t, size_t>>{{0, 2}, {2, 4}, {4, 5}}));
    EXPECT_EQ(backend->bulkRemoveCalls(), 3u);
    for (const WriteOnceKey & key : keys)
        EXPECT_FALSE(op.head(key.str(), Retry::once()).has_value()) << key.str();
}

TEST(CASGCCohortDelete, CohortHelperPropagatesNotImplemented)
{
    {
        SCOPED_TRACE("request size 1000, the first request is rejected");
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPlainPool(backend);
        CasOperation op = store->openRequests().admit();
        const std::vector<WriteOnceKey> keys = manifestKeys(1000);
        seedBodies(op, keys);
        backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "no batch delete")));
        size_t callbacks = 0;

        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
            [&] { removeCohortWriteOnce(op, keys, 1000, Retry::once(), [&](size_t, size_t) { ++callbacks; }); });

        EXPECT_EQ(backend->bulkRemoveCalls(), 1u) << "a rejected request is never re-sent key by key";
        EXPECT_EQ(callbacks, 0u);
        for (const WriteOnceKey & key : keys)
            EXPECT_TRUE(op.head(key.str(), Retry::once()).has_value()) << key.str();
    }
    {
        SCOPED_TRACE("request size 250, the second request is rejected");
        auto backend = std::make_shared<InMemoryBackend>();
        auto store = openPlainPool(backend);
        CasOperation op = store->openRequests().admit();
        const std::vector<WriteOnceKey> keys = manifestKeys(1000);
        seedBodies(op, keys);
        /// The hook runs only on a call that applies, so it arms the rejection for the call after the first.
        backend->onBeforeBulkRemove([&]
        {
            backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::NOT_IMPLEMENTED, "no batch delete")));
        });
        size_t callbacks = 0;

        expectThrowsCode(DB::ErrorCodes::NOT_IMPLEMENTED,
            [&] { removeCohortWriteOnce(op, keys, 250, Retry::once(), [&](size_t, size_t) { ++callbacks; }); });

        EXPECT_EQ(backend->bulkRemoveCalls(), 2u);
        EXPECT_EQ(callbacks, 1u);
        for (size_t i = 0; i < keys.size(); ++i)
            EXPECT_EQ(op.head(keys[i].str(), Retry::once()).has_value(), i >= 250) << keys[i].str();
    }
}

TEST(CASGCCohortDelete, ARealErrorStopsTheRemainingRequestsAndPropagates)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openPlainPool(backend);
    CasOperation op = store->openRequests().admit();
    const std::vector<WriteOnceKey> keys = manifestKeys(6);
    seedBodies(op, keys);
    backend->onBeforeBulkRemove([&]
    {
        backend->failNextBulkRemoveWith(std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "not a capability problem")));
    });

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { removeCohortWriteOnce(op, keys, 2, Retry::once(), [](size_t, size_t) {}); });

    EXPECT_EQ(backend->bulkRemoveCalls(), 2u) << "request 1 applied, request 2 failed, request 3 never sent";
    for (size_t i = 0; i < keys.size(); ++i)
        EXPECT_EQ(op.head(keys[i].str(), Retry::once()).has_value(), i >= 2) << keys[i].str();
}
```

First add `PhaseReads` and `phaseReadsSink` to `src/Disks/tests/cas_test_helpers.h`, inside `namespace DB::Cas::tests` after `BatchCapabilityBackend` (`phaseReadsSink` as an `inline` function); both test files below use them. Then, in `gtest_cas_gc_manifest_bulk_delete.cpp`, replace `NotImplementedFallsBackToOneRequestPerKeyAndStillRecordsAllOfThem` with the three tests after the helper block. The file already has `using namespace DB::Cas::tests;`.

```cpp
/// In cas_test_helpers.h. Keeps `row` = the metrics of the last `phase` row, and the catalog and `gc/state`
/// reads made between the end of `before_phase` and the end of `phase`.
struct PhaseReads
{
    std::map<String, UInt64> row;
    uint64_t catalog_before = 0;
    uint64_t state_before = 0;
    uint64_t catalog_in_phase = 0;
    uint64_t state_in_phase = 0;
};

inline DB::Cas::GcPhaseSink phaseReadsSink(PhaseReads & reads, const CountingBackend & backend, const DB::Cas::Layout & layout,
                                           const String & before_phase, const String & phase)
{
    return [&reads, &backend, layout, before_phase, phase](const GcPhaseRecord & record)
    {
        if (record.phase == before_phase)
        {
            reads.catalog_before = backend.getCount(layout.refCatalogKey());
            reads.state_before = backend.getCount(layout.gcStateKey());
        }
        else if (record.phase == phase)
        {
            reads.row = record.metrics;
            reads.catalog_in_phase = backend.getCount(layout.refCatalogKey()) - reads.catalog_before;
            reads.state_in_phase = backend.getCount(layout.gcStateKey()) - reads.state_before;
        }
    };
}
```

```cpp
/// In gtest_cas_gc_manifest_bulk_delete.cpp.
TEST(CASGCManifestBulkDelete, ManifestCleanupSendsCohortAsStorageRequests)
{
    PhaseReads reads;
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setBatchDeleteSupported(false);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_bulk_delete_chunk_keys = 4;
    auto store = Pool::open(backend, config);
    const auto ids = seedDroppedManifests(*backend, store->layout(), 10);

    Gc gc(store, kGc);
    gc.setPhaseSink(phaseReadsSink(reads, *backend, store->layout(), "handoff_reclaim", "manifest_deletes"));
    const uint64_t deleted = reclaim(gc, store, *backend, ids, 16);
    gc.setPhaseSink({});

    EXPECT_EQ(deleted, 10u);
    const std::vector<size_t> calls = backend->callSizes();
    EXPECT_EQ(calls.size(), 10u);
    EXPECT_TRUE(std::all_of(calls.begin(), calls.end(), [](size_t n) { return n == 1; }));
    EXPECT_EQ(reads.row["requests"], 10u);
    EXPECT_EQ(reads.row["accepted"], 10u);
    EXPECT_EQ(reads.row["unsent"], 0u);
    EXPECT_EQ(reads.catalog_in_phase, 0u) << "manifest cleanup reads no authority, as before";
    EXPECT_EQ(reads.state_in_phase, 0u);
}

TEST(CASGCManifestBulkDelete, ZeroStorageLimitCutsOneKeyRequests)
{
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStorageLimit(0);
    auto store = Pool::open(backend, PoolConfig{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0});
    const auto ids = seedDroppedManifests(*backend, store->layout(), 5);
    Gc gc(store, kGc);
    EXPECT_EQ(reclaim(gc, store, *backend, ids, 16), 5u);
    const std::vector<size_t> calls = backend->callSizes();
    EXPECT_EQ(calls, std::vector<size_t>(5, 1));
}

TEST(CASGCManifestBulkDelete, ManifestCleanupContainsCapabilityRejection)
{
    std::vector<std::map<String, UInt64>> rows;
    auto backend = std::make_shared<BatchCapabilityBackend>();
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_bulk_delete_chunk_keys = 2;
    auto store = Pool::open(backend, config);
    const auto ids = seedDroppedManifests(*backend, store->layout(), 5);
    /// The first cohort goes through; the store then rejects `DeleteObjects` on the next one.
    backend->onBeforeBulkRemove([&] { backend->setStoreRejectsBatches(true); });

    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & record)
    {
        if (record.phase == "manifest_deletes" && record.metrics.at("attempted") > 0)
            rows.push_back(record.metrics);
    });
    for (size_t round = 0; round < 16 && rows.empty(); ++round)
    {
        ASSERT_NO_THROW((void)runRegularRoundReclaiming(gc)) << "a rejected batch must not escape the round";
        store->renewWatermarkOnce();
    }
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0]["capability_learned"], 1u);
    EXPECT_EQ(rows[0]["accepted"], 2u) << "the cohort before the rejection stays recorded";
    EXPECT_EQ(rows[0]["unsent"], 3u);
    EXPECT_EQ(backend->requestSizes(), (std::vector<size_t>{2, 2})) << "no one-key request for the rejected cohort this round";

    /// The intake cursor that found the bodies is committed, so only the orphan-manifest sweep reclaims them.
    gc.setPhaseSink({});
    OperationForTest op(*backend);
    bool all_gone = false;
    for (size_t round = 0; round < 64 && !all_gone; ++round)
    {
        (void)runRegularRoundReclaiming(gc);
        store->renewWatermarkOnce();
        all_gone = std::none_of(ids.begin(), ids.end(),
            [&](const ManifestId & id) { return (*op).head(store->layout().manifestKey(id), Retry::once()).has_value(); });
    }
    EXPECT_TRUE(all_gone) << "the orphan-manifest sweep deletes the bodies the rejected round left";
}
```

If the orphan-manifest sweep has a grace period that 64 forced rounds do not cover, read `CasOrphanManifestSweep.cpp` for its pacing, set the smallest `PoolConfig` knob that shortens it in this test, and say which in the commit body.

In `gtest_cas_ref_gc.cpp`, replace `RefObjectCleanupFallsBackToOnePerKeyWhenBatchDeleteIsUnsupported` by the tests below. Extract the existing fixture (live namespace, manifests, transitions, snapshots, checkpoint, plan) into a file-local helper `RefCleanupFixture seedRefCleanup(Backend & backend, const Layout & layout, const RootNamespace & ns, uint64_t logs)` that publishes `logs` committed transitions (`t1`..`tN`, manifest refs `mref(1..N)`), writes the first and the last snapshot, sets the checkpoint at the last one, and returns the life and the `RefCleanupPlan` computed exactly as the old test did. With `logs == 2` it must reproduce the old fixture byte for byte.

```cpp
TEST(CASRefGc, RefCleanupContainsCapabilityRejection)
{
    std::map<String, UInt64> row;
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStoreRejectsBatches(true);
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds*/ 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, layout, ns);
    const RefCleanupFixture seeded = seedRefCleanup(*backend, layout, ns, 3);
    const size_t cohort_size = seeded.plan.deletable_logs.size() + seeded.plan.deletable_snapshots.size();
    ASSERT_GE(cohort_size, 2u) << "a one-key cohort would not reach the batch rejection";

    Gc gc(store, kGc);
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "ref_object_cleanup") row = record.metrics; });
    ASSERT_NO_THROW(ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease));
    EXPECT_EQ(row["capability_learned"], 1u);
    EXPECT_EQ(backend->requestSizes(), (std::vector<size_t>{cohort_size})) << "no one-key request that round";
    OperationForTest op(*backend);
    for (const RefTxnId & id : seeded.plan.deletable_logs)
        EXPECT_TRUE((*op).head(layout.refLogKey(seeded.life, id), Retry::once()).has_value());

    store->renewWatermarkOnce();
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    for (const RefTxnId & id : seeded.plan.deletable_logs)
        EXPECT_FALSE((*op).head(layout.refLogKey(seeded.life, id), Retry::once()).has_value());
    for (const RefTxnId & id : seeded.plan.deletable_snapshots)
        EXPECT_FALSE((*op).head(layout.refSnapshotKey(seeded.life, id), Retry::once()).has_value());
    const std::vector<size_t> requests = backend->requestSizes();
    EXPECT_TRUE(std::all_of(requests.begin() + 1, requests.end(), [](size_t n) { return n == 1; }))
        << "the next round cuts one-key requests";
}

TEST(CASRefGc, RefCleanupReadsAuthorityOncePerCohort)
{
    /// Scaled from 2,500 keys in cohorts of 1,000: 11 keys in cohorts of 4 give the same three cohorts.
    PhaseReads reads;
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setBatchDeleteSupported(false);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_bulk_delete_chunk_keys = 4;
    auto store = Pool::open(backend, config);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, layout, ns);
    const RefCleanupFixture seeded = seedRefCleanup(*backend, layout, ns, 11);
    const size_t cohort_size = seeded.plan.deletable_logs.size() + seeded.plan.deletable_snapshots.size();
    ASSERT_GT(cohort_size, 8u);
    ASSERT_LE(cohort_size, 12u) << "the assertions below expect exactly three cohorts of at most 4";

    Gc gc(store, kGc);
    gc.setPhaseSink(phaseReadsSink(reads, *backend, layout, "namespace_cleanup", "ref_object_cleanup"));
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    EXPECT_EQ(reads.catalog_in_phase, 3u);
    EXPECT_EQ(reads.state_in_phase, 3u);
    const std::vector<size_t> calls = backend->callSizes();
    EXPECT_EQ(calls, std::vector<size_t>(cohort_size, 1)) << "base sends a 4-key request first and falls back";
}

TEST(CASRefGc, ZeroStorageLimitCutsOneKeyRefCleanupRequests)
{
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStorageLimit(0);
    auto store = openPoolForTest(backend, 0);
    const Layout & layout = store->layout();
    const RootNamespace ns{"00/aa@cas@"};
    fixture::admitLive(*backend, layout, ns);
    const RefCleanupFixture seeded = seedRefCleanup(*backend, layout, ns, 3);
    const size_t cohort_size = seeded.plan.deletable_logs.size() + seeded.plan.deletable_snapshots.size();
    Gc gc(store, kGc);
    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    EXPECT_EQ(backend->callSizes(), std::vector<size_t>(cohort_size, 1));
}
```

`PhaseReads` and `phaseReadsSink` are needed in both files: define them once, as `inline` functions in `cas_test_helpers.h` inside `namespace DB::Cas::tests`, not in each test file.

- [ ] **Step 2: Build and run to verify they fail**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build unit_tests_dbms > build/t2_red.log 2>&1; echo NINJA_EXIT=$? >> build/t2_red.log; tail -1 build/t2_red.log; grep -a -n "error:" build/t2_red.log | head
```
Expected: `NINJA_EXIT=1` on `removeCohortWriteOnce` (not declared). Record it. Then temporarily build only the manifest and ref tests against base behaviour is not possible without the helper; the red evidence for the family tests is the assertion on `callSizes`/`capability_learned`, observed in Step 4's first run if you implement the helper before the families. Do exactly that: implement Step 3's helper and the rename in the two callers WITHOUT the containment and the request cut, build, run `CASGCManifestBulkDelete*:CASRefGc*Cohort*:CASRefGc*Capability*:CASRefGc*ZeroStorage*`, and record the failures (`capability_learned` missing, 4-key call sizes, round throwing `NOT_IMPLEMENTED`).

- [ ] **Step 3: Implement**

`CasGc.h`: replace the `removeChunkWriteOnceOrOneByOne` comment and declaration with:

```cpp
/// Deletes `cohort` as consecutive `removeManyWriteOnce` requests of at most `request_keys` keys and calls
/// `on_request_done(begin, end)` after each one returns. Every failure propagates, `NOT_IMPLEMENTED` included:
/// re-sending a rejected batch key by key would delete under a decision nobody made for those keys, and the
/// capability the rejection teaches makes the next round cut one-key requests anyway.
void removeCohortWriteOnce(
    CasOperation & op,
    const std::vector<WriteOnceKey> & cohort,
    size_t request_keys,
    const Retry & policy,
    const std::function<void(size_t begin, size_t end)> & on_request_done);
```

`CasGc.cpp`: replace the helper body.

```cpp
void removeCohortWriteOnce(
    CasOperation & op,
    const std::vector<WriteOnceKey> & cohort,
    size_t request_keys,
    const Retry & policy,
    const std::function<void(size_t begin, size_t end)> & on_request_done)
{
    const size_t step = std::clamp<size_t>(request_keys, 1, kBulkDeleteMaxKeys);
    for (size_t begin = 0; begin < cohort.size(); begin += step)
    {
        const size_t end = std::min(cohort.size(), begin + step);
        op.removeManyWriteOnce(std::vector<WriteOnceKey>(cohort.begin() + begin, cohort.begin() + end), policy);
        on_request_done(begin, end);
    }
}
```

Manifest cleanup (`PHASE 15/18`, ~:1266-1300): replace the block from `const size_t chunk_keys = ...` through the metric lines with:

```cpp
        const size_t cohort_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        const size_t request_keys = bulkDeleteChunkKeys();
        uint64_t attempted = 0;
        uint64_t requests = 0;
        bool capability_learned = false;
        std::vector<WriteOnceKey> chunk;
        std::vector<const std::pair<const ManifestId, Etag> *> chunk_entries;
        const auto flush = [&]
        {
            if (chunk.empty())
                return;
            removeCohortWriteOnce(op, chunk, request_keys, Retry::standard(), [&](size_t begin, size_t end)
            {
                ++requests;
                for (size_t i = begin; i < end; ++i)
                {
                    const auto * entry = chunk_entries[i];
                    ++report.manifests_deleted;
                    EventEmitter{*store}.emit([&](CasEvent & e)
                    {
                        e.type = CasEventType::ManifestDelete;
                        e.namespace_ = entry->first.root_namespace.string();
                        e.object_kind = CasEventObjectKind::Manifest;
                        e.object_hash = manifestRefDebugString(entry->first.ref);
                        e.token = entry->second.render();
                        e.round = new_round;
                        e.gen = generation;
                        e.outcome = "deleted_or_absent";
                        e.reason = "owner-removed manifest body; batch delete of a write-once key after decrements adopted";
                    });
                }
            });
            chunk.clear();
            chunk_entries.clear();
        };
        try
        {
            for (const auto & entry : mf_cleanup_now)
            {
                ++attempted;
                chunk.push_back(layout.writeOnceManifestKey(entry.first));
                chunk_entries.push_back(&entry);
                if (chunk.size() >= cohort_keys)
                    flush();
            }
            flush();
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::NOT_IMPLEMENTED)
                throw;
            /// The intake cursor that found these bodies is committed, so the orphan-manifest sweep reclaims
            /// the rest; the storage now reports no batch delete, and later requests carry one key.
            capability_learned = true;
            LOG_WARNING(logger, "CAS GC manifest_deletes: the storage rejected a batch delete; the remaining "
                "owner-removed manifest bodies are left to the orphan-manifest sweep: {}", e.message());
        }
        const uint64_t accepted = report.manifests_deleted - manifests_deleted_before;
        t.metric("attempted", attempted);
        t.metric("accepted", accepted);
        t.metric("requests", requests);
        t.metric("unsent", mf_cleanup_now.size() - accepted);
        t.metric("capability_learned", capability_learned ? 1 : 0);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
```

Also shorten the block comment above it: drop "A backend without `DeleteObjects` (GCS) falls back to one admitted delete per key here" and the similar lines; keep the sentences about write-once keys and all-or-nothing recording.

`cleanupRefObjects`: change the declaration in `CasGc.h` to

```cpp
    /// ... existing comment ... Returns true when the storage rejected a batch delete: the family stops for
    /// this round and the next round's plan recomputes the same candidates.
    bool cleanupRefObjects(
        const FoldResult & folded, const GcLease & adopted_lease, bool suppress_destructive,
        GcRoundWorkBudget & work_budget);
```

In the definition, every `return;` becomes `return false;`, the function ends with `return false;`, and the chunk loop becomes:

```cpp
        const size_t cohort_keys = std::clamp<size_t>(store->poolConfig().gc_bulk_delete_chunk_keys, 1, kBulkDeleteMaxKeys);
        const size_t request_keys = bulkDeleteChunkKeys();
        for (size_t begin = 0; begin < cohort.size(); )
        {
            /// Cumulative per-round cap in KEYS; a cohort is cut to what remains. The plan recomputes the same
            /// remaining candidates from durable state next round, so nothing here needs its own cursor.
            if (!work_budget.refCleanupAvailable())
                return false;
            size_t end = std::min(cohort.size(), begin + cohort_keys);
            if (work_budget.max_ref_cleanup_objects != 0)
                end = std::min(end, begin + (work_budget.max_ref_cleanup_objects - work_budget.ref_cleanup_objects_used));
            std::vector<WriteOnceKey> chunk(cohort.begin() + begin, cohort.begin() + end);
            /// One authority read per cohort, not per request: on a store without batch delete a per-request
            /// read would add two GETs per key.
            if (!authorityHolds(chunk.front().str()))
                return false;
            try
            {
                removeCohortWriteOnce(op, chunk, request_keys, Retry::standard(), [&](size_t request_begin, size_t request_end)
                {
                    work_budget.ref_cleanup_objects_used += request_end - request_begin;
                    ProfileEvents::increment(ProfileEvents::CASRefCleanupObjectsDeleted, request_end - request_begin);
                });
            }
            catch (const Exception & e)
            {
                if (e.code() != ErrorCodes::NOT_IMPLEMENTED)
                    throw;
                LOG_WARNING(logger, "CAS GC ref cleanup: the storage rejected a batch delete; the remaining candidates "
                    "wait for the next round: {}", e.message());
                return true;
            }
            begin = end;
        }
```

The ref cleanup phase block (~:1336-1345):

```cpp
    {
        GcPhaseTimer t(phase_sink, "ref_object_cleanup");
        bool capability_learned = false;
        if (trim_enabled)
            capability_learned = cleanupRefObjects(folded, state.lease, suppress_destructive, round_work_budget);
        t.metric("suppressed", suppress_destructive ? 1 : 0);
        t.metric("trim_enabled", trim_enabled ? 1 : 0);
        t.metric("namespaces_planned", folded.ref_tables.size());
        t.metric("capability_learned", capability_learned ? 1 : 0);
    }
```

Confirm `ErrorCodes::NOT_IMPLEMENTED` is declared in `CasGc.cpp`'s `namespace ErrorCodes` block (the old helper used it).

Comments naming the removed fallback: `grep -rn "removeChunkWriteOnceOrOneByOne\|per-key fallback\|falls back to one\|one key per admitted request\|retries one key" src/Disks src/IO` and rewrite every hit to the current truth, for example in `ObjectStorageBackend::removeManyWriteOnce`: `/// NOT_IMPLEMENTED from a storage without a batch delete propagates; this layer never loops per key, and the GC caller stops the family for the round.` In `S3ObjectStorage.cpp:~674`: `/// A batch of exactly one object is a plain DeleteObject, never DeleteObjects -- the same rule deleteFilesFromS3 applies. A storage without DeleteObjects (GCS) rejects the verb itself, so a one-object DeleteObjects would fail there too.` The final grep for `removeChunkWriteOnceOrOneByOne` must print nothing.

- [ ] **Step 4: Build and run the tests**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build unit_tests_dbms > build/t2_build.log 2>&1; echo NINJA_EXIT=$? >> build/t2_build.log; tail -1 build/t2_build.log
( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CASGCCohortDelete*:CASGCManifestBulkDelete*:CASRefGc*:CASBulkDeleteBackend*' > build/t2_green.log 2>&1; echo GATE_EXIT=$? >> build/t2_green.log ); tail -3 build/t2_green.log
```
Expected: `NINJA_EXIT=0`, `GATE_EXIT=0`.

- [ ] **Step 5: Gate**

Run the `CAS*` gate into `build/t2_gate.log`. Expected `GATE_EXIT=0`. A failure in a test whose `Backend` subclass derives from `Backend` directly and wraps an inner backend (e.g. `ForwardingBackend` in `gtest_cas_pool.cpp`) means it now reports a limit of 1: add `size_t bulkDeleteKeyLimit() const override { return inner->bulkDeleteKeyLimit(); }` to that decorator (the same forwarding rule as `InstrumentedBackend`) and say so in the commit body. Any other failure: root-cause it before continuing.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
C=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
git add -- $C/Gc/CasGc.h $C/Gc/CasGc.cpp $C/Backend/CasObjectStorageBackend.cpp \
  src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.h \
  src/Disks/tests/cas_test_helpers.h src/Disks/tests/gtest_cas_gc_bulk_delete_fallback.cpp \
  src/Disks/tests/gtest_cas_gc_manifest_bulk_delete.cpp src/Disks/tests/gtest_cas_ref_gc.cpp \
  src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp
git commit -s -m "cas: stop GC write-once families on a rejected batch instead of deleting per key"
```

Add any decorator test file Step 5 touched, by exact path.

---

### Task 3: S3 adapter keeps the canonical error name on delete failures

**Files:**
- Modify: `src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp:751` and `:757-769`
- Modify: `src/IO/S3/deleteFileFromS3.cpp:~74-78`
- Test: `src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp` (suite `CASS3BatchDelete`)

**Interfaces:**
- Consumes: `cas_scripted_s3_server.h` (Task 1).
- Produces: every `S3Exception` from the CAS delete paths carries `getExceptionName()` = the S3 `<Code>`.

- [ ] **Step 1: Write the failing tests**

Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>` and, inside `#if USE_AWS_S3`:

```cpp
namespace
{

void expectRefusalNamed(const std::function<void()> & remove, const std::string & name)
{
    try
    {
        remove();
        FAIL() << "expected an S3Exception named " << name;
    }
    catch (const DB::S3Exception & e)
    {
        EXPECT_EQ(e.getExceptionName(), name) << e.message();
        EXPECT_TRUE(DB::Cas::isDefinitelyRefusedWrite(e)) << e.message();
    }
}

const DB::StoredObjects kTwoObjects{DB::StoredObject("key-a"), DB::StoredObject("key-b")};

}

TEST(CASS3BatchDelete, S3BatchErrorKeepsCanonicalName)
{
    (void)contextForTest();
    for (const std::string code : {"EntityTooLarge", "MalformedXML"})
    {
        SCOPED_TRACE("whole-response " + code);
        ScriptedS3Server server([code](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, code, "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, code);
    }
    {
        SCOPED_TRACE("per-key EntityTooLarge");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendDeleteObjectsResult(response, {{"key-b", "EntityTooLarge"}});
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
    }
    {
        SCOPED_TRACE("one-key EntityTooLarge through deleteFileFromS3");
        ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge", "refused");
        });
        auto storage = makeStorageForTest(server.getUrl(), DB::S3Capabilities{false});
        expectRefusalNamed([&] { storage->removeObjectsIfExistUnderProfile({DB::StoredObject("solo")}, DB::ObjectStorageControlRequest{}); }, "EntityTooLarge");
        EXPECT_EQ(server.countMethod("DELETE"), 1u);
    }
}

TEST(CASS3BatchDelete, NameOnlyRefusalWithRefreshCallbackCostsAtMostOneExtraRequest)
{
    (void)contextForTest();
    ScriptedS3Server server([](const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        sendDeleteObjectsResult(response, {{"key-b", "EntityTooLarge"}});
    });
    const std::string url = server.getUrl();
    std::atomic<size_t> callbacks{0};
    auto storage = makeStorageForTest(url, DB::S3Capabilities{}, {.credentials_refresh_callback = [&]() -> std::unique_ptr<const DB::S3::Client>
    {
        ++callbacks;
        return makeClientForTest(url);
    }});

    EXPECT_THROW(storage->removeObjectsIfExistUnderProfile(kTwoObjects, DB::ObjectStorageControlRequest{}), DB::S3Exception);
    EXPECT_LE(server.countMethod("POST"), 2u);
    EXPECT_LE(callbacks.load(), 1u);
}
```

- [ ] **Step 2: Build and run to verify they fail**

Build into `build/t3_red_build.log`; run `--gtest_filter='CASS3BatchDelete*'` into `build/t3_red.log`.
Expected: `S3BatchErrorKeepsCanonicalName` FAILS: `getExceptionName()` is empty for all four cases. `NameOnlyRefusalWithRefreshCallbackCostsAtMostOneExtraRequest` may already pass; record its counts.

- [ ] **Step 3: Implement**

`S3ObjectStorage.cpp`, whole-response site (~:751):

```cpp
        throw S3Exception(
            PreformattedMessage::create("{} (Code: {}) while removing {} objects from S3 in one request",
                err.GetMessage(), static_cast<size_t>(err.GetErrorType()), objects.size()),
            err.GetErrorType(), err.GetExceptionName());
```

Per-key site (~:757-769):

```cpp
    String failed_keys;
    std::optional<Aws::S3::S3Errors> first_error_type;
    String first_error_name;
    for (const auto & err : outcome.GetResult().GetErrors())
    {
        const auto error_type = classifyDeleteObjectsErrorCode(err.GetCode());
        if (S3::isNotFoundError(error_type))
            continue;
        if (!failed_keys.empty())
            failed_keys += ", ";
        failed_keys += err.GetKey() + " (" + err.GetCode() + ": " + err.GetMessage() + ")";
        if (!first_error_type)
        {
            first_error_type = error_type;
            first_error_name = err.GetCode();
        }
    }
    if (first_error_type)
        throw S3Exception(
            PreformattedMessage::create("batch removal left objects behind: [{}]", failed_keys), *first_error_type, first_error_name);
```

`deleteFileFromS3.cpp` (~:76):

```cpp
        const auto & err = outcome.GetError();
        throw S3Exception(
            PreformattedMessage::create("{} (Code: {}) while removing object with path {} from S3",
                err.GetMessage(), static_cast<size_t>(err.GetErrorType()), key),
            err.GetErrorType(), err.GetExceptionName());
```

- [ ] **Step 4: Build and run**

Build into `build/t3_build.log`; run `--gtest_filter='CASS3BatchDelete*:S3BulkDeleteFallback*'` into `build/t3_green.log`. Expected: `GATE_EXIT=0`. Record the POST and callback counts of the refresh test in the commit body (they decide whether the preserved name removes the refresh reissue).

- [ ] **Step 5: Gate**

`CAS*` into `build/t3_gate.log`; expected `GATE_EXIT=0`.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
git add -- src/Disks/DiskObjectStorage/ObjectStorages/S3/S3ObjectStorage.cpp src/IO/S3/deleteFileFromS3.cpp \
  src/Disks/tests/gtest_cas_s3_bulk_delete_fallback.cpp
git commit -s -m "s3: keep the canonical error name on delete failures"
```

---

### Task 4: Janitor phase: page loop, batch jobs (sequential), budget, cursor prefix

**Files:**
- Modify: `.../Formats/CasLayout.h` (add `writeOnceStreamKey`, private `refStreamObjectKey`; route `refLogKey`/`refSnapshotKey` through it — port of `1edf28f2a9a`)
- Modify: `.../Gc/CasNamespaceJanitor.h`, `.../Gc/CasNamespaceJanitor.cpp` (full rewrite below)
- Modify: `.../Gc/CasGc.h:550-556` (`runNamespaceJanitorPage` → `runNamespaceJanitor`), `.../Gc/CasGc.cpp:363-391`, `:823`, `:1334`
- Modify: `.../Pool/CasPool.h:217` (seam)
- Create: `src/Disks/tests/cas_namespace_janitor_test_helpers.h`, `src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp`
- Modify: `src/Disks/tests/gtest_cas_namespace_janitor.cpp` (migrate), `src/Disks/tests/gtest_cas_write_once_key.cpp`

**Interfaces:**
- Consumes: `Gc::bulkDeleteChunkKeys()`, `BatchCapabilityBackend` (Task 1).
- Produces:
  - `std::optional<WriteOnceKey> Layout::writeOnceStreamKey(const ParsedRefObjectKey & parsed, std::string_view listed_key) const`.
  - `struct NamespaceJanitorResult` with fields `pages, keys, deleted, leaked, batches, batches_leaked, batches_held, delete_jobs, delete_jobs_skipped, jobs_in_flight_max, delete_calls, batch_keys, list_ms, catalog_ms, delete_ms, drain_wait_ms, list_requests_process, cursor_start_fp, cursor_end_fp` (all `uint64_t`), `capability_learned, budget_exhausted, cursor_advanced` (`bool`), `anomalies` (`std::vector<String>`).
  - `struct JanitorRunContext { Liveness liveness; std::function<void()> refresh_authority; size_t batch_keys = kBulkDeleteMaxKeys; uint64_t budget_ms = max; std::function<uint64_t()> now_ms; }` (Task 6 appends fields after `now_ms`).
  - `void NamespaceJanitor::run(bool suppress_deletes, const JanitorRunContext & context, NamespaceJanitorResult & out)`; constructor `NamespaceJanitor(CasRequests &, const Layout &, size_t page_keys)`.
  - `void Gc::runNamespaceJanitor(const GcState &, bool suppress_destructive, uint64_t cleanup_evidence_rows)`; `namespace_cleanup` row carries every result field under the names in the Observability table of the spec.
  - `std::optional<uint64_t> PoolConfig::gc_janitor_phase_budget_ms_for_test`.
  - Test helpers in `DB::Cas::tests::janitor` (header below).

- [ ] **Step 1: Write the test helper header**

Create `src/Disks/tests/cas_namespace_janitor_test_helpers.h`. Before writing it, `grep -n "present\|seedLogs\|publishedCursor\|runPhase\|runOnePage\|seedLiveNamespace\|JanitorFixture\|CountdownForTest" src/Disks/tests/cas_test_helpers.h` must print nothing (no name clash).

```cpp
#pragma once

#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <base/scope_guard.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace DB::Cas::tests::janitor
{

inline void createObj(Backend & backend, const String & key, const String & bytes)
{
    OperationForTest op(backend);
    ASSERT_TRUE(std::holds_alternative<Committed>((*op).create(key, bytes, Retry::once()))) << key;
}

inline std::optional<Object> readObj(Backend & backend, const String & key)
{
    OperationForTest op(backend);
    return (*op).read(key, Retry::standard());
}

inline bool present(Backend & backend, const String & key)
{
    return readObj(backend, key).has_value();
}

inline void seedCatalog(Backend & backend, const Layout & layout, RefCatalog catalog = {})
{
    createObj(backend, layout.refCatalogKey(), encodeRefCatalog(catalog));
}

inline NamespaceLifeId life(const char * name, uint64_t id)
{
    return NamespaceLifeId::fromCatalogEntry(RootNamespace{name}, UInt128{id});
}

/// `readGcMaintenanceState` takes an admitted operation, which cannot bind to an rvalue.
inline GcMaintenanceReadResult readState(CasRequests & requests, const Layout & layout)
{
    auto op = requests.admit();
    return readGcMaintenanceState(op, layout);
}

/// The published janitor cursor; `std::nullopt` while nothing was ever published.
inline std::optional<String> publishedCursor(CasRequests & requests, const Layout & layout)
{
    const GcMaintenanceReadResult state = readState(requests, layout);
    if (state.status != GcMaintenanceReadStatus::Valid || !state.state)
        return std::nullopt;
    return state.state->janitor_cursor;
}

/// `count` `_log` keys of `owner`, in listing order.
inline std::vector<String> seedLogs(Backend & backend, const Layout & layout, const NamespaceLifeId & owner, uint64_t count)
{
    std::vector<String> keys;
    keys.reserve(count);
    for (uint64_t i = 1; i <= count; ++i)
        keys.push_back(layout.refLogKey(owner, RefTxnId{1, i}));
    std::sort(keys.begin(), keys.end());
    for (const String & key : keys)
        createObj(backend, key, "log");
    return keys;
}

/// A live namespace with a checkpoint, the shape a GC round's fold expects; returns its life.
inline NamespaceLifeId seedLiveNamespace(Backend & backend, const Layout & layout)
{
    const RootNamespace live_namespace{"00/live@cas@"};
    fixture::admitLive(backend, layout, live_namespace);
    const NamespaceLifeId live = fixture::fixtureLife(live_namespace);
    createObj(backend, layout.refCkptKey(live),
        encodeRefCkpt(RefCkpt{.life_epoch = std::optional<uint64_t>{1}, .checkpoint_snapshot_id = std::nullopt, .last_epoch_seal = std::nullopt}));
    return live;
}

inline NamespaceJanitorResult runPhase(CasRequests & requests, const Layout & layout, const JanitorRunContext & context,
                                       bool suppress_deletes = false, size_t page_keys = 1000)
{
    NamespaceJanitorResult result;
    NamespaceJanitor(requests, layout, page_keys).run(suppress_deletes, context, result);
    return result;
}

/// One page, no refresh, sequential: the contract the single-page tests were written against.
inline NamespaceJanitorResult runOnePage(NamespaceJanitor janitor, bool suppress_deletes, Liveness liveness)
{
    NamespaceJanitorResult result;
    janitor.run(suppress_deletes, JanitorRunContext{.liveness = std::move(liveness), .budget_ms = 0}, result);
    return result;
}

/// Counts down from several threads; `wait` gives up after 20 s so a broken ordering fails the test
/// instead of hanging it.
class CountdownForTest
{
public:
    explicit CountdownForTest(size_t count_) : count(count_) {}

    void countDown()
    {
        {
            std::lock_guard lock(mutex);
            if (count > 0)
                --count;
        }
        cv.notify_all();
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::seconds(20), [this] { return count == 0; }))
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "CAS test countdown timed out");
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    size_t count;
};

/// The batch-capability store with the janitor's requests counted apart and hooks at fixed points. Hooks
/// run with no backend lock held; their own store access goes through the uncounted primitives.
class JanitorBackend : public BatchCapabilityBackend
{
public:
    using Access = TransportAccess;
    using Keys = std::vector<WriteOnceKey>;

    struct BulkCall
    {
        std::vector<String> keys;
        std::thread::id thread;
    };

    RawListPage list(const String & prefix, const String & cursor, size_t limit, Access & access) override
    {
        RawListPage page = BatchCapabilityBackend::list(prefix, cursor, limit, access);
        if (!prefix.ends_with("/cas/ns/"))
            return page;
        if (tokenless)
            for (auto & key : page.keys)
                key.value.reset();
        const size_t index = namespace_lists.fetch_add(1);
        if (after_namespace_list)
            after_namespace_list(index, access);
        return page;
    }

    bool supportsListTokens() const override { return !tokenless; }

    RawRemoval remove(const String & key, const String & expected_value, Access & access) override
    {
        {
            std::lock_guard lock(calls_mutex);
            exact_removes.push_back(key);
        }
        if (before_exact_remove)
            before_exact_remove(key, access);
        return BatchCapabilityBackend::remove(key, expected_value, access);
    }

    void removeManyWriteOnce(const Keys & keys, Access & access) override
    {
        const size_t running = ++bulk_in_flight;
        size_t seen = bulk_in_flight_max.load();
        while (running > seen && !bulk_in_flight_max.compare_exchange_weak(seen, running))
        {
        }
        SCOPE_EXIT(--bulk_in_flight);
        {
            std::lock_guard lock(calls_mutex);
            BulkCall call{.keys = {}, .thread = std::this_thread::get_id()};
            for (const WriteOnceKey & key : keys)
                call.keys.push_back(key.str());
            bulk_calls.push_back(std::move(call));
        }
        if (before_bulk)
            before_bulk(keys, access);
        BatchCapabilityBackend::removeManyWriteOnce(keys, access);
        if (after_bulk)
            after_bulk(keys);
    }

    std::vector<BulkCall> bulkCalls() const
    {
        std::lock_guard lock(calls_mutex);
        return bulk_calls;
    }

    std::vector<String> exactRemoves() const
    {
        std::lock_guard lock(calls_mutex);
        return exact_removes;
    }

    size_t namespaceLists() const { return namespace_lists.load(); }
    size_t bulkInFlightMax() const { return bulk_in_flight_max.load(); }

    /// A concurrent actor's delete of whatever `key` holds now.
    void removeUncounted(const String & key, Access & access)
    {
        if (const auto current = InMemoryBackend::read(key, access)) // NOLINT(bugprone-parent-virtual-call)
            (void)InMemoryBackend::remove(key, current->value, access); // NOLINT(bugprone-parent-virtual-call)
    }

    /// A concurrent actor's write: a create when `key` is absent, else a replacement.
    void writeUncounted(const String & key, const String & bytes, Access & access)
    {
        const auto current = InMemoryBackend::read(key, access); // NOLINT(bugprone-parent-virtual-call)
        const std::optional<String> expected = current ? std::optional<String>(current->value) : std::nullopt;
        ASSERT_TRUE(InMemoryBackend::write(key, bytes, expected, access).has_value()); // NOLINT(bugprone-parent-virtual-call)
    }

    bool tokenless = false;
    std::function<void(size_t index, Access &)> after_namespace_list;
    std::function<void(const String &, Access &)> before_exact_remove;
    std::function<void(const Keys &, Access &)> before_bulk;
    std::function<void(const Keys &)> after_bulk;

private:
    mutable std::mutex calls_mutex;
    std::vector<BulkCall> bulk_calls;
    std::vector<String> exact_removes;
    std::atomic<size_t> namespace_lists{0};
    std::atomic<size_t> bulk_in_flight{0};
    std::atomic<size_t> bulk_in_flight_max{0};
};

inline const Layout & janitorLayout()
{
    static const Layout layout("p");
    return layout;
}

/// A pool-less janitor: an open fence, a catalog, and a request and phase clock the test drives.
struct JanitorFixture
{
    std::shared_ptr<JanitorBackend> backend = std::make_shared<JanitorBackend>();
    FakeClock clock;
    CasRequests requests{backend, Fence::open(), clock.nowFn(), clock.sleepFn()};

    explicit JanitorFixture(RefCatalog catalog = {}) { seedCatalog(*backend, janitorLayout(), std::move(catalog)); }

    JanitorRunContext context(size_t batch_keys = kBulkDeleteMaxKeys)
    {
        JanitorRunContext result;
        result.batch_keys = batch_keys;
        result.now_ms = clock.nowFn();
        return result;
    }

    NamespaceJanitorResult run(const JanitorRunContext & context, bool suppress_deletes = false)
    {
        return runPhase(requests, janitorLayout(), context, suppress_deletes);
    }

    std::optional<String> cursor() { return publishedCursor(requests, janitorLayout()); }
};

}
```

If the helper header's `ASSERT_TRUE` inside `createObj` trips `-Werror` on a non-void context, it does not: all three helpers that assert return `void`.

- [ ] **Step 2: Migrate the existing janitor test file**

In `src/Disks/tests/gtest_cas_namespace_janitor.cpp`:
- replace `#include "cas_test_helpers.h"` and the two janitor includes by `#include "cas_namespace_janitor_test_helpers.h"`; add `using namespace DB::Cas::tests::janitor;`;
- delete the anonymous-namespace definitions of `readState`, `createObj`, `readObj`, `seedCatalog`, `life` (now in the header);
- rewrite every call site:

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
perl -0pi -e 's/(NamespaceJanitor\([^()]*\))\.runOnePage\(/runOnePage($1, /g; s/\b(\w+)\.runOnePage\(/runOnePage($1, /g' src/Disks/tests/gtest_cas_namespace_janitor.cpp
grep -n "\.runOnePage(" src/Disks/tests/gtest_cas_namespace_janitor.cpp   # expect no output
```

Update the comment above `ThrowingListAndAmbiguousWriteBackend` that names `NamespaceJanitor::runOnePage` to name `NamespaceJanitor::run`.

- [ ] **Step 3: Write the failing tests**

`src/Disks/tests/gtest_cas_write_once_key.cpp`: add the parked test verbatim (it pins the non-canonical exact path at the layout level; `parseRefObjectKey` and `parseRefTxnId` accept only canonical spellings, so no parseable non-canonical key exists to list, and the spec's `NonCanonicalStreamKeyUsesExactPath` guard is this test):

```cpp
TEST(CASWriteOnceKey, StreamKeyMintedFromAListedKeyMustBeThatKey)
{
    const Layout layout{"p"};
    const NamespaceLifeId life = NamespaceLifeId::fromCatalogEntry(RootNamespace{"test/aa@cas@"}, DB::UInt128(0x1234));
    const RefTxnId id{5, 7};

    for (const String & listed : {layout.refLogKey(life, id), layout.refSnapshotKey(life, id)})
    {
        const auto parsed = layout.parseRefObjectKey(listed);
        ASSERT_TRUE(parsed.has_value()) << listed;
        const auto minted = layout.writeOnceStreamKey(*parsed, listed);
        ASSERT_TRUE(minted.has_value()) << listed;
        EXPECT_EQ(minted->str(), listed);
    }

    const auto parsed_log = layout.parseRefObjectKey(layout.refLogKey(life, id));
    ASSERT_TRUE(parsed_log.has_value());
    EXPECT_FALSE(layout.writeOnceStreamKey(*parsed_log, layout.refSnapshotKey(life, id)).has_value());
    EXPECT_FALSE(layout.writeOnceStreamKey(*parsed_log, layout.refLogKey(life, RefTxnId{5, 8})).has_value());
    EXPECT_FALSE(layout.writeOnceStreamKey(*parsed_log, "q" + layout.refLogKey(life, id).substr(1)).has_value());
}
```

Create `src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp`:

```cpp
#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Poco/Exception.h>
#include <fmt/format.h>

/// The janitor phase at `gc_io_concurrency` 1: exact request orders and counts.

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
    extern const int NETWORK_ERROR;
}

namespace
{

const Layout & kLayout = janitorLayout();

bool allAbsent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
        if (present(backend, keys[i]))
            return false;
    return true;
}

bool allPresent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    for (size_t i = begin; i < end; ++i)
        if (!present(backend, keys[i]))
            return false;
    return true;
}

}

TEST(CASNamespaceJanitor, DeadLifeStreamKeysDrainInBatches)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 201), 2500);

    const NamespaceJanitorResult result = f.run(f.context());

    const auto calls = f.backend->bulkCalls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].keys.size(), 1000u);
    EXPECT_EQ(calls[1].keys.size(), 1000u);
    EXPECT_EQ(calls[2].keys.size(), 500u);
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_EQ(result.pages, 3u);
    EXPECT_EQ(result.delete_calls, 3u);
    EXPECT_EQ(result.batches, 3u);
    EXPECT_EQ(result.delete_jobs, 3u);
    EXPECT_EQ(result.deleted, 2500u);
    EXPECT_EQ(result.batch_keys, 1000u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, keys.size()));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, BatchSizeFollowsTheStorageLimit)
{
    JanitorFixture f;
    (void)seedLogs(*f.backend, kLayout, life("dead", 202), 1000);
    const NamespaceJanitorResult result = f.run(f.context(250));
    const auto calls = f.backend->bulkCalls();
    ASSERT_EQ(calls.size(), 4u);
    for (const auto & call : calls)
        EXPECT_EQ(call.keys.size(), 250u);
    EXPECT_EQ(result.batch_keys, 250u);
}

TEST(CASNamespaceJanitor, StateKeysNeverEnterTheBatchPath)
{
    for (const bool tokenless : {false, true})
    {
        SCOPED_TRACE(tokenless ? "tokenless listing" : "token listing");
        JanitorFixture f;
        f.backend->tokenless = tokenless;
        const NamespaceLifeId dead = life("dead", 203);
        const std::vector<String> stream = seedLogs(*f.backend, kLayout, dead, 1);
        const String ckpt = kLayout.refCkptKey(dead);
        const String file = kLayout.namespaceFilesPrefix(dead) + "data";
        createObj(*f.backend, ckpt, "ckpt");
        createObj(*f.backend, file, "file");
        /// Both state objects are rewritten between observation and delete: only an exact-token delete keeps the rewrite.
        f.backend->before_exact_remove = [&](const String & key, JanitorBackend::Access & access)
        {
            f.backend->writeUncounted(key, "winner", access);
        };

        const NamespaceJanitorResult result = f.run(f.context());

        const auto calls = f.backend->bulkCalls();
        ASSERT_EQ(calls.size(), 1u);
        EXPECT_EQ(calls[0].keys, stream);
        EXPECT_EQ(f.backend->exactRemoves(), (std::vector<String>{ckpt, file}));
        EXPECT_EQ(readObj(*f.backend, ckpt)->bytes, "winner");
        EXPECT_EQ(readObj(*f.backend, file)->bytes, "winner");
        EXPECT_FALSE(present(*f.backend, stream.front()));
        EXPECT_EQ(result.deleted, 1u);
    }
}

TEST(CASNamespaceJanitor, RecreatedLifeBetweenListAndDeleteKeepsItsKeys)
{
    JanitorFixture f;
    const NamespaceLifeId dropped = life("t", 211);
    const NamespaceLifeId recreated = life("t", 212);
    const std::vector<String> old_keys = seedLogs(*f.backend, kLayout, dropped, 3);
    const std::vector<String> new_keys{kLayout.refLogKey(recreated, RefTxnId{1, 1}), kLayout.refSnapshotKey(recreated, RefTxnId{1, 1})};
    f.backend->after_namespace_list = [&](size_t, JanitorBackend::Access & access)
    {
        RefCatalog live;
        live.entries.push_back(CatalogEntry{.ns = recreated.ns, .state = NsState::Live, .incarnation = recreated.incarnation});
        f.backend->writeUncounted(kLayout.refCatalogKey(), encodeRefCatalog(live), access);
        for (const String & key : new_keys)
            f.backend->writeUncounted(key, "new", access);
    };

    const NamespaceJanitorResult result = f.run(f.context());

    ASSERT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->bulkCalls()[0].keys, old_keys);
    EXPECT_EQ(result.deleted, old_keys.size());
    for (const String & key : new_keys)
        EXPECT_EQ(readObj(*f.backend, key)->bytes, "new") << key;
}

TEST(CASNamespaceJanitor, RecreatedLifeAfterClassificationKeepsItsKeys)
{
    JanitorFixture f;
    const NamespaceLifeId dropped = life("t", 213);
    const NamespaceLifeId recreated = life("t", 214);
    const std::vector<String> old_keys = seedLogs(*f.backend, kLayout, dropped, 3);
    const std::vector<String> new_keys{kLayout.refLogKey(recreated, RefTxnId{1, 1}), kLayout.refLogKey(recreated, RefTxnId{1, 2})};
    std::atomic<bool> recreated_once{false};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access & access)
    {
        if (recreated_once.exchange(true))
            return;
        RefCatalog live;
        live.entries.push_back(CatalogEntry{.ns = recreated.ns, .state = NsState::Live, .incarnation = recreated.incarnation});
        f.backend->writeUncounted(kLayout.refCatalogKey(), encodeRefCatalog(live), access);
        for (const String & key : new_keys)
            f.backend->writeUncounted(key, "new", access);
    };

    (void)f.run(f.context());

    ASSERT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->bulkCalls()[0].keys, old_keys);
    EXPECT_TRUE(allAbsent(*f.backend, old_keys, 0, old_keys.size()));
    for (const String & key : new_keys)
        EXPECT_EQ(readObj(*f.backend, key)->bytes, "new") << key;
}

TEST(CASNamespaceJanitor, LeaseReplacedBeforeFinalBatch)
{
    JanitorFixture f;
    const NamespaceLifeId dropped = life("t", 215);
    const NamespaceLifeId recreated = life("t", 216);
    const std::vector<String> old_keys = seedLogs(*f.backend, kLayout, dropped, 10);
    const std::vector<String> new_keys{kLayout.refLogKey(recreated, RefTxnId{1, 1}), kLayout.refSnapshotKey(recreated, RefTxnId{1, 1})};
    std::atomic<bool> durable_lease_is_ours{true};
    std::atomic<bool> authority_held{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access & access)
    {
        if (!durable_lease_is_ours.exchange(false))
            return;
        RefCatalog live;
        live.entries.push_back(CatalogEntry{.ns = recreated.ns, .state = NsState::Live, .incarnation = recreated.incarnation});
        f.backend->writeUncounted(kLayout.refCatalogKey(), encodeRefCatalog(live), access);
        for (const String & key : new_keys)
            f.backend->writeUncounted(key, "new", access);
        f.backend->writeUncounted(kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = "second-leader"}), access);
    };
    JanitorRunContext context = f.context();
    context.liveness = [&] { return authority_held.load(); };
    context.refresh_authority = [&] { authority_held = durable_lease_is_ours.load(); };

    const NamespaceJanitorResult result = f.run(context);

    ASSERT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->bulkCalls()[0].keys, old_keys);
    EXPECT_TRUE(allAbsent(*f.backend, old_keys, 0, old_keys.size()));
    for (const String & key : new_keys)
        EXPECT_EQ(readObj(*f.backend, key)->bytes, "new") << key;
    EXPECT_EQ(f.cursor(), String("second-leader")) << "the competing leader's cursor stands";
    EXPECT_FALSE(result.cursor_advanced);
    for (const String & anomaly : result.anomalies)
        EXPECT_EQ(anomaly.find("cursor publication"), String::npos) << "a Conflict is silent: " << anomaly;

    const size_t lists_before = f.backend->namespaceLists();
    expectThrowsCode(DB::ErrorCodes::NETWORK_ERROR, [&] { (void)f.run(context); });
    EXPECT_EQ(f.backend->namespaceLists(), lists_before) << "the next refresh stops the deposed leader before its first LIST";
}

TEST(CASNamespaceJanitor, PhaseBudgetCheckedBetweenPages)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 221), 5000);
    f.backend->after_namespace_list = [&](size_t, JanitorBackend::Access &) { f.clock.now += 8'000; };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 3u) << "pages start at 0, 8 and 16 s; none at 24 s";
    EXPECT_EQ(result.pages, 3u);
    EXPECT_TRUE(result.budget_exhausted);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 3000)) << "the page in progress at the deadline completes";
    EXPECT_TRUE(allPresent(*f.backend, keys, 3000, 5000));
    EXPECT_EQ(f.cursor(), keys[2999]);
}

TEST(CASNamespaceJanitor, SlowFirstPageCompletesThenStops)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 222), 3000);
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 0)
            f.clock.now += 30'000;
    };
    JanitorRunContext context = f.context();
    context.budget_ms = 1'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_TRUE(result.budget_exhausted);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 1000));
    EXPECT_EQ(f.cursor(), keys[999]);
}

TEST(CASNamespaceJanitor, LaterPageReadFailurePublishesPrefix)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 223), 4000);
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 2)
            f.backend->failNextReadWith(kLayout.refCatalogKey(),
                std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected catalog read failure")));
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.deleted, 2000u);
    EXPECT_FALSE(result.anomalies.empty());
    EXPECT_EQ(f.cursor(), keys[1999]) << "the complete prefix is published; no reset";
    EXPECT_TRUE(allPresent(*f.backend, keys, 2000, 4000));
}

TEST(CASNamespaceJanitor, ObservedAuthorityLossPublishesNothing)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 224), 4000);
    size_t refreshes = 0;
    bool held = true;
    JanitorRunContext context = f.context();
    context.liveness = [&] { return held; };
    context.refresh_authority = [&]
    {
        if (++refreshes == 3)
            held = false;
    };

    (void)f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 2000)) << "earlier pages' deletes stand";
    EXPECT_TRUE(allPresent(*f.backend, keys, 2000, 4000));
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, SteadyStateTakesOnePage)
{
    const CatalogEntry live{.ns = RootNamespace{"live"}, .state = NsState::Live, .incarnation = UInt128{225}};
    JanitorFixture f(RefCatalog{.entries = {live}});
    (void)seedLogs(*f.backend, kLayout, NamespaceLifeId::fromCatalogEntry(live.ns, live.incarnation), 5000);
    f.backend->resetCounts();

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.batches, 0u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_EQ(f.backend->writeCount(kLayout.gcMaintenanceStateKey()), 1u);
}

TEST(CASNamespaceJanitor, DeferredRoundTakesOneSuppressedPage)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 226), 3000);
    f.backend->resetCounts();

    const NamespaceJanitorResult result = f.run(f.context(), /*suppress_deletes=*/ true);

    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.deleted, 0u);
    EXPECT_TRUE(f.backend->bulkCalls().empty());
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(f.backend->writeCount(kLayout.gcMaintenanceStateKey()), 0u);
}

TEST(CASNamespaceJanitor, EndOfPassStopsWithoutWrapping)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 227), 10);
    createObj(*f.backend, kLayout.gcMaintenanceStateKey(), encodeGcMaintenanceState({.janitor_cursor = keys[4]}));

    (void)f.run(f.context());

    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 5)) << "no wrap to the start within the phase";
    EXPECT_TRUE(allAbsent(*f.backend, keys, 5, 10));
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, CursorFingerprintShowsStuckVsMoving)
{
    JanitorFixture f;
    (void)seedLogs(*f.backend, kLayout, life("dead", 228), 4000);
    JanitorRunContext one_page = f.context();
    one_page.budget_ms = 0;

    const NamespaceJanitorResult a = f.run(one_page);
    EXPECT_EQ(a.cursor_start_fp, 0u) << "the empty cursor";
    EXPECT_TRUE(a.cursor_advanced);

    std::atomic<bool> hold{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (hold)
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    const NamespaceJanitorResult b = f.run(one_page);
    const NamespaceJanitorResult c = f.run(one_page);
    EXPECT_NE(b.cursor_start_fp, 0u);
    EXPECT_EQ(b.cursor_start_fp, a.cursor_end_fp);
    EXPECT_EQ(b.cursor_start_fp, c.cursor_start_fp);
    EXPECT_FALSE(b.cursor_advanced);
    EXPECT_FALSE(c.cursor_advanced);
    EXPECT_EQ(c.cursor_end_fp, c.cursor_start_fp);

    hold = false;
    const NamespaceJanitorResult d = f.run(one_page);
    const NamespaceJanitorResult e = f.run(one_page);
    EXPECT_TRUE(d.cursor_advanced);
    EXPECT_NE(e.cursor_start_fp, d.cursor_start_fp);
}

TEST(CASNamespaceJanitor, DeterministicLocalFailureHolds)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 229), 2000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(f.backend->bulkCalls().size(), 1u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 2000));
}

TEST(CASNamespaceJanitor, AuthorityLostBeforeRetryHolds)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 230), 3000);
    f.backend->failNextBulkRemoveWith(std::make_exception_ptr(Poco::TimeoutException("injected throttle")));
    JanitorRunContext context = f.context(500);
    context.liveness = [&] { return f.backend->bulkRemoveCalls() == 0; };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->bulkRemoveCalls(), 1u);
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, 1000));
    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.batches, 1u);
    EXPECT_EQ(f.backend->bulkCalls().size(), 1u) << "no second batch";
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, AuthorityLostDuringSuccessfulBatchCounts)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 231), 3000);
    std::atomic<bool> held{true};
    f.backend->onBeforeBulkRemove([&] { held = false; });
    JanitorRunContext context = f.context(500);
    context.liveness = [&] { return held.load(); };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->bulkRemoveCalls(), 1u);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 500));
    EXPECT_TRUE(allPresent(*f.backend, keys, 500, 1000));
    EXPECT_EQ(result.batches, 1u);
    EXPECT_EQ(result.deleted, 500u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitor, EmptyNamespaceTakesOnePage)
{
    JanitorFixture f;
    const NamespaceJanitorResult result = f.run(f.context());
    EXPECT_EQ(result.pages, 1u);
    EXPECT_EQ(result.keys, 0u);
    EXPECT_EQ(result.delete_jobs, 0u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, FirstPageCatalogReadFailureThrowsAndPublishesNothing)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 232), 10);
    f.backend->failNextReadWith(kLayout.refCatalogKey(),
        std::make_exception_ptr(DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected catalog read failure")));

    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { (void)f.run(f.context()); });

    EXPECT_TRUE(f.backend->bulkCalls().empty());
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_TRUE(allPresent(*f.backend, keys, 0, keys.size()));
}

TEST(CASNamespaceJanitor, ClockGoingBackwardsDoesNotExhaustTheBudget)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 233), 2000);
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 0)
            f.clock.now -= 5'000;
    };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_FALSE(result.budget_exhausted);
    EXPECT_TRUE(allAbsent(*f.backend, keys, 0, 2000));
}

TEST(CASNamespaceJanitorIntegration, PoolWrappedStorageLimitCutsJanitorBatches)
{
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStorageLimit(250);
    auto store = openPoolForTest(backend, /*gc_fold_max_defer_rounds=*/ 0);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    /// With the live checkpoint, 999 dead keys fill exactly one page.
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 999);
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{252});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["batch_keys"], 250u);
    EXPECT_EQ(row["batches"], 4u) << "a decorator that dropped the forwarding would send 999 one-key requests";
    for (const String & key : keys)
        EXPECT_FALSE(present(*backend, key)) << key;
}

TEST(CASNamespaceJanitorIntegration, ZeroStorageLimitCutsOneKeyBatches)
{
    auto backend = std::make_shared<BatchCapabilityBackend>();
    backend->setStorageLimit(0);
    auto store = openPoolForTest(backend, 0);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 3);
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{253});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["batch_keys"], 1u);
    EXPECT_EQ(row["batches"], 3u);
    for (const String & key : keys)
        EXPECT_FALSE(present(*backend, key)) << key;
}

TEST(CASNamespaceJanitorIntegration, FoldingRoundDrainsWithinBudget)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openPoolForTest(backend, 0);
    const Layout & layout = store->layout();
    const NamespaceLifeId live = seedLiveNamespace(*backend, layout);
    const NamespaceLifeId dead = life("dead", 1);
    /// Dead `_files` list before the live life's, so the pass reaches live keys after 3,500 dead ones.
    ASSERT_LT(layout.namespaceFilesPrefix(dead), layout.namespaceFilesPrefix(live));
    std::vector<String> dead_keys;
    for (size_t i = 0; i < 3500; ++i)
    {
        dead_keys.push_back(layout.namespaceFilesPrefix(dead) + fmt::format("f{:05}", i));
        createObj(*backend, dead_keys.back(), "dead");
    }
    for (size_t i = 0; i < 1500; ++i)
        createObj(*backend, layout.namespaceFilesPrefix(live) + fmt::format("f{:05}", i), "live");
    backend->resetCounts();
    std::map<String, UInt64> row;
    Gc gc(store, UInt128{254});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["janitor_pages"], 5u) << "four pages with dead keys, then one without";
    EXPECT_EQ(row["janitor_deleted"], 3500u);
    EXPECT_EQ(row["budget_exhausted"], 0u);
    EXPECT_EQ(backend->writeCount(layout.gcMaintenanceStateKey()), 1u) << "one cursor publication per phase";
    for (const String & key : dead_keys)
        EXPECT_FALSE(present(*backend, key)) << key;
}
```

Then run cmake (new file): `cmake -S /home/mfilimonov/workspace/ClickHouse/cas-83 -B /home/mfilimonov/workspace/ClickHouse/cas-83/build > /home/mfilimonov/workspace/ClickHouse/cas-83/build/cmake_t4.log 2>&1`.

- [ ] **Step 4: Build to verify the tests fail**

Build into `build/t4_red.log`. Expected: `NINJA_EXIT=1` on `NamespaceJanitor::run`, `JanitorRunContext`, `writeOnceStreamKey`. Record the error list.

- [ ] **Step 5: Implement `writeOnceStreamKey`**

In `CasLayout.h`, apply the `1edf28f2a9a` hunk: `refLogKey` and `refSnapshotKey` return `refStreamObjectKey(ns_id.incarnation, RefObjectKind::Log|Snap, id)`; add after `writeOnceRefSnapshotKey`:

```cpp
    /// The write-once form of a LISTED `_log`/`_snap` key, minted from the identity `parseRefObjectKey`
    /// recovered from it. `std::nullopt` when the canonical key for that identity is not `listed_key`: a
    /// token-free delete must name only keys this layout writes.
    std::optional<WriteOnceKey> writeOnceStreamKey(const ParsedRefObjectKey & parsed, std::string_view listed_key) const
    {
        String canonical = refStreamObjectKey(parsed.life_id, parsed.kind, parsed.txn_id);
        if (canonical != listed_key)
            return std::nullopt;
        return WriteOnceKey(std::move(canonical));
    }
```

and in the private section:

```cpp
    String refStreamObjectKey(NamespaceLifePhysicalId life_id, RefObjectKind kind, const RefTxnId & id) const
    {
        const bool log = kind == RefObjectKind::Log;
        return namespaceStreamRootPrefix() + renderIncarnation(life_id) + (log ? "/_log/" : "/_snap/") + renderRefTxnId(id)
            + String(storedSuffix(log ? FormatId::RefLog : FormatId::RefSnapshot));
    }
```

- [ ] **Step 6: Implement the janitor**

Replace `CasNamespaceJanitor.h`:

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequests.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h>
#include <functional>
#include <limits>
#include <vector>

namespace DB::Cas
{

/// Counts of one janitor phase, summed over its pages. `run` fills them as pages and jobs settle, so a
/// phase that throws still reports the work it finished.
struct NamespaceJanitorResult
{
    uint64_t pages = 0;
    uint64_t keys = 0;
    uint64_t deleted = 0;
    uint64_t leaked = 0;
    uint64_t batches = 0;
    uint64_t batches_leaked = 0;
    uint64_t batches_held = 0;
    uint64_t delete_jobs = 0;
    uint64_t delete_jobs_skipped = 0;
    uint64_t jobs_in_flight_max = 0;
    uint64_t delete_calls = 0;
    uint64_t batch_keys = 0;
    uint64_t list_ms = 0;
    uint64_t catalog_ms = 0;
    uint64_t delete_ms = 0;
    uint64_t drain_wait_ms = 0;
    uint64_t list_requests_process = 0;
    uint64_t cursor_start_fp = 0;
    uint64_t cursor_end_fp = 0;
    bool capability_learned = false;
    bool budget_exhausted = false;
    bool cursor_advanced = false;
    std::vector<String> anomalies;
};

/// What one janitor phase runs with.
struct JanitorRunContext
{
    /// Sampled before every request; cheap and never throws.
    Liveness liveness;
    /// Re-reads authority before each page; `liveness` answers its verdict. Empty: no refresh.
    std::function<void()> refresh_authority;
    /// Keys per delete job, fixed for the phase; clamped to [1, `kBulkDeleteMaxKeys`].
    size_t batch_keys = kBulkDeleteMaxKeys;
    /// No page starts once this much time has passed since the phase began; the page in progress finishes.
    uint64_t budget_ms = std::numeric_limits<uint64_t>::max();
    /// Phase clock; empty reads as a clock that never moves.
    std::function<uint64_t()> now_ms;
};

/// Reclaims the objects of namespace lives absent from the catalog, page by page from the persisted cursor.
class NamespaceJanitor
{
public:
    NamespaceJanitor(CasRequests & requests_, const Layout & layout_, size_t page_keys_)
        : requests(requests_), layout(layout_), page_keys(page_keys_) {}

    /// One phase. Each page: refresh authority, LIST, one catalog cut after the LIST, exact-token deletes of
    /// dead `_ckpt`/`_files`, then jobs of `batch_keys` token-free deletes of dead canonical `_log`/`_snap`.
    /// Another page starts only within the budget, while no job has held, after a decided page that had a
    /// dead-life candidate and a next cursor. At the end one publication of the cursor after the longest
    /// prefix of complete pages. `suppress_deletes` lists and classifies one page and publishes nothing.
    /// Throws when the maintenance read, the first page's LIST (after resetting the cursor) or its catalog
    /// read fails; `out` keeps what completed.
    void run(bool suppress_deletes, const JanitorRunContext & context, NamespaceJanitorResult & out);

private:
    CasRequests & requests;
    const Layout & layout;
    size_t page_keys;
};

}
```

Replace `CasNamespaceJanitor.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcMaintenanceState.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefCatalog.h>
#include <Common/Exception.h>
#include <Common/ProfileEvents.h>
#include <Common/SipHash.h>
#include <Common/Stopwatch.h>
#include <base/scope_guard.h>
#include <fmt/format.h>
#include <algorithm>
#include <atomic>
#include <deque>

namespace ProfileEvents
{
    extern const Event DiskS3ListObjects;
}

namespace DB::Cas
{

namespace
{

/// The legacy `casPut` this write replaces reported a definite conflict as a value (never a failure to
/// this caller) and reported a store failure -- a refusal, an exhausted policy -- by throwing. Only
/// `Refused`/`GaveUp` are the alternatives a thrown exception used to carry, so only those propagate;
/// `Committed`/`Declined`/`Conflict` stay silent exactly as they did before.
void throwOnRefusedOrGaveUp(WriteResult && result, std::string_view what)
{
    if (std::holds_alternative<Refused>(result) || std::holds_alternative<GaveUp>(result))
        (void)orThrow(std::move(result), what);
}

uint64_t cursorFingerprint(const String & cursor)
{
    return cursor.empty() ? 0 : sipHash64(cursor);
}

uint64_t processListRequests()
{
    return ProfileEvents::global_counters[ProfileEvents::DiskS3ListObjects].load(std::memory_order_relaxed);
}

enum class JobOutcome : uint8_t
{
    Deleted,
    Leaked,
    Held,
    Skipped,
};

/// One delete job: written by whoever runs it, read by the round thread after every job finished.
struct JobSlot
{
    size_t page = 0;
    std::vector<WriteOnceKey> keys;
    JobOutcome outcome = JobOutcome::Skipped;
    bool sent = false;
    std::exception_ptr error;
};

/// A page is complete when it was decided and none of its jobs held or was skipped.
struct PageRecord
{
    String next_cursor;
    bool decided = false;
    bool held = false;
};

String describeKeys(const std::vector<WriteOnceKey> & keys)
{
    if (keys.size() == 1)
        return "'" + keys.front().str() + "'";
    return fmt::format("'{}' .. '{}' ({} keys)", keys.front().str(), keys.back().str(), keys.size());
}

/// Runs one job on its own operation, resumed from the phase's generation, so the mount fence and the
/// liveness predicate admit it on its own. Any failure holds the job's page.
void runJob(JobSlot & slot, CasRequests & requests, uint64_t generation, const Liveness & liveness, std::atomic<bool> & stop)
{
    if (stop.load())
        return;
    CasOperation job_op = requests.resume(generation, liveness);
    if (!job_op.admitted())
    {
        slot.outcome = JobOutcome::Held;
        stop.store(true);
        return;
    }
    slot.sent = true;
    try
    {
        job_op.removeManyWriteOnce(slot.keys, Retry::standard());
        slot.outcome = JobOutcome::Deleted;
    }
    catch (...)
    {
        slot.error = std::current_exception();
        slot.outcome = JobOutcome::Held;
        stop.store(true);
    }
}

/// The exact-token delete of one listed `_ckpt`/`_files` key; false when admission was lost.
bool removeExactly(CasOperation & op, const ListedKey & listed, NamespaceJanitorResult & out)
{
    std::optional<Etag> etag = listed.etag;
    if (!etag)
    {
        try
        {
            const std::optional<Meta> current = op.head(listed.key, Retry::standard());
            if (!current)
                return true;
            etag = current->etag;
        }
        catch (const std::exception & e)
        {
            ++out.leaked;
            out.anomalies.push_back("leaked dead-life object '" + listed.key + "': exact HEAD failed: " + e.what());
            return true;
        }
    }
    if (!op.admitted())
        return false;
    ++out.delete_calls;
    try
    {
        if (op.remove(listed.key, *etag, Retry::standard()) == Removal::Removed)
            ++out.deleted;
    }
    catch (const std::exception & e)
    {
        ++out.leaked;
        out.anomalies.push_back("leaked dead-life object '" + listed.key + "': exact delete failed: " + e.what());
    }
    return true;
}

}

void NamespaceJanitor::run(bool suppress_deletes, const JanitorRunContext & context, NamespaceJanitorResult & out)
{
    const auto now = [&] { return context.now_ms ? context.now_ms() : uint64_t{0}; };
    const uint64_t phase_start = now();
    /// Saturates: a clock read below the start is no time spent, never a wrapped huge one.
    const auto elapsed = [&]
    {
        const uint64_t current = now();
        return current > phase_start ? current - phase_start : uint64_t{0};
    };
    const uint64_t list_requests_start = processListRequests();
    SCOPE_EXIT(out.list_requests_process = processListRequests() - list_requests_start);
    const size_t batch_keys = std::clamp<size_t>(context.batch_keys, 1, kBulkDeleteMaxKeys);
    out.batch_keys = batch_keys;

    if (context.refresh_authority)
        context.refresh_authority();
    CasOperation op = requests.admit(context.liveness);
    const GcMaintenanceReadResult progress = readGcMaintenanceState(op, layout);
    if (progress.status == GcMaintenanceReadStatus::Corrupt)
    {
        out.anomalies.push_back(progress.diagnostic);
        throwOnRefusedOrGaveUp(
            casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::standard()),
            "CAS namespace janitor: corrupt maintenance-state reset");
        return;
    }
    const String start_cursor = progress.state ? progress.state->janitor_cursor : String{};
    out.cursor_start_fp = cursorFingerprint(start_cursor);
    out.cursor_end_fp = out.cursor_start_fp;

    std::deque<JobSlot> slots;
    std::vector<PageRecord> pages;
    std::atomic<bool> stop{false};
    const uint64_t generation = op.generation();

    const auto submit = [&](size_t page_index, std::vector<WriteOnceKey> keys)
    {
        if (stop.load())
            return false;
        JobSlot & slot = slots.emplace_back();
        slot.page = page_index;
        slot.keys = std::move(keys);
        ++out.delete_jobs;
        runJob(slot, requests, generation, context.liveness, stop);
        return true;
    };

    String cursor = start_cursor;
    for (size_t page_index = 0;; ++page_index)
    {
        if (page_index > 0)
        {
            if (context.refresh_authority)
                context.refresh_authority();
            if (!op.admitted())
                break;
        }

        ListPage page;
        try
        {
            Stopwatch watch;
            page = op.list(layout.namespaceRootPrefix(), cursor, page_keys, Retry::standard());
            out.list_ms += watch.elapsedMilliseconds();
        }
        catch (...)
        {
            if (page_index > 0)
            {
                out.anomalies.push_back("namespace page LIST failed: " + getCurrentExceptionMessage(false));
                break;
            }
            /// Only the persisted cursor is reset: a cursor the store rejects would fail every round.
            (void)casGcMaintenanceState(op, layout, progress.etag, GcMaintenanceState{}, Retry::once());
            throw;
        }
        ++out.pages;
        out.keys += page.keys.size();

        std::optional<CasRefCatalog::Snapshot> catalog_cut;
        try
        {
            Stopwatch watch;
            catalog_cut.emplace(CasRefCatalog::read(op, layout));
            out.catalog_ms += watch.elapsedMilliseconds();
        }
        catch (...)
        {
            if (page_index == 0)
                throw;
            out.anomalies.push_back("namespace page catalog read failed: " + getCurrentExceptionMessage(false));
            break;
        }

        bool ambiguous = false;
        try
        {
            catalog_cut->life_index.throwIfAmbiguous("CAS namespace janitor");
        }
        catch (const DB::Exception & e)
        {
            out.anomalies.push_back(e.message());
            ambiguous = true;
        }
        /// A page is decided only when the round had deletion authority for every dead-life candidate on it.
        /// Advancing while the global gate is closed could phase-lock a dead page onto every suppressed round.
        /// Malformed keys, absent objects and token mismatches are final per-key outcomes.
        bool decided = !ambiguous && !suppress_deletes;
        bool dead_candidate = false;
        std::vector<const ListedKey *> exact_keys;
        std::vector<WriteOnceKey> stream_keys;
        for (const ListedKey & listed : page.keys)
        {
            std::optional<NamespaceLifePhysicalId> life_id;
            std::optional<ParsedRefObjectKey> stream_key;
            try
            {
                if (listed.key.starts_with(layout.namespaceStreamRootPrefix()))
                {
                    stream_key = layout.parseRefObjectKey(listed.key);
                    if (stream_key)
                        life_id = stream_key->life_id;
                }
                else if (listed.key.starts_with(layout.namespaceStateRootPrefix()))
                {
                    if (const auto parsed = layout.parseRefCkptKey(listed.key))
                        life_id = *parsed;
                    else if (const auto file_parsed = layout.parseNamespaceFileKey(listed.key))
                        life_id = file_parsed->life_id;
                }
            }
            catch (const DB::Exception & e)
            {
                out.anomalies.push_back(listed.key + ": " + e.message());
                continue;
            }
            if (!life_id)
            {
                out.anomalies.push_back(listed.key + ": unrecognized namespace object key");
                continue;
            }
            if (ambiguous || suppress_deletes || catalog_cut->life_index.resolve(*life_id))
                continue;
            dead_candidate = true;
            /// A dead life's `_log`/`_snap` are write-once and never reborn under its prefix, so they need no token.
            if (stream_key)
            {
                if (std::optional<WriteOnceKey> write_once = layout.writeOnceStreamKey(*stream_key, listed.key))
                {
                    stream_keys.push_back(std::move(*write_once));
                    continue;
                }
            }
            exact_keys.push_back(&listed);
        }

        pages.push_back(PageRecord{.next_cursor = page.next_cursor});
        Stopwatch delete_watch;
        for (const ListedKey * listed : exact_keys)
        {
            if (!removeExactly(op, *listed, out))
            {
                decided = false;
                break;
            }
        }
        for (size_t begin = 0; decided && begin < stream_keys.size(); begin += batch_keys)
        {
            if (!op.admitted())
            {
                decided = false;
                break;
            }
            const size_t end = std::min(stream_keys.size(), begin + batch_keys);
            if (!submit(page_index, std::vector<WriteOnceKey>(stream_keys.begin() + begin, stream_keys.begin() + end)))
            {
                pages.back().held = true;
                break;
            }
        }
        out.delete_ms += delete_watch.elapsedMilliseconds();
        pages.back().decided = decided;

        if (!decided || !dead_candidate || page.next_cursor.empty() || stop.load())
            break;
        if (elapsed() >= context.budget_ms)
        {
            out.budget_exhausted = true;
            break;
        }
        cursor = page.next_cursor;
    }

    out.jobs_in_flight_max = out.delete_jobs > 0 ? 1 : 0;
    for (JobSlot & slot : slots)
    {
        if (slot.sent)
        {
            ++out.batches;
            ++out.delete_calls;
        }
        switch (slot.outcome)
        {
            case JobOutcome::Deleted:
                out.deleted += slot.keys.size();
                break;
            case JobOutcome::Leaked:
                out.leaked += slot.keys.size();
                ++out.batches_leaked;
                out.anomalies.push_back("leaked dead-life objects " + describeKeys(slot.keys) + ": " + getExceptionMessage(slot.error, false));
                break;
            case JobOutcome::Held:
                ++out.batches_held;
                pages[slot.page].held = true;
                out.anomalies.push_back("held dead-life objects " + describeKeys(slot.keys) + ": "
                    + (slot.error ? getExceptionMessage(slot.error, false) : String("authority lost before the request")));
                break;
            case JobOutcome::Skipped:
                ++out.delete_jobs_skipped;
                pages[slot.page].held = true;
                break;
        }
    }

    std::optional<String> publish_cursor;
    for (const PageRecord & record : pages)
    {
        if (!record.decided || record.held)
            break;
        publish_cursor = record.next_cursor;
    }
    /// Re-checked even when no page had a dead candidate: a tenure that lost its fence must not publish.
    if (!publish_cursor || !op.admitted())
        return;
    try
    {
        const WriteResult published = casGcMaintenanceState(
            op, layout, progress.etag, GcMaintenanceState{.janitor_cursor = *publish_cursor}, Retry::standard());
        if (std::holds_alternative<Refused>(published) || std::holds_alternative<GaveUp>(published))
            out.anomalies.push_back("cursor publication did not commit");
        else if (std::holds_alternative<Committed>(published))
        {
            out.cursor_advanced = true;
            out.cursor_end_fp = cursorFingerprint(*publish_cursor);
        }
    }
    catch (const std::exception & e)
    {
        out.anomalies.push_back("cursor publication failed: " + String(e.what()));
    }
}

}
```

`CasPool.h`, after `gc_io_pool_refuse_at_for_test`:

```cpp
    /// TEST SEAM: the namespace janitor's phase budget instead of its 20 s constant.
    std::optional<uint64_t> gc_janitor_phase_budget_ms_for_test = std::nullopt;
```

`CasGc.h`: rename the declaration and its comment:

```cpp
    /// One namespace-janitor phase: pages from the persisted cursor until the soft budget, a hold, or a page
    /// without dead-life debris. DEFER passes suppression, which lists one page and deletes nothing. Owns only
    /// janitor I/O and phase metrics, never lifecycle transitions.
    void runNamespaceJanitor(
        const GcState & leased_state, bool suppress_destructive, uint64_t cleanup_evidence_rows);
```

`CasGc.cpp`: in the file-local anonymous namespace near the top (create one after the `ErrorCodes` block if absent):

```cpp
/// Soft limit on one janitor phase: no page starts after it. A third of the default `gc_interval_sec`; at
/// 20 ms RTT it drains ~140 batch pages, or ~15 pages of one-key deletes at 16 in flight.
constexpr uint64_t kJanitorPhaseBudgetMs = 20'000;
constexpr size_t kJanitorPageKeys = 1000;
```

and replace `Gc::runNamespaceJanitorPage`:

```cpp
void Gc::runNamespaceJanitor(
    const GcState & leased_state, bool suppress_destructive, uint64_t cleanup_evidence_rows)
{
    GcPhaseTimer t(phase_sink, "namespace_cleanup");
    t.metric("evidence_rows", cleanup_evidence_rows);
    NamespaceJanitorResult result;
    try
    {
        const PoolConfig & config = store->poolConfig();
        const uint64_t lease_seq = leased_state.lease.seq;
        JanitorRunContext context;
        /// One authority read per page, from the refresh; the predicate is sampled before every request.
        context.liveness = [this] { return authority_held; };
        context.refresh_authority = [this, lease_seq] { refreshAuthority(lease_seq); };
        context.batch_keys = bulkDeleteChunkKeys();
        context.budget_ms = config.gc_janitor_phase_budget_ms_for_test.value_or(kJanitorPhaseBudgetMs);
        context.now_ms = mono_ms_fn;
        NamespaceJanitor(store->openRequests(), store->layout(), kJanitorPageKeys).run(suppress_destructive, context, result);
    }
    catch (const std::exception & e)
    {
        LOG_WARNING(logger, "CAS namespace janitor stopped this round: {}", e.what());
    }
    for (const String & anomaly : result.anomalies)
        LOG_WARNING(logger, "CAS namespace janitor: {}", anomaly);
    if (result.leaked)
        ProfileEvents::increment(ProfileEvents::CASGCNamespaceCleanupLeaks, result.leaked);
    t.metric("janitor_pages", result.pages);
    t.metric("janitor_keys", result.keys);
    t.metric("janitor_deleted", result.deleted);
    t.metric("leaked", result.leaked);
    t.metric("batches", result.batches);
    t.metric("batches_leaked", result.batches_leaked);
    t.metric("batches_held", result.batches_held);
    t.metric("delete_jobs", result.delete_jobs);
    t.metric("delete_jobs_skipped", result.delete_jobs_skipped);
    t.metric("jobs_in_flight_max", result.jobs_in_flight_max);
    t.metric("drain_wait_ms", result.drain_wait_ms);
    t.metric("list_requests_process", result.list_requests_process);
    t.metric("list_ms", result.list_ms);
    t.metric("catalog_ms", result.catalog_ms);
    t.metric("delete_ms", result.delete_ms);
    t.metric("batch_keys", result.batch_keys);
    t.metric("capability_learned", result.capability_learned ? 1 : 0);
    t.metric("delete_calls", result.delete_calls);
    t.metric("budget_exhausted", result.budget_exhausted ? 1 : 0);
    t.metric("cursor_start_fp", result.cursor_start_fp);
    t.metric("cursor_end_fp", result.cursor_end_fp);
    t.metric("cursor_advanced", result.cursor_advanced ? 1 : 0);
}
```

Update both call sites (`:823` deferred, `:1334` fold) from `runNamespaceJanitorPage` to `runNamespaceJanitor`. `grep -rn "runNamespaceJanitorPage\|runOnePage" src docs/en` must print nothing afterwards, except inside `src/Disks/tests/cas_namespace_janitor_test_helpers.h` (the test helper `runOnePage`); fix any doc or comment hit.

- [ ] **Step 7: Build and run**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build unit_tests_dbms > build/t4_build.log 2>&1; echo NINJA_EXIT=$? >> build/t4_build.log; tail -1 build/t4_build.log
( ulimit -v 33554432; build/src/unit_tests_dbms --gtest_filter='CASNamespaceJanitor*:CASGcMaintenanceState*:CASWriteOnceKey*' > build/t4_green.log 2>&1; echo GATE_EXIT=$? >> build/t4_green.log ); tail -3 build/t4_green.log
```
Expected: `GATE_EXIT=0`. Every old `CASNamespaceJanitor` test passes unchanged through `runOnePage`.

- [ ] **Step 8: Gate**

`CAS*` into `build/t4_gate.log`; expected `GATE_EXIT=0`. A GC test that asserted `janitor_pages` 1 with several pages of dead debris now sees more pages: read the test's intent; if it pins the old one-page cap, update the expectation to the drained count and say so in the commit body. Any other red: root-cause.

- [ ] **Step 9: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
C=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
git add -- $C/Formats/CasLayout.h $C/Gc/CasNamespaceJanitor.h $C/Gc/CasNamespaceJanitor.cpp $C/Gc/CasGc.h $C/Gc/CasGc.cpp $C/Pool/CasPool.h \
  src/Disks/tests/cas_namespace_janitor_test_helpers.h src/Disks/tests/gtest_cas_namespace_janitor.cpp \
  src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp src/Disks/tests/gtest_cas_write_once_key.cpp
git commit -s -m "cas: drain dead-life ref streams in batches under a janitor phase budget"
```

---

### Task 5: Batch failure rule: leak and advance, hold only on authority, capability and local bugs

**Files:**
- Modify: `.../Gc/CasNamespaceJanitor.cpp` (`JobSlot`, `runJob`, aggregation)
- Test: `src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp`
- Create: `src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp`

**Interfaces:**
- Consumes: `run`, `JanitorFixture`, `JanitorBackend` (Task 4); `cas_scripted_s3_server.h` (Task 1); names preserved by Task 3.
- Produces: job outcome classification used by Task 6 unchanged; `NamespaceJanitorResult::capability_learned` set on a `NOT_IMPLEMENTED` job.

- [ ] **Step 1: Write the failing tests (in-memory)**

Append to `gtest_cas_namespace_janitor_batches.cpp`:

```cpp
TEST(CASNamespaceJanitor, ExhaustedTransientBatchLeaksAndAdvances)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 301), 1500);
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        throw Poco::TimeoutException("injected throttle, every attempt");
    };
    JanitorRunContext context = f.context();
    context.budget_ms = 20'000;

    const NamespaceJanitorResult result = f.run(context);

    const auto calls = f.backend->bulkCalls();
    ASSERT_GT(calls.size(), 1u) << "the policy reissues the batch before giving up";
    for (const auto & call : calls)
        EXPECT_EQ(call.keys, calls.front().keys) << "no per-key or partial re-cut";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_TRUE(result.budget_exhausted) << "the 90 s retry window on the phase clock exceeds the budget";
    EXPECT_EQ(f.cursor(), keys[999]);
}

TEST(CASNamespaceJanitor, SlowDownForeverDoesNotStarveLaterDebris)
{
    const auto seeded = [](JanitorFixture & f)
    {
        f.backend->setBatchDeleteSupported(false);
        const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 302), 3000);
        f.backend->before_bulk = [bad = keys[499]](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
        {
            if (batch.front().str() == bad)
                throw Poco::TimeoutException("injected throttle, forever");
        };
        return keys;
    };
    {
        SCOPED_TRACE("budget above the retry window");
        JanitorFixture f;
        const std::vector<String> keys = seeded(f);
        JanitorRunContext context = f.context(f.backend->bulkDeleteKeyLimit());
        context.budget_ms = 10'000'000;
        const NamespaceJanitorResult result = f.run(context);
        EXPECT_EQ(result.leaked, 1u);
        EXPECT_EQ(result.batches_leaked, 1u);
        EXPECT_EQ(result.deleted, 2999u);
        EXPECT_TRUE(present(*f.backend, keys[499]));
        EXPECT_EQ(f.cursor(), String{});
    }
    {
        SCOPED_TRACE("default budget");
        JanitorFixture f;
        const std::vector<String> keys = seeded(f);
        JanitorRunContext context = f.context(f.backend->bulkDeleteKeyLimit());
        context.budget_ms = 20'000;
        const NamespaceJanitorResult first = f.run(context);
        EXPECT_TRUE(first.budget_exhausted);
        EXPECT_EQ(f.cursor(), keys[999]);
        const NamespaceJanitorResult second = f.run(context);
        EXPECT_EQ(second.deleted, 2000u);
        EXPECT_EQ(f.cursor(), String{});
        const NamespaceJanitorResult wrapped = f.run(context);
        EXPECT_EQ(wrapped.leaked, 1u) << "the next pass leaks the same key again";
        EXPECT_EQ(wrapped.keys, 1u);
    }
}

TEST(CASNamespaceJanitor, PerKeyErrorInOkResponseLeaksTheBatch)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 303), 1000);
    std::atomic<bool> fail{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access & access)
    {
        if (!fail)
            return;
        for (const WriteOnceKey & key : batch)
            if (key.str() != keys[500])
                f.backend->removeUncounted(key.str(), access);
        throw Poco::TimeoutException("the survivor answers a throttle on every attempt");
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.deleted, 0u) << "a failed batch counts nothing as deleted";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_EQ(f.cursor(), String{});
    fail = false;
    const NamespaceJanitorResult next = f.run(f.context());
    EXPECT_EQ(next.keys, 1u) << "the next pass lists only the survivor";
    EXPECT_EQ(next.deleted, 1u);
}

TEST(CASNamespaceJanitor, UnknownCapabilityRejectionStopsThePhase)
{
    JanitorFixture f;
    f.backend->setStoreRejectsBatches(true);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 304), 3000);

    const NamespaceJanitorResult result = f.run(f.context(f.backend->bulkDeleteKeyLimit()));

    EXPECT_EQ(f.backend->requestSizes(), (std::vector<size_t>{1000}));
    EXPECT_EQ(f.backend->callSizes().size(), 1u) << "no one-key call for the rejected keys";
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_TRUE(result.capability_learned);
    EXPECT_EQ(result.batches_held, 1u);

    const NamespaceJanitorResult next = f.run(f.context(f.backend->bulkDeleteKeyLimit()));
    EXPECT_EQ(next.batch_keys, 1u);
    EXPECT_EQ(next.deleted, 3000u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitor, CapabilityFlippedByAnotherUserStopsThePhase)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 305), 3000);
    f.backend->after_bulk = [&](const JanitorBackend::Keys &) { f.backend->setBatchDeleteSupported(false); };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.backend->requestSizes(), (std::vector<size_t>{1000}));
    EXPECT_EQ(f.backend->callSizes().size(), 2u) << "page 2's batch is refused without a request";
    EXPECT_TRUE(result.capability_learned);
    EXPECT_EQ(f.backend->namespaceLists(), 2u);
    EXPECT_EQ(f.cursor(), keys[999]);
}
```

- [ ] **Step 2: Write the failing tests (S3)**

Create `src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp`:

```cpp
#include "config.h"

#if USE_AWS_S3

#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/tests/cas_scripted_s3_server.h>
#include <IO/S3Common.h>

/// Janitor tests that need `S3Exception` classification or the real S3 delete adapter.

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;
using namespace DB::Cas::tests::s3;

namespace
{

const Layout & kLayout = janitorLayout();

std::exception_ptr accessDenied()
{
    return std::make_exception_ptr(DB::S3Exception("Access Denied", Aws::S3::S3Errors::ACCESS_DENIED, "AccessDenied"));
}

/// The in-memory store whose `removeManyWriteOnce` goes through a real `S3ObjectStorage` against a scripted
/// server: the adapter's request shapes, error names and capability learning, every other verb in memory.
/// After each call it mirrors the keys the server reports removed.
class S3DeletePathBackend : public CountingBackend
{
public:
    S3DeletePathBackend(std::shared_ptr<DB::S3ObjectStorage> storage_, std::shared_ptr<ScriptedDeletes> deleted_)
        : storage(std::move(storage_)), deleted(std::move(deleted_)) {}

    size_t bulkDeleteKeyLimit() const override { return storage->batchDeleteKeyLimit(); }

    RawListPage list(const String & prefix, const String & cursor, size_t limit, TransportAccess & access) override
    {
        if (prefix.ends_with("/cas/ns/"))
            record("list");
        return CountingBackend::list(prefix, cursor, limit, access);
    }

    void removeManyWriteOnce(const std::vector<WriteOnceKey> & keys, TransportAccess & access) override
    {
        {
            std::lock_guard lock(mutex);
            threads.push_back(std::this_thread::get_id());
        }
        DB::StoredObjects objects;
        for (const WriteOnceKey & key : keys)
            objects.emplace_back(key.str());
        std::exception_ptr failure;
        try
        {
            storage->removeObjectsIfExistUnderProfile(objects, DB::ObjectStorageControlRequest{});
        }
        catch (...)
        {
            failure = std::current_exception();
        }
        std::vector<WriteOnceKey> gone;
        for (const WriteOnceKey & key : keys)
        {
            if (deleted->contains(key.str()))
            {
                gone.push_back(key);
                record("delete " + key.str());
            }
        }
        InMemoryBackend::removeManyWriteOnce(gone, access); // NOLINT(bugprone-parent-virtual-call)
        if (failure)
            std::rethrow_exception(failure);
    }

    std::vector<std::thread::id> callThreads() const
    {
        std::lock_guard lock(mutex);
        return threads;
    }

    std::vector<String> events() const
    {
        std::lock_guard lock(mutex);
        return event_log;
    }

private:
    void record(String event)
    {
        std::lock_guard lock(mutex);
        event_log.push_back(std::move(event));
    }

    std::shared_ptr<DB::S3ObjectStorage> storage;
    std::shared_ptr<ScriptedDeletes> deleted;
    mutable std::mutex mutex;
    std::vector<std::thread::id> threads;
    std::vector<String> event_log;
};

/// Accepts every delete: a one-key DELETE and every key of a `DeleteObjects` are recorded removed.
void acceptAll(ScriptedDeletes & deleted, const Poco::Net::HTTPServerRequest & request, const std::string & body,
               Poco::Net::HTTPServerResponse & response)
{
    if (request.getMethod() == "DELETE")
    {
        deleted.add(keyOfDeleteObject(request));
        sendDeleteObjectSuccess(response);
        return;
    }
    for (const std::string & key : keysOfDeleteObjects(body))
        deleted.add(key);
    sendDeleteObjectsResult(response, {});
}

/// The janitor over `S3DeletePathBackend`. `respond` must be set before the first request.
struct S3PathFixture
{
    std::shared_ptr<ScriptedDeletes> deleted = std::make_shared<ScriptedDeletes>();
    std::function<void(const Poco::Net::HTTPServerRequest &, const std::string &, Poco::Net::HTTPServerResponse &)> respond
        = [this](const auto & request, const std::string & body, auto & response) { acceptAll(*deleted, request, body, response); };
    ScriptedS3Server server{[this](const auto & request, const std::string & body, auto & response) { respond(request, body, response); }};
    FakeClock clock;
    std::shared_ptr<S3DeletePathBackend> backend;
    std::unique_ptr<CasRequests> requests;

    explicit S3PathFixture(const DB::S3Capabilities & capabilities, const StorageOptions & options = {})
    {
        (void)contextForTest();
        backend = std::make_shared<S3DeletePathBackend>(makeStorageForTest(server.getUrl(), capabilities, options), deleted);
        requests = std::make_unique<CasRequests>(backend, Fence::open(), clock.nowFn(), clock.sleepFn());
        seedCatalog(*backend, kLayout);
    }

    JanitorRunContext context()
    {
        JanitorRunContext result;
        result.batch_keys = backend->bulkDeleteKeyLimit();
        result.now_ms = clock.nowFn();
        return result;
    }

    NamespaceJanitorResult run(const JanitorRunContext & context) { return runPhase(*requests, kLayout, context); }
    std::optional<String> cursor() { return publishedCursor(*requests, kLayout); }
};

}

TEST(CASNamespaceJanitorS3, PermanentRefusalLeaksAndAdvances)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 311), 2000);
    std::atomic<bool> refuse{true};
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (refuse.exchange(false))
            std::rethrow_exception(accessDenied());
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.backend->bulkCalls().size(), 2u) << "one attempt for the refused batch, one for page 2";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    for (size_t i = 1000; i < 2000; ++i)
        EXPECT_FALSE(present(*f.backend, keys[i])) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, UndeletableKeyDoesNotStarveLaterDebris)
{
    JanitorFixture f;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 312), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access & access)
    {
        const bool has_bad = std::any_of(batch.begin(), batch.end(), [&](const WriteOnceKey & key) { return key.str() == keys[10]; });
        if (!has_bad)
            return;
        for (const WriteOnceKey & key : batch)
            if (key.str() != keys[10])
                f.backend->removeUncounted(key.str(), access);
        std::rethrow_exception(accessDenied());
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_TRUE(present(*f.backend, keys[10]));
    for (size_t i = 1000; i < 3000; ++i)
        EXPECT_FALSE(present(*f.backend, keys[i])) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, MixedPerKeyErrorsClassifyByFirst)
{
    for (const bool refusal_first : {true, false})
    {
        SCOPED_TRACE(refusal_first ? "AccessDenied then SlowDown" : "SlowDown then AccessDenied");
        S3PathFixture f(DB::S3Capabilities{});
        const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 313), 10);
        f.respond = [&](const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse & response)
        {
            for (const std::string & key : keysOfDeleteObjects(body))
                if (key != keys[1] && key != keys[2])
                    f.deleted->add(key);
            if (refusal_first)
                sendDeleteObjectsResult(response, {{keys[1], "AccessDenied"}, {keys[2], "SlowDown"}});
            else
                sendDeleteObjectsResult(response, {{keys[2], "SlowDown"}, {keys[1], "AccessDenied"}});
        };

        const NamespaceJanitorResult result = f.run(f.context());

        if (refusal_first)
            EXPECT_EQ(f.server.countMethod("POST"), 1u);
        else
            EXPECT_GT(f.server.countMethod("POST"), 1u) << "a throttle first retries until the policy gives up";
        EXPECT_EQ(result.leaked, 10u);
        EXPECT_EQ(result.batches_leaked, 1u);
        EXPECT_EQ(f.cursor(), String{});
    }
}

TEST(CASNamespaceJanitorS3, NameOnlyRefusalLeaksAndAdvances)
{
    S3PathFixture f(DB::S3Capabilities{});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 314), 1500);
    f.respond = [&](const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        bool refuse = false;
        for (const std::string & key : keysOfDeleteObjects(body))
        {
            if (key == keys[3])
                refuse = true;
            else
                f.deleted->add(key);
        }
        if (refuse)
            sendDeleteObjectsResult(response, {{keys[3], "EntityTooLarge"}});
        else
            sendDeleteObjectsResult(response, {});
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(f.server.countMethod("POST"), 2u) << "one attempt for the refused batch, one for page 2";
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_TRUE(present(*f.backend, keys[3]));
    for (size_t i = 1000; i < 1500; ++i)
        EXPECT_FALSE(present(*f.backend, keys[i])) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, NameOnlyRefusalWithRefreshCallbackReissuesAtMostOnce)
{
    std::atomic<size_t> callbacks{0};
    std::string url;
    StorageOptions options;
    options.credentials_refresh_callback = [&]() -> std::unique_ptr<const DB::S3::Client>
    {
        ++callbacks;
        return makeClientForTest(url);
    };
    S3PathFixture f(DB::S3Capabilities{}, options);
    url = f.server.getUrl();
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 315), 1500);
    f.respond = [&](const Poco::Net::HTTPServerRequest &, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        bool refuse = false;
        for (const std::string & key : keysOfDeleteObjects(body))
        {
            if (key == keys[3])
                refuse = true;
            else
                f.deleted->add(key);
        }
        if (refuse)
            sendDeleteObjectsResult(response, {{keys[3], "EntityTooLarge"}});
        else
            sendDeleteObjectsResult(response, {});
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_LE(f.server.countMethod("POST"), 3u) << "at most 2 requests for the refused batch, 1 for page 2";
    EXPECT_LE(callbacks.load(), 1u);
    EXPECT_EQ(result.leaked, 1000u);
    EXPECT_EQ(result.delete_calls, 2u) << "a refresh reissue is not a new logical call";
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorS3, OneKeyNameOnlyRefusalLeaksAfterOneAttempt)
{
    S3PathFixture f(DB::S3Capabilities{false});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 316), 1500);
    std::atomic<size_t> bad_attempts{0};
    f.respond = [&](const Poco::Net::HTTPServerRequest & request, const std::string &, Poco::Net::HTTPServerResponse & response)
    {
        const std::string key = keyOfDeleteObject(request);
        if (key == keys[10])
        {
            ++bad_attempts;
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, "EntityTooLarge", "refused");
            return;
        }
        f.deleted->add(key);
        sendDeleteObjectSuccess(response);
    };

    const NamespaceJanitorResult result = f.run(f.context());

    EXPECT_EQ(result.batch_keys, 1u);
    EXPECT_EQ(bad_attempts.load(), 1u);
    EXPECT_EQ(result.leaked, 1u);
    EXPECT_FALSE(result.budget_exhausted);
    EXPECT_EQ(f.server.countMethod("POST"), 0u);
    for (size_t i = 0; i < keys.size(); ++i)
        EXPECT_EQ(present(*f.backend, keys[i]), i == 10) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
}

#endif
```

Run cmake (new file) into `build/cmake_t5.log`.

- [ ] **Step 3: Build and run to verify they fail**

Build into `build/t5_red_build.log` (expect `NINJA_EXIT=0`: the tests use only Task 4 interfaces). Run `--gtest_filter='CASNamespaceJanitor*'` into `build/t5_red.log`.
Expected FAIL: every test above except `CapabilityFlippedByAnotherUserStopsThePhase`'s request-size lines; failures show `batches_held` 1 where `batches_leaked` 1 is expected, cursors unpublished, `capability_learned` false.

- [ ] **Step 4: Implement the classification**

In `CasNamespaceJanitor.cpp`: add `bool capability = false;` to `JobSlot` (after `sent`); add in the anonymous namespace, before `runJob`:

```cpp
/// Holding the cursor on an ordinary store failure would let one key that always fails starve every page
/// behind it; leaking costs one pass and never deletes wrongly. So only lost authority, `NOT_IMPLEMENTED`
/// and a deterministic local failure hold the page. Every other failure has spent its retry policy and leaks.
JobOutcome classifyFailure(const std::exception_ptr & error, const CasOperation & job_op, bool & capability)
{
    /// Both halves of the gate are monotone within a phase, so a re-sample that reads false means the
    /// refusal was about authority, whatever the exception says.
    if (!job_op.admitted())
        return JobOutcome::Held;
    try
    {
        std::rethrow_exception(error);
    }
    catch (const DB::Exception & e)
    {
        capability = e.code() == ErrorCodes::NOT_IMPLEMENTED;
        return isDeterministicLocalFailure(e.code()) ? JobOutcome::Held : JobOutcome::Leaked;
    }
    catch (const Poco::Exception &)
    {
        return JobOutcome::Leaked;
    }
    catch (...)
    {
        return JobOutcome::Held;
    }
}
```

Add `#include <Poco/Exception.h>` and `namespace DB::ErrorCodes { extern const int NOT_IMPLEMENTED; }`. Replace the `catch (...)` of `runJob` and update its comment's last sentence to "A failed job leaks or holds by `classifyFailure`.":

```cpp
    catch (...)
    {
        slot.error = std::current_exception();
        slot.outcome = classifyFailure(slot.error, job_op, slot.capability);
        if (slot.outcome == JobOutcome::Held)
            stop.store(true);
    }
```

In the aggregation, `case JobOutcome::Held:` gains `out.capability_learned = out.capability_learned || slot.capability;`.

- [ ] **Step 5: Build and run**

Build into `build/t5_build.log`; run `CASNamespaceJanitor*:CASS3BatchDelete*` into `build/t5_green.log`. Expected `GATE_EXIT=0`.

- [ ] **Step 6: Gate**

`CAS*` into `build/t5_gate.log`; expected `GATE_EXIT=0`.

- [ ] **Step 7: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
git add -- src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasNamespaceJanitor.cpp \
  src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp
git commit -s -m "cas: leak and advance on a failed janitor batch; hold only on authority, capability, local bugs"
```

---

### Task 6: Jobs on the GC I/O pool: slot window, stop flag, authority latch, refresh without a transient false

**Files:**
- Modify: `.../Gc/CasNamespaceJanitor.h` (context fields), `.../Gc/CasNamespaceJanitor.cpp` (`run` fan-out)
- Modify: `.../Gc/CasGc.h:991` (`std::atomic<bool> authority_held`), `.../Gc/CasGc.cpp` (`refreshAuthority` ~:4850, `runNamespaceJanitor`)
- Modify: `.../Pool/CasPool.h` (seam), `src/Common/setThreadName.h:37` (`CAS_GC_JANITOR`)
- Create: `src/Disks/tests/gtest_cas_namespace_janitor_pipeline.cpp`
- Test: `src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp`

**Interfaces:**
- Consumes: Tasks 4 and 5.
- Produces:
  - `JanitorRunContext` gains, after `now_ms`: `ThreadPool * io_pool = nullptr; size_t io_concurrency = 1; std::optional<size_t> * schedule_refuse_at_for_test = nullptr; std::function<void()> on_final_wait_for_test; std::function<void(size_t page, size_t job)> on_job_start_for_test;` (page and job are 0-based within the phase and the page).
  - `std::function<void(size_t, size_t)> PoolConfig::gc_janitor_job_start_hook_for_test`.
  - `ThreadName::CAS_GC_JANITOR`.

- [ ] **Step 1: Write the failing tests**

Create `src/Disks/tests/gtest_cas_namespace_janitor_pipeline.cpp`:

```cpp
#include "cas_namespace_janitor_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.h>
#include <Common/CurrentMetrics.h>
#include <Common/ThreadPool.h>
#include <Poco/Exception.h>

/// The janitor phase with jobs on a real pool at concurrency 4. Assertions hold for any interleaving.

namespace CurrentMetrics
{
    extern const Metric LocalThread;
    extern const Metric LocalThreadActive;
    extern const Metric LocalThreadScheduled;
}

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

using namespace DB::Cas;
using namespace DB::Cas::tests;
using namespace DB::Cas::tests::janitor;

namespace
{

const Layout & kLayout = janitorLayout();

std::unique_ptr<ThreadPool> makeJobPool(size_t threads)
{
    return std::make_unique<ThreadPool>(
        CurrentMetrics::LocalThread, CurrentMetrics::LocalThreadActive, CurrentMetrics::LocalThreadScheduled,
        /*max_threads*/ threads, /*max_free_threads*/ threads, /*queue_size*/ 0);
}

JanitorRunContext parallel(JanitorFixture & f, ThreadPool & pool, size_t batch_keys, size_t concurrency = 4)
{
    JanitorRunContext context = f.context(batch_keys);
    context.io_pool = &pool;
    context.io_concurrency = concurrency;
    return context;
}

size_t countPresent(Backend & backend, const std::vector<String> & keys, size_t begin, size_t end)
{
    size_t count = 0;
    for (size_t i = begin; i < end; ++i)
        count += present(backend, keys[i]) ? 1 : 0;
    return count;
}

}

TEST(CASNamespaceJanitorPipeline, PipelineListsNextPageWhileJobsRun)
{
    JanitorFixture f;
    ManualBarrier page_two_listed;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 401), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0])
            page_two_listed.arriveAndWait();
    };
    f.backend->after_namespace_list = [&](size_t index, JanitorBackend::Access &)
    {
        if (index == 1)
            page_two_listed.release();
    };
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1000));

    EXPECT_EQ(result.batches_held, 0u) << "a round thread that waited for the batch would time the barrier out";
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 3000), 0u);
    EXPECT_EQ(f.cursor(), String{});
}

TEST(CASNamespaceJanitorPipeline, OutstandingJobsNeverExceedConcurrency)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 402), 3000);
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, f.backend->bulkDeleteKeyLimit()));

    EXPECT_LE(f.backend->bulkInFlightMax(), 4u);
    EXPECT_GE(result.jobs_in_flight_max, 1u);
    EXPECT_LE(result.jobs_in_flight_max, 4u);
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 3000), 0u);
}

TEST(CASNamespaceJanitorPipeline, NoBatchCapabilitySendsOneKeyRequestsWithoutProbe)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 403), 1000);
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, f.backend->bulkDeleteKeyLimit()));

    const auto calls = f.backend->bulkCalls();
    ASSERT_EQ(calls.size(), 1000u);
    for (const auto & call : calls)
    {
        EXPECT_EQ(call.keys.size(), 1u);
        EXPECT_NE(call.thread, std::this_thread::get_id());
    }
    EXPECT_TRUE(f.backend->exactRemoves().empty());
    EXPECT_EQ(result.batch_keys, 1u);
    EXPECT_EQ(result.delete_jobs, 1000u);
    EXPECT_EQ(result.batches_held, 0u);
    EXPECT_EQ(f.backend->callSizes(), f.backend->requestSizes()) << "no locally refused multi-key call";
}

TEST(CASNamespaceJanitorPipeline, HeldPageStopsCursorButLaterDeletesStand)
{
    JanitorFixture f;
    CountdownForTest pages_one_and_three(2);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 404), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() != keys[1000])
            return;
        pages_one_and_three.wait();
        throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    f.backend->after_bulk = [&](const JanitorBackend::Keys & batch)
    {
        if (batch.front().str() == keys[0] || batch.front().str() == keys[2000])
            pages_one_and_three.countDown();
    };
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1000));

    EXPECT_EQ(countPresent(*f.backend, keys, 0, 1000), 0u);
    EXPECT_EQ(countPresent(*f.backend, keys, 1000, 2000), 1000u);
    EXPECT_EQ(countPresent(*f.backend, keys, 2000, 3000), 0u) << "a later page's deletes stand";
    EXPECT_EQ(f.cursor(), keys[999]);
    EXPECT_EQ(result.batches_held, 1u);
    EXPECT_EQ(f.backend->namespaceLists(), 3u);
}

TEST(CASNamespaceJanitorPipeline, SoftBudgetStopsNewPagesInFlightJobsComplete)
{
    JanitorFixture f;
    ManualBarrier final_wait;
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 405), 3000);
    f.backend->after_namespace_list = [&](size_t, JanitorBackend::Access &) { f.clock.now += 2'000; };
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0])
            final_wait.arriveAndWait();
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = parallel(f, *pool, 1000);
    context.budget_ms = 1'000;
    context.on_final_wait_for_test = [&] { final_wait.release(); };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_TRUE(result.budget_exhausted);
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 1000), 0u) << "the in-flight batch completes after the budget";
    EXPECT_EQ(f.cursor(), keys[999]);
}

TEST(CASNamespaceJanitorPipeline, ParallelPathAuthorityLostMidPageHolds)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    std::atomic<bool> held{true};
    std::atomic<size_t> calls{0};
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 406), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys &, JanitorBackend::Access &)
    {
        if (++calls == 100)
            held = false;
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = parallel(f, *pool, 1);
    context.liveness = [&] { return held.load(); };

    const NamespaceJanitorResult result = f.run(context);

    const size_t absent = 3000 - countPresent(*f.backend, keys, 0, 3000);
    EXPECT_EQ(absent, result.deleted);
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 1000) + result.deleted, 1000u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_GE(result.batches_held + result.delete_jobs_skipped, 1u);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitorPipeline, ParallelPathLocalFailureStopsSubmission)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 407), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[10])
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1));

    EXPECT_GE(result.batches_held, 1u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.delete_jobs, result.batches + result.delete_jobs_skipped) << "a job either sent or found the stop flag";
    EXPECT_EQ(result.batches, f.backend->bulkCalls().size());
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
}

TEST(CASNamespaceJanitorPipeline, ParallelPathScheduleRefusalHolds)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 408), 1000);
    auto pool = makeJobPool(4);
    std::optional<size_t> refuse_at = 10;
    JanitorRunContext context = parallel(f, *pool, 1);
    context.schedule_refuse_at_for_test = &refuse_at;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_FALSE(refuse_at.has_value()) << "the seam resets once it fires";
    EXPECT_EQ(result.delete_jobs, 10u);
    EXPECT_EQ(result.batches + result.delete_jobs_skipped, 10u);
    EXPECT_EQ(f.backend->bulkCalls().size(), result.batches);
    EXPECT_EQ(result.deleted, result.batches);
    EXPECT_EQ(1000 - countPresent(*f.backend, keys, 0, 1000), result.batches);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.batches_held, 1u) << "the refusal";
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitorPipeline, LivenessRefusalIsLatchedForThePhase)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    std::atomic<std::thread::id> target{};
    std::atomic<bool> fired{false};
    std::atomic<size_t> samples_after{0};
    (void)seedLogs(*f.backend, kLayout, life("dead", 409), 3000);
    auto pool = makeJobPool(4);
    JanitorRunContext context = parallel(f, *pool, 1);
    context.on_job_start_for_test = [&](size_t page, size_t job)
    {
        if (page == 0 && job == 5)
            target = std::this_thread::get_id();
    };
    /// False for exactly one sample: the first one job 5's own thread takes after it started.
    context.liveness = [&]
    {
        if (fired)
        {
            ++samples_after;
            return true;
        }
        if (std::this_thread::get_id() == target.load())
        {
            fired = true;
            return false;
        }
        return true;
    };

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_TRUE(fired.load());
    EXPECT_EQ(samples_after.load(), 0u) << "after a false the phase never samples the predicate again";
    EXPECT_GE(result.batches_held, 1u);
    EXPECT_EQ(result.leaked, 0u);
    EXPECT_EQ(result.batches + result.batches_held + result.delete_jobs_skipped, result.delete_jobs);
    EXPECT_EQ(f.backend->namespaceLists(), 1u);
    EXPECT_FALSE(f.cursor().has_value());
}

TEST(CASNamespaceJanitorPipeline, ZeroConcurrencyRunsInline)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 410), 2000);
    auto pool = makeJobPool(4);

    const NamespaceJanitorResult result = f.run(parallel(f, *pool, 1, /*concurrency=*/ 0));

    for (const auto & call : f.backend->bulkCalls())
        EXPECT_EQ(call.thread, std::this_thread::get_id());
    EXPECT_EQ(result.jobs_in_flight_max, 1u);
    EXPECT_EQ(countPresent(*f.backend, keys, 0, 2000), 0u);
}

namespace
{

/// Blocks the `gc/state` read of the janitor's second refresh -- the first one after its first LIST --
/// until the test releases it.
class RefreshGateBackend : public JanitorBackend
{
public:
    RefreshGateBackend(ManualBarrier & gate_, String gc_state_key_) : gate(gate_), gc_state_key(std::move(gc_state_key_)) {}

    std::optional<Raw> read(const String & key, TransportAccess & access) override
    {
        if (key == gc_state_key && namespaceLists() == 1 && !gated.exchange(true))
            gate.arriveAndWait();
        return JanitorBackend::read(key, access);
    }

private:
    ManualBarrier & gate;
    const String gc_state_key;
    std::atomic<bool> gated{false};
};

}

TEST(CASNamespaceJanitorIntegration, RefreshInFlightKeepsAuthority)
{
    ManualBarrier refresh_read;
    std::atomic<size_t> page_one_applied{0};
    std::atomic<bool> first_attempt_failed{false};
    std::vector<String> keys;
    std::map<String, UInt64> row;
    auto backend = std::make_shared<RefreshGateBackend>(refresh_read, Layout("p").gcStateKey());
    backend->setStorageLimit(500);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_io_concurrency = 4;
    /// Page 1's second job starts its admission inside the blocked refresh read.
    config.gc_janitor_job_start_hook_for_test = [&](size_t page, size_t job)
    {
        if (page == 0 && job == 1)
            refresh_read.waitUntilArrived();
    };
    auto store = Pool::open(backend, config);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    /// With the live checkpoint, 2,999 dead keys fill exactly three pages: page 1 has 999, cut 500 + 499.
    keys = seedLogs(*backend, layout, life("dead", 1), 2999);
    backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[0] && !first_attempt_failed.exchange(true))
        {
            refresh_read.waitUntilArrived();
            throw Poco::TimeoutException("injected throttle; the reissue is gated inside the refresh");
        }
    };
    backend->after_bulk = [&](const JanitorBackend::Keys & batch)
    {
        const String & first = batch.front().str();
        if ((first == keys[0] || first == keys[500]) && ++page_one_applied == 2)
            refresh_read.release();
    };
    Gc gc(store, UInt128{411});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);
    gc.setPhaseSink({});

    EXPECT_EQ(row["batches_held"], 0u) << "a refresh that stores false first would hold both page-1 jobs";
    EXPECT_EQ(row["delete_jobs_skipped"], 0u);
    EXPECT_EQ(row["janitor_deleted"], 2999u);
    for (const String & key : keys)
        EXPECT_FALSE(present(*backend, key)) << key;
    EXPECT_EQ(publishedCursor(store->openRequests(), layout), String{});
}

TEST(CASNamespaceJanitorIntegration, TeardownDuringPhaseHoldsWithoutLeaking)
{
    Pool * store_ptr = nullptr;
    std::map<String, UInt64> row;
    auto backend = std::make_shared<JanitorBackend>();
    backend->setStorageLimit(500);
    PoolConfig config{.pool_prefix = "p", .server_root_id = "test", .gc_fold_max_defer_rounds = 0};
    config.gc_io_concurrency = 4;
    config.gc_janitor_job_start_hook_for_test = [&](size_t page, size_t job)
    {
        if (page == 0 && job == 1)
            store_ptr->beginTeardown();
    };
    auto store = Pool::open(backend, config);
    store_ptr = store.get();
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 999);
    Gc gc(store, UInt128{412});
    gc.setPhaseSink([&](const GcPhaseRecord & record) { if (record.phase == "namespace_cleanup") row = record.metrics; });

    /// The round's later phases fail on the closed open plane; only the janitor's accounting is asserted.
    try
    {
        (void)runRegularRoundReclaiming(gc);
    }
    catch (const DB::Exception &)
    {
    }
    gc.setPhaseSink({});

    EXPECT_GE(row["batches_held"] + row["delete_jobs_skipped"], 1u);
    EXPECT_EQ(row["leaked"], 0u);
    CasOperation verify = store->mountRequests().admit();
    for (size_t i = 500; i < 999; ++i)
        EXPECT_TRUE(verify.head(keys[i], Retry::once()).has_value()) << "refused at admission: " << keys[i];
}
```

Append to `gtest_cas_namespace_janitor_s3.cpp`, before `#endif` (add `#include <Common/CurrentMetrics.h>`, `#include <Common/ThreadPool.h>` and the `CurrentMetrics` externs as in the pipeline file, and a file-local `makeJobPool` identical to the pipeline file's):

```cpp
TEST(CASNamespaceJanitorS3, ParallelPathLeaksOneFailingKeyAndAdvances)
{
    JanitorFixture f;
    f.backend->setBatchDeleteSupported(false);
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 421), 3000);
    f.backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[499])
            std::rethrow_exception(accessDenied());
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = f.context(1);
    context.io_pool = pool.get();
    context.io_concurrency = 4;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(result.leaked, 1u);
    EXPECT_EQ(result.batches_leaked, 1u);
    EXPECT_EQ(result.batches_held, 0u);
    for (size_t i = 0; i < keys.size(); ++i)
        EXPECT_EQ(present(*f.backend, keys[i]), i == 499) << keys[i];
    EXPECT_EQ(f.cursor(), String{});
    EXPECT_EQ(std::count_if(result.anomalies.begin(), result.anomalies.end(),
        [&](const String & anomaly) { return anomaly.find(keys[499]) != String::npos; }), 1) << "one warning per failed job";
}

TEST(CASNamespaceJanitorS3, UnknownCapabilityConcurrentProbesAllStop)
{
    S3PathFixture f(DB::S3Capabilities{});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 422), 3000);
    std::atomic<size_t> rejected{0};
    f.respond = [&](const Poco::Net::HTTPServerRequest & request, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        if (request.getMethod() == "POST")
        {
            ++rejected;
            sendBatchNotImplemented(response);
            return;
        }
        acceptAll(*f.deleted, request, body, response);
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = f.context();
    context.io_pool = pool.get();
    context.io_concurrency = 4;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_GE(rejected.load(), 1u);
    EXPECT_LE(rejected.load(), 4u);
    EXPECT_EQ(f.server.countMethod("DELETE"), 0u);
    EXPECT_TRUE(result.capability_learned);
    EXPECT_FALSE(f.cursor().has_value());
    EXPECT_EQ(f.backend->bulkDeleteKeyLimit(), 1u) << "the storage remembers false";

    JanitorRunContext next = f.context();
    next.io_pool = pool.get();
    next.io_concurrency = 4;
    const NamespaceJanitorResult drained = f.run(next);
    EXPECT_EQ(drained.batch_keys, 1u);
    EXPECT_EQ(drained.deleted, 3000u);
    EXPECT_EQ(f.server.countMethod("DELETE"), 3000u);
}

TEST(CASNamespaceJanitorS3, ParallelPathRequestCountOnNoBatchStore)
{
    S3PathFixture f(DB::S3Capabilities{false});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 423), 2500);
    std::atomic<bool> failed_once{false};
    f.respond = [&](const Poco::Net::HTTPServerRequest & request, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        if (keyOfDeleteObject(request) == keys[7] && !failed_once.exchange(true))
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError", "try again");
            return;
        }
        acceptAll(*f.deleted, request, body, response);
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = f.context();
    context.io_pool = pool.get();
    context.io_concurrency = 4;

    const NamespaceJanitorResult result = f.run(context);

    EXPECT_EQ(f.server.countMethod("DELETE"), 2501u) << "the store records the retried attempt";
    EXPECT_EQ(f.server.countMethod("POST"), 0u);
    EXPECT_EQ(result.delete_calls, 2500u) << "a retried attempt is not a new logical call";
    EXPECT_EQ(result.batches, 2500u);
    EXPECT_EQ(result.batch_keys, 1u);
    EXPECT_EQ(result.deleted, 2500u);
}

TEST(CASNamespaceJanitorS3, ParallelPathAtConcurrencyOneRunsInline)
{
    S3PathFixture f(DB::S3Capabilities{false});
    const std::vector<String> keys = seedLogs(*f.backend, kLayout, life("dead", 424), 2500);
    std::atomic<bool> failed_once{false};
    f.respond = [&](const Poco::Net::HTTPServerRequest & request, const std::string & body, Poco::Net::HTTPServerResponse & response)
    {
        if (keyOfDeleteObject(request) == keys[7] && !failed_once.exchange(true))
        {
            sendSingleDeleteError(response, Poco::Net::HTTPResponse::HTTP_INTERNAL_SERVER_ERROR, "InternalError", "try again");
            return;
        }
        acceptAll(*f.deleted, request, body, response);
    };
    auto pool = makeJobPool(4);
    JanitorRunContext context = f.context();
    context.io_pool = pool.get();
    context.io_concurrency = 1;

    const NamespaceJanitorResult result = f.run(context);

    for (const std::thread::id thread : f.backend->callThreads())
        EXPECT_EQ(thread, std::this_thread::get_id());
    const std::vector<String> events = f.backend->events();
    std::vector<String> deleted_in_order;
    std::vector<size_t> list_positions;
    for (size_t i = 0; i < events.size(); ++i)
    {
        if (events[i] == "list")
            list_positions.push_back(i);
        else
            deleted_in_order.push_back(events[i].substr(String("delete ").size()));
    }
    EXPECT_EQ(deleted_in_order, keys) << "submit order, one delete per key";
    ASSERT_EQ(list_positions.size(), 3u);
    EXPECT_EQ(list_positions[1], 1u + 1000u) << "page 1's deletes all precede page 2's LIST";
    EXPECT_EQ(list_positions[2], 2u + 2000u);
    EXPECT_EQ(result.delete_jobs, 2500u);
    EXPECT_EQ(result.batches, 2500u);
    EXPECT_EQ(result.delete_calls, 2500u);
    EXPECT_EQ(f.server.countMethod("DELETE"), 2501u);
}
```

Run cmake (new file) into `build/cmake_t6.log`.

- [ ] **Step 2: Build to verify the tests fail**

Build into `build/t6_red.log`. Expected `NINJA_EXIT=1` on `io_pool`, `io_concurrency`, `schedule_refuse_at_for_test`, `on_final_wait_for_test`, `on_job_start_for_test`, `gc_janitor_job_start_hook_for_test`. After Step 3's header change only (fields added, `run` unchanged), build and run `CASNamespaceJanitorPipeline*:CASNamespaceJanitorS3*:CASNamespaceJanitorIntegration.RefreshInFlight*` into `build/t6_red_assert.log`, and record the assertion failures (calls on the caller's thread, `jobs_in_flight_max` never above 1, the pipeline barrier timing out, `RefreshInFlightKeepsAuthority` holding both jobs).

- [ ] **Step 3: Implement**

`setThreadName.h`, after `CAS_GC_REDELETE`:

```cpp
    M(CAS_GC_JANITOR, "CasGcJanitor") \
```

`CasNamespaceJanitor.h`: add `#include <Common/ThreadPool_fwd.h>` and `#include <optional>`; append to `JanitorRunContext` after `now_ms`:

```cpp
    /// Delete jobs run here with at most `io_concurrency` outstanding; null or a concurrency of at most 1
    /// runs them inline on the caller's thread, in submit order.
    ThreadPool * io_pool = nullptr;
    size_t io_concurrency = 1;
    /// TEST SEAM: the enqueue at this index within the phase throws; reset once it fires.
    std::optional<size_t> * schedule_refuse_at_for_test = nullptr;
    /// TEST SEAM: runs on the caller's thread right before the final wait for jobs.
    std::function<void()> on_final_wait_for_test;
    /// TEST SEAM: runs in each job before its stop-flag check and its admission.
    std::function<void(size_t page, size_t job)> on_job_start_for_test;
```

Also extend the `run` comment: "Jobs share a phase-wide stop flag: a job that holds sets it, a job that finds it set sends nothing, and the caller stops submitting. The first `false` the liveness predicate returns is final for the phase."

`CasNamespaceJanitor.cpp`: add includes `<Common/ThreadPool.h>`, `<Common/scope_guard_safe.h>`, `<Common/setThreadName.h>`, `<Common/threadPoolCallbackRunner.h>`, `<condition_variable>`, `<mutex>`; `namespace DB::ErrorCodes { extern const int CANNOT_SCHEDULE_TASK; extern const int NOT_IMPLEMENTED; }`. Add to the anonymous namespace:

```cpp
/// Bounds outstanding jobs: the caller takes a slot before each submit and a job returns it last. The pool
/// queue stays unbounded, so without it the caller would list page after page ahead of a pool that drains
/// `io_concurrency` jobs at a time.
class JobWindow
{
public:
    explicit JobWindow(size_t limit_) : limit(limit_) {}

    void acquire()
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [this] { return outstanding < limit; });
        ++outstanding;
        max_outstanding = std::max(max_outstanding, outstanding);
    }

    void release()
    {
        {
            std::lock_guard lock(mutex);
            --outstanding;
        }
        cv.notify_one();
    }

    size_t maxOutstanding() const
    {
        std::lock_guard lock(mutex);
        return max_outstanding;
    }

private:
    const size_t limit;
    mutable std::mutex mutex;
    std::condition_variable cv;
    size_t outstanding = 0;
    size_t max_outstanding = 0;
};
```

In `run`:

1. After `out.batch_keys = batch_keys;` add the latch and use it everywhere `context.liveness` was used (`requests.admit(...)` and `runJob(...)`):

```cpp
    /// Jobs sample authority while the caller refreshes it; the first `false` any request sees is final
    /// for the phase, so no job sees authority come back.
    const auto authority_lost = std::make_shared<std::atomic<bool>>(false);
    const Liveness liveness = [authority_lost, inner = context.liveness]
    {
        if (authority_lost->load())
            return false;
        if (inner && !inner())
        {
            authority_lost->store(true);
            return false;
        }
        return true;
    };
```

2. Replace the `submit` lambda and add the pool machinery right after `const uint64_t generation = op.generation();`:

```cpp
    const bool parallel = context.io_pool && context.io_concurrency > 1;
    JobWindow window(parallel ? context.io_concurrency : 1);
    std::unique_ptr<ThreadPoolCallbackRunnerLocal<void>> runner;
    if (parallel)
        runner = std::make_unique<ThreadPoolCallbackRunnerLocal<void>>(*context.io_pool, ThreadName::CAS_GC_JANITOR);
    std::vector<std::shared_ptr<ThreadPoolCallbackRunnerLocal<void>::Task>> handles;
    std::vector<JobSlot *> handle_slots;
    /// Every exit waits for every job: jobs reference `slots`, `stop` and `window`.
    SCOPE_EXIT_SAFE({ ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles); });
    size_t enqueued = 0;

    const auto submit = [&](size_t page_index, size_t job_index, std::vector<WriteOnceKey> keys)
    {
        if (stop.load())
            return false;
        JobSlot & slot = slots.emplace_back();
        slot.page = page_index;
        slot.keys = std::move(keys);
        const auto job = [&context, &requests = requests, &liveness, &stop, generation, slot_ptr = &slot, page_index, job_index]
        {
            if (context.on_job_start_for_test)
                context.on_job_start_for_test(page_index, job_index);
            runJob(*slot_ptr, requests, generation, liveness, stop);
        };
        if (!parallel)
        {
            ++out.delete_jobs;
            job();
            return true;
        }
        window.acquire();
        try
        {
            if (context.schedule_refuse_at_for_test && *context.schedule_refuse_at_for_test == enqueued)
            {
                context.schedule_refuse_at_for_test->reset();
                throw Exception(ErrorCodes::CANNOT_SCHEDULE_TASK, "Injected CAS namespace janitor enqueue refusal at job {}", enqueued);
            }
            handles.push_back(runner->enqueueAndGiveOwnership([&window, job]
            {
                SCOPE_EXIT(window.release());
                job();
            }));
        }
        catch (...)
        {
            window.release();
            slot.outcome = JobOutcome::Held;
            slot.error = std::current_exception();
            stop.store(true);
            return false;
        }
        handle_slots.push_back(&slot);
        ++enqueued;
        ++out.delete_jobs;
        return true;
    };
```

3. The job loop passes the job index: before the loop `size_t job_index = 0;`, and the call becomes `submit(page_index, job_index++, std::vector<WriteOnceKey>(...))`.

4. Replace `out.jobs_in_flight_max = out.delete_jobs > 0 ? 1 : 0;` with the final wait:

```cpp
    if (context.on_final_wait_for_test)
        context.on_final_wait_for_test();
    Stopwatch drain_watch;
    ThreadPoolCallbackRunnerLocal<void>::waitForAllToFinish(handles);
    out.drain_wait_ms = drain_watch.elapsedMilliseconds();
    for (size_t i = 0; i < handles.size(); ++i)
    {
        try
        {
            handles[i]->future.get();
        }
        catch (...)
        {
            handle_slots[i]->outcome = JobOutcome::Held;
            handle_slots[i]->error = std::current_exception();
        }
    }
    out.jobs_in_flight_max = parallel ? window.maxOutstanding() : (out.delete_jobs > 0 ? 1 : 0);
```

`CasPool.h`, after `gc_janitor_phase_budget_ms_for_test`:

```cpp
    /// TEST SEAM: runs in each namespace-janitor delete job before its stop-flag check and admission.
    std::function<void(size_t page, size_t job)> gc_janitor_job_start_hook_for_test = {};
```

`CasGc.h`: `bool authority_held = false;` becomes `std::atomic<bool> authority_held{false};` and its comment gains: "Atomic because namespace-janitor jobs read it while the round thread refreshes it." If `Gc` was movable or copyable anywhere, the build says so; `Gc` owns a `ThreadPool`, so it is not.

`CasGc.cpp` `refreshAuthority`:

```cpp
void Gc::refreshAuthority(uint64_t admitted_generation)
{
    /// Janitor jobs read the flag during this read, so it keeps the confirmed value until one verdict is
    /// stored at the end; every failure still stores `false`.
    bool held = false;
    try
    {
        CasOperation op = store->openRequests().admit();
        if (const auto got = op.read(store->layout().gcStateKey(), Retry::standard()))
        {
            const GcState current = decodeGcState(got->bytes);
            held = current.lease.owner == gc_id && current.lease.seq == admitted_generation;
        }
    }
    catch (...)
    {
        tryLogCurrentException(logger,
            "CAS gc: the leader-authority probe failed; this round's destructive operations treat the "
            "lease as lost");
    }
    authority_held.store(held);
}
```

`CasGc.cpp` `runNamespaceJanitor`: the liveness lambda becomes `[this] { return authority_held.load(); }` and add, before the `NamespaceJanitor(...)` call:

```cpp
        context.io_pool = io_pool.get();
        context.io_concurrency = static_cast<size_t>(config.gc_io_concurrency);
        context.schedule_refuse_at_for_test = &io_pool_refuse_at_for_test;
        context.on_job_start_for_test = config.gc_janitor_job_start_hook_for_test;
```

Update the `io_pool` member comment in `CasGc.h` to list "the namespace-janitor delete jobs" among its users.

- [ ] **Step 4: Build and run**

Build into `build/t6_build.log`; run `CASNamespaceJanitor*` into `build/t6_green.log`, then the same filter with `--gtest_repeat=20` into `build/t6_repeat.log`. Expected `GATE_EXIT=0` for both (the repeat is the interleaving check; a single flake is a red to root-cause, not to rerun).

- [ ] **Step 5: Gate**

`CAS*` into `build/t6_gate.log`; expected `GATE_EXIT=0`.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
C=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
git add -- $C/Gc/CasNamespaceJanitor.h $C/Gc/CasNamespaceJanitor.cpp $C/Gc/CasGc.h $C/Gc/CasGc.cpp $C/Pool/CasPool.h \
  src/Common/setThreadName.h src/Disks/tests/gtest_cas_namespace_janitor_pipeline.cpp src/Disks/tests/gtest_cas_namespace_janitor_s3.cpp
git commit -s -m "cas: run janitor deletes on the GC I/O pool while listing the next page"
```

---

### Task 7: ProfileEvents, setting descriptions and docs

**Files:**
- Modify: `src/Common/ProfileEvents.cpp:~930` (two events)
- Modify: `.../Gc/CasGc.cpp` (extern + increments in `runNamespaceJanitor`)
- Modify: `.../ContentAddressed/ContentAddressedSettings.cpp:82-83`
- Modify: `docs/en/antalya/cas/architecture/garbage-collection.md` (phase table row 16 ~:60, text ~:67, :87, :232, section `## Phase 16` ~:519-541, `## What a round costs` ~:756)
- Modify: `docs/en/operations/system-tables/cas_gc_log.md:89` (`namespace_cleanup` row) and the `manifest_deletes`/`ref_object_cleanup` rows
- Modify: `docs/en/antalya/cas/configuration.md:109` and `:182`
- Test: `src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp`

**Interfaces:**
- Consumes: `NamespaceJanitorResult` (Tasks 4–6).
- Produces: `ProfileEvents::CASGCNamespaceCleanupObjectsDeleted`, `ProfileEvents::CASGCNamespaceCleanupBatchFailures`.

- [ ] **Step 1: Write the failing test**

Append to `gtest_cas_namespace_janitor_batches.cpp` (add `namespace ProfileEvents { extern const Event CASGCNamespaceCleanupObjectsDeleted; extern const Event CASGCNamespaceCleanupBatchFailures; extern const Event CASGCNamespaceCleanupLeaks; }` and `#include <Common/ProfileEvents.h>`):

```cpp
TEST(CASNamespaceJanitorIntegration, JanitorEventsCountDeletedKeysAndFailedBatches)
{
    auto backend = std::make_shared<JanitorBackend>();
    backend->setStorageLimit(500);
    auto store = openPoolForTest(backend, 0);
    const Layout & layout = store->layout();
    (void)seedLiveNamespace(*backend, layout);
    const std::vector<String> keys = seedLogs(*backend, layout, life("dead", 1), 999);
    /// A local failure holds the second batch at once; a transient one would retry for the real 90 s window.
    backend->before_bulk = [&](const JanitorBackend::Keys & batch, JanitorBackend::Access &)
    {
        if (batch.front().str() == keys[500])
            throw DB::Exception(DB::ErrorCodes::CORRUPTED_DATA, "injected local failure");
    };
    const auto counter = [](ProfileEvents::Event event) { return ProfileEvents::global_counters[event].load(); };
    const uint64_t deleted_before = counter(ProfileEvents::CASGCNamespaceCleanupObjectsDeleted);
    const uint64_t failures_before = counter(ProfileEvents::CASGCNamespaceCleanupBatchFailures);
    const uint64_t leaks_before = counter(ProfileEvents::CASGCNamespaceCleanupLeaks);
    Gc gc(store, UInt128{501});

    ASSERT_TRUE(runRegularRoundReclaiming(gc).acquired_lease);

    EXPECT_EQ(counter(ProfileEvents::CASGCNamespaceCleanupObjectsDeleted) - deleted_before, 500u);
    EXPECT_EQ(counter(ProfileEvents::CASGCNamespaceCleanupBatchFailures) - failures_before, 1u) << "held jobs count too";
    EXPECT_EQ(counter(ProfileEvents::CASGCNamespaceCleanupLeaks) - leaks_before, 0u) << "a held batch leaks nothing";
}
```

The global counters are process-wide; this test runs alone in its round, so the deltas are its own (gtest runs tests sequentially).

- [ ] **Step 2: Build to verify it fails**

Build into `build/t7_red.log`: `NINJA_EXIT=1` on the two undefined events (the `extern` resolves at link time: expect an undefined-symbol link error).

- [ ] **Step 3: Implement**

`ProfileEvents.cpp`, after `CASGCNamespaceCleanupLeaks`:

```cpp
    M(CASGCNamespaceCleanupObjectsDeleted, "Number of dead-life namespace objects the perpetual janitor deleted. Includes keys that were already absent, since a batch delete of write-once keys cannot tell the two apart.", ValueType::Number) \
    M(CASGCNamespaceCleanupBatchFailures, "Number of namespace-janitor delete jobs that failed: leaked (their keys are retried on the next pass) or held (the cursor stops before their page).", ValueType::Number) \
```

Also extend the `CASGCNamespaceCleanupLeaks` description: "because HEAD, exact delete or a batch delete failed". In `CasGc.cpp`, declare both in the `ProfileEvents` extern block and, in `runNamespaceJanitor` after the leak increment:

```cpp
    if (result.deleted)
        ProfileEvents::increment(ProfileEvents::CASGCNamespaceCleanupObjectsDeleted, result.deleted);
    if (const uint64_t failed = result.batches_leaked + result.batches_held)
        ProfileEvents::increment(ProfileEvents::CASGCNamespaceCleanupBatchFailures, failed);
```

`ContentAddressedSettings.cpp`:

```cpp
    DECLARE(UInt64, gc_io_concurrency, 16, "Maximum number of threads in the GC I/O pool. Used for fold and rebuild read-ahead, orphan-manifest sweep planning reads, pending_deletes HEAD plus conditional DELETE, and namespace janitor deletes of dead ref logs and snapshots. Per-hash meta writes use gc_meta_pool_size; other GC requests run on the round thread. 1 disables parallel GC I/O", 0) \
    DECLARE(UInt64, gc_bulk_delete_chunk_keys, 1000, "Keys per batch delete request in GC's write-once families (owner-removed manifest bodies, covered ref logs and snapshots, dead-life ref logs and snapshots of the namespace janitor), capped by the storage's batch-delete limit; 1 to 1000", 0) \
```

Docs, `garbage-collection.md`:
- phase table row 16: `| 16 | \`namespace_cleanup\` | leader; one suppressed page on \`DEFER\` | The perpetual namespace janitor: pages of dead-life debris under a 20 s soft budget |`;
- `grep -n "one bounded page\|janitor page\|one suppressed\|namespace-janitor page" docs/en/antalya/cas/architecture/garbage-collection.md` and make each hit say that a folding round runs pages until the budget and a deferred round lists one suppressed page;
- replace the body of `## Phase 16 — namespace cleanup {#phase-16-namespace-cleanup}` with:

```markdown
The perpetual namespace janitor deletes the physical objects of namespace lives no longer in the catalog
(dead-life debris), page by page from the durable `janitor_cursor`.

- **Runs on:** the fold path; on the deferred path, one suppressed page right after phase 4
- **Per page (round thread):** a `gc/state` read for authority, one `LIST` page (≤ 1000 keys) of `cas/ns/`,
  one ref-catalog read after the `LIST`, exact-token `DELETE`s of dead `_ckpt` and `_files`
- **Dead `_log` / `_snap` (GC I/O pool):** jobs of `cas_gc_bulk_delete_chunk_keys` keys, capped by the
  storage's batch-delete limit (`objects_chunk_size_to_delete` on S3); one-key jobs on a store without
  batch delete. At most `cas_gc_io_concurrency` jobs are outstanding; the next page is listed while they run
- **Budget:** 20 s, soft. No page starts after it; the page in progress and the jobs in flight finish
- **Stops early when:** a page had no dead-life debris, the pass ended, authority was lost, a job held, or
  a later page's read failed
- **Writes:** one `CAS` on the maintenance state at the end, with the cursor after the last complete page
- **Failed jobs:** an exhausted or refused delete leaks its keys and its page stays complete; the next pass
  retries them. Lost authority, a rejected batch delete (`NOT_IMPLEMENTED`) or a local bug holds the page:
  the cursor stops before it
- **Safety:** every deleted key belongs to a life absent from a catalog cut taken after its page's `LIST`;
  a re-created table writes under a fresh incarnation prefix, so its keys are never in a job
- **Fails the round if:** nothing — the phase is wrapped in a catch-all ("namespace janitor stopped this round")
- **Observability:** phase row `namespace_cleanup`, see
  [`system.cas_gc_log`](/operations/system-tables/cas_gc_log#per-phase-rows); events
  `CASGCNamespaceCleanupObjectsDeleted`, `CASGCNamespaceCleanupBatchFailures`, `CASGCNamespaceCleanupLeaks`

### Unversioned buckets {#phase-16-unversioned-buckets}

A batch delete without a token assumes an unversioned bucket, which the mount probe checks. If versioning
is turned on after mount, the batch reports success while noncurrent versions remain: an empty `LIST`
then proves the namespace drained, not that storage was reclaimed.
```

`cas_gc_log.md`: the `namespace_cleanup` row becomes:

```markdown
| `namespace_cleanup` | The perpetual dead-life janitor: pages of `cas/ns/` under a 20 s soft budget. `phase_metrics`: `janitor_pages`, `janitor_keys`, `janitor_deleted` (keys of succeeded jobs, absent ones included), `leaked`, `batches` (jobs that sent a request), `batches_leaked`, `batches_held`, `delete_jobs`, `delete_jobs_skipped`, `jobs_in_flight_max`, `batch_keys`, `delete_calls`, `capability_learned`, `budget_exhausted`, `list_requests_process` (process-wide `DiskS3ListObjects` delta, the janitor's own only on an idle server), `list_ms`, `catalog_ms`, `delete_ms`, `drain_wait_ms`, `cursor_start_fp` and `cursor_end_fp` (`sipHash64` of the cursor, 0 when empty), `cursor_advanced`. The same `cursor_start_fp` with `cursor_advanced` 0 on every folding round is a stuck page. Delete jobs run on the GC I/O pool, so this row's `ProfileEvents` do not include their requests. | per page: one `gc/state` `GET`, one namespace-root page `LIST`, one catalog `GET`, exact-token deletes; one batch or one-key `DELETE` per job; one maintenance-state `CAS` per phase |
```

and add `capability_learned` and `unsent` to the `manifest_deletes` description, `capability_learned` to `ref_object_cleanup`, with one sentence each: "`capability_learned` is 1 when the storage rejected a batch delete; the family stops for the round". Correct the `manifest_deletes` and `ref_object_cleanup` request columns if they still say "one `DELETE` per body" / "one `HEAD` + one `DELETE` per deletable object": both send one request per `cas_gc_bulk_delete_chunk_keys` keys, capped by the storage's batch limit.

`configuration.md:182` (`cas_gc_bulk_delete_chunk_keys` row, last cell): "Keys per batch delete request in GC's write-once families: owner-removed manifest bodies, covered ref logs and snapshots, and dead-life ref logs and snapshots of the namespace janitor. Capped by the storage's batch-delete limit (`objects_chunk_size_to_delete` on S3, where 0 acts as 1); a storage without batch delete gets one-key requests". `:109` (`cas_gc_io_concurrency`): add "the namespace janitor's dead-life deletes" to the covered list.

Every new header added under `docs/` carries an explicit `{#anchor}`.

- [ ] **Step 4: Build and run**

Build into `build/t7_build.log`; run `CASNamespaceJanitorIntegration*` into `build/t7_green.log`; expected `GATE_EXIT=0`.

- [ ] **Step 5: Gate**

`CAS*` into `build/t7_gate.log`; expected `GATE_EXIT=0`.

- [ ] **Step 6: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
C=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
git add -- src/Common/ProfileEvents.cpp $C/Gc/CasGc.cpp $C/ContentAddressedSettings.cpp \
  docs/en/antalya/cas/architecture/garbage-collection.md docs/en/operations/system-tables/cas_gc_log.md \
  docs/en/antalya/cas/configuration.md src/Disks/tests/gtest_cas_namespace_janitor_batches.cpp
git commit -s -m "cas: janitor delete events, setting descriptions and docs for batched phases"
```

---

### Task 8: Integration test on RustFS: a dropped table drains in one folding round

**Files:**
- Create: `tests/integration/test_cas_janitor_drain/__init__.py` (empty), `tests/integration/test_cas_janitor_drain/configs/storage_conf.xml`, `tests/integration/test_cas_janitor_drain/test.py`

**Interfaces:**
- Consumes: the `namespace_cleanup` row fields (Tasks 4–7).

- [ ] **Step 1: Write the test**

`configs/storage_conf.xml`: three CAS disks on one RustFS bucket, distinct prefixes and server-root ids, rounds driven by `SYSTEM CAS GC RUN`:

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <cas_batch>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <cas_server_root_id>itest-janitor-batch</cas_server_root_id>
                <endpoint>http://rustfs1:11121/test/cas_janitor_batch/</endpoint>
                <access_key_id>clickhouse</access_key_id>
                <secret_access_key>clickhouse</secret_access_key>
                <cas_gc_enabled>1</cas_gc_enabled>
                <cas_gc_interval_sec>86400</cas_gc_interval_sec>
                <!-- One key per round leaves the dropped table's stream to the janitor. -->
                <cas_gc_round_ref_cleanup_budget>1</cas_gc_round_ref_cleanup_budget>
            </cas_batch>
            <cas_small_chunk>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <cas_server_root_id>itest-janitor-small-chunk</cas_server_root_id>
                <endpoint>http://rustfs1:11121/test/cas_janitor_small_chunk/</endpoint>
                <access_key_id>clickhouse</access_key_id>
                <secret_access_key>clickhouse</secret_access_key>
                <objects_chunk_size_to_delete>100</objects_chunk_size_to_delete>
                <cas_gc_enabled>1</cas_gc_enabled>
                <cas_gc_interval_sec>86400</cas_gc_interval_sec>
                <cas_gc_round_ref_cleanup_budget>1</cas_gc_round_ref_cleanup_budget>
            </cas_small_chunk>
            <cas_no_batch>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <cas_server_root_id>itest-janitor-no-batch</cas_server_root_id>
                <endpoint>http://rustfs1:11121/test/cas_janitor_no_batch/</endpoint>
                <access_key_id>clickhouse</access_key_id>
                <secret_access_key>clickhouse</secret_access_key>
                <support_batch_delete>false</support_batch_delete>
                <cas_gc_enabled>1</cas_gc_enabled>
                <cas_gc_interval_sec>86400</cas_gc_interval_sec>
                <cas_gc_round_ref_cleanup_budget>1</cas_gc_round_ref_cleanup_budget>
            </cas_no_batch>
        </disks>
        <policies>
            <cas_batch><volumes><main><disk>cas_batch</disk></main></volumes></cas_batch>
            <cas_small_chunk><volumes><main><disk>cas_small_chunk</disk></main></volumes></cas_small_chunk>
            <cas_no_batch><volumes><main><disk>cas_no_batch</disk></main></volumes></cas_no_batch>
        </policies>
    </storage_configuration>
</clickhouse>
```

`test.py`:

```python
"""
A dropped table's ref stream drains in one folding round of the namespace janitor on RustFS: batches of
the storage's size on a batch store, one-key jobs on a store without batch delete.
"""
import pytest

from helpers.cluster import ClickHouseCluster

cluster = ClickHouseCluster(__file__)

INSERTS = 2500
MAX_ROUNDS = 3


@pytest.fixture(scope="module", autouse=True)
def start_cluster():
    cluster.add_instance(
        "node",
        main_configs=["configs/storage_conf.xml"],
        with_rustfs=True,
        stay_alive=True,
    )
    try:
        cluster.start()
        yield cluster
    finally:
        cluster.shutdown()


def stream_keys(disk):
    prefix = f"cas_janitor_{disk.removeprefix('cas_')}/cas/ns/stream/"
    return [
        o.object_name
        for o in cluster.rustfs_client.list_objects(cluster.rustfs_bucket, prefix, recursive=True)
        if "/_log/" in o.object_name or "/_snap/" in o.object_name
    ]


def seed_and_drop(node, disk):
    node.query(f"DROP TABLE IF EXISTS t_{disk} SYNC")
    node.query(
        f"CREATE TABLE t_{disk} (k UInt64) ENGINE = MergeTree ORDER BY k SETTINGS storage_policy = '{disk}'"
    )
    node.query(f"SYSTEM STOP MERGES t_{disk}")
    for i in range(INSERTS):
        node.query(f"INSERT INTO t_{disk} VALUES ({i})")
    assert len(stream_keys(disk)) >= INSERTS
    node.query(f"DROP TABLE t_{disk} SYNC")


def janitor_rows(node, disk, since):
    node.query("SYSTEM FLUSH LOGS")
    columns = [
        "janitor_deleted", "janitor_pages", "budget_exhausted", "batch_keys",
        "batches", "delete_jobs", "jobs_in_flight_max",
    ]
    select = ", ".join(f"phase_metrics['{c}']" for c in columns)
    out = node.query(
        f"SELECT {select} FROM system.cas_gc_log WHERE event_type = 'Phase' AND phase = 'namespace_cleanup' "
        f"AND disk_name = '{disk}' AND event_time_microseconds >= toDateTime64('{since}', 6) "
        "ORDER BY event_time_microseconds FORMAT TSV"
    )
    return [dict(zip(columns, map(int, line.split("\t")))) for line in out.strip().splitlines()]


def drain(node, disk):
    since = node.query("SELECT now64(6)").strip()
    seed_and_drop(node, disk)
    for _ in range(MAX_ROUNDS):
        node.query(f"SYSTEM CAS GC RUN {disk}")
        if not stream_keys(disk):
            break
    assert not stream_keys(disk), f"{disk}: dead ref stream keys left after {MAX_ROUNDS} rounds"
    deleting = [r for r in janitor_rows(node, disk, since) if r["janitor_deleted"] > 0]
    assert len(deleting) == 1, f"{disk}: expected one deleting round, got {deleting}"
    return deleting[0]


def test_batch_store_drains_in_one_round():
    row = drain(cluster.instances["node"], "cas_batch")
    assert row["janitor_pages"] >= 3
    assert row["budget_exhausted"] == 0


def test_storage_chunk_setting_sizes_batches():
    row = drain(cluster.instances["node"], "cas_small_chunk")
    assert row["batch_keys"] == 100
    assert row["batches"] >= 25


# The disks leave `cas_gc_io_concurrency` at its default.
GC_IO_CONCURRENCY = 16


def test_store_without_batch_delete_uses_parallel_one_key_jobs():
    row = drain(cluster.instances["node"], "cas_no_batch")
    assert row["batch_keys"] == 1
    assert row["delete_jobs"] >= INSERTS
    assert row["jobs_in_flight_max"] <= GC_IO_CONCURRENCY
```

Move the `GC_IO_CONCURRENCY` constant up next to `INSERTS` when writing the file.

If the SQL `SYSTEM CAS GC RUN <disk>` form differs in this tree, check `src/Parsers/ASTSystemQuery.cpp` for the CAS GC command and use its syntax. If the disk-level key for the S3 chunk setting is not `objects_chunk_size_to_delete`, the `batch_keys == 100` assertion fails with 1000: read `S3Settings::loadFromConfig` for the disk prefix and fix the XML key.

- [ ] **Step 2: Build the server and run the test**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build clickhouse > build/t8_clickhouse.log 2>&1; echo NINJA_EXIT=$? >> build/t8_clickhouse.log; tail -1 build/t8_clickhouse.log
mkdir -p ci/tmp && cp build/programs/clickhouse ci/tmp/clickhouse
python -m ci.praktika run "integration" --test test_cas_janitor_drain > build/t8_integration.log 2>&1; echo PRAKTIKA_EXIT=$? >> build/t8_integration.log
tail -5 build/t8_integration.log
```
Expected: `NINJA_EXIT=0`, three tests PASSED, `PRAKTIKA_EXIT=0`. Delegate reading the log to a subagent if it is long; report the per-test outcome and each deleting row.

- [ ] **Step 3: Verify it fails on base**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
cp tmp/ab/clickhouse-base-e2dd0f5d343 ci/tmp/clickhouse
python -m ci.praktika run "integration" --test test_cas_janitor_drain > build/t8_integration_base.log 2>&1; echo PRAKTIKA_EXIT=$? >> build/t8_integration_base.log
cp build/programs/clickhouse ci/tmp/clickhouse
```
Expected: FAIL on base with keys left after 3 rounds (base deletes one page per folding round). Record it in the commit body.

- [ ] **Step 4: Commit**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
git add -- tests/integration/test_cas_janitor_drain/__init__.py tests/integration/test_cas_janitor_drain/configs/storage_conf.xml \
  tests/integration/test_cas_janitor_drain/test.py
git commit -s -m "cas: integration test for a janitor drain in one folding round on RustFS"
```

---

### Task 9: A/B value proof on the local stand (M1, M3, M4; M2 reported)

**Files:**
- Create: `/home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value/gc-off-nobatch.xml`, `.../value/m4-run.sh`, `.../value/m4ref.py`, `.../value/ab-table.py`
- Modify: `/home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value/m1.py` (authority GET count, optional)
- Create: `/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/2026-10-01-cas-29-9-janitor-ab/README.md`

**Interfaces:**
- Consumes: base binary `/home/mfilimonov/workspace/ClickHouse/cas-83/tmp/ab/clickhouse-base-e2dd0f5d343` (Task 1 Step 0); after binary built from the branch tip.

Pass bars are the spec's [Value proof](#) tables (`## Value proof {#value-proof}` in the spec); copy each table into the report with a "measured" column. Runs alternate base and after (B, A, B, A, ...). Only one stand runs at a time: `stand.sh` refuses while any `ca-soak-*` container is up.

- [ ] **Step 1: Prepare binaries and scripts**

```bash
cd /home/mfilimonov/workspace/ClickHouse/cas-83
ninja -C build clickhouse > build/t9_clickhouse.log 2>&1; echo NINJA_EXIT=$? >> build/t9_clickhouse.log; tail -1 build/t9_clickhouse.log
cp build/programs/clickhouse tmp/ab/clickhouse-after-$(git rev-parse --short HEAD)
ls -la tmp/ab/
```

`tmp/gc-cuts-stand/value/gc-off-nobatch.xml`:

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <ca>
                <cas_gc_interval_sec>36000</cas_gc_interval_sec>
                <support_batch_delete>false</support_batch_delete>
            </ca>
        </disks>
    </storage_configuration>
</clickhouse>
```

`tmp/gc-cuts-stand/value/m4-run.sh` (copy of `m1-run.sh` with the no-batch config):

```bash
#!/bin/bash
# m4-run.sh LABEL BINARY TARGET [LAT]: M1's drain on a disk with support_batch_delete false.
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value
./stand.sh down >/dev/null 2>&1
./stand.sh up $2 $PWD/gc-off-nobatch.xml
python3 m1.py $1 $3 ${4:-0}
echo "M4RC=$? $1"
./stand.sh down > m4-$1.down.log 2>&1
```

In `m1.py`, after each round's `ph` row is read, add the authority GET count for M4 (a `Read` row per `gc/state` GET inside the phase window; `blob_storage_log` may not log reads, in which case the count is `None` and the report says "unmeasured"):

```python
    auth = None
    if ph:
        end = ph.get("event_time_microseconds")
        dur = int(ph.get("phase_duration_microseconds") or 0)
        try:
            auth = int(q(
                "SELECT count() FROM system.blob_storage_log WHERE event_type = 'Read' AND remote_path LIKE '%/gc/state' "
                f"AND event_time_microseconds BETWEEN toDateTime64('{end}', 6) - toIntervalMicrosecond({dur}) AND toDateTime64('{end}', 6)"
            ).strip())
        except subprocess.CalledProcessError:
            auth = None
```

and store `"ns_auth_gets": auth` in the round dict; add `event_time_microseconds` to the `namespace_cleanup` SELECT. Check first with `grep -n "Read" src/Interpreters/BlobStorageLog.h` (cas-83) whether a `Read` event type exists; if not, set `auth = None` without querying and report the per-page authority GET as unmeasured.

`tmp/gc-cuts-stand/value/m4ref.py` (M4 ref-cleanup comparison, stack must be up with `gc-off-nobatch.xml` and netem armed by the caller):

```python
#!/usr/bin/env python3
"""M4 ref-cleanup: single-row inserts until ref_object_cleanup deletes >= 2000 keys in a round, then its
authority GETs, delete requests and duration per phase. usage: m4ref.py LABEL"""
import json, subprocess, sys, time

label = sys.argv[1]
here = "/home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value"

def q(sql, fmt=None):
    cmd = ["docker", "exec", "ca-soak-ch1-1", "clickhouse-client", "-q", sql]
    if fmt:
        cmd += ["--format", fmt]
    return subprocess.check_output(cmd, text=True, timeout=1800)

q("DROP TABLE IF EXISTS r SYNC")
q("CREATE TABLE r (k UInt64) ENGINE = MergeTree ORDER BY k SETTINGS storage_policy = 'ca'")
rows = []
for batch in range(20):
    for i in range(500):
        q(f"INSERT INTO r VALUES ({batch * 500 + i})")
    q("SYSTEM CAS GC RUN ca")
    q("SYSTEM FLUSH LOGS")
    ph = q("SELECT event_time_microseconds, phase_duration_microseconds, phase_metrics FROM system.cas_gc_log "
           "WHERE event_type = 'Phase' AND phase = 'ref_object_cleanup' ORDER BY event_time_microseconds DESC LIMIT 1",
           "JSONEachRow").strip()
    if not ph:
        continue
    ph = json.loads(ph)
    end, dur = ph["event_time_microseconds"], int(ph["phase_duration_microseconds"])
    window = (f"event_time_microseconds BETWEEN toDateTime64('{end}', 6) - toIntervalMicrosecond({dur}) "
              f"AND toDateTime64('{end}', 6)")
    counts = json.loads(q(
        "SELECT countIf(event_type = 'Read' AND (remote_path LIKE '%/gc/state' OR remote_path LIKE '%/cas/ref_catalog')) AS auth_reads, "
        "countIf(event_type = 'Delete' AND remote_path LIKE '%/cas/ns/stream/%') AS ref_deletes "
        f"FROM system.blob_storage_log WHERE {window}", "JSONEachRow").strip())
    rows.append({"batch": batch, "duration_us": dur, "metrics": ph["phase_metrics"], **counts})
    print(label, rows[-1], flush=True)
    if counts["ref_deletes"] >= 2000:
        break
json.dump(rows, open(f"{here}/m4ref-{label}.json", "w"), indent=1)
print("DONE", label, len(rows))
```

`tmp/gc-cuts-stand/value/ab-table.py`: reads `m1-<label>.json` / `m4-...` files given on the command line and prints, per label: janitor-active rounds (rounds with `ns_metrics.janitor_deleted > 0`), drain wall (`drain_wall_s`), `DiskS3DeleteObjects` and `DiskS3ListObjects` over the drain (`drain_events`), per janitor-active round the phase time (`ns_us`), `janitor_pages`, `batches`, `delete_calls`, `batches_leaked`, `batches_held`, `budget_exhausted`, `jobs_in_flight_max`, `drain_wait_ms`, `list_ms`, `catalog_ms`, `delete_ms`, and per-page time (`ns_us / janitor_pages`); then per cell the median (min..max) per side. Write it with `json`, `statistics`, `sys` only; base rows lack the new metrics and print `-`.

- [ ] **Step 2: M1 — long-stream drain, no latency and 20 ms**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value
B=/home/mfilimonov/workspace/ClickHouse/cas-83/tmp/ab/clickhouse-base-e2dd0f5d343
A=$(ls /home/mfilimonov/workspace/ClickHouse/cas-83/tmp/ab/clickhouse-after-* | tail -1)
for i in 1 2 3; do
  for cell in "7000 0" "20000 0" "7000 20"; do
    set -- $cell
    ./m1-run.sh ab-base-$1-lat$2-$i $B $1 $2 > ab-base-$1-lat$2-$i.log 2>&1
    ./m1-run.sh ab-after-$1-lat$2-$i $A $1 $2 > ab-after-$1-lat$2-$i.log 2>&1
  done
done
for i in 1 2 3; do ./m1-run.sh ab-after-100000-lat0-$i $A 100000 0 > ab-after-100000-lat0-$i.log 2>&1; done
python3 ab-table.py m1-ab-*.json > ab-m1.md
```

Run it in the background (`nohup ... &` with a marker line at the end, per the harness notes) and check `M1RC=0` in every log. If seeding 100k keys takes more than 1 h, stop at 50k and say so in the report. Pass bars: the spec's M1 table. The **Fail** rule applies: janitor-active rounds on the 9-round cadence with `budget_exhausted` 0 means the loop did not continue; debug before re-measuring. Every after-run reports `list_ms`, `catalog_ms`, `delete_ms`, `drain_wait_ms` per page: that answers the spec's open question on report 08's per-page time; state which step dominates.

- [ ] **Step 3: M3 — S11 steady state, no regression**

```bash
export GC_AB_OUT=/home/mfilimonov/workspace/ClickHouse/master/tmp/gc-ab-cas299 && mkdir -p $GC_AB_OUT && cd $GC_AB_OUT
S=/home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak/gc-ab
for i in 1 2 3; do
  for side in before after; do
    BIN=$([ $side = before ] && echo $B || echo $A)
    $S/run-stand.sh run $side-s11ci-$i $BIN --scenario S11 --seed 20260930 --duration 300s --scale ci > $side-s11ci-$i.log 2>&1
    python3 $S/extract.py runs/$side-s11ci-$i > $side-s11ci-$i.txt
    $S/run-stand.sh down
  done
done
python3 $S/compare.py > COMPARISON.md
python3 $S/t2.py > JANITOR.md
```

Pass bars: on every folding round `janitor_pages` 1, `batches` 0, phase time within spread; process-wide `DiskS3ListObjects` per round (metric_log window, `extract.py`) after ≤ base; request mix per round unchanged.

- [ ] **Step 4: M4a — store without batch delete, 20 ms, base vs after**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value
chmod +x m4-run.sh
for i in 1 2; do
  ./m4-run.sh m4-base-20000-lat20-$i $B 20000 20 > m4-base-$i.log 2>&1
  ./m4-run.sh m4-after-20000-lat20-$i $A 20000 20 > m4-after-$i.log 2>&1
done
./m4-run.sh m4-base-20000-lat0-1 $B 20000 0 > m4-base-lat0.log 2>&1
./m4-run.sh m4-after-20000-lat0-1 $A 20000 0 > m4-after-lat0.log 2>&1
python3 ab-table.py m1-m4-*.json > ab-m4.md
```

A base run may stop after 5 janitor-active rounds (~30 rounds) once its one-page-per-round cadence shows: add a `MAX_JANITOR_ROUNDS` env check to `m1.py`'s loop (`break` when that many rounds had `janitor_deleted > 0`) and extrapolate the drain wall in the report. Pass bars: the spec's M4a table; if the stand's median one-key DELETE latency is above 20 ms, apply the spec's scaled bar and report the round count unjudged.

- [ ] **Step 5: M4 ref-cleanup comparison**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/tmp/gc-cuts-stand/value
for i in 1 2; do
  for side in base after; do
    BIN=$([ $side = base ] && echo $B || echo $A)
    ./stand.sh down >/dev/null 2>&1; ./stand.sh up $BIN $PWD/gc-off-nobatch.xml
    ./netem.sh arm 20
    python3 m4ref.py m4ref-$side-$i > m4ref-$side-$i.log 2>&1
    ./netem.sh disarm; ./stand.sh down > /dev/null 2>&1
  done
done
```

Pass bars: authority GETs per phase after = base = ⌈keys / 1,000⌉ each (catalog and `gc/state`), delete requests after = base ≈ keys, phase duration after ≤ base + spread. If `blob_storage_log` holds no `Read` rows, report the authority rows as unmeasured and compare the delete rows only.

- [ ] **Step 6: M2 — S30 create/drop churn (reported, not a pass bar)**

```bash
cd $GC_AB_OUT
for i in 1 2 3; do
  for side in before after; do
    BIN=$([ $side = before ] && echo $B || echo $A)
    $S/run-stand.sh run $side-s30-$i $BIN --scenario S30 --seed 20260930 --duration 300s --scale ci > $side-s30-$i.log 2>&1
    python3 $S/extract.py runs/$side-s30-$i > $side-s30-$i.txt
    $S/run-stand.sh down
  done
done
python3 $S/t2.py > S30-JANITOR.md
```

Report `defer_decision.dead_life_debris`, `ref_keys_listed`, leader round duration over the last third, and `janitor_pages` on folding rounds without debris. If base does not grow at S30's scale, say so.

- [ ] **Step 7: Write the report**

`/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/reports/2026-10-01-cas-29-9-janitor-ab/README.md`: decision first (pass or fail per cell against the spec's bars), then one section per measurement with the table (predicted / base / after / pass), the binaries' SHAs, run labels and log paths, and every deviation (skipped cell, scaled bar, unmeasured row) with its reason. Per the scenario-results convention, a results table has columns: № / description / result / artifacts / fix. Include the per-page time split that answers the spec's open question. Figures are re-read from the JSON files at write time, not recalled.

- [ ] **Step 8: Commit the report (master worktree)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
git add -- docs/superpowers/reports/2026-10-01-cas-29-9-janitor-ab/README.md
git commit -s -m "report(cas): CAS-29.9 janitor batches A/B on the local stand"
```

Scripts under `tmp/` stay uncommitted.

---

## Self-Review

**Spec coverage.** Decision bullets: pages on the round thread, jobs on `io_pool` (T4, T6); job = one storage request (T4, T6); ≤ `gc_io_concurrency` outstanding (T6); one wait, one publication (T4, T6); storage-sized batches (T1, T4); authority per cohort (T2, T4); best-effort budget (T4); capability read once, batch size fixed (T4 via `batch_keys` at phase start, T5); no per-key recovery (T2, T5); leak and advance (T5); adapter names (T3); deferred rounds unchanged (T4 `DeferredRoundTakesOneSuppressedPage`); concurrency 1 sequential (T6 `ParallelPathAtConcurrencyOneRunsInline`); no transient false (T6); proof before merge (T9). Observability table: every metric is a `NamespaceJanitorResult` field emitted in T4; events in T7. Settings section: T7. Tests section: every named test is in T1–T7 except `NonCanonicalStreamKeyUsesExactPath`, pinned at the layout level by `StreamKeyMintedFromAListedKeyMustBeThatKey` (T4, reason stated there); `RejectedPersistedCursorResetsExactlyAndDeletesNothing` is the existing `BackendRejectedCursorResetsExactlyAndDeletesNothing`, kept by the T4 migration; `RefCleanupReadsAuthorityOncePerCohort` and `ManifestCleanupSendsCohortAsStorageRequests` are scaled (11 and 10 keys in cohorts of 4 instead of 2,500 in cohorts of 1,000; same three-cohort shape). Integration section: T8. Value proof M1–M4 and M2: T9.

**Capability accessor.** The spec reads `isBatchDeleteSupported()` once per phase. The CAS side has no separate accessor: `bulkDeleteKeyLimit` already answers 1 when the capability is false and the storage limit when it is true or unknown, which is exactly the spec's capability table, so the phase fixes its batch size from it. `InstrumentedBackend` forwards it (T1).

**Type consistency.** `JanitorRunContext` fields are declared in T4 (`liveness, refresh_authority, batch_keys, budget_ms, now_ms`) and appended in T6 in the order the tests use them; tests set T6 fields by assignment, never by designated initializer. `removeCohortWriteOnce(op, cohort, request_keys, policy, on_request_done)` is the same in T2's header, body and tests. `bulkDeleteKeyLimit` (CAS `Backend`) vs `batchDeleteKeyLimit` (`IObjectStorage`) follow the spec's naming.

**Placeholder scan.** No TBD/TODO. Conditional instructions are decision rules with their check (`ScriptedS3Server` destructor, `SYSTEM CAS GC RUN` syntax, disk XML key, orphan sweep pacing, `blob_storage_log` read rows).

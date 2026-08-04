# CAS streaming conditional overwrite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the condemned-blob resurrection path write a body of any size without holding it in memory, and delete the memory-admission semaphore that exists only because it could not.

**Architecture:** Add `putOverwriteStream` to the `Backend` seam, symmetric to the existing `putIfAbsentStream` — same `WriteSink` return, same contract, `If-Match` instead of `If-None-Match`. Switch the local-source arm of the condemned displacement to it, then delete `initializeCondemnedUploadAdmission`, `ByteWeightedSemaphore`, and the `cas_condemned_upload_memory_bytes` server setting outright. GCS keeps its existing hard limit but starts refusing early and legibly.

**Tech Stack:** C++ (ClickHouse), gtest, `ninja`, ClickHouse's `WriteBufferFromS3` / `IObjectStorage::writeObject`.

**Spec:** `docs/superpowers/specs/2026-08-04-cas-streaming-conditional-overwrite-design.md`

## Global Constraints

- Allman braces (opening brace on its own line) — enforced by the CI style check.
- Every new gtest suite name MUST start with `CAS`. The CA gate filter is derived from suite names beginning with `Ca`/`CAS`; a suite that escapes the prefix escapes the gate. This has happened twice before.
- Never use `sleep` to resolve a race in C++ — use the existing test hooks.
- A test that asserts a `LOGICAL_ERROR` needs the `*DeathTest` suite split, otherwise it aborts the whole binary under sanitizers and hides every test after it.
- Comments carry the REASON, never a citation of this plan, the spec, a task number, or a BACKLOG anchor. Those documents get deleted from the branch; the code must read without them.
- No compatibility shim for the removed setting: this is pre-release with no persisted data. An accepted-and-ignored setting is worse than an absent one.
- Build: `ninja -C build clickhouse unit_tests_dbms`, output redirected to a log file under `build/`. Do not pass `-j`.
- Gate before any commit that touches backend or write-path code: release AND ASan. ASan carries 18 `*DeathTest` suites that do not exist in the release build.

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h` | The storage seam and its contract | Add `putOverwriteStream` declaration + doc comment |
| `.../Backend/CasObjectStorageBackend.{h,cpp}` | Native (S3/GCS) and EmulatedSingleProcess | Implement both; add the GCS size guard; new `EmulatedFileSink` |
| `.../Backend/CasInMemoryBackend.{h,cpp}` | Test backend | Implement via a buffered sink |
| `.../Backend/CasInstrumentedBackend.{h,cpp}` | Counting proxy | Delegate + count |
| `.../Pool/CasPartWriteTxn.cpp` | The condemned-displacement call site | Switch the local arm to the stream; drop the admission lock |
| `.../Pool/CasBlobUploadPool.{h,cpp}` | Upload pool + (today) the admission | Delete `ByteWeightedSemaphore` and the three admission functions |
| `src/Core/ServerSettings.cpp` | Server settings | Delete `cas_condemned_upload_memory_bytes` |
| `programs/server/Server.cpp`, `programs/local/LocalServer.cpp`, `programs/disks/DisksApp.cpp` | Startup/shutdown | Remove the init/shutdown calls |
| `src/Disks/tests/cas_test_helpers.h` | Shared test helpers | Remove `ensureCondemnedUploadAdmissionForTest` |
| `src/Disks/tests/gtest_cas_backend_contract.cpp` | Seam contract tests | New streaming-overwrite cases |
| `src/Disks/tests/gtest_cas_backend_generation.cpp` | GCS dialect tests | New guard test |
| `src/Disks/tests/gtest_cas_upload_fanout.cpp` | Fan-out behaviour | Delete 2 admission tests; add the no-materialization test |
| `src/Disks/tests/gtest_cas_blob_upload_pool_env.cpp` | Admission lifecycle env | Delete the file |
| `docs/en/antalya/cas/architecture/backend.md`, `architecture/blob-protocol.md`, `configuration.md`, `bucket-requirements.md` | User-facing docs | State the GCS size consequence |

---

## Task 1: The `putOverwriteStream` seam, all four backends

Adding a pure virtual to `Backend` breaks every implementation at once, so all four land together. The emulated implementation here is deliberately the SAME whole-body buffering that exists today — Task 3 replaces it. That keeps this task's diff about the seam.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h` (after the `putOverwrite` overloads, ~line 245)
- Modify: `.../Backend/CasObjectStorageBackend.{h,cpp}`
- Modify: `.../Backend/CasInMemoryBackend.{h,cpp}`
- Modify: `.../Backend/CasInstrumentedBackend.{h,cpp}`
- Test: `src/Disks/tests/gtest_cas_backend_contract.cpp`

**Interfaces:**
- Consumes: `WriteSink` (`buffer()`, `finalize() -> PutResult`, `cancel() noexcept`), `Token`, `ObjectMeta`, `PutResult`, `PutOutcome` — all existing in `CasBackend.h`.
- Produces: `virtual WriteSinkPtr Backend::putOverwriteStream(const String & key, const Token & expected, uint64_t declared_size, const ObjectMeta & meta)` plus the 3-argument convenience overload. Tasks 2, 3, 4 and 6 all use exactly this signature.

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_backend_contract.cpp`. These run against every backend the file already parameterizes; follow the file's existing fixture pattern for constructing `backend`.

```cpp
TEST(CASBackendContract, PutOverwriteStreamReplacesOnMatchingToken)
{
    auto backend = makeBackendForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/8);
    writeString(String("replaced"), sink->buffer());
    const PutResult res = sink->finalize();

    EXPECT_EQ(res.outcome, PutOutcome::Done);
    EXPECT_NE(res.token, created.token) << "a replacement is a new incarnation and must carry a new token";
    const auto got = backend->get("k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "replaced");
}

TEST(CASBackendContract, PutOverwriteStreamRefusesWrongTokenAndLeavesObjectIntact)
{
    auto backend = makeBackendForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    Token stale = created.token;
    stale.value += "-stale";

    auto sink = backend->putOverwriteStream("k", stale, /*declared_size=*/8);
    writeString(String("replaced"), sink->buffer());
    const PutResult res = sink->finalize();

    EXPECT_EQ(res.outcome, PutOutcome::PreconditionFailed);
    const auto got = backend->get("k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "original") << "a refused conditional write must leave the object untouched";
    EXPECT_EQ(got->token, created.token);
}

TEST(CASBackendContract, PutOverwriteStreamCancelPublishesNothing)
{
    auto backend = makeBackendForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    {
        auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/8);
        writeString(String("replaced"), sink->buffer());
        sink->cancel();
    }

    const auto got = backend->get("k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "original") << "cancel must publish nothing, exactly like putIfAbsentStream";
}

TEST(CASBackendContract, PutOverwriteStreamRefusesAbsentKey)
{
    auto backend = makeBackendForTest();
    auto sink = backend->putOverwriteStream("missing", Token{}, /*declared_size=*/4);
    writeString(String("body"), sink->buffer());
    EXPECT_EQ(sink->finalize().outcome, PutOutcome::PreconditionFailed);
    EXPECT_FALSE(backend->get("missing").has_value());
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
ninja -C build unit_tests_dbms > build/t1_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASBackendContract.PutOverwriteStream*'
```

Expected: compilation FAILS with "no member named 'putOverwriteStream'".

- [ ] **Step 3: Declare the seam**

In `CasBackend.h`, immediately after the existing `putOverwrite` convenience overload:

```cpp
    /// Streaming variant of putOverwrite. Same contract: replaces the current object only when its
    /// token equals `expected`; a mismatch leaves the object unchanged and `finalize()` reports
    /// `PreconditionFailed`. Blob bodies have no size cap, so any caller writing one MUST use this
    /// rather than the whole-`String` form -- that form holds the entire body in memory, which for a
    /// body larger than RAM is not a slow path, it is an impossible one.
    ///
    /// `declared_size` is the total byte count the caller will write, header included, and it is
    /// REQUIRED rather than advisory: a generation-token backend cannot perform a conditional write
    /// above its single-part cap AT ALL, and must refuse before the first byte leaves rather than
    /// after the whole body has been sent. Backends that do not need it ignore it. The sink does not
    /// police it against what is actually written -- the storage's own size accounting already covers
    /// a caller that lies, and a second check here would only add a second thing to keep in sync.
    virtual WriteSinkPtr putOverwriteStream(const String & key, const Token & expected,
                                            uint64_t declared_size, const ObjectMeta & meta) = 0;
    WriteSinkPtr putOverwriteStream(const String & key, const Token & expected, uint64_t declared_size)
    {
        return putOverwriteStream(key, expected, declared_size, {});
    }
```

- [ ] **Step 4: Implement it in `ObjectStorageBackend`**

Header (`CasObjectStorageBackend.h`), next to `putIfAbsentStream`:

```cpp
    WriteSinkPtr putOverwriteStream(const String & key, const Token & expected,
                                    uint64_t declared_size, const ObjectMeta & meta) override;
```

Implementation (`CasObjectStorageBackend.cpp`), directly after `putIfAbsentStream`. `NativeStreamingSink` needs no change: it never knew about the condition, which rides on the write buffer.

```cpp
WriteSinkPtr ObjectStorageBackend::putOverwriteStream(const String & key, const Token & expected,
                                                      uint64_t declared_size, const ObjectMeta & meta)
{
    /// A wrong-dialect expected token can never match, and letting it reach the wire would spend the
    /// whole body to learn that -- mirrors putOverwrite's own up-front check.
    if (!mintingTypeMatches(expected.type))
        return std::make_unique<RefusingSink>(PutOutcome::PreconditionFailed);

    if (mode == Mode::Native)
    {
        WriteSettings ws = conditionalWriteSettings();
        ws.object_storage_write_if_match = expected.value;
        std::optional<ObjectAttributes> attrs;
        if (!meta.empty())
            attrs.emplace(meta.begin(), meta.end());
        auto buf = object_storage->writeObject(
            StoredObject(key), WriteMode::Rewrite, attrs, DBMS_DEFAULT_BUFFER_SIZE, ws);
        return std::make_unique<NativeStreamingSink>(*this, key, std::move(buf));
    }

    return std::make_unique<EmulatedOverwriteBufferedSink>(*this, key, expected, meta);
}
```

Add the two small sinks in the same anonymous namespace as `NativeStreamingSink`:

```cpp
/// A sink that accepts writes and refuses at finalize. It exists so a rejection decided BEFORE any
/// byte is written still reaches the caller through the ordinary `finalize()` result, instead of
/// forcing every call site to handle a null sink.
class RefusingSink final : public WriteSink
{
public:
    explicit RefusingSink(PutOutcome outcome_) : outcome(outcome_) { }

    WriteBuffer & buffer() override { return sink_buf; }
    PutResult finalize() override
    {
        chassert(!done);
        done = true;
        sink_buf.cancel();
        return {outcome, {}};
    }
    void cancel() noexcept override
    {
        done = true;
        sink_buf.cancel();
    }
    ~RefusingSink() override
    {
        if (!done)
            cancel();
    }

private:
    const PutOutcome outcome;
    WriteBufferFromOwnString sink_buf;
    bool done = false;
};

/// EmulatedSingleProcess counterpart of `EmulatedBufferedSink`, for the conditional-overwrite form:
/// accumulates and delegates the atomic publish to `putOverwrite` under `emu_mutex`. Task 3 replaces
/// the buffering with a scratch file; the publish shape stays.
class EmulatedOverwriteBufferedSink final : public WriteSink
{
public:
    EmulatedOverwriteBufferedSink(Backend & backend_, String key_, Token expected_, ObjectMeta meta_)
        : backend(backend_), key(std::move(key_)), expected(std::move(expected_)), meta(std::move(meta_))
    {
    }

    WriteBuffer & buffer() override { return buf; }

    PutResult finalize() override
    {
        chassert(!done);
        done = true;
        return backend.putOverwrite(key, buf.str(), expected, meta);
    }

    void cancel() noexcept override
    {
        done = true;
        buf.cancel();
    }

    ~EmulatedOverwriteBufferedSink() override
    {
        if (!done)
            cancel();
    }

private:
    Backend & backend;
    const String key;
    const Token expected;
    const ObjectMeta meta;
    WriteBufferFromOwnString buf;
    bool done = false;
};
```

- [ ] **Step 5: Implement it in `InMemoryBackend`**

Header, next to `putIfAbsentStream`:

```cpp
    WriteSinkPtr putOverwriteStream(const String & key, const Token & expected,
                                    uint64_t declared_size, const ObjectMeta & meta) override;
```

Implementation — reuse the existing sink shape, delegating to `putOverwrite` instead of `putIfAbsent`:

```cpp
namespace
{

class InMemoryOverwriteSink final : public WriteSink
{
public:
    InMemoryOverwriteSink(InMemoryBackend & backend, String key, Token expected, ObjectMeta meta)
        : backend_(backend), key_(std::move(key)), expected_(std::move(expected)), meta_(std::move(meta))
    {
    }

    WriteBuffer & buffer() override { return buf_; }

    PutResult finalize() override
    {
        chassert(!done_);
        done_ = true;
        return backend_.putOverwrite(key_, buf_.str(), expected_, meta_);
    }

    void cancel() noexcept override
    {
        done_ = true;
        buf_.cancel();
    }

    ~InMemoryOverwriteSink() override
    {
        if (!done_)
            cancel();
    }

private:
    InMemoryBackend & backend_;
    const String key_;
    const Token expected_;
    const ObjectMeta meta_;
    WriteBufferFromOwnString buf_;
    bool done_ = false;
};

}

WriteSinkPtr InMemoryBackend::putOverwriteStream(const String & key, const Token & expected,
                                                 uint64_t /*declared_size*/, const ObjectMeta & meta)
{
    return std::make_unique<InMemoryOverwriteSink>(*this, key, expected, meta);
}
```

- [ ] **Step 6: Implement it in `InstrumentedBackend`**

Follow the file's existing `putIfAbsentStream` proxy exactly, counting the same way it does:

```cpp
WriteSinkPtr InstrumentedBackend::putOverwriteStream(const String & key, const Token & expected,
                                                     uint64_t declared_size, const ObjectMeta & meta)
{
    WriteSinkPtr sink = inner->putOverwriteStream(key, expected, declared_size, meta);
    return wrapSinkForCounting(std::move(sink));
}
```

If `wrapSinkForCounting` does not exist under that name, mirror whatever `putIfAbsentStream` does in this file verbatim — the point is that both streaming forms are counted identically.

Declaration order matters: both sink classes must appear in the anonymous namespace ABOVE
`putOverwriteStream`, next to `NativeStreamingSink`, not below it.

- [ ] **Step 7: Make a refused write abort its upload explicitly**

`NativeStreamingSink::finalize` currently returns `PreconditionFailed` without cancelling the write
buffer. For `putIfAbsentStream` that was tolerable — losing the create race is rare. For a conditional
OVERWRITE it is not: a racing writer displacing the condemned token first is an expected outcome the
caller already handles, so this would become routine. The buffer's destructor does abort, but it logs
`"WriteBufferFromS3 was neither finished nor aborted"` as a WARNING each time, and the stateless
harness fails any test whose server writes to stderr. Uploaded parts also stay billable until a
lifecycle rule reaps them.

Write the test first:

```cpp
TEST(CASBackendContract, PutOverwriteStreamRefusalLeavesNoInFlightUpload)
{
    auto backend = makeBackendForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    Token stale = created.token;
    stale.value += "-stale";

    auto sink = backend->putOverwriteStream("k", stale, /*declared_size=*/8);
    writeString(String("replaced"), sink->buffer());
    ASSERT_EQ(sink->finalize().outcome, PutOutcome::PreconditionFailed);

    /// The sink is dead after finalize; destroying it must not abort anything, because finalize
    /// already did. A destructor-time abort is what emits the "neither finished nor aborted" warning.
    EXPECT_NO_THROW(sink.reset());
    EXPECT_EQ(inFlightUploadCountForTest(*backend), 0u);
}
```

Then change `NativeStreamingSink::finalize` so the refusal path cancels before returning:

```cpp
        if (finalizeConditionalWriteInstrumented(*write_buf) == PutOutcome::PreconditionFailed)
        {
            /// Losing the condition is an ORDINARY outcome here, not an error: a racing writer may
            /// legitimately have displaced the token first. Abort now rather than leaving it to the
            /// destructor, which warns on every occurrence and would leave uploaded parts billable.
            write_buf->cancel();
            return {PutOutcome::PreconditionFailed, {}};
        }
```

If `inFlightUploadCountForTest` has no equivalent in the test backend, assert instead that no WARNING
containing `"neither finished nor aborted"` was logged, using the log-capture helper already used by
`gtest_cas_ref_catalog.cpp` (`ScopedCasGcLogCapture` is GC-scoped; add a sibling if needed, or drop
this assertion and keep the `EXPECT_NO_THROW` plus a comment stating what could not be asserted and
why). Do not silently omit it.

- [ ] **Step 8: Run the tests**

```bash
ninja -C build unit_tests_dbms > build/t1_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASBackendContract.PutOverwriteStream*'
```

Expected: all five PASS. Also run `--gtest_filter='CASBackend*'` — `NativeStreamingSink` is shared with
`putIfAbsentStream`, and its refusal path just changed.

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend src/Disks/tests/gtest_cas_backend_contract.cpp
git commit -m "cas: add putOverwriteStream to the backend seam

The conditional overwrite had only a whole-String form, so its one blob-body caller had to
materialize the body to write it. The condition was never the obstacle: it rides on the write
buffer, and WriteBufferFromS3 already applies If-Match on both PutObject and CompleteMultipartUpload.
Only the caller's input shape was."
```

---

## Task 2: GCS refuses early instead of after the body

**Files:**
- Modify: `.../Backend/CasObjectStorageBackend.cpp` (`putOverwriteStream`, added in Task 1)
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`

**Interfaces:**
- Consumes: `putOverwriteStream` (Task 1), `nativeTokenType()`, `setNativeTokenTypeForTest(TokenType)`, `conditional_single_put_cap` — all existing members of `ObjectStorageBackend`.
- Produces: nothing new; a behavioural guarantee later tasks rely on only through the seam.

- [ ] **Step 1: Write the failing test**

`gtest_cas_backend_generation.cpp` already exercises the generation dialect; follow its fixture.

```cpp
TEST(CASBackendGeneration, ConditionalOverwriteAboveSinglePutCapRefusesBeforeWriting)
{
    auto backend = makeObjectStorageBackendForTest(/*conditional_single_put_cap=*/1024);
    backend->setNativeTokenTypeForTest(TokenType::Generation);

    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    /// The refusal must be decided from `declared_size` alone, before a single byte is offered.
    EXPECT_THROW_MESSAGE_CONTAINS(
        backend->putOverwriteStream("k", created.token, /*declared_size=*/4096),
        "gcs_max_conditional_put_bytes");

    const auto got = backend->get("k");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "original");
}

TEST(CASBackendGeneration, ConditionalOverwriteAtOrBelowCapIsAllowed)
{
    auto backend = makeObjectStorageBackendForTest(/*conditional_single_put_cap=*/1024);
    backend->setNativeTokenTypeForTest(TokenType::Generation);
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/1024);
    EXPECT_NE(sink, nullptr) << "exactly at the cap is allowed; the cap is a maximum, not a strict bound";
}

TEST(CASBackendGeneration, EtagDialectHasNoConditionalOverwriteSizeLimit)
{
    auto backend = makeObjectStorageBackendForTest(/*conditional_single_put_cap=*/1024);
    backend->setNativeTokenTypeForTest(TokenType::ETag);
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/1ULL << 40);
    EXPECT_NE(sink, nullptr) << "the cap exists only because GCS forbids multipart for conditional writes";
}
```

`EXPECT_THROW_MESSAGE_CONTAINS` is a convention placeholder ONLY if the file has no such helper — check first. If it does not, use the file's existing throw-assertion helper (`DB::Cas::tests::expectThrowsCode` with `ErrorCodes::BAD_ARGUMENTS`) and assert the message separately by catching:

```cpp
    try
    {
        backend->putOverwriteStream("k", created.token, /*declared_size=*/4096);
        FAIL() << "expected a refusal";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), ErrorCodes::BAD_ARGUMENTS);
        EXPECT_NE(String(e.message()).find("gcs_max_conditional_put_bytes"), String::npos);
    }
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/t2_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASBackendGeneration.*ConditionalOverwrite*:CASBackendGeneration.EtagDialect*'
```

Expected: FAIL — no throw; the oversized call currently returns a working sink.

- [ ] **Step 3: Add the guard**

At the top of `ObjectStorageBackend::putOverwriteStream`, before building any settings:

```cpp
    /// GCS honours NO precondition on multipart completion -- it completes the upload and drops the
    /// condition, which for a token protocol is the worst outcome available: a silent overwrite where
    /// a refusal was required. Conditional writes on a generation-token store are therefore forced
    /// single-part, and a single part is bounded. Refuse here, from the declared size, rather than
    /// after the body has crossed the network to learn the same thing.
    if (mode == Mode::Native && native_token_type == TokenType::Generation
        && declared_size > conditional_single_put_cap)
    {
        throw Exception(ErrorCodes::BAD_ARGUMENTS,
            "CAS conditional overwrite of {} bytes exceeds this backend's single-PUT budget of {} bytes "
            "(gcs_max_conditional_put_bytes). A generation-token store cannot condition a multipart "
            "completion, so a conditional write must fit in one part",
            declared_size, conditional_single_put_cap);
    }
```

- [ ] **Step 4: Run to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/t2_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASBackendGeneration.*'
```

Expected: PASS, including the pre-existing generation-dialect tests.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/tests/gtest_cas_backend_generation.cpp
git commit -m "cas: refuse an over-cap conditional overwrite on GCS before sending the body

The limit is not new and is not ours to lift here: GCS drops preconditions on multipart completion,
so conditional writes are forced single-part and a single part is bounded. What changes is when the
caller learns it -- from the declared size, rather than from a storage error after gigabytes."
```

---

## Task 3: The emulated sink stops buffering the body

**Files:**
- Modify: `.../Backend/CasObjectStorageBackend.cpp` (`EmulatedOverwriteBufferedSink` → `EmulatedOverwriteFileSink`)
- Test: `src/Disks/tests/gtest_cas_backend_contract.cpp`

**Interfaces:**
- Consumes: `putOverwriteStream` (Task 1).
- Produces: no signature change. The seam is unchanged; only the emulated implementation is.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CASBackendContract, EmulatedPutOverwriteStreamLeavesNoTempFileOnAnyOutcome)
{
    /// Emulated mode only: the Native path has no local temp file to leak.
    auto [backend, scratch] = makeEmulatedBackendWithScratchForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);
    const size_t files_before = countFilesRecursively(scratch);

    {
        auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/8);
        writeString(String("replaced"), sink->buffer());
        ASSERT_EQ(sink->finalize().outcome, PutOutcome::Done);
    }
    EXPECT_EQ(countFilesRecursively(scratch), files_before) << "success must leave no scratch residue";

    Token stale = backend->head("k").token;
    stale.value += "-stale";
    {
        auto sink = backend->putOverwriteStream("k", stale, /*declared_size=*/8);
        writeString(String("nope"), sink->buffer());
        ASSERT_EQ(sink->finalize().outcome, PutOutcome::PreconditionFailed);
    }
    EXPECT_EQ(countFilesRecursively(scratch), files_before) << "a refused write must leave no scratch residue";

    {
        auto sink = backend->putOverwriteStream("k", backend->head("k").token, /*declared_size=*/8);
        writeString(String("nope"), sink->buffer());
        sink->cancel();
    }
    EXPECT_EQ(countFilesRecursively(scratch), files_before) << "cancel must leave no scratch residue";
}
```

If `makeEmulatedBackendWithScratchForTest` and `countFilesRecursively` do not exist, add them to `src/Disks/tests/cas_test_helpers.h` in this step — a helper that creates a `ObjectStorageBackend` in `EmulatedSingleProcess` over a temp directory and returns the directory, plus a `std::filesystem::recursive_directory_iterator` count.

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/t3_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASBackendContract.EmulatedPutOverwriteStreamLeavesNoTempFile*'
```

Expected: FAIL to compile (missing helpers) or, once helpers exist, PASS vacuously — the buffered sink writes no temp file at all. That vacuous pass is expected and is why Step 3 adds the real assertion.

- [ ] **Step 3: Add the assertion that actually bites**

Extend the test with the property the file sink must have and the buffered one cannot:

```cpp
TEST(CASBackendContract, EmulatedPutOverwriteStreamDoesNotHoldTheBodyInMemory)
{
    auto [backend, scratch] = makeEmulatedBackendWithScratchForTest();
    const PutResult created = backend->putIfAbsent("k", "original");
    ASSERT_EQ(created.outcome, PutOutcome::Done);

    /// While the sink is open and half the body has been written, the bytes must be ON DISK, not in
    /// the sink: a scratch file exists and is non-empty. This is the whole point of the change, and
    /// it is the one assertion a memory-buffering implementation cannot satisfy.
    auto sink = backend->putOverwriteStream("k", created.token, /*declared_size=*/1024);
    writeString(String(512, 'x'), sink->buffer());
    sink->buffer().next();
    EXPECT_GT(totalFileBytesRecursively(scratch), 0u)
        << "the emulated sink must spill to scratch, not accumulate in memory";

    writeString(String(512, 'y'), sink->buffer());
    EXPECT_EQ(sink->finalize().outcome, PutOutcome::Done);
}
```

- [ ] **Step 4: Replace the sink**

Rename `EmulatedOverwriteBufferedSink` to `EmulatedOverwriteFileSink` and change its body: open a `WriteBufferFromFile` on a unique scratch path, and at `finalize()` read the file and hand it to `putOverwrite` under `emu_mutex` — the emulation's serialization point does not move. Delete the temp file on every exit path.

```cpp
class EmulatedOverwriteFileSink final : public WriteSink
{
public:
    EmulatedOverwriteFileSink(Backend & backend_, String key_, Token expected_, ObjectMeta meta_,
                              std::filesystem::path temp_path_)
        : backend(backend_)
        , key(std::move(key_))
        , expected(std::move(expected_))
        , meta(std::move(meta_))
        , temp_path(std::move(temp_path_))
        , file_buf(temp_path.string())
    {
    }

    WriteBuffer & buffer() override { return file_buf; }

    PutResult finalize() override
    {
        chassert(!done);
        done = true;
        file_buf.finalize();
        SCOPE_EXIT({ std::filesystem::remove(temp_path); });
        String body;
        readStringUntilEOF(body, *createReadBufferFromFileBase(temp_path.string(), {}));
        return backend.putOverwrite(key, body, expected, meta);
    }

    void cancel() noexcept override
    {
        done = true;
        file_buf.cancel();
        std::error_code ignored;
        std::filesystem::remove(temp_path, ignored);
    }

    ~EmulatedOverwriteFileSink() override
    {
        if (!done)
            cancel();
    }

private:
    Backend & backend;
    const String key;
    const Token expected;
    const ObjectMeta meta;
    const std::filesystem::path temp_path;
    WriteBufferFromFile file_buf;
    bool done = false;
};
```

Note honestly in the class comment that `finalize` still reads the file into a `String` to call `putOverwrite`, so this bounds the OPEN window rather than the publish instant; the emulated path exists for local development and tests, and its publish is a whole-file operation by construction.

- [ ] **Step 5: Run to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/t3_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASBackendContract.*'
```

Expected: all PASS.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp src/Disks/tests/gtest_cas_backend_contract.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "cas: emulated conditional-overwrite sink spills to scratch instead of memory

The emulated mode is what local development and the stateless lanes run on, so leaving it buffering
whole bodies would keep the defect alive everywhere except S3."
```

---

## Task 4: The condemned displacement writes through the stream

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.cpp` (the `else` arm of `if (source.server_side_copy_from)`, ~lines 726-760)
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`

**Interfaces:**
- Consumes: `Backend::putOverwriteStream(key, expected, declared_size, meta)` (Task 1).
- Produces: no new API. After this task nothing calls `condemnedUploadAdmission()`, which Task 5 relies on.

- [ ] **Step 1: Write the failing test**

```cpp
TEST(CASUploadFanout, CondemnedResurrectionDoesNotMaterializeTheBody)
{
    /// The body is generated on the fly and never exists in the test either, so a run that passes
    /// proves the write path did not need it whole. Sized far above any buffer but small enough that
    /// a regression fails fast rather than exhausting the machine.
    constexpr uint64_t kBodyBytes = 256ULL << 20;

    auto env = makeCondemnedResurrectionEnvForTest();
    env.source.size = kBodyBytes;
    env.source.write_payload = [](WriteBuffer & out)
    {
        const String chunk(1 << 20, 'z');
        for (uint64_t written = 0; written < kBodyBytes; written += chunk.size())
            out.write(chunk.data(), chunk.size());
    };

    const uint64_t rss_before = peakResidentBytesForTest();
    const PutResult res = env.runResurrection();
    const uint64_t rss_after = peakResidentBytesForTest();

    ASSERT_EQ(res.outcome, PutOutcome::Done);
    EXPECT_LT(rss_after - rss_before, kBodyBytes / 4)
        << "peak memory must not scale with the body: the bytes are streamed, never materialized";
}
```

If `peakResidentBytesForTest` does not exist, implement it in `cas_test_helpers.h` by reading `VmHWM` from `/proc/self/status` and resetting it via `/proc/self/clear_refs` where available; if the platform does not support a reset, compare against the process-lifetime high-water mark and document that the test is one-shot per binary. State whichever you chose in the test's comment — an unstated measurement method is what makes such a test rot.

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/t4_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASUploadFanout.CondemnedResurrectionDoesNotMaterializeTheBody'
```

Expected: FAIL — peak memory grows by roughly the body size.

- [ ] **Step 3: Switch the call site**

Replace the `String overwrite_body` block and its `ByteWeightedSemaphoreLock admit(...)` with:

```cpp
    const uint64_t declared_size = meta.blob_header_len + source.size;
    auto sink = store->backend().putOverwriteStream(key, condemned_token, declared_size);
    writeString(buildHeader(), sink->buffer());
    const size_t before = sink->buffer().count();
    source.write_payload(sink->buffer());
    const size_t written = sink->buffer().count() - before;
    /// The payload length is part of the identity we are re-establishing, so a source that writes a
    /// different number of bytes than it declared must not become a live incarnation.
    if (written != source.size)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS resurrect: source wrote {} payload bytes, declared {}", written, source.size);
    const PutResult overwrite_res = sink->finalize();
```

Keep everything else in the arm exactly as it is: the fence re-check before the write, `buildHeader()` minting a fresh `incarnation_tag`, and the `PreconditionFailed` handling below. Delete the now-unused `#include` of the admission header if nothing else in the file needs it.

- [ ] **Step 4: Run to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/t4_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASUploadFanout.*:CASPartWriteTxn*'
```

Expected: PASS, including the pre-existing resurrection tests (`DuplicateCondemnedS3ResurrectsCorrectly` above all — it pins INV-NO-RETURN).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp src/Disks/tests/gtest_cas_upload_fanout.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "cas: stream the condemned-blob resurrection instead of materializing it

The bytes were already on disk; re-reading them into a String was an artifact of the seam's shape,
and it made the peak memory of this path equal to the largest blob in the part."
```

---

## Task 5: Delete the admission semaphore and its setting

**Files:**
- Modify: `.../Pool/CasBlobUploadPool.h` (delete `ByteWeightedSemaphore`, `ByteWeightedSemaphoreLock`, the three admission functions)
- Modify: `.../Pool/CasBlobUploadPool.cpp` (delete their definitions and `admission_instance`/`admission_mutex`)
- Modify: `src/Core/ServerSettings.cpp` (delete the `cas_condemned_upload_memory_bytes` DECLARE)
- Modify: `programs/server/Server.cpp:1502,1725`, `programs/local/LocalServer.cpp:430,913`, `programs/disks/DisksApp.cpp:563,677`
- Modify: `src/Disks/tests/cas_test_helpers.h:92` (delete `ensureCondemnedUploadAdmissionForTest`)
- Delete: `src/Disks/tests/gtest_cas_blob_upload_pool_env.cpp`
- Modify: `src/Disks/tests/gtest_cas_upload_fanout.cpp` (delete `CondemnedCapLimitsPeakBytes` at ~831 and `OverweightBlobRunsExclusively` at ~895)

**Interfaces:**
- Consumes: nothing — Task 4 removed the last production caller.
- Produces: nothing. This task only removes.

- [ ] **Step 1: Prove there is no caller left**

```bash
grep -rn "condemnedUploadAdmission\|ByteWeightedSemaphore\|initializeCondemnedUploadAdmission\|cas_condemned_upload_memory_bytes" src/ programs/ docs/ | grep -v "^docs/superpowers/"
```

Expected: only the definitions, the startup calls, the two doomed tests, the test helper, and the two documentation rows. If anything else appears, STOP — a caller was missed and this task's premise is wrong.

- [ ] **Step 2: Delete the two admission tests and the env file**

Delete `TEST(CASUploadFanout, CondemnedCapLimitsPeakBytes)` and `TEST(CASUploadFanout, OverweightBlobRunsExclusively)` in full, and `git rm src/Disks/tests/gtest_cas_blob_upload_pool_env.cpp`.

These test properties OF THE SEMAPHORE — an aggregate that can be exceeded, and an overweight admission that must not starve. With the semaphore gone neither property exists to be violated: nothing accumulates in memory, so there is no aggregate, and there is no admission to be unfair. They are deleted, not ported.

- [ ] **Step 3: Delete the production code**

Remove from `CasBlobUploadPool.h`: `class ByteWeightedSemaphore` (with `StatsForTest`, `setHeldHookForTest`, `setWaitHookForTest`, `statsForTest`, `resetStatsForTest`), `ByteWeightedSemaphoreLock`, and the declarations of `initializeCondemnedUploadAdmission`, `condemnedUploadAdmission`, `shutdownCondemnedUploadAdmission`. Remove their definitions and the file-static `admission_instance` / `admission_mutex` from `CasBlobUploadPool.cpp`.

- [ ] **Step 4: Delete the setting and its call sites**

Remove the `DECLARE(UInt64, cas_condemned_upload_memory_bytes, ...)` block from `src/Core/ServerSettings.cpp`, the `ServerSetting::cas_condemned_upload_memory_bytes` extern if the file declares one, and every `initializeCondemnedUploadAdmission` / `shutdownCondemnedUploadAdmission` call in the three programs. In `Server.cpp` the init call passes two settings — the surviving `cas_blob_upload_pool_size` must still reach the upload pool's own initializer, so delete only the admission call, not the pool one.

- [ ] **Step 5: Build and gate**

```bash
ninja -C build clickhouse unit_tests_dbms > build/t5_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CAS*:Ca*' > build/t5_gate_release.log 2>&1; tail -3 build/t5_gate_release.log
ninja -C build_asan unit_tests_dbms > build/t5_asan_build.log 2>&1 && echo ASAN_BUILD_OK
build_asan/src/unit_tests_dbms --gtest_filter='CAS*:Ca*' > build/t5_gate_asan.log 2>&1; tail -3 build/t5_gate_asan.log
```

Expected: both green. The ASan run is not optional — it carries the `*DeathTest` suites the release build does not compile.

- [ ] **Step 6: Commit**

```bash
git add -A src/Disks src/Core/ServerSettings.cpp programs
git commit -m "cas: delete the condemned-upload memory admission

It existed because one branch could not stream. That branch streams now, so the aggregate it rationed
no longer accumulates. Its guarantee was max(capacity, largest single body) and the second term was
unbounded, so it never protected against the case that motivated this work -- one blob larger than
memory.

The two tests that drove it are deleted rather than ported: they asserted that an aggregate is not
exceeded and that an overweight admission does not starve, and neither property exists once nothing
is admitted."
```

---

## Task 6: Say the GCS consequence in the user-facing docs

**Files:**
- Modify: `docs/en/antalya/cas/architecture/backend.md` (after the conditional-`CompleteMultipartUpload` paragraph, ~line 55)
- Modify: `docs/en/antalya/cas/architecture/blob-protocol.md:229`
- Modify: `docs/en/antalya/cas/configuration.md:97`
- Modify: `docs/en/antalya/cas/bucket-requirements.md:38`

**Interfaces:**
- Consumes: the exact error message text from Task 2 — the docs must agree with what an operator sees.
- Produces: nothing consumed by code.

- [ ] **Step 1: Add the consequence to the backend page**

After the existing sentence explaining that a conditional `CompleteMultipartUpload` throws:

```markdown
The practical consequence is a size ceiling, not just a code path: on a generation-dialect backend a
conditional write must fit in ONE part, so `gcs_max_conditional_put_bytes` (default 1 GiB) is the
largest body that can be conditionally overwritten — and that part is buffered whole in memory, so the
setting bounds peak memory as well as size. A blob larger than the ceiling cannot be resurrected from
a condemned incarnation on GCS at all; the write is refused before any byte is sent. `ETag` dialects
have no such ceiling: their conditional writes may take the multipart path, where the precondition is
honoured.
```

- [ ] **Step 2: Fix both settings tables**

In `blob-protocol.md:229` and `configuration.md:97`, change the description of `gcs_max_conditional_put_bytes` to say it is a functional ceiling rather than a tuning knob:

```markdown
| `gcs_max_conditional_put_bytes` | 1 GiB | Largest body that can be conditionally written on a generation-token store (GCS). A hard ceiling, not a tuning knob: GCS honours no preconditions on multipart completion, so conditional writes are forced single-part. Bodies above this are refused. Irrelevant on `ETag` stores |
```

- [ ] **Step 3: Qualify the support table**

In `bucket-requirements.md:38`, keep the `✓` and add the qualifier to the notes cell:

```markdown
| Google Cloud Storage | ✓ | Generation-token dialect: conditional headers are rewritten to `x-goog-if-generation-match`, opted into via `http_client = gcs_hmac` or `gcp_oauth`. Conditional writes are capped at `gcs_max_conditional_put_bytes` (default 1 GiB) — see [the backend page](/antalya/cas/architecture/backend) |
```

- [ ] **Step 4: Check the docs against the code, not against this plan**

```bash
grep -rn "gcs_max_conditional_put_bytes" docs/en/antalya/cas/
grep -rn "gcs_max_conditional_put_bytes" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
```

Read the error message added in Task 2 and confirm the docs use the same setting name and say the same thing about why. If they disagree, the code wins and the docs change.

- [ ] **Step 5: Verify every heading still carries its anchor**

Every heading in `docs/en/` must end with an explicit `{#kebab-case-anchor}`. This task adds prose, not headings, but check that nothing was disturbed:

```bash
grep -nE "^#{1,6} " docs/en/antalya/cas/architecture/backend.md docs/en/antalya/cas/bucket-requirements.md | grep -v "{#"
```

Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add docs/en/antalya/cas
git commit -m "docs: state the GCS conditional-write ceiling, not just the mechanism

The pages explained that a conditional CompleteMultipartUpload is rejected and stopped there. What an
operator needs is the consequence: on GCS a conditional overwrite is capped, a larger blob cannot be
resurrected at all, and the cap bounds memory too. Both settings tables listed the value as if it were
a tuning knob."
```

---

## Final verification

- [ ] **Full CA gate, both lanes, on the integrated result**

```bash
ninja -C build clickhouse unit_tests_dbms > build/final_build.log 2>&1 && echo REL_OK
build/src/unit_tests_dbms --gtest_filter='CAS*:Ca*' > build/final_release.log 2>&1; tail -3 build/final_release.log
ninja -C build_asan unit_tests_dbms > build/final_asan_build.log 2>&1 && echo ASAN_OK
build_asan/src/unit_tests_dbms --gtest_filter='CAS*:Ca*' > build/final_asan.log 2>&1; tail -3 build/final_asan.log
```

- [ ] **Stateless CAS lanes**

```bash
python3 -m ci.praktika run "Stateless tests (amd_binary, cas storage, parallel)" > build/final_stateless.log 2>&1
```

- [ ] **A soak, because this is a write-path change**

The resurrection path is reached only when a per-hash meta read observes `Condemned`, which the unit tests force but ordinary traffic reaches rarely. Run a short soak so the path is exercised under real concurrency:

```bash
cd utils/ca-soak && python3 -m soak.run --phase 3 --duration 20m --metrics ../../build/soak_stream_overwrite.sqlite
```

Expected: `PHASE3 OK`, `SOAK_EXIT=0`, and `dangling=0 chain_broken=0 lifeless_keys=0` in the closing fsck.

---

## Notes for the implementer

**The one invariant you must not break.** `buildHeader()` mints a FRESH `incarnation_tag` on this path, and that is not decoration: it makes the resurrected body differ from the condemned incarnation, so the queued exact-token delete of the condemned one cannot destroy the live resurrection. If you find yourself "simplifying" by reusing the staging header or by server-side copying the condemned bytes, stop — that is the data-loss shape this arm was built to avoid.

**Do not touch the other arm.** When `source.server_side_copy_from` is set, the S3-native staging path calls `resurrectStaged`, and the bytes never traverse the server at all. It is strictly better where available and is out of scope.

**Do not widen `putOverwrite`'s other callers.** Root manifests, `gc/state` and mount records are small by construction and read-modify-write in memory anyway. Only the blob-body caller moves.

**Emulated mode is not test-only.** The comment on the existing `EmulatedBufferedSink` says "unit tests only"; that is now inaccurate — local object storage runs this mode, including the stateless lanes. Do not use that comment as a licence to leave the emulated path buffering.

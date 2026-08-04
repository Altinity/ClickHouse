# CAS unconditional resurrect — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Collapse the two condemned-blob resurrection arms into one unconditional streaming operation whose source is a reader, so a body of any size can be resurrected on any backend — and drop the conditional-overwrite seam built for the superseded design.

**Architecture:** `resurrectStaged(staging_key, blob_key, fresh_header, offset)` becomes `resurrect(ReadBuffer & payload, blob_key, fresh_header)`. `BlobSource` supplies a reader factory instead of a write callback. The local arm re-reads HEAD and the blob meta on every attempt and, when the meta says `Condemned`, streams an unconditional write; a lost race costs one redundant upload and nothing else.

**Tech Stack:** C++ (ClickHouse), gtest, `ninja`, `IObjectStorage::writeObject` / `readObject`.

**Spec:** `docs/superpowers/specs/2026-08-04-cas-unconditional-resurrect-design.md`

## Global Constraints

- Allman braces (opening brace on its own line) — enforced by the CI style check.
- Every new gtest suite name MUST start with `CAS`. The gate filter is derived from suite names beginning with `Ca`/`CAS`; a suite that escapes the prefix escapes the gate.
- Never use `sleep` to resolve a race in C++ — use the existing test hooks.
- A test asserting a `LOGICAL_ERROR` needs the `*DeathTest` suite split, or it aborts the binary under sanitizers and hides every test after it.
- Comments carry the REASON, never a citation of this plan, the spec, a task number, or a BACKLOG anchor.
- No rebase, no amend — reverts are new commits.
- Build: `ninja -C build clickhouse unit_tests_dbms`, redirected to a log under `build/`. No `-j`.
- Gate before every commit that touches backend or write-path code: release AND ASan. ASan carries 18 `*DeathTest` suites the release build does not compile. `tmp/run_ca_gate_both.sh` runs both and writes `build/ca_gate_both.log`.
- **INV-NO-RETURN is the invariant this whole path exists to hold:** the resurrected body MUST carry a FRESH `incarnation_tag`, so its ETag differs from the condemned incarnation and GC's already-queued exact-token delete misses. Never reuse the source header; never copy the condemned object.

---

## File Structure

| File | Change |
|---|---|
| `.../Backend/CasBackend.h` | `resurrectStaged` → `resurrect(ReadBuffer &, …)`; remove `putOverwriteStream` (revert) |
| `.../Backend/CasObjectStorageBackend.{h,cpp}` | Implement `resurrect`; the Native body is today's code with the reader passed in |
| `.../Backend/CasInMemoryBackend.{h,cpp}`, `.../Backend/CasInstrumentedBackend.{h,cpp}` | Same rename, delegation unchanged |
| `.../Pool/CasPartWriteTxn.h` | `BlobSource::write_payload` → `open` |
| `.../Pool/CasPartWriteTxn.cpp` | Both arms call `resurrect`; per-attempt HEAD+meta; `fromString` opens a string reader |
| `.../ContentAddressedTransaction.cpp:293` | Producer returns `ReadBufferFromFile` instead of a callback |
| `src/Disks/tests/gtest_cas_{part_write,s3_staging,fence_generation}.cpp` | `write_payload` producers → `open` |
| `src/Disks/tests/gtest_cas_upload_fanout.cpp` | New: no-materialization, lost-race-harmless |
| `src/Disks/tests/gtest_cas_backend_generation.cpp` | New: resurrect above the old cap succeeds |
| `docs/en/antalya/cas/**` | GCS pages must not imply this path has a ceiling |

---

## Task 1: Revert the superseded design, keep the one fix worth keeping

**Files:**
- Revert: commits `784308ddc76` and `531adeebd6b`
- Modify: `.../Backend/CasObjectStorageBackend.cpp` (re-apply the explicit abort)

**Interfaces:**
- Consumes: nothing.
- Produces: a tree with no `putOverwriteStream`, and `NativeStreamingSink::finalize` still cancelling explicitly on a refused condition.

- [ ] **Step 1: Revert both commits, newest first**

```bash
git revert --no-edit 784308ddc76
git revert --no-edit 531adeebd6b
```

If either conflicts, resolve by taking the pre-change state — nothing else has touched these files since.

- [ ] **Step 2: Confirm the seam is gone**

```bash
grep -rn "putOverwriteStream\|declared_size\|RefusingSink" src/ | wc -l
```

Expected: `0`.

- [ ] **Step 3: Re-apply the explicit abort, on its own merits**

The revert also removed a correction that has nothing to do with the conditional-overwrite design: `putIfAbsentStream` loses conditions too (a racing create wins the slot), and leaving the abort to the write buffer's destructor logs `"was neither finished nor aborted"` on every occurrence — which the stateless harness turns into a failed test — while uploaded parts stay billable. In `NativeStreamingSink::finalize`:

```cpp
        if (finalizeConditionalWriteInstrumented(*write_buf) == PutOutcome::PreconditionFailed)
        {
            /// Losing the condition is an ORDINARY outcome, not an error: another writer may
            /// legitimately have taken the slot. Abort HERE rather than leaving it to the buffer's
            /// destructor, which warns on every occurrence -- and a server that writes that to stderr
            /// fails the test around it -- while the uploaded parts stay billable until a lifecycle
            /// rule reaps them.
            write_buf->cancel();
            return {PutOutcome::PreconditionFailed, {}};
        }
```

- [ ] **Step 4: Gate**

```bash
nohup setsid bash tmp/run_ca_gate_both.sh > /dev/null 2>&1 < /dev/null &
# wait for CA_GATE_BOTH_FINISHED=1 in build/ca_gate_both.log
grep -E "_EXIT=" build/ca_gate_both.log
```

Expected: all four `_EXIT=0`; test counts back to 1993 (release) / 1998 (ASan), i.e. the six contract tests and three generation tests are gone with the revert.

- [ ] **Step 5: Commit the re-applied fix**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp
git commit -m "cas: abort the upload explicitly when a streaming conditional write is refused

Losing the condition is an ordinary outcome on the streaming create path, not an error. Left to the
write buffer's destructor it logs a warning every time -- which any test whose server writes to stderr
turns into a failure -- and leaves uploaded parts billable until a lifecycle rule reaps them.

Kept from the reverted conditional-overwrite work, where it was found; it stands on its own."
```

---

## Task 2: `BlobSource` supplies a reader, not a write callback

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.h` (the `BlobSource` struct)
- Modify: `.../Pool/CasPartWriteTxn.cpp` (`fromString`, and every `write_payload(...)` call)
- Modify: `.../ContentAddressedTransaction.cpp:293`
- Modify: `src/Disks/tests/gtest_cas_part_write.cpp`, `gtest_cas_s3_staging.cpp`, `gtest_cas_fence_generation.cpp`

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `struct BlobSource { uint64_t size; std::function<std::unique_ptr<ReadBuffer>()> open; std::optional<String> server_side_copy_from; static BlobSource fromString(String); }`. Task 4 calls `source.open()`.

- [ ] **Step 1: Change the struct**

```cpp
struct BlobSource
{
    uint64_t size = 0;
    /// Opens a FRESH reader over exactly `size` payload bytes. A factory rather than one buffer
    /// because this path retries: it re-uploads after the object vanishes and re-decides after a lost
    /// race, and each attempt must read from the beginning. Replaces the former push-shaped
    /// `write_payload` callback, which forced every producer to own the copy loop.
    std::function<std::unique_ptr<ReadBuffer>()> open;
    /// When set, the bytes already live in a staging object with this key: the ordinary create becomes
    /// a WRITE-ONCE conditional SERVER-SIDE COPY (`promoteStaged`). The RESURRECT streams either way —
    /// this only selects which reader `open` returns.
    std::optional<String> server_side_copy_from;
    static BlobSource fromString(String bytes);
};
```

- [ ] **Step 2: Update `fromString`**

```cpp
BlobSource BlobSource::fromString(String bytes)
{
    BlobSource source;
    source.size = bytes.size();
    source.open = [b = std::move(bytes)]() -> std::unique_ptr<ReadBuffer>
    {
        return std::make_unique<ReadBufferFromString>(b);
    };
    return source;
}
```

Note the capture is by value and the lambda is called repeatedly — each call must hand back a reader positioned at the start, which a fresh `ReadBufferFromString` over the retained string does.

- [ ] **Step 3: Update the production producer**

`ContentAddressedTransaction.cpp:293` currently wraps a file read in a callback. It becomes the read itself:

```cpp
            const std::string staging_key = pb.staging_key;
            source.open = [staging_key]() -> std::unique_ptr<ReadBuffer>
            {
                return std::make_unique<ReadBufferFromFile>(staging_key);
            };
```

- [ ] **Step 4: Update every remaining call site and test producer**

```bash
grep -rn "write_payload" src/
```

Every producer becomes an `open` returning a reader; every consumer (`source.write_payload(out)`) becomes `copyData(*source.open(), out)`. Do not leave a compatibility shim that accepts both.

- [ ] **Step 5: Build and run the touched suites**

```bash
ninja -C build unit_tests_dbms > build/t2_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASPartWrite*:CASS3Staging*:CASFenceGeneration*:CASUploadFanout*'
```

Expected: PASS. This task changes no behaviour — it changes who owns the copy loop.

- [ ] **Step 6: Commit**

```bash
git add -A src/Disks
git commit -m "cas: BlobSource supplies a reader factory instead of a write callback

Every producer already had a reader behind its callback -- the production one opened
ReadBufferFromFile and copied, the test one wrote a string. Pushing the copy loop into each producer
meant the source could only ever be consumed by something willing to be written INTO, which is why
the two resurrect arms could not share an implementation.

A factory rather than a single buffer because this path retries and each attempt must read from the
start."
```

---

## Task 3: `resurrectStaged` becomes `resurrect(ReadBuffer &, …)`

**Files:**
- Modify: `.../Backend/CasBackend.h` (the virtual and its doc comment)
- Modify: `.../Backend/CasObjectStorageBackend.{h,cpp}`, `.../Backend/CasInMemoryBackend.{h,cpp}`, `.../Backend/CasInstrumentedBackend.h`
- Modify: `.../Pool/CasPartWriteTxn.cpp` (the staging arm opens its own reader)
- Test: `src/Disks/tests/gtest_cas_s3_staging.cpp`

**Interfaces:**
- Consumes: `BlobSource::open` (Task 2).
- Produces: `virtual Token Backend::resurrect(ReadBuffer & payload, const String & blob_key, const String & fresh_header)`. Task 4 calls it for the local arm.

- [ ] **Step 1: Change the seam**

```cpp
    /// Re-establishes a live incarnation of an object whose current incarnation is CONDEMNED, by
    /// writing `[fresh_header][payload]` UNCONDITIONALLY and returning the new incarnation's token.
    ///
    /// Unconditional is deliberate, not an omission. An `If-Match` on the condemned token would save a
    /// redundant re-upload when another writer resurrects the same blob first, and would prevent
    /// nothing: two racing resurrections write payload-identical bodies, no consumer reads a dep
    /// token's VALUE, and what actually protects the resurrection is `fresh_header` carrying a FRESH
    /// incarnation tag -- which changes the bytes, hence the ETag, so every already-queued exact-token
    /// delete of the condemned incarnation misses (INV-NO-RETURN).
    ///
    /// The caller supplies the reader and has already skipped any envelope header on it. The payload
    /// is streamed, never materialized: blob bodies have no size cap.
    virtual Token resurrect(ReadBuffer & payload, const String & blob_key, const String & fresh_header) = 0;
```

Delete `resurrectStaged` entirely — no deprecated overload.

- [ ] **Step 2: Implement it in `ObjectStorageBackend`**

The Native body is today's code with the two reader lines lifted out:

```cpp
Token ObjectStorageBackend::resurrect(ReadBuffer & payload, const String & blob_key, const String & fresh_header)
{
    if (mode != Mode::Native)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "ObjectStorageBackend::resurrect is Native-mode only");

    /// Default WriteSettings: no precondition, therefore no forced single part, therefore multipart on
    /// every backend including a generation-token store -- where a CONDITIONAL write would have been
    /// capped because GCS drops preconditions on multipart completion.
    auto out = object_storage->writeObject(
        StoredObject(blob_key), WriteMode::Rewrite, /*attributes=*/std::nullopt,
        DBMS_DEFAULT_BUFFER_SIZE, WriteSettings{});
    out->write(fresh_header.data(), fresh_header.size());
    copyData(payload, *out);
    out->finalize();

    const HeadResult hr = nativeHead(blob_key);
    if (!hr.exists)
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "ObjectStorageBackend::resurrect: blob {} is absent immediately after the resurrect write",
            blob_key);
    return hr.token;
}
```

Keep the existing post-write HEAD and its absent-check verbatim — it is what mints the returned token.

- [ ] **Step 3: Implement it in the other two backends**

`InMemoryBackend`: read the whole reader (test backend, bodies are small), store `fresh_header + payload`, return the minted token — the same store mutation `resurrectStaged` did. `InstrumentedBackend`: delegate and count exactly as before, with the reader passed through.

- [ ] **Step 4: Update the staging arm to open its own reader**

In `CasPartWriteTxn.cpp`, where `resurrectStaged` was called with a staging key and offset:

```cpp
        auto payload = store->backend().getStream(*source.server_side_copy_from);
        if (!payload)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "CAS resurrect: staging object {} vanished", *source.server_side_copy_from);
        payload->stream->ignore(staging_payload_offset);
        tok = store->backend().resurrect(*payload->stream, key, buildHeader());
```

The offset skip moves here because the caller is what knows the staging envelope's shape.

- [ ] **Step 5: Build and run the staging suite**

```bash
ninja -C build unit_tests_dbms > build/t3_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASS3Staging*'
```

Expected: PASS, unchanged — the staging arm's behaviour is identical, only the seam moved.

- [ ] **Step 6: Commit**

```bash
git add -A src/Disks
git commit -m "cas: resurrect takes a reader, not a staging key

The method never performed a server-side copy despite its name: it read the staging object, streamed
it through the client, and wrote with default settings. The server-side copy is in promoteStaged.

So the two resurrect arms differed in exactly one thing -- where the reader came from -- and making
that a parameter collapses them into one operation. The payload offset moves to the caller, which is
what knows the staging envelope's shape."
```

---

## Task 4: The local arm resurrects unconditionally, deciding per attempt

**Files:**
- Modify: `.../Pool/CasPartWriteTxn.cpp` (the condemned-displacement block)
- Test: `src/Disks/tests/gtest_cas_upload_fanout.cpp`

**Interfaces:**
- Consumes: `Backend::resurrect` (Task 3), `BlobSource::open` (Task 2).
- Produces: no new API. After this task nothing calls `putOverwrite` with a blob body.

- [ ] **Step 1: Write the failing tests**

```cpp
/// The body is generated on the fly and never exists whole in the test either, so a passing run
/// proves the write path did not need it whole. Peak RSS is read from /proc/self/status (VmHWM);
/// the test is one-shot per binary because that high-water mark cannot be reset portably.
TEST(CASUploadFanout, CondemnedResurrectionDoesNotMaterializeTheBody)
{
    constexpr uint64_t kBodyBytes = 256ULL << 20;

    auto env = makeCondemnedResurrectionEnvForTest();
    env.source.size = kBodyBytes;
    env.source.open = []() -> std::unique_ptr<ReadBuffer>
    {
        return std::make_unique<GeneratedReadBuffer>(kBodyBytes, 'z');
    };

    const uint64_t rss_before = peakResidentBytesForTest();
    const auto res = env.runResurrection();
    const uint64_t rss_after = peakResidentBytesForTest();

    ASSERT_EQ(res.outcome, PutOutcome::Done);
    EXPECT_LT(rss_after - rss_before, kBodyBytes / 4)
        << "peak memory must not scale with the body: the payload is streamed, never materialized";
}

/// Two writers that both observe Condemned both write; the last one wins. The loser spent an upload
/// and broke nothing -- that is the trade the unconditional write makes, and this pins it.
TEST(CASUploadFanout, LostResurrectRaceLeavesBothPartsReadable)
{
    auto env = makeCondemnedResurrectionEnvForTest();
    const auto first = env.runResurrection();
    ASSERT_EQ(first.outcome, PutOutcome::Done);
    const auto second = env.runResurrection();
    ASSERT_EQ(second.outcome, PutOutcome::Done);

    EXPECT_NE(first.token, second.token) << "each resurrect mints a fresh incarnation";
    const auto got = env.backend->get(env.blob_key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes.substr(env.header_len), env.expected_payload);

    /// GC's queued exact-token delete of the ORIGINAL condemned incarnation must miss both.
    EXPECT_EQ(env.backend->deleteExact(env.blob_key, env.condemned_token).kind,
              DeleteOutcome::Kind::TokenMismatch);
    EXPECT_TRUE(env.backend->head(env.blob_key).exists);
}

/// The decision is made per attempt, never carried: a meta flipped to Clean between attempts means
/// someone else resurrected, and the next attempt must adopt rather than write again.
TEST(CASUploadFanout, ResurrectReDecidesFromMetaOnEveryAttempt)
{
    auto env = makeCondemnedResurrectionEnvForTest();
    ASSERT_EQ(env.runResurrection().outcome, PutOutcome::Done);   /// flips the meta to Clean

    const uint64_t writes_before = env.backend->writeCount();
    const auto again = env.runUpload();
    EXPECT_EQ(again.outcome_kind, BlobUploadOutcome::HeadMissAdopted)
        << "a Clean meta means adopt, not resurrect";
    EXPECT_EQ(env.backend->writeCount(), writes_before) << "adopting must write nothing";
}
```

`GeneratedReadBuffer`, `peakResidentBytesForTest`, and `makeCondemnedResurrectionEnvForTest` go in `src/Disks/tests/cas_test_helpers.h` if absent. `GeneratedReadBuffer` yields N bytes of a repeated character without holding them; `peakResidentBytesForTest` parses `VmHWM:` from `/proc/self/status` and returns bytes.

- [ ] **Step 2: Run to verify they fail**

```bash
ninja -C build unit_tests_dbms > build/t4_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CASUploadFanout.CondemnedResurrection*:CASUploadFanout.LostResurrect*:CASUploadFanout.ResurrectReDecides*'
```

Expected: FAIL — peak memory tracks the body, because the arm still builds a `String`.

- [ ] **Step 3: Replace the local arm**

The `else` branch of `if (source.server_side_copy_from)` becomes:

```cpp
        /// Unconditional, matching the staging arm: an If-Match on the condemned token would save a
        /// redundant upload on a lost race and prevent nothing -- INV-NO-RETURN is carried by the
        /// fresh incarnation tag in the header, which makes this body's ETag differ from the condemned
        /// one so every queued exact-token delete of that incarnation misses.
        auto payload = source.open();
        tok = store->backend().resurrect(*payload, key, buildHeader());
```

Both arms now end in the same call, so fold them: open the reader (staging object or local source) and resurrect once.

- [ ] **Step 4: Make the decision per attempt**

The block that decides `condemned` already runs a HEAD and a meta point-read. Ensure the retry paths re-enter it rather than reusing the earlier observation: after a resurrect, the meta flip to `Clean` is what a later attempt must see. Re-read HEAD and meta at the top of every attempt; do not cache `hr` or `lm` across one.

- [ ] **Step 5: Run to verify they pass**

```bash
ninja -C build unit_tests_dbms > build/t4_build.log 2>&1 && echo BUILD_OK
build/src/unit_tests_dbms --gtest_filter='CASUploadFanout*:CASPartWrite*:CASS3Staging*:CASUploadDetached*:CaWiring*'
```

Expected: PASS. `CASUploadFanout.DuplicateCondemnedS3ResurrectsCorrectly` and the protocol-scenario suites pin INV-NO-RETURN — treat any failure there as this task being wrong, not the test.

- [ ] **Step 6: Gate, then commit**

```bash
nohup setsid bash tmp/run_ca_gate_both.sh > /dev/null 2>&1 < /dev/null &
# wait, then:
grep -E "_EXIT=" build/ca_gate_both.log
git add -A src/Disks
git commit -m "cas: resurrect a condemned blob unconditionally, streaming from the source reader

The local arm was the last caller that had to materialize a blob body: putOverwrite took a String, so
a body larger than memory could not be resurrected at all. It now streams, like the staging arm always
did, and drops the condition for the same reason that arm did -- an If-Match saves a redundant upload
on a lost race and prevents no data loss.

The decision is re-made from HEAD and the meta on every attempt, so a resurrect that another writer
completed first is adopted rather than repeated."
```

---

## Task 5: The GCS ceiling stops applying to this path

**Files:**
- Modify: `docs/en/antalya/cas/architecture/backend.md`
- Test: `src/Disks/tests/gtest_cas_backend_generation.cpp`

**Interfaces:**
- Consumes: `Backend::resurrect` (Task 3).
- Produces: nothing.

- [ ] **Step 1: Write the regression test**

```cpp
/// The single-PUT cap binds CONDITIONAL writes, because that is where GCS drops the precondition. A
/// resurrect carries no precondition, so it must not be capped -- this is the regression test for a
/// ceiling that used to apply here and no longer does.
TEST(CASBackendGeneration, ResurrectIsNotBoundByTheSinglePutCap)
{
    auto b = std::make_shared<ObjectStorageBackend>(
        DB::Cas::tests::makeLocalObjectStorageForTest(), ObjectStorageBackend::Mode::Native,
        /*conditional_single_put_cap=*/16);
    b->setNativeTokenTypeForTest(TokenType::Generation);

    ASSERT_EQ(b->putIfAbsent("p/gen/res", "original").outcome, PutOutcome::Done);

    const String payload(1024, 'x');   /// far above the 16-byte cap
    ReadBufferFromString in(payload);
    const Token tok = b->resurrect(in, "p/gen/res", String("HDR"));
    EXPECT_FALSE(tok.empty());

    auto got = b->get("p/gen/res");
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->bytes, "HDR" + payload);
}
```

- [ ] **Step 2: Run it**

```bash
build/src/unit_tests_dbms --gtest_filter='CASBackendGeneration.ResurrectIsNotBound*'
```

Expected: PASS immediately — Task 3 already used default `WriteSettings`. A test that passes on arrival is correct here: it pins a property that a future "let's route resurrect through conditionalWriteSettings for consistency" would silently break.

- [ ] **Step 3: Correct the backend page**

`architecture/backend.md` explains that a conditional `CompleteMultipartUpload` throws. Add what that does and does not bound:

```markdown
This bounds CONDITIONAL writes only. The write-once create carries `If-None-Match`, so on a
generation-dialect backend it is single-part and limited by `gcs_max_conditional_put_bytes`. The
condemned-blob RESURRECT carries no precondition — it re-establishes a fresh incarnation whose
identity comes from a freshly minted tag rather than from a compare-and-swap — so it takes the
multipart path and has no size limit on any backend.
```

- [ ] **Step 4: Verify no page still implies a universal ceiling**

```bash
grep -rn "gcs_max_conditional_put_bytes" docs/en/antalya/cas/
```

Each hit must describe the cap as binding conditional writes, not all writes.

- [ ] **Step 5: Commit**

```bash
git add docs/en/antalya/cas src/Disks/tests/gtest_cas_backend_generation.cpp
git commit -m "docs: the GCS single-PUT cap binds conditional writes, not the resurrect

The pages described the mechanism -- a conditional CompleteMultipartUpload is rejected because GCS
drops the precondition -- without saying which writes it therefore bounds. The write-once create is
bounded; the resurrect is not, because it carries no precondition at all.

The test pins that on the code side, so a later consistency-minded refactor cannot route the resurrect
back through conditionalWriteSettings unnoticed."
```

---

## Final verification

- [ ] **Full gate, both lanes**

```bash
nohup setsid bash tmp/run_ca_gate_both.sh > /dev/null 2>&1 < /dev/null &
grep -E "_EXIT=" build/ca_gate_both.log
```

- [ ] **Stateless CAS lanes**

```bash
python3 -m ci.praktika run "Stateless tests (amd_binary, cas storage, parallel)" > build/final_stateless.log 2>&1
```

- [ ] **Soak — this is a write-path change on a rarely-taken branch**

The resurrect arm is reached only when a per-hash meta read observes `Condemned`, which unit tests force and ordinary traffic reaches rarely. Run a soak so the path is exercised under real concurrency:

```bash
cd utils/ca-soak && python3 -m soak.run --phase 3 --duration 20m --metrics ../../build/soak_resurrect.sqlite
```

Expected: `PHASE3 OK`, `SOAK_EXIT=0`, closing fsck `dangling=0 chain_broken=0 lifeless_keys=0`.

---

## Notes for the implementer

**The invariant, stated once more because everything else is negotiable and this is not.** The
resurrected body must carry a FRESH `incarnation_tag`. That is what makes its ETag differ from the
condemned incarnation, so GC's already-queued exact-token delete misses it. Reusing the source header
— or server-side copying the condemned object — reproduces the condemned ETag and lets that queued
delete destroy the live resurrection. This is a data-loss bug, and it is invisible in any test that
does not run a GC round afterwards.

**Never read the condemned object.** Revival is always a fresh write from the writer's own source.
`get`/`copyObject` on the condemned key is forbidden even when it looks like the cheapest path.

**`putOverwrite(String)` stays.** Root manifests, `gc/state` and mount records read-modify-write small
bodies in memory; they are not what this plan is about.

**If a test fails in `CaWiring*` or `CASProtocolScenarios*`, suspect the change, not the test.** Those
suites encode the condemn/resurrect ordering and have caught real regressions on this exact path.

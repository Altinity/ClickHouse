# PR #2073 sanitizer-abort fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the PR #2073 sanitizer CI lanes green by fixing three distinct pre-existing findings surfaced by the first sanitizer run — two `LOGICAL_ERROR`-in-`EXPECT_THROW` unit aborts (GCS conditional dialect; dedup-log null-writer) and one UBSan `memcpy`-null undefined behaviour on an empty file read.

**Architecture:** Findings #1 and #2 are *test-hygiene* fixes: the production `LOGICAL_ERROR` is CORRECT (a genuine broken invariant), it just aborts at construction under `abort_on_logical_error` (debug/sanitizer builds) instead of being catchable, so the affected `EXPECT_THROW` tests are split into a release-build `EXPECT_THROW` arm plus a sanitizer-build `EXPECT_DEATH` arm — the repo's established pattern (`0d5f0be10c5`, `99879af4aca`, `def79031982`). Production code is NOT changed for #1/#2 and the error code is NOT downgraded. Finding #4 is a *production* one-line defensive guard on an upstream `memcpy` plus a unit test.

**Tech Stack:** C++ (ClickHouse), GoogleTest (`EXPECT_THROW`/`EXPECT_DEATH`), `DEBUG_OR_SANITIZER_BUILD` macro (`base/base/sanitizer_defs.h`), ninja builds (`build` plain, `build_asan` address, `build_debug` Debug — the latter two define `DEBUG_OR_SANITIZER_BUILD` and enable `abort_on_logical_error`).

## Global Constraints

- Branch `cas-gc-rebuild`, SHARED with a parallel session: NEW commits only (never rebase/amend); `git add` only the exact files you changed BY PATH (never `git add -A`/`.`); verify `git diff --cached --name-only` before every commit and verify HEAD after; do NOT `git push` (a separate per-instance mandate is required).
- For #1 and #2: keep the production `LOGICAL_ERROR` exactly as-is (broken-invariant signal). Do NOT change `GCSConditionalDialect.cpp` or `MergeTreeDeduplicationLog`'s throw sites. Do NOT downgrade the code to `BAD_ARGUMENTS`/other. The fix lives ENTIRELY in the test files.
- Death-test convention: the `EXPECT_THROW` (release) arm is guarded `#ifndef DEBUG_OR_SANITIZER_BUILD`; the parallel `EXPECT_DEATH` arm is guarded `#if defined(DEBUG_OR_SANITIZER_BUILD)`; the death suite name carries a `DeathTest` suffix (gtest runs `*DeathTest` suites first). `EXPECT_DEATH({ <call>; }, "<regex>")` — the regex may be `""` (any output) or a stable substring of the message.
- C++ Allman braces (opening brace on its own line).
- Build ONE ninja at a time, in the FOREGROUND, redirect to a log under the build dir, read the exit code before proceeding.
- Verification builds: #1/#2 death arms run under `build_asan` (or `build_debug`); the release arms run under plain `build`. #4's UBSan RED reproduces only under a UBSan build (the CI `amd_asan_ubsan` lane) — there is no local UBSan build, so #4 is verified locally by its unit test (which passes on all local builds) plus code review, and confirmed on CI.
- RED is already established for #1/#2: the current `build_asan/src/unit_tests_dbms` aborts at `GCSConditionalDialect.NonNumericIfMatchThrows` and at `DeduplicationLogNullWriterFixture.AddPartThrowsLogicalErrorInsteadOfCrashing` (reproduced from a clean CWD). Excluding GCS + `DeduplicationLogNullWriterFixture` + the unrelated `SilkFiberSocketTest*` yields a clean run (`12954` tests, 0 aborts) — so #1 and #2 are the ONLY `LOGICAL_ERROR` unit aborts.
- Out of scope: `SilkFiberSocketTest/1` (SecurePolicy) `contrib/silk` fiber assertion — pre-existing Altinity silk-fiber feature, not CAS, not fixed here.

---

### Task 1: GCS conditional dialect — split the 3 throwing tests into release + death arms

**Files:**
- Modify: `src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp` (tests `NonNumericIfMatchThrows` :46, `NonStarIfNoneMatchThrows` :53, `ConditionalCompleteMultipartUploadThrows` :82)
- Do NOT touch: `src/IO/S3/GCSConditionalDialect.cpp` (the three `LOGICAL_ERROR` throws stay).

**Interfaces:**
- Consumes: `applyGcsConditionalDialectToRequest(Aws::Http::HttpRequest &)` (throws `LOGICAL_ERROR` on the three guarded conditions); the file-local `static Aws::Http::Standard::StandardHttpRequest makeRequest(url=..., method=HTTP_PUT)`.
- Produces: unchanged public test behaviour on release builds; new `GCSConditionalDialectDeathTest.*` suite on sanitizer/debug builds.

- [ ] **Step 1: Confirm RED (reference — already reproduced).** Under `build_asan`, running `GCSConditionalDialect.*` aborts at `NonNumericIfMatchThrows`. No new action needed beyond noting it; the abort is why the CI tsan/asan unit lanes fail.

- [ ] **Step 2: Ensure the macro is available.** At the top of `gtest_gcs_conditional_dialect.cpp`, after the existing includes, add:

```cpp
#include <base/defines.h>   /// DEBUG_OR_SANITIZER_BUILD
```

- [ ] **Step 3: Replace the three throwing tests** (`NonNumericIfMatchThrows`, `NonStarIfNoneMatchThrows`, `ConditionalCompleteMultipartUploadThrows`) with the split form. Leave every other test in the file unchanged.

```cpp
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(GCSConditionalDialect, NonNumericIfMatchThrows)
{
    /// The guard throws LOGICAL_ERROR (a broken-invariant signal: an S3-style ETag reached a
    /// generation-dialect client). Under abort_on_logical_error that aborts at construction instead of
    /// being catchable, so GCSConditionalDialectDeathTest.NonNumericIfMatchAborts proves it there.
    auto r = makeRequest();
    r.SetHeaderValue("if-match", "\"6654c734ccab8f440ff0825eb443dc7f\"");  /// an ETag leaked into a generation dialect
    EXPECT_THROW(applyGcsConditionalDialectToRequest(r), DB::Exception);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(GCSConditionalDialectDeathTest, NonNumericIfMatchAborts)
{
    auto r = makeRequest();
    r.SetHeaderValue("if-match", "\"6654c734ccab8f440ff0825eb443dc7f\"");
    EXPECT_DEATH({ applyGcsConditionalDialectToRequest(r); }, "");
}
#endif

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(GCSConditionalDialect, NonStarIfNoneMatchThrows)
{
    /// LOGICAL_ERROR (broken invariant); aborts under abort_on_logical_error -- see the DeathTest below.
    auto r = makeRequest();
    r.SetHeaderValue("if-none-match", "\"123\"");
    EXPECT_THROW(applyGcsConditionalDialectToRequest(r), DB::Exception);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(GCSConditionalDialectDeathTest, NonStarIfNoneMatchAborts)
{
    auto r = makeRequest();
    r.SetHeaderValue("if-none-match", "\"123\"");
    EXPECT_DEATH({ applyGcsConditionalDialectToRequest(r); }, "");
}
#endif

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST(GCSConditionalDialect, ConditionalCompleteMultipartUploadThrows)
{
    /// GCS silently IGNORES preconditions on CompleteMultipartUpload (measured live 2026-07-03) --
    /// sending one would be silent data loss, so the dialect fails closed client-side with a
    /// LOGICAL_ERROR; aborts under abort_on_logical_error -- see the DeathTest below.
    auto r = makeRequest("https://storage.googleapis.com/b/k?uploadId=abc", Aws::Http::HttpMethod::HTTP_POST);
    r.SetHeaderValue("if-none-match", "*");
    EXPECT_THROW(applyGcsConditionalDialectToRequest(r), DB::Exception);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST(GCSConditionalDialectDeathTest, ConditionalCompleteMultipartUploadAborts)
{
    auto r = makeRequest("https://storage.googleapis.com/b/k?uploadId=abc", Aws::Http::HttpMethod::HTTP_POST);
    r.SetHeaderValue("if-none-match", "*");
    EXPECT_DEATH({ applyGcsConditionalDialectToRequest(r); }, "");
}
#endif
```

- [ ] **Step 4: Build + verify GREEN under a sanitizer build.**

Run: `ninja -C build_asan unit_tests_dbms > build_asan/build_t1.log 2>&1` (foreground; check exit).
Then from a CLEAN cwd (avoids the `CoordinationTest ./logs` collision):
`cd "$(mktemp -d)" && ASAN_OPTIONS=abort_on_error=1 <repo>/build_asan/src/unit_tests_dbms --gtest_filter='GCSConditionalDialect*' > gcs_t1.log 2>&1; echo EXIT=$?`
Expected: EXIT=0; the `GCSConditionalDialectDeathTest.*Aborts` tests run and pass (`[ OK ]`), the non-throwing `GCSConditionalDialect.*` tests still pass, no SIGABRT.

- [ ] **Step 5: Verify GREEN on a plain build (release arm).**

Run: `ninja -C build unit_tests_dbms > build/build_t1.log 2>&1` then
`./build/src/unit_tests_dbms --gtest_filter='GCSConditionalDialect*'`
Expected: the `#ifndef` `EXPECT_THROW` tests run and pass; the `*DeathTest` suite is compiled out (absent). All pass.

- [ ] **Step 6: Commit.**

```bash
git add src/IO/S3/tests/gtest_gcs_conditional_dialect.cpp
git diff --cached --name-only   # must show exactly that one file
git commit -m "cas: death-test the GCS conditional-dialect LOGICAL_ERROR guards (PR#2073 sanitizer abort)"
```

---

### Task 2: Dedup-log null-writer — split the 2 throwing tests into release + death arms

**Files:**
- Modify: `src/Storages/MergeTree/tests/gtest_deduplication_log_null_writer.cpp` (tests `AddPartThrowsLogicalErrorInsteadOfCrashing` :66, `DropPartThrowsLogicalErrorInsteadOfCrashing` :91)
- Do NOT touch: `MergeTreeDeduplicationLog` production code (the `LOGICAL_ERROR "no writer"` throws in `addPart`/`dropPart` stay).

**Interfaces:**
- Consumes: the existing `DeduplicationLogNullWriterFixture` (leaves `current_writer` null); `log->addPart({...}, part_info)` / `log->dropPart(part_info)` throw `LOGICAL_ERROR` with a message containing `"no writer"`.
- Produces: unchanged release behaviour; new `DeduplicationLogNullWriterDeathTest` suite (a fixture alias) on sanitizer/debug builds.

- [ ] **Step 1: Confirm RED (reference — already reproduced).** Under `build_asan`, `DeduplicationLogNullWriterFixture.AddPartThrowsLogicalErrorInsteadOfCrashing` aborts.

- [ ] **Step 2: Add the macro include.** After the existing includes (near `#include <Common/Exception.h>`), add:

```cpp
#include <base/defines.h>   /// DEBUG_OR_SANITIZER_BUILD
```

- [ ] **Step 3: Add a death-test fixture alias.** After the `DeduplicationLogNullWriterFixture` struct definition (and its closing `}` of the anonymous namespace, i.e. below line 64), add:

```cpp
#if defined(DEBUG_OR_SANITIZER_BUILD)
/// gtest runs *DeathTest suites before others; reuse the same fixture via an alias so the death arm
/// gets the same null-writer precondition.
using DeduplicationLogNullWriterDeathTest = DeduplicationLogNullWriterFixture;
#endif
```

(If `DeduplicationLogNullWriterFixture` is inside the anonymous namespace, place the `using` alias in the same translation unit AFTER the namespace closes; it will still see the type. If the compiler cannot see it due to the anonymous namespace, move the `using` inside a second anonymous-namespace block right after the first.)

- [ ] **Step 4: Guard the two existing `TEST_F`s with `#ifndef DEBUG_OR_SANITIZER_BUILD`** (wrap each of the two existing tests, lines 66-89 and 91-109, unchanged in body, in the `#ifndef … #endif`), and add the two death-arm tests. Final shape:

```cpp
#ifndef DEBUG_OR_SANITIZER_BUILD
TEST_F(DeduplicationLogNullWriterFixture, AddPartThrowsLogicalErrorInsteadOfCrashing)
{
    /// LOGICAL_ERROR "no writer" is a broken-invariant guard (addPart on a null current_writer). Under
    /// abort_on_logical_error it aborts at construction instead of being catchable -- the DeathTest
    /// below proves the abort in those builds.
    auto part_info = MergeTreePartInfo::fromPartName("all_0_0_0", FORMAT_VERSION);

    EXPECT_THROW(
        {
            try
            {
                log->addPart({"block-1"}, part_info);
            }
            catch (const Exception & e)
            {
                EXPECT_EQ(e.code(), ErrorCodes::LOGICAL_ERROR);
                EXPECT_NE(e.message().find("no writer"), std::string::npos);
                throw;
            }
        },
        Exception);

    EXPECT_THROW(log->addPart({"block-1"}, part_info), Exception);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST_F(DeduplicationLogNullWriterDeathTest, AddPartAborts)
{
    auto part_info = MergeTreePartInfo::fromPartName("all_0_0_0", FORMAT_VERSION);
    EXPECT_DEATH({ log->addPart({"block-1"}, part_info); }, "no writer");
}
#endif

#ifndef DEBUG_OR_SANITIZER_BUILD
TEST_F(DeduplicationLogNullWriterFixture, DropPartThrowsLogicalErrorInsteadOfCrashing)
{
    auto part_info = MergeTreePartInfo::fromPartName("all_0_0_0", FORMAT_VERSION);

    EXPECT_THROW(
        {
            try
            {
                log->dropPart(part_info);
            }
            catch (const Exception & e)
            {
                EXPECT_EQ(e.code(), ErrorCodes::LOGICAL_ERROR);
                EXPECT_NE(e.message().find("no writer"), std::string::npos);
                throw;
            }
        },
        Exception);
}
#endif

#if defined(DEBUG_OR_SANITIZER_BUILD)
TEST_F(DeduplicationLogNullWriterDeathTest, DropPartAborts)
{
    auto part_info = MergeTreePartInfo::fromPartName("all_0_0_0", FORMAT_VERSION);
    EXPECT_DEATH({ log->dropPart(part_info); }, "no writer");
}
#endif
```

Note: `EXPECT_DEATH`'s regex is `"no writer"` — a stable substring of the guard message; if the abort output does not surface the message (some sanitizer configs print only the stack), fall back to `""`.

- [ ] **Step 5: Build + verify GREEN under sanitizer.**

`ninja -C build_asan unit_tests_dbms > build_asan/build_t2.log 2>&1` (foreground; check exit), then from a clean cwd:
`ASAN_OPTIONS=abort_on_error=1 <repo>/build_asan/src/unit_tests_dbms --gtest_filter='DeduplicationLogNullWriter*' > dedup_t2.log 2>&1; echo EXIT=$?`
Expected: EXIT=0; `DeduplicationLogNullWriterDeathTest.AddPartAborts`/`DropPartAborts` pass; no SIGABRT.

- [ ] **Step 6: Verify GREEN on plain build.**

`ninja -C build unit_tests_dbms > build/build_t2.log 2>&1` then
`./build/src/unit_tests_dbms --gtest_filter='DeduplicationLogNullWriter*'`
Expected: the two `#ifndef` `TEST_F`s pass; the `*DeathTest` suite is compiled out. All pass.

- [ ] **Step 7: Commit.**

```bash
git add src/Storages/MergeTree/tests/gtest_deduplication_log_null_writer.cpp
git diff --cached --name-only   # exactly that one file
git commit -m "cas: death-test the dedup-log null-writer LOGICAL_ERROR guards (PR#2073 sanitizer abort)"
```

---

### Task 3: ReadBufferFromMemory — guard the empty-file `memcpy` (UBSan nonnull, STID 5930-5afa)

**Files:**
- Modify: `src/IO/ReadBufferFromMemory.cpp` (the `owns_memory` `std::memcpy` at lines 79-80)
- Create: `src/IO/tests/gtest_read_buffer_from_memory.cpp` (new focused unit test)

**Interfaces:**
- Consumes: `ReadBufferFromMemoryFileBase(bool owns_memory, String file_name_, std::string_view data)`.
- Produces: constructing with `owns_memory=true` and empty `data` no longer executes `memcpy(dst, nullptr, 0)`.

- [ ] **Step 1: Write the failing test** — create `src/IO/tests/gtest_read_buffer_from_memory.cpp`:

```cpp
#include <gtest/gtest.h>

#include <IO/ReadBufferFromMemory.h>

#include <string_view>

using namespace DB;

/// An empty file materialized into an OWNED in-memory buffer must construct without undefined
/// behaviour: std::memcpy's pointer arguments are __attribute__((nonnull)), so memcpy(dst, nullptr, 0)
/// -- which an empty std::string_view (data() == nullptr) produces -- is UB that the asan_ubsan lane
/// aborts on (STID 5930-5afa, PR #2073). The buffer must construct and be immediately at EOF.
TEST(ReadBufferFromMemoryFileBase, EmptyOwnedBufferConstructsWithoutUB)
{
    ReadBufferFromMemoryFileBase buf(/*owns_memory=*/true, "empty", std::string_view{});
    EXPECT_TRUE(buf.eof());
}
```

- [ ] **Step 2: Run it to verify it builds/links and passes on a plain build** (the UB does NOT abort without UBSan, so this passes locally regardless; it exists to document the contract and to give the CI `asan_ubsan` lane a direct RED→GREEN):

`ninja -C build unit_tests_dbms > build/build_t3.log 2>&1` (foreground; check exit — confirms the new gtest file is picked up by the `src/IO/tests` glob; if not picked up, `ninja -C build` a full reconfigure or check `src/IO/tests/CMakeLists.txt`/parent glob).
`./build/src/unit_tests_dbms --gtest_filter='ReadBufferFromMemoryFileBase.EmptyOwnedBufferConstructsWithoutUB'` → PASS.
(If the `.eof()`/ctor API does not compile as written, adjust to the real public API of `ReadBufferFromMemoryFileBase` — e.g. check `buf.buffer().size() == 0` — keeping the empty-owned construction as the point of the test.)

- [ ] **Step 3: Apply the guard** in `src/IO/ReadBufferFromMemory.cpp` — replace:

```cpp
    if (owns_memory)
        std::memcpy(internal_buffer.begin(), data.data(), data.size());
```

with:

```cpp
    /// memcpy's pointers are __attribute__((nonnull)) even when the length is 0. An empty file yields
    /// data.data() == nullptr, so guard on non-empty to avoid the nonnull-attribute UB the asan_ubsan
    /// lane aborts on (STID 5930-5afa). Nothing to copy when empty.
    if (owns_memory && !data.empty())
        std::memcpy(internal_buffer.begin(), data.data(), data.size());
```

- [ ] **Step 4: Rebuild + re-run the unit test (GREEN on plain build).**

`ninja -C build unit_tests_dbms > build/build_t3b.log 2>&1` then
`./build/src/unit_tests_dbms --gtest_filter='ReadBufferFromMemoryFileBase.EmptyOwnedBufferConstructsWithoutUB'` → PASS.
(UBSan RED→GREEN is not locally reproducible — no local UBSan build; it is confirmed on the CI `amd_asan_ubsan` lane after push.)

- [ ] **Step 5: Commit.**

```bash
git add src/IO/ReadBufferFromMemory.cpp src/IO/tests/gtest_read_buffer_from_memory.cpp
git diff --cached --name-only   # exactly those two files
git commit -m "cas: guard empty-file memcpy in ReadBufferFromMemory to avoid UBSan nonnull UB (PR#2073, STID 5930-5afa)"
```

---

## Notes for the executor

- Task order is independent; do them in any order. Each is a self-contained commit.
- The single most likely gotcha: `DEBUG_OR_SANITIZER_BUILD` not resolving in a test TU — the explicit `#include <base/defines.h>` (Task 1 Step 2, Task 2 Step 2) prevents it. Verify with `grep -c 'define DEBUG_OR_SANITIZER_BUILD' base/base/sanitizer_defs.h` if in doubt.
- `EXPECT_DEATH` forks; under a fixture (`TEST_F`) the fixture `SetUp` runs in the parent, then the death statement runs in the fork. This is the same pattern the repo already uses (`gtest_cas_gc_state_format.cpp`, `gtest_cas_ref_cow_manifest_set.cpp`).
- After all three, the CI `amd_asan_ubsan` / `tsan` unit lanes should pass GCS + dedup; the `amd_asan_ubsan` stateless lane should clear STID 5930-5afa. `SilkFiberSocketTest/1` is deliberately NOT addressed — if it then blocks a lane, escalate it as a separate Altinity silk-fiber/sanitizer triage.

## Self-Review

- Spec coverage: #1 (Task 1), #2 (Task 2), #4 (Task 3). #3 Silk excluded per instruction. ✓
- Placeholder scan: all code blocks are complete; the two "adjust if the API doesn't compile" notes are genuine fallbacks, not placeholders. ✓
- Type/name consistency: death suites `GCSConditionalDialectDeathTest` / `DeduplicationLogNullWriterDeathTest` used consistently; fixture alias defined before use. ✓
- Production code changed ONLY in Task 3 (the guard); #1/#2 keep `LOGICAL_ERROR` untouched per the design decision. ✓

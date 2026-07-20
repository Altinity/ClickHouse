# CasJsonWriter Bulk-Append Encoding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `WriteBuffer` in all CAS text-format encode paths with a flat bulk-append writer (`CasJsonWriter`) so hotpath serialization lands within 3× of a plain memcpy floor, with byte-identical output.

**Architecture:** A new `CasJsonWriter` class in `Formats/CasTextFormat.h` owns a `String` and appends inline (no `WriteBuffer` lifecycle, no per-byte calls, zero per-record heap allocations). The shared write-side vocabulary (`writeKey`, `writeStringValue`, …) grows `CasJsonWriter` overloads; the ~12 format codecs migrate mechanically; the old `WriteBuffer` overloads are then deleted. The one streaming format (`cas_run`) assembles each NDJSON line in a reused scratch writer and issues one bulk `WriteBuffer::write` per line.

**Tech Stack:** C++ (ClickHouse tree), gtest (`unit_tests_dbms`), Google Benchmark (`benchmark_cas_ref_protocol`, already `ENABLE_BENCHMARKS=ON` in `build/`).

**Spec:** `docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md`

## Global Constraints

- **Byte-identical output.** Every encoded CAS object must keep exactly the bytes the current implementation produces. Task 1 pins them; the pinned literals MUST NEVER be edited after Task 1's commit.
- **No trust-based shortcuts.** One escaping `stringValue` for all strings; no "pre-validated raw" string writer.
- **Streaming stays streaming.** `cas_run` memory is bounded by one line (`line_cap` 4 KiB), never by record count.
- **Untouched:** the read side (`JsonObjectReader`, all decode functions), `src/IO/WriteHelpers.h`, and every on-wire byte.
- **Allman braces** in all C++ (style check enforces this).
- Branch: work on `cas-gc-rebuild` (current). Never commit to `master`. No rebase/amend — new commits only.
- Build dir: `build/`. Always redirect build output to a log file in the build dir; use a subagent to summarize build/test logs.
- Unit test gate filter (the CORRECTED one — `Cas*:CA*` alone under-tests):
  `Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*`
- Format type words (exact, from `CasFormat.cpp`): `cas_ref_log`, `cas_ref_snap`, `cas_run`, `cas_part_manifest`, `cas_gc_state`, `cas_gc_outcomes`, `cas_fold_seal`, `cas_pool_meta`, `cas_blob_meta`, `cas_blob`. `currentCompatibilityVersion` is 3.

---

### Task 1: Pin canonical encoder bytes (pre-change golden corpus)

Pins today's exact bytes for the three formats this plan touches most deeply, BEFORE any production change. Later tasks must keep these tests green unmodified.

**Files:**
- Create: `src/Disks/tests/gtest_cas_encoding_pins.cpp`
- Modify: none (production untouched in this task)

**Interfaces:**
- Consumes: `encodeRefLogTxn` (`Formats/CasRefLogFormat.h`), `encodeRefTableSnapshot` (`Formats/CasRefSnapshotFormat.h`), `SourceEdgeRunWriter` (`Formats/CasRecordStreamFormat.h`) — all existing.
- Produces: test suite `CasEncodingPins` (name must start with `Cas` to match the gate filter). Tasks 5–9 rerun it as their byte-identity gate.

- [ ] **Step 1: Write the pin test**

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>
#include <IO/WriteBufferFromString.h>

using namespace DB;
using namespace DB::Cas;

/// These literals pin the CANONICAL BYTES of the CAS text encoders as of the commit that
/// introduced this file. The CasJsonWriter migration (2026-07-20 spec) must keep every one of
/// them green UNMODIFIED: canonical text is byte-compared on retries and deterministic adoption,
/// and the incremental ref budget counters assume these exact sizes. Never edit an expected
/// string here to make a test pass — that means the encoder's bytes drifted, which is the bug.

TEST(CasEncodingPins, RefLogTxnAllOpKinds)
{
    RefLogTxn txn;
    txn.ns = "roots/pin";
    txn.txn_id = RefTxnId{7, 9};

    RefOp birth;
    birth.kind = RefOpKind::NamespaceBirth;
    txn.ops.push_back(birth);

    RefOp transition;
    transition.kind = RefOpKind::OwnerTransition;
    transition.old_binding = RefOwnerBinding{RefOwnerKind::Precommit, "20260101_0_1_1_1", ManifestRef{1, 2, 3}};
    transition.new_binding = RefOwnerBinding{RefOwnerKind::Committed, "20260101_0_1_1_1", ManifestRef{1, 2, 3}};
    txn.ops.push_back(transition);

    RefOp payload;
    payload.kind = RefOpKind::SetPayload;
    payload.ref_name = "20260101_0_1_1_1";
    payload.expected_manifest_ref = ManifestRef{1, 2, 3};
    /// NOTE the split literals: "\x01" "e" (else the hex escape would swallow the 'e') and
    /// "\xA8" "f" (else it would swallow the 'f'). The payload exercises quote, backslash,
    /// newline, a bare control byte, and the three-byte U+2028 sequence.
    payload.payload = String("a\"b\\c\nd") + "\x01" "e" + "\xE2\x80\xA8" "f";
    payload.published_at_ms = 1234;
    txn.ops.push_back(payload);

    RefOp removal;
    removal.kind = RefOpKind::RemoveNamespace;
    txn.ops.push_back(removal);

    const String expected =
        "{\"type\":\"cas_ref_log\",\"v\":3}\n"
        "{\"ns\":\"roots/pin\",\"we\":\"7\",\"rs\":\"9\"}\n"
        "{\"op\":\"namespace_birth\"}\n"
        "{\"op\":\"owner_transition\",\"obk\":\"precommit\",\"orn\":\"20260101_0_1_1_1\","
        "\"ome\":\"1\",\"omb\":\"2\",\"omo\":3,\"nbk\":\"committed\",\"nrn\":\"20260101_0_1_1_1\","
        "\"nme\":\"1\",\"nmb\":\"2\",\"nmo\":3}\n"
        "{\"op\":\"set_payload\",\"rn\":\"20260101_0_1_1_1\",\"me\":\"1\",\"mb\":\"2\",\"mo\":3,"
        "\"pl\":\"a\\\"b\\\\c\\nd\\u0001e\\u2028f\",\"ts\":1234}\n"
        "{\"op\":\"remove_namespace\"}\n"
        "{\"n\":4}\n";
    EXPECT_EQ(encodeRefLogTxn(txn), expected);
}

TEST(CasEncodingPins, RefSnapshotLiveWithSealedFrom)
{
    RefTableSnapshot snap;
    snap.ns = "roots/pin";
    snap.snapshot_id = RefTxnId{7, 9};
    snap.lifecycle = RefLifecycle::Live;
    snap.sealed_from = RefTxnId{7, 8};

    RefCommittedRow row;
    row.ref_name = "20260101_0_1_1_1";
    row.manifest_ref = ManifestRef{1, 2, 3};
    row.payload = "p";
    row.published_at_ms = 5;
    snap.committed.push_back(row);

    snap.precommits.push_back(RefOwnerBinding{RefOwnerKind::Precommit, "20260102_0_2_2_2", ManifestRef{4, 5, 6}});

    const String expected =
        "{\"type\":\"cas_ref_snap\",\"v\":3}\n"
        "{\"ns\":\"roots/pin\",\"we\":\"7\",\"rs\":\"9\",\"lc\":\"live\",\"sfe\":\"7\",\"sfs\":\"8\"}\n"
        "{\"k\":\"c\",\"rn\":\"20260101_0_1_1_1\",\"me\":\"1\",\"mb\":\"2\",\"mo\":3,\"pl\":\"p\",\"ts\":5}\n"
        "{\"k\":\"p\",\"rn\":\"20260102_0_2_2_2\",\"me\":\"4\",\"mb\":\"5\",\"mo\":6}\n"
        "{\"n\":2}\n";
    EXPECT_EQ(encodeRefTableSnapshot(snap), expected);
}

TEST(CasEncodingPins, SourceEdgeRunLines)
{
    WriteBufferFromOwnString out;
    SourceEdgeRunWriter writer(out);

    SourceEdgeRecord active;
    active.ref = BlobRef{BlobHashAlgo::CityHash128, UInt128(2)};
    active.source_id = UInt128(5);
    active.marker = kEdgeActive;
    writer.append(active);

    writer.finish();
    out.finalize();

    /// The exact "b" rendering (algo byte + digest hex) is pinned as a whole line; the point is
    /// that Task 8's line-scratch rewrite must reproduce it byte-for-byte.
    const String text = out.str();
    EXPECT_TRUE(text.starts_with("{\"type\":\"cas_run\",\"v\":3,\"kind\":\"source_edge\"}\n")) << text;
    EXPECT_TRUE(text.ends_with("{\"n\":1}\n")) << text;
    /// Pin the full middle line too:
    const String expected_record =
        "{\"b\":\"0100000000000000000000000000000002\",\"s\":\"00000000000000000000000000000005\",\"m\":\"edge\"}\n";
    EXPECT_NE(text.find(expected_record), String::npos) << text;
}
```

Note for the implementer: field/constructor shapes for `RefLogTxn`, `RefOp`, `RefOwnerBinding`, `ManifestRef`, `RefTableSnapshot`, `RefCommittedRow`, `BlobRef` are declared in the included Formats/Primitives headers; if an initializer above doesn't compile (e.g. `snap.committed` is not a plain vector, or `BlobRef` aggregates differently), adapt the CONSTRUCTION code to the real types — never the expected strings' framing.

- [ ] **Step 2: Register the test in the build if needed**

Check whether `src/Disks/tests/*.cpp` are globbed automatically: `grep -rn "gtest_cas_ref_log_format\|GLOB" src/Disks/tests/CMakeLists.txt src/CMakeLists.txt | head`. If existing `gtest_cas_*` files are picked up by a glob (they are in most ClickHouse trees), no CMake change is needed.

- [ ] **Step 3: Build and run — expect possible literal corrections**

```bash
ninja -C build unit_tests_dbms > build/build_task1.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasEncodingPins.*' > build/test_task1.log 2>&1; tail -30 build/test_task1.log
```

Expected: PASS. If a pin fails on THIS first run, the derived literal is wrong, not the encoder — inspect gtest's actual-vs-expected diff, confirm the difference is only in the literal's derivation (e.g. a different digest rendering in `b`), and fix the LITERAL to match the current encoder's actual output. This correction is legal ONLY in this task, before any production change. From the moment of this task's commit, the literals are frozen.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/tests/gtest_cas_encoding_pins.cpp
git commit -m "cas: pin canonical encoder bytes for ref log / ref snapshot / source-edge run

Pre-change golden corpus for the CasJsonWriter bulk-encoding migration
(docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: `CasJsonWriter` core (everything except string escaping)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h` (add class after the includes, before the free-function vocabulary)
- Create: `src/Disks/tests/gtest_cas_json_writer.cpp`

**Interfaces:**
- Produces (used by every later task):
  - `class DB::Cas::CasJsonWriter` with methods `explicit CasJsonWriter(size_t reserve_hint = 256)`, `void append(std::string_view)`, `void appendChar(char)`, `void key(std::string_view name, bool & first)`, `void key(std::string_view prefix, std::string_view name, bool & first)`, `void stringValue(std::string_view)` (declared now, defined in Task 3), `void u64Number(uint64_t)`, `void u64StringValue(uint64_t)`, `void hex128Value(const UInt128 &)`, `void boolValue(bool)`, `void closeObject(bool & first)`, `void newline()`, `size_t size() const`, `std::string_view view() const`, `void clear()`, `String take() &&`.

- [ ] **Step 1: Write failing tests**

In `src/Disks/tests/gtest_cas_json_writer.cpp`:

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h>

using namespace DB;
using namespace DB::Cas;

TEST(CasJsonWriter, KeyValueSequenceMatchesCanonicalShape)
{
    CasJsonWriter w;
    bool first = true;
    w.key("we", first);
    w.u64StringValue(7);
    w.key("mo", first);
    w.u64Number(3);
    w.key("ok", first);
    w.boolValue(true);
    w.key("o", "me", first);
    w.u64StringValue(1);
    w.closeObject(first);
    w.newline();
    EXPECT_EQ(std::move(w).take(), "{\"we\":\"7\",\"mo\":3,\"ok\":true,\"ome\":\"1\"}\n");
}

TEST(CasJsonWriter, EmptyObjectAndClear)
{
    CasJsonWriter w;
    bool first = true;
    w.closeObject(first);
    EXPECT_EQ(w.view(), "{}");
    w.clear();
    EXPECT_EQ(w.size(), 0u);
}

TEST(CasJsonWriter, Hex128MatchesU128ToHex)
{
    const UInt128 v = (UInt128(0x0123456789abcdefULL) << 64) | UInt128(0xfedcba9876543210ULL);
    CasJsonWriter w;
    w.hex128Value(v);
    EXPECT_EQ(std::move(w).take(), "\"" + u128ToHex(v) + "\"");
}

TEST(CasJsonWriter, U64Extremes)
{
    CasJsonWriter w;
    w.u64Number(0);
    w.appendChar(' ');
    w.u64Number(UINT64_MAX);
    EXPECT_EQ(std::move(w).take(), "0 18446744073709551615");
}
```

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/build_task2a.log 2>&1; echo EXIT=$?
```
Expected: compile FAILURE (`CasJsonWriter` not declared).

- [ ] **Step 3: Implement the class**

In `CasTextFormat.h`, add to the includes: `#include <base/itoa.h>` and `#include <base/hex.h>`. Then, inside `namespace DB::Cas`, before the free-function vocabulary declarations:

```cpp
/// Bulk-append writer for canonical CAS JSON text. Replaces WriteBuffer in every CAS encode
/// path: appends are inline stores into an owned String (no per-call finalized/canceled
/// lifecycle, no per-byte writes, no heap allocations per record). Two usage modes:
/// whole-object assembly (bounded formats; `take` at the end) and line-scratch (RecordStream:
/// assemble one line, bulk-write it to the surrounding WriteBuffer, `clear` — memory stays
/// bounded by the largest line). The JSON escaping semantics of `stringValue` are statically
/// fixed to the CAS canon (forward slashes NOT escaped); process-wide FormatSettings cannot
/// influence CAS bytes.
class CasJsonWriter
{
public:
    explicit CasJsonWriter(size_t reserve_hint = 256) { buf.reserve(reserve_hint); }

    void append(std::string_view s) { buf.append(s.data(), s.size()); }
    void appendChar(char c) { buf.push_back(c); }

    /// '{' on the first call, ',' after, then "name": . `name` must be plain ASCII (written raw).
    void key(std::string_view name, bool & first)
    {
        appendChar(first ? '{' : ',');
        first = false;
        appendChar('"');
        append(name);
        append("\":");
    }

    /// Same, for the prefixed key vocabulary ("o"/"n" + "me"/"mb"/"mo"/"bk"/"rn") — the
    /// prefix and name are appended back to back, no composed temporary.
    void key(std::string_view prefix, std::string_view name, bool & first)
    {
        appendChar(first ? '{' : ',');
        first = false;
        appendChar('"');
        append(prefix);
        append(name);
        append("\":");
    }

    /// Quoted JSON string with full escaping (bulk-run scan). Defined in CasTextFormat.cpp.
    void stringValue(std::string_view s);

    void u64Number(uint64_t v)
    {
        char digits[20];
        char * end = itoa(v, digits);
        buf.append(digits, static_cast<size_t>(end - digits));
    }

    void u64StringValue(uint64_t v)
    {
        appendChar('"');
        u64Number(v);
        appendChar('"');
    }

    void hex128Value(const UInt128 & v)
    {
        char hex[32];
        writeHexUIntLowercase(v, hex);
        appendChar('"');
        buf.append(hex, sizeof(hex));
        appendChar('"');
    }

    void boolValue(bool v) { append(v ? std::string_view{"true"} : std::string_view{"false"}); }

    void closeObject(bool & first)
    {
        if (first)
            appendChar('{');
        first = false;
        appendChar('}');
    }

    void newline() { appendChar('\n'); }

    size_t size() const { return buf.size(); }
    std::string_view view() const { return buf; }
    void clear() { buf.clear(); }
    String take() && { return std::move(buf); }

private:
    String buf;
};
```

For this task only, add a temporary empty definition in `CasTextFormat.cpp` so the link succeeds (Task 3 replaces it):

```cpp
void CasJsonWriter::stringValue(std::string_view s)
{
    /// Placeholder until the bulk-escaping implementation lands (next commit); no caller yet.
    appendChar('"');
    append(s);
    appendChar('"');
}
```

- [ ] **Step 4: Build and run**

```bash
ninja -C build unit_tests_dbms > build/build_task2b.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasJsonWriter.*' > build/test_task2.log 2>&1; tail -20 build/test_task2.log
```
Expected: all `CasJsonWriter.*` PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp \
        src/Disks/tests/gtest_cas_json_writer.cpp
git commit -m "cas: CasJsonWriter bulk-append core (keys, numbers, hex128, bool, framing)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: `stringValue` bulk-run escaping + differential fuzz vs `writeJSONString`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h` (declare scan helpers), `.../CasTextFormat.cpp` (real `stringValue`, scan implementations)
- Test: `src/Disks/tests/gtest_cas_json_writer.cpp` (extend)

**Interfaces:**
- Produces: `const char * findNextSpecialJsonByte(const char * pos, const char * end)` (dispatch) and `const char * findNextSpecialJsonByteScalar(const char * pos, const char * end)` (reference path, exposed for tests) declared in `CasTextFormat.h`; final `CasJsonWriter::stringValue`.

- [ ] **Step 1: Write the failing differential tests**

Append to `gtest_cas_json_writer.cpp`:

```cpp
#include <IO/WriteBufferFromString.h>
#include <IO/WriteHelpers.h>
#include <Formats/FormatSettings.h>
#include <random>

namespace
{
String referenceJson(std::string_view s)
{
    DB::FormatSettings settings;
    settings.json.escape_forward_slashes = false;   /// the pinned CAS canon
    DB::WriteBufferFromOwnString out;
    DB::writeJSONString(s, out, settings);
    out.finalize();
    return out.str();
}

String writerJson(std::string_view s)
{
    DB::Cas::CasJsonWriter w;
    w.stringValue(s);
    return std::move(w).take();
}
}

TEST(CasJsonWriterEscaping, TargetedCorpusMatchesWriteJSONString)
{
    const std::vector<String> corpus = {
        "",
        "plain_safe_ref_name_20260101_0_1_1_1",
        "roots/pin",                                    /// '/' must stay UNESCAPED
        "quote\"inside", "back\\slash", "both\\\"x",
        String("\b\f\n\r\t"),
        String(1, '\0'), String("a") + '\0' + "b",
        String("\x01\x02\x03\x1e\x1f"),
        "\xE2\x80\xA8", "\xE2\x80\xA9",                 /// U+2028 / U+2029 ->   /  
        "x\xE2\x80\xA8" "y",
        "\xE2",                                          /// truncated lead byte at end
        "\xE2\x80",                                      /// truncated pair at end
        "\xE2\x21\x21",                                  /// 0xE2 + non-continuation bytes
        "\xE2\x80\x21",
        "\xE2\xE2\x80\xA8",                              /// lead byte immediately before a real sequence
        "\xC3\xA9\xF0\x9F\x98\x80",                      /// ordinary multi-byte UTF-8 passes through
        "\xff\xfe invalid utf8 \x80",
        String(1000, 'a'),                               /// long safe run (vector path)
        String(1000, '"'),                               /// special-dense
    };
    for (const String & s : corpus)
        EXPECT_EQ(writerJson(s), referenceJson(s)) << "input bytes: " << s.size();
}

TEST(CasJsonWriterEscaping, FuzzMatchesWriteJSONString)
{
    std::mt19937 rng(20260720);
    for (int iter = 0; iter < 5000; ++iter)
    {
        const size_t len = rng() % 200;
        String s(len, '\0');
        const int mode = iter % 3;
        for (auto & c : s)
        {
            if (mode == 0)
                c = static_cast<char>(rng() % 256);                     /// full byte range
            else if (mode == 1)
                c = static_cast<char>('a' + rng() % 26);                /// safe-only
            else
            {
                static constexpr char specials[] = {'"', '\\', '\n', '\x01', '\xE2', '\x80', '\xA8', 'z'};
                c = specials[rng() % (sizeof(specials))];               /// special-dense
            }
        }
        ASSERT_EQ(writerJson(s), referenceJson(s)) << "iter " << iter;
    }
}

TEST(CasJsonWriterEscaping, VectorAndScalarScansAgree)
{
    std::mt19937 rng(20260721);
    for (int iter = 0; iter < 2000; ++iter)
    {
        const size_t len = rng() % 100;
        String s(len, '\0');
        for (auto & c : s)
            c = static_cast<char>(rng() % 256);
        const char * b = s.data();
        const char * e = s.data() + s.size();
        const char * pos = b;
        while (true)
        {
            const char * v = DB::Cas::findNextSpecialJsonByte(pos, e);
            const char * sc = DB::Cas::findNextSpecialJsonByteScalar(pos, e);
            ASSERT_EQ(v, sc) << "iter " << iter << " offset " << (pos - b);
            if (v == e)
                break;
            pos = v + 1;
        }
    }
}
```

- [ ] **Step 2: Run to verify failure**

```bash
ninja -C build unit_tests_dbms > build/build_task3a.log 2>&1; echo EXIT=$?
```
Expected: compile FAILURE (`findNextSpecialJsonByte` undeclared); after declaring, the escaping tests FAIL against the Task 2 placeholder.

- [ ] **Step 3: Implement**

In `CasTextFormat.h`, after the class:

```cpp
/// Position of the next byte `stringValue` treats specially (control byte, '"', '\\', or the
/// 0xE2 lead byte of the U+2028/U+2029 lookahead), or `end`. The scalar variant is the
/// reference implementation, exposed so tests can cross-check the vectorized dispatch.
const char * findNextSpecialJsonByte(const char * pos, const char * end);
const char * findNextSpecialJsonByteScalar(const char * pos, const char * end);
```

In `CasTextFormat.cpp` (replace the Task 2 placeholder; add `#include <bit>` and, under `#if defined(__SSE2__)`, `#include <emmintrin.h>`, under `#elif defined(__aarch64__)`, `#include <arm_neon.h>`):

```cpp
namespace
{
constexpr bool isSpecialJsonByte(unsigned char c)
{
    return c < 0x20 || c == '"' || c == '\\' || c == 0xE2;
}
}

const char * findNextSpecialJsonByteScalar(const char * pos, const char * end)
{
    for (; pos != end; ++pos)
        if (isSpecialJsonByte(static_cast<unsigned char>(*pos)))
            return pos;
    return end;
}

const char * findNextSpecialJsonByte(const char * pos, const char * end)
{
#if defined(__SSE2__)
    const __m128i quote = _mm_set1_epi8('"');
    const __m128i backslash = _mm_set1_epi8('\\');
    const __m128i e2 = _mm_set1_epi8('\xE2');
    const __m128i ctl_bound = _mm_set1_epi8(0x1F);
    for (; pos + 16 <= end; pos += 16)
    {
        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(pos));
        /// unsigned c <= 0x1F  <=>  min(c, 0x1F) == c
        const __m128i is_ctl = _mm_cmpeq_epi8(_mm_min_epu8(chunk, ctl_bound), chunk);
        const __m128i hit = _mm_or_si128(
            _mm_or_si128(is_ctl, _mm_cmpeq_epi8(chunk, quote)),
            _mm_or_si128(_mm_cmpeq_epi8(chunk, backslash), _mm_cmpeq_epi8(chunk, e2)));
        if (const unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(hit)))
            return pos + std::countr_zero(mask);
    }
#elif defined(__aarch64__)
    const uint8x16_t quote = vdupq_n_u8('"');
    const uint8x16_t backslash = vdupq_n_u8('\\');
    const uint8x16_t e2 = vdupq_n_u8(0xE2);
    const uint8x16_t ctl_bound = vdupq_n_u8(0x20);
    for (; pos + 16 <= end; pos += 16)
    {
        const uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t *>(pos));
        const uint8x16_t hit = vorrq_u8(
            vorrq_u8(vcltq_u8(chunk, ctl_bound), vceqq_u8(chunk, quote)),
            vorrq_u8(vceqq_u8(chunk, backslash), vceqq_u8(chunk, e2)));
        /// Narrow each byte lane to a nibble: any hit -> nonzero nibble in the u64 mask.
        const uint64_t mask
            = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(hit), 4)), 0);
        if (mask)
            return pos + (std::countr_zero(mask) >> 2);
    }
#endif
    return findNextSpecialJsonByteScalar(pos, end);
}

void CasJsonWriter::stringValue(std::string_view s)
{
    appendChar('"');
    const char * pos = s.data();
    const char * const end = s.data() + s.size();
    while (pos != end)
    {
        const char * next = findNextSpecialJsonByte(pos, end);
        if (next != pos)
        {
            buf.append(pos, static_cast<size_t>(next - pos));
            pos = next;
            if (pos == end)
                break;
        }
        const unsigned char c = static_cast<unsigned char>(*pos);
        switch (c)
        {
            case '\b': append("\\b");   ++pos; break;
            case '\f': append("\\f");   ++pos; break;
            case '\n': append("\\n");   ++pos; break;
            case '\r': append("\\r");   ++pos; break;
            case '\t': append("\\t");   ++pos; break;
            case '\\': append("\\\\");  ++pos; break;
            case '"':  append("\\\"");  ++pos; break;
            case 0xE2:
                if (end - pos >= 3 && pos[1] == '\x80' && (pos[2] == '\xA8' || pos[2] == '\xA9'))
                {
                    append(pos[2] == '\xA8' ? std::string_view{"\\u2028"} : std::string_view{"\\u2029"});
                    pos += 3;
                }
                else
                {
                    appendChar('\xE2');
                    ++pos;
                }
                break;
            default:
            {
                /// A control byte without a named escape: \u00XY with writeJSONString's exact
                /// nibble rendering (uppercase A-F for the low nibble).
                const unsigned char lower_half = c & 0xF;
                append("\\u00");
                appendChar(static_cast<char>('0' + (c >> 4)));
                appendChar(static_cast<char>(lower_half <= 9 ? '0' + lower_half : 'A' + lower_half - 10));
                ++pos;
                break;
            }
        }
    }
    appendChar('"');
}
```

- [ ] **Step 4: Build and run**

```bash
ninja -C build unit_tests_dbms > build/build_task3b.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasJsonWriter*' > build/test_task3.log 2>&1; tail -20 build/test_task3.log
```
Expected: all `CasJsonWriter*` PASS (including the 7000+ differential iterations).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp \
        src/Disks/tests/gtest_cas_json_writer.cpp
git commit -m "cas: CasJsonWriter bulk-run string escaping, differential-fuzzed against writeJSONString

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: `CasJsonWriter` overloads of the shared vocabulary

**Files:**
- Modify: `Formats/CasTextFormat.h` + `.cpp` (overloads incl. `writeHeaderLine`/`writeTrailerLine`), `Formats/CasWireVocab.h` + `.cpp` (`writeTokenFields`/`writeBlobRefFields`/`writeManifestRefFields` overloads)
- Test: `src/Disks/tests/gtest_cas_json_writer.cpp` (extend)

**Interfaces:**
- Produces (exact signatures later tasks call): in `DB::Cas` —
  `writeKey(CasJsonWriter &, std::string_view, bool &)`, `writeStringValue(CasJsonWriter &, std::string_view)`, `writeHex128Value(CasJsonWriter &, const UInt128 &)`, `writeU64StringValue(CasJsonWriter &, uint64_t)`, `writeBoolValue(CasJsonWriter &, bool)`, `closeObject(CasJsonWriter &, bool &)`, `writeChar(char, CasJsonWriter &)`, `writeIntText(uint64_t, CasJsonWriter &)`, `writeHeaderLine(CasJsonWriter &, FormatId)`, `writeTrailerLine(CasJsonWriter &, uint64_t)`, `writeTokenFields(CasJsonWriter &, bool &, const Token &)`, `writeBlobRefFields(CasJsonWriter &, bool &, const BlobRef &)`, `writeManifestRefFields(CasJsonWriter &, bool &, std::string_view prefix, const ManifestRef &)`.
- The `WriteBuffer` originals coexist until Task 9 — during migration each codec resolves the right overload by the type of `out` (ADL covers the `WriteBuffer` side).

- [ ] **Step 1: Write the failing differential test**

Both overload sets exist simultaneously, so the reference is the production `WriteBuffer` version itself:

```cpp
TEST(CasJsonWriterVocab, MatchesWriteBufferVocabulary)
{
    using namespace DB::Cas;
    const UInt128 h = (UInt128(0xdeadbeefULL) << 64) | UInt128(42);

    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;
    bool rf = true;
    bool wf = true;

    writeKey(ref, "a", rf);           writeKey(w, "a", wf);
    writeStringValue(ref, "x/\"y");   writeStringValue(w, "x/\"y");
    writeKey(ref, "h", rf);           writeKey(w, "h", wf);
    writeHex128Value(ref, h);         writeHex128Value(w, h);
    writeKey(ref, "u", rf);           writeKey(w, "u", wf);
    writeU64StringValue(ref, UINT64_MAX); writeU64StringValue(w, UINT64_MAX);
    writeKey(ref, "b", rf);           writeKey(w, "b", wf);
    writeBoolValue(ref, false);       writeBoolValue(w, false);
    writeKey(ref, "n", rf);           writeKey(w, "n", wf);
    DB::writeIntText(uint64_t(12345), ref); writeIntText(uint64_t(12345), w);
    closeObject(ref, rf);             closeObject(w, wf);
    DB::writeChar('\n', ref);         writeChar('\n', w);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}

TEST(CasJsonWriterVocab, HeaderTrailerAndManifestFieldsMatch)
{
    using namespace DB::Cas;
    DB::WriteBufferFromOwnString ref;
    CasJsonWriter w;

    writeHeaderLine(ref, FormatId::RefLog);   writeHeaderLine(w, FormatId::RefLog);
    bool rf = true;
    bool wf = true;
    writeManifestRefFields(ref, rf, "o", ManifestRef{1, 2, 3});
    writeManifestRefFields(w, wf, "o", ManifestRef{1, 2, 3});
    closeObject(ref, rf);                     closeObject(w, wf);
    DB::writeChar('\n', ref);                 writeChar('\n', w);
    writeTrailerLine(ref, 9);                 writeTrailerLine(w, 9);
    ref.finalize();
    EXPECT_EQ(std::move(w).take(), ref.str());
}
```

(Analogous one-liner checks for `writeTokenFields`/`writeBlobRefFields` if `Token`/`BlobRef` construct trivially in the test — same both-sides pattern.)

- [ ] **Step 2: Run to verify compile failure, then implement**

`CasTextFormat.h` — after the class, alongside the existing `WriteBuffer` declarations:

```cpp
/// CasJsonWriter overloads of the same vocabulary. During the WriteBuffer->CasJsonWriter
/// migration both sets coexist; the WriteBuffer set is deleted once the last codec migrates.
inline void writeKey(CasJsonWriter & out, std::string_view key, bool & first) { out.key(key, first); }
inline void writeStringValue(CasJsonWriter & out, std::string_view s) { out.stringValue(s); }
inline void writeHex128Value(CasJsonWriter & out, const UInt128 & v) { out.hex128Value(v); }
inline void writeU64StringValue(CasJsonWriter & out, uint64_t v) { out.u64StringValue(v); }
inline void writeBoolValue(CasJsonWriter & out, bool v) { out.boolValue(v); }
inline void closeObject(CasJsonWriter & out, bool & first) { out.closeObject(first); }
/// Argument order mirrors the IO helpers so migrated codecs keep their call shapes.
inline void writeChar(char c, CasJsonWriter & out) { out.appendChar(c); }
inline void writeIntText(uint64_t v, CasJsonWriter & out) { out.u64Number(v); }
void writeHeaderLine(CasJsonWriter & out, FormatId id);
void writeTrailerLine(CasJsonWriter & out, uint64_t n);
```

`CasTextFormat.cpp` — definitions mirroring the `WriteBuffer` ones call-for-call:

```cpp
void writeHeaderLine(CasJsonWriter & out, FormatId id)
{
    const FormatTraits & t = traitsFor(id);
    bool first = true;
    writeKey(out, "type", first);
    writeStringValue(out, t.type);
    writeKey(out, "v", first);
    writeIntText(currentCompatibilityVersion(), out);
    closeObject(out, first);
    writeChar('\n', out);
}

void writeTrailerLine(CasJsonWriter & out, uint64_t n)
{
    bool first = true;
    writeKey(out, "n", first);
    writeIntText(n, out);
    closeObject(out, first);
    writeChar('\n', out);
}
```

`CasWireVocab.h`/`.cpp` — add overloads next to the existing ones (bodies identical except the prefixed keys use the two-part `key`, killing the `String(prefix) + ...` allocations):

```cpp
void writeTokenFields(CasJsonWriter & out, bool & first, const Token & t)
{
    writeKey(out, "tt", first);
    writeStringValue(out, tokenTypeToWord(t.type));
    writeKey(out, "tv", first);
    writeStringValue(out, t.value);
}

void writeBlobRefFields(CasJsonWriter & out, bool & first, const BlobRef & r)
{
    writeKey(out, "ha", first);
    writeStringValue(out, blobHashAlgoName(r.algo));
    writeKey(out, "h", first);
    writeStringValue(out, codecFor(r.algo).toHex(r.digest));
}

void writeManifestRefFields(CasJsonWriter & out, bool & first, std::string_view prefix, const ManifestRef & r)
{
    out.key(prefix, "me", first);
    out.u64StringValue(r.writer_epoch);
    out.key(prefix, "mb", first);
    out.u64StringValue(r.build_sequence);
    out.key(prefix, "mo", first);
    out.u64Number(r.manifest_ordinal);
}
```

- [ ] **Step 3: Build, run, expect green**

```bash
ninja -C build unit_tests_dbms > build/build_task4.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasJsonWriter*' > build/test_task4.log 2>&1; tail -20 build/test_task4.log
```

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasWireVocab.cpp \
        src/Disks/tests/gtest_cas_json_writer.cpp
git commit -m "cas: CasJsonWriter overloads of the shared JSON vocabulary, differential-tested against the WriteBuffer set

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Migrate the hot codecs — `CasRefLogFormat` + `CasRefSnapshotFormat`

**Files:**
- Modify: `Formats/CasRefLogFormat.cpp`, `Formats/CasRefSnapshotFormat.cpp` (encode side only; decode untouched)
- Gate: `CasEncodingPins.*`, full corrected filter

**Interfaces:**
- Consumes: everything from Task 4.
- Produces: no signature changes visible outside the two `.cpp` files — `encodeRefLogTxn`, `encodeRefTableSnapshot`, `removalOpEncodedSize`, `removalFramingSize`, `snapshotFramingSize` keep their public signatures.

- [ ] **Step 1: Migrate `CasRefLogFormat.cpp`**

Change every file-local write helper's first parameter `WriteBuffer & out` → `CasJsonWriter & out`: `writeBindingFields`, `writeOp`, `writeLogMeta`. Bodies keep their exact call sequences (the Task 4 overloads resolve); the ONLY body change is `writeBindingFields` dropping the `String` concatenations:

```cpp
void writeBindingFields(CasJsonWriter & out, bool & first, std::string_view prefix, const RefOwnerBinding & b)
{
    checkCanonicalRefName(b.ref_name, "RefLogTxn", "owner binding ref_name");
    checkManifestRef(b.manifest_ref, "RefLogTxn", "owner binding manifest_ref");
    out.key(prefix, "bk", first);
    writeStringValue(out, refOwnerKindToWord(b.kind));
    out.key(prefix, "rn", first);
    writeStringValue(out, b.ref_name);
    writeManifestRefFields(out, first, prefix, b.manifest_ref);
}
```

Entry points swap the buffer for the writer — pattern (applies identically to `removalOpEncodedSize` and `removalFramingSize`, which only need `.size()` of the result):

```cpp
String encodeRefLogTxn(const RefLogTxn & txn)
{
    checkTxnIdNonzero(txn.txn_id);

    CasJsonWriter out(512);
    writeHeaderLine(out, FormatId::RefLog);
    writeLogMeta(out, txn.ns, txn.txn_id);
    for (const RefOp & op : txn.ops)
        writeOp(out, op);
    writeTrailerLine(out, txn.ops.size());

    String text = std::move(out).take();
    checkBudget(txn.ops, text.size());
    return text;
}
```

For the two size helpers, `out.size()` replaces `out.finalize(); out.str().size()`:

```cpp
size_t removalOpEncodedSize(RefOwnerKind owner_kind, const String & ref_name, const ManifestRef & manifest_ref)
{
    RefOp op;
    op.kind = RefOpKind::OwnerTransition;
    op.old_binding = RefOwnerBinding{owner_kind, ref_name, manifest_ref};

    CasJsonWriter out(256);
    writeOp(out, op);
    return out.size();
}
```

(`removalFramingSize`: same swap — `CasJsonWriter out(256);`, drop `finalize`/`str`, return `out.size()`.)
Drop the now-unused `#include <IO/WriteBufferFromString.h>` if no other use remains in the file.

- [ ] **Step 2: Migrate `CasRefSnapshotFormat.cpp`**

Same transformation for `writeIdFields`, `writeCommittedRow`, `writePrecommitRow`, `writeSnapshotMeta` (parameter type swap only — bodies unchanged), then:

```cpp
String encodeRefTableSnapshot(const RefTableSnapshot & snapshot)
{
    checkSnapshotInvariants(snapshot);

    CasJsonWriter out(256 + 128 * (snapshot.committed.size() + snapshot.precommits.size()));
    writeHeaderLine(out, FormatId::RefSnapshot);
    writeSnapshotMeta(out, snapshot);
    for (const RefCommittedRow & row : snapshot.committed)
        writeCommittedRow(out, row);
    for (const RefOwnerBinding & row : snapshot.precommits)
        writePrecommitRow(out, row);
    writeTrailerLine(out, snapshot.committed.size() + snapshot.precommits.size());

    String text = std::move(out).take();
    if (text.size() > ref_snapshot_max_bytes)
        throw Exception(ErrorCodes::CORRUPTED_DATA,
            "RefTableSnapshot: encoded size {} exceeds the snapshot byte limit {}", text.size(), ref_snapshot_max_bytes);
    return text;
}
```

If the file has other encode-side helpers using `WriteBufferFromOwnString` (e.g. a `snapshotFramingSize` or a row-size helper below line 170), apply the identical swap there — grep the file: `grep -n "WriteBufferFromOwnString\|WriteBuffer &" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp` and convert every WRITE-side hit; leave `ReadBuffer` code alone.

- [ ] **Step 3: Build, gate on pins + corrected filter**

```bash
ninja -C build unit_tests_dbms > build/build_task5.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasEncodingPins.*' > build/test_task5_pins.log 2>&1; tail -5 build/test_task5_pins.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task5_full.log 2>&1; tail -5 build/test_task5_full.log
```
Expected: pins green UNMODIFIED; full gate green (dispatch a subagent to summarize the full-gate log).

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefLogFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp
git commit -m "cas: ref log + ref snapshot encoders on CasJsonWriter (hotpath, zero per-record allocations)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Migrate the GC-side codecs

**Files:**
- Modify: `Formats/CasPartManifestFormat.cpp`, `Formats/CasGcStateFormat.cpp`, `Formats/CasGcOutcomesFormat.cpp`, `Formats/CasFoldSealFormat.cpp`

**Interfaces:**
- Consumes: Task 4 overloads. Produces: no public signature changes.

- [ ] **Step 1: Apply the encode-side transformation to each file**

Find every write-side site first:

```bash
grep -n "WriteBufferFromOwnString\|WriteBuffer & out" \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.cpp
```

Then, per file, (a) every file-local `void writeX(WriteBuffer & out, ...)` helper (e.g. `writeEntryRecord` in the part manifest, `writeRun`/`writeSortedRuns` in the fold seal) becomes `void writeX(CasJsonWriter & out, ...)` with an unchanged body; (b) every encode entry point swaps

```cpp
WriteBufferFromOwnString out;      →    CasJsonWriter out(256);
...                                      ...
out.finalize();                    →    return std::move(out).take();
return out.str();
```

(keep any post-encode size/limit checks exactly where they are, operating on the returned `String`); (c) drop `#include <IO/WriteBufferFromString.h>` when write-side-unused. Decode paths (anything taking `ReadBuffer`/`std::string_view`) stay untouched.

- [ ] **Step 2: Build + full gate**

```bash
ninja -C build unit_tests_dbms > build/build_task6.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task6.log 2>&1; tail -5 build/test_task6.log
```
Expected: green — the per-format gtest files carry golden/battery texts that pin these formats' bytes.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcStateFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasGcOutcomesFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.cpp
git commit -m "cas: part manifest + gc state + gc outcomes + fold seal encoders on CasJsonWriter

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Migrate the pool-side codecs

**Files:**
- Modify: `Formats/CasPoolMetaFormat.cpp`, `Formats/CasBlobMetaFormat.cpp`, `Formats/CasBlobEnvelopeFormat.cpp`, `Formats/CasServerRootFormats.cpp`

- [ ] **Step 1: Apply the encode-side transformation to each file**

Find every write-side site:

```bash
grep -n "WriteBufferFromOwnString\|WriteBuffer & out" \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.cpp \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp
```

Then, per file: (a) every file-local `void writeX(WriteBuffer & out, ...)` helper becomes `void writeX(CasJsonWriter & out, ...)` with an unchanged body; (b) every encode entry point swaps

```cpp
WriteBufferFromOwnString out;      →    CasJsonWriter out(256);
...                                      ...
out.finalize();                    →    return std::move(out).take();
return out.str();
```

(keep any post-encode size/limit checks exactly where they are, operating on the returned `String`); (c) drop `#include <IO/WriteBufferFromString.h>` when write-side-unused; decode paths untouched. `CasBlobEnvelopeFormat.cpp` writes its header inline (`writeKey(buf, "v", first); writeIntText(currentCompatibilityVersion(), buf);`) — the `writeIntText(uint64_t, CasJsonWriter &)` overload from Task 4 absorbs it with no call-shape change.

- [ ] **Step 2: Build + full gate** (same commands, logs `build/build_task7.log` / `build/test_task7.log`). Expected: green.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobMetaFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasBlobEnvelopeFormat.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp
git commit -m "cas: pool meta + blob meta + blob envelope + server-root encoders on CasJsonWriter

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 8: `cas_run` streaming — line-scratch mode

**Files:**
- Modify: `Formats/CasRecordStreamFormat.h` (private `CasJsonWriter scratch` member), `Formats/CasRecordStreamFormat.cpp` (`writeRunHeaderLine`, `SourceEdgeRunWriter::append`, `SourceEdgeRunWriter::finish`)
- Gate: `CasEncodingPins.SourceEdgeRunLines` + record-stream tests

**Interfaces:**
- Consumes: Task 4 overloads. Produces: `SourceEdgeRunWriter`'s public `WriteBuffer &` contract is UNCHANGED — callers stream as before.

- [ ] **Step 1: Rework the three functions**

`writeRunHeaderLine` keeps its `WriteBuffer &` signature; internally it assembles the line and issues one bulk write:

```cpp
void writeRunHeaderLine(WriteBuffer & out, std::string_view kind)
{
    const FormatTraits & t = traitsFor(FormatId::RunFile);
    CasJsonWriter line(64);
    bool first = true;
    writeKey(line, "type", first);
    writeStringValue(line, t.type);
    writeKey(line, "v", first);
    writeIntText(currentCompatibilityVersion(), line);
    writeKey(line, "kind", first);
    writeStringValue(line, kind);
    closeObject(line, first);
    writeChar('\n', line);
    out.write(line.view().data(), line.size());
}
```

`SourceEdgeRunWriter` gains `CasJsonWriter scratch;` (private member, after `bool finished = false;` in the header — add the `CasTextFormat.h` include to `CasRecordStreamFormat.h`). `append` assembles into the scratch and bulk-writes ONE line per record — memory stays bounded by the largest line because `clear` keeps capacity:

```cpp
void SourceEdgeRunWriter::append(const SourceEdgeRecord & rec)
{
    /* ...unchanged finished/order checks and prev_* bookkeeping... */

    scratch.clear();
    bool first = true;
    writeKey(scratch, "b", first);
    writeStringValue(scratch, renderB(rec.ref));
    writeKey(scratch, "s", first);
    writeHex128Value(scratch, rec.source_id);
    writeKey(scratch, "m", first);
    writeStringValue(scratch, markerToWord(rec.marker));
    if (rec.marker == kCondemned)
    {
        writeKey(scratch, "pend", first);
        writeBoolValue(scratch, rec.delete_pending);
        writeTokenFields(scratch, first, rec.token);
        writeKey(scratch, "sz", first);
        writeIntText(rec.size, scratch);
        writeKey(scratch, "cr", first);
        writeU64StringValue(scratch, rec.condemn_round);
        writeKey(scratch, "mc", first);
        writeBoolValue(scratch, rec.marker_confirmed);
    }
    closeObject(scratch, first);
    writeChar('\n', scratch);
    out.write(scratch.view().data(), scratch.size());
    ++count;
}
```

`finish`: same pattern — assemble the trailer via `writeTrailerLine(CasJsonWriter&, count)` into the scratch (after `scratch.clear()`), then one `out.write`.

- [ ] **Step 2: Build + gates**

```bash
ninja -C build unit_tests_dbms > build/build_task8.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='CasEncodingPins.SourceEdgeRunLines:CasRecordStream*:CasRun*' > build/test_task8_pins.log 2>&1; tail -5 build/test_task8_pins.log
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task8_full.log 2>&1; tail -5 build/test_task8_full.log
```
Expected: green (byte-identity of run lines pinned by Task 1).

- [ ] **Step 3: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRecordStreamFormat.cpp
git commit -m "cas: cas_run writer assembles each NDJSON line in a reused scratch, one bulk write per record

Memory stays bounded by the largest line, never by record count.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 9: Delete the `WriteBuffer` vocabulary; reference moves into the test

**Files:**
- Modify: `Formats/CasTextFormat.h` + `.cpp` (delete `WriteBuffer` versions of `writeKey`, `writeStringValue`, `writeHex128Value`, `writeU64StringValue`, `writeBoolValue`, `closeObject`, `writeHeaderLine`, `writeTrailerLine`), `Formats/CasWireVocab.h` + `.cpp` (delete `WriteBuffer` versions of the three field writers)
- Modify: `src/Disks/tests/gtest_cas_json_writer.cpp` (Task 4's differential tests lose their production reference — give them a test-local one)
- Possibly modify: any straggler caller the grep below finds

- [ ] **Step 1: Find every remaining `WriteBuffer`-vocabulary caller**

```bash
grep -rn "writeKey(\|writeStringValue(\|writeHex128Value(\|writeU64StringValue(\|writeBoolValue(\|closeObject(\|writeManifestRefFields(\|writeTokenFields(\|writeBlobRefFields(" \
    src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ --include="*.cpp" --include="*.h" | grep -v tests
```
Every hit must now be a `CasJsonWriter` call site (Tasks 5–8 covered the known set; migrate any straggler with the same transformation before deleting).

- [ ] **Step 2: Delete the `WriteBuffer` overloads; fix the differential tests**

In `gtest_cas_json_writer.cpp`, replace the production references in `CasJsonWriterVocab.*` with a test-local verbatim copy of the deleted implementations (this preserves the spec's "reference implementation lives on in gtest only"):

```cpp
namespace reference_vocab
{
/// Verbatim copy of the retired WriteBuffer-based CAS vocabulary (CasTextFormat.cpp pre-CasJsonWriter),
/// kept as the differential reference. jsonWriteSettings is inlined: escape_forward_slashes=false.
const DB::FormatSettings & settings()
{
    static const DB::FormatSettings s = []
    {
        DB::FormatSettings fs;
        fs.json.escape_forward_slashes = false;
        return fs;
    }();
    return s;
}

void writeKey(DB::WriteBuffer & out, std::string_view key, bool & first)
{
    DB::writeChar(first ? '{' : ',', out);
    first = false;
    DB::writeChar('"', out);
    out.write(key.data(), key.size());
    DB::writeChar('"', out);
    DB::writeChar(':', out);
}

void writeStringValue(DB::WriteBuffer & out, std::string_view s) { DB::writeJSONString(s, out, settings()); }

void writeHex128Value(DB::WriteBuffer & out, const UInt128 & v)
{
    DB::writeChar('"', out);
    const String hex = DB::Cas::u128ToHex(v);
    out.write(hex.data(), hex.size());
    DB::writeChar('"', out);
}

void writeU64StringValue(DB::WriteBuffer & out, uint64_t v)
{
    DB::writeChar('"', out);
    DB::writeIntText(v, out);
    DB::writeChar('"', out);
}

void writeBoolValue(DB::WriteBuffer & out, bool v) { DB::writeCString(v ? "true" : "false", out); }

void closeObject(DB::WriteBuffer & out, bool & first)
{
    if (first)
        DB::writeChar('{', out);
    first = false;
    DB::writeChar('}', out);
}
}
```

Update the `CasJsonWriterVocab.*` tests to call `reference_vocab::...` on the `WriteBuffer` side (the `CasJsonWriter` side is unchanged). The header/trailer/manifest differential can be dropped OR rebuilt against reference_vocab compositions — keep whichever compiles cleanly; the pins + per-format goldens already gate those paths end to end.

- [ ] **Step 3: Verify nothing else breaks, then gate**

```bash
ninja -C build unit_tests_dbms > build/build_task9.log 2>&1; echo EXIT=$?
grep -rn "WriteBufferFromOwnString" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/ | grep -v tests
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task9.log 2>&1; tail -5 build/test_task9.log
```
Expected: the grep returns no write-side hits in `Formats/` (decode-side and non-Formats users are out of scope); the full gate is green. Also build the whole server once to catch out-of-subsystem callers: `ninja -C build clickhouse > build/build_task9_full.log 2>&1; echo EXIT=$?` (subagent-summarize the log).

- [ ] **Step 4: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats src/Disks/tests/gtest_cas_json_writer.cpp
git commit -m "cas: retire the WriteBuffer JSON vocabulary; CasJsonWriter is the only CAS text writer

The old implementation survives verbatim inside the gtest as the differential reference.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 10: Benchmark floor + acceptance

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp`

**Interfaces:**
- Consumes: `encodeRefLogTxn`, `makeSamplePromoteTxn` (already in the file).

- [ ] **Step 1: Add the memcpy floor benchmark**

```cpp
/// The "near-memcpy" floor for BM_EncodeRefLogTxn: the SAME encoded bytes assembled from
/// precomputed 16-byte fragments by plain String appends — approximating the writer's append
/// granularity with zero formatting/escaping work. Acceptance gate for the CasJsonWriter
/// migration (docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md):
/// BM_EncodeRefLogTxn must land within 3x of this floor (2x is the aspiration).
static void BM_MemcpyTxnBytes(benchmark::State & state)
{
    const RefLogTxn txn = makeSamplePromoteTxn();
    const String encoded = encodeRefLogTxn(txn);
    std::vector<std::string_view> fragments;
    constexpr size_t kFragment = 16;
    for (size_t off = 0; off < encoded.size(); off += kFragment)
        fragments.push_back(std::string_view(encoded).substr(off, kFragment));

    String buf;
    buf.reserve(encoded.size());
    for (auto _ : state)
    {
        buf.clear();
        for (const auto f : fragments)
            buf.append(f.data(), f.size());
        benchmark::DoNotOptimize(buf.data());
    }
}
BENCHMARK(BM_MemcpyTxnBytes);
```

Also update the file's header comment: extend the history block with the "after CasJsonWriter (2026-07-20)" numbers once measured in Step 2.

- [ ] **Step 2: Build and measure**

```bash
ninja -C build benchmark_cas_ref_protocol > build/build_task10.log 2>&1; echo EXIT=$?
build/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol \
    --benchmark_filter='BM_EncodeRefLogTxn|BM_MemcpyTxnBytes|BM_WriteJSONStringSafe|BM_RawBulkWriteSafe' \
    > build/bench_task10.log 2>&1; cat build/bench_task10.log
```
(If the binary lands elsewhere, `find build -name benchmark_cas_ref_protocol -type f`.)

Acceptance: `BM_EncodeRefLogTxn` ≤ 3 × `BM_MemcpyTxnBytes` (hard gate; ≤2× is the aspiration). Record both numbers plus the old 753ns baseline in the file's history comment.

- [ ] **Step 3 (ONLY if the ratio exceeds 3×): contingency ladder**

Rung 1 — merged key literals in the two hottest writers. Add to `CasJsonWriter`:

```cpp
/// `rendered` is the FULLY rendered key text including quotes and colon, e.g. "\"rn\":".
/// One separator store + one literal append per key.
void keyLiteral(std::string_view rendered, bool & first)
{
    appendChar(first ? '{' : ',');
    first = false;
    append(rendered);
}
```

and in `writeOp` (`CasRefLogFormat.cpp`) / `writeCommittedRow` (`CasRefSnapshotFormat.cpp`) replace `writeKey(out, "rn", first)` with `out.keyLiteral("\"rn\":", first)` etc. for the fixed unprefixed keys. Re-run Step 2; pins and the full gate must stay green. Only escalate to rung 2 (merging adjacent punctuation into the value writers) if still above 3× — and stop to discuss with the user first, since that trades readability.

- [ ] **Step 4: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/benchmarks/benchmark_cas_ref_protocol.cpp
git commit -m "cas: BM_MemcpyTxnBytes floor benchmark + post-CasJsonWriter encode numbers

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 11: Close out — BACKLOG, spec status, full verification

**Files:**
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` (the "OPTIMIZATION OPPORTUNITY … byte-by-byte" entry), `docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md` (status line)

- [ ] **Step 1: Flip the BACKLOG entry**

Retitle the entry `## RESOLVED (CPU) — ref-ledger JSON encoding writes byte-by-byte instead of bulk-copying safe runs` (matching the style of the neighboring RESOLVED `admits()` entry) and append a resolution block: date, chosen direction (a combined #2+#4: `CasJsonWriter` bulk-append writer, all CAS formats), the measured before/after (`BM_EncodeRefLogTxn` 753ns → NEW ns; floor `BM_MemcpyTxnBytes` = FLOOR ns; ratio = R), and a pointer to the spec + this plan. Update the spec's `- **Status:**` line to `implemented (<commit range>)`.

- [ ] **Step 2: Full verification battery**

```bash
ninja -C build clickhouse unit_tests_dbms > build/build_task11.log 2>&1; echo EXIT=$?
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*:CaDedupCache*:CaTransaction*:CaPartPathParser*:CaWiring*:CaInlinePlacement*' > build/test_task11.log 2>&1; tail -5 build/test_task11.log
```
Both green (subagent-summarize the logs). Any red = RCA before proceeding — no known-red tolerance.

- [ ] **Step 3: Soak smoke (integration gate)**

```bash
cd utils/ca-soak
docker compose down -v
docker compose up -d
python3 -m soak.run --seed 1 --phase 1 > ../../build/soak_task11.log 2>&1; echo EXIT=$?
```
Expected: phase-1 run completes clean (consult `utils/ca-soak/README.md` if the compose stack needs the freshly built binary remounted — `down -v` handles the clean remount). The full-length soak remains the user's call.

- [ ] **Step 4: Commit**

```bash
git add utils/ca-soak/scenarios/BACKLOG.md docs/superpowers/specs/2026-07-20-cas-json-writer-bulk-encoding-design.md
git commit -m "cas: mark the byte-by-byte ref-ledger encoding BACKLOG item RESOLVED (CasJsonWriter, measured numbers)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

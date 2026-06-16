# CA RootShard Protobuf Codec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the JSON `RootShard` manifest codec with Protobuf to remove the dominant CA write-path CPU cost (char-by-char `writeJSONString` + Poco `Var`) and halve manifest bytes, while reading existing JSON pools transparently.

**Architecture:** A new `cas_root_shard.proto` (proto3) compiled by `protobuf_generate_cpp` into a `clickhouse_cas_proto` static lib linked into `dbms` (mirrors `src/Client/BuzzHouse/Proto`). `encodeRootShard` emits protobuf (deterministic serialization); `decodeRootShard` dispatches on the first byte — `{` → legacy JSON decoder (retained verbatim), else → protobuf decoder. The two semantic journal invariants (`at_version <= shard_version`; non-decreasing `at_version`) are enforced in BOTH decoders.

**Tech Stack:** C++ (Allman braces), Google Protobuf (vendored `ch_contrib::protoc`), CMake `protobuf_generate_cpp`, gtest (`unit_tests_dbms`).

**Spec:** `docs/superpowers/specs/2026-06-16-ca-rootshard-protobuf-codec-design.md`. Backlog: B164a.

**Build/test commands (per CLAUDE.md — redirect to a log, analyze with a subagent, no `-j`/`nproc`):**
- Configure (only after adding the new CMakeLists / subdir): `cd build && cmake .. > build/cmake_b164.log 2>&1`
- Build the codec test target: `ninja -C build unit_tests_dbms > build/build_b164.log 2>&1`
- Run the codec tests: `build/src/unit_tests_dbms --gtest_filter='CasRootShardCodec*' > build/test_b164.log 2>&1`

---

## File Structure

- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_root_shard.proto` — the schema.
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/CMakeLists.txt` — codegen → `clickhouse_cas_proto`.
- Modify: `src/CMakeLists.txt` — `add_subdirectory` of the Proto dir + link `clickhouse_cas_proto` into `dbms` (guarded by `ch_contrib::protobuf`).
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp` — protobuf encode + first-byte-dispatch decode; rename the current JSON decode body to `decodeRootShardJson`.
- Create: `src/Disks/tests/gtest_cas_rootshard_codec.cpp` — round-trip, golden, legacy-JSON, dispatch, fail-closed tests.
- Modify: `src/Disks/tests/CMakeLists.txt` (only if the test dir doesn't auto-glob — verify first).

`CasRootShardCodec.h` is unchanged (the `RootShard`/`RefPayload`/`JournalRecord` structs and the `encodeRootShard`/`decodeRootShard` signatures stay), so the call sites in `CasStore.cpp` need no edits.

---

### Task 1: Protobuf schema + build wiring (codegen compiles and links)

**Files:**
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_root_shard.proto`
- Create: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Write the schema**

`Proto/cas_root_shard.proto`:
```proto
syntax = "proto3";
package DB.Cas.Proto;

message RefPayload {
  bytes tree_id = 1;                  // raw 16 bytes (UInt128, big-endian)
  uint64 tree_size = 2;
  map<string, string> mutable_files = 3;
}

enum JournalOp {
  JOURNAL_OP_UNSPECIFIED = 0;         // decode fails closed on 0
  JOURNAL_OP_ADD = 1;
  JOURNAL_OP_REMOVE = 2;
}

message JournalRecord {
  JournalOp op = 1;
  string ref_name = 2;
  bytes tree_id = 3;                  // raw 16 bytes
  uint64 at_version = 4;
}

message RootShardManifest {
  uint32 codec_version = 1;           // = 2; future value -> NOT_IMPLEMENTED
  uint64 shard_version = 2;
  uint64 fence_round = 3;
  map<string, RefPayload> refs = 4;
  repeated JournalRecord journal = 5; // insertion order preserved
}
```

- [ ] **Step 2: Write the codegen CMakeLists** (mirrors `src/Client/BuzzHouse/Proto/CMakeLists.txt`)

`Proto/CMakeLists.txt`:
```cmake
protobuf_generate_cpp(clickhouse_cas_proto_sources clickhouse_cas_proto_headers cas_root_shard.proto)

add_library(clickhouse_cas_proto ${clickhouse_cas_proto_headers} ${clickhouse_cas_proto_sources})
target_include_directories(clickhouse_cas_proto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(clickhouse_cas_proto PUBLIC ch_contrib::protoc)
# Ignore warnings while compiling protobuf-generated *.pb.h and *.pb.cpp files.
target_compile_options(clickhouse_cas_proto PRIVATE "-w")
# Disable clang-tidy for protobuf-generated *.pb.h and *.pb.cpp files.
set_target_properties(clickhouse_cas_proto PROPERTIES CXX_CLANG_TIDY "")
```

- [ ] **Step 3: Wire into `src/CMakeLists.txt`** — find the BuzzHouse block (`clickhouse_buzzhouse_proto`, ~line 855-860) and the protobuf guard (`if (TARGET ch_contrib::protobuf)`, ~line 700). Add, inside a `ch_contrib::protobuf` guard, an `add_subdirectory` and a link:

```cmake
if (TARGET ch_contrib::protobuf)
    add_subdirectory("Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto")
    target_link_libraries(dbms PRIVATE clickhouse_cas_proto)
endif()
```
(Place near the existing BuzzHouse `target_link_libraries(dbms PRIVATE clickhouse_buzzhouse_proto)` so it follows the established pattern. Verify the exact surrounding `if(...)` structure when editing — do not duplicate an existing guard; add inside one if present.)

- [ ] **Step 4: Configure + build the proto lib to verify codegen**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/build && cmake .. > cmake_b164.log 2>&1 && ninja clickhouse_cas_proto > build_b164_proto.log 2>&1`
Expected: `cmake_b164.log` ends configuring with no error; `clickhouse_cas_proto` builds; `build/.../Proto/cas_root_shard.pb.h` exists. (Use a subagent to summarize the logs.)

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/ src/CMakeLists.txt
git commit -m "CA B164a: add cas_root_shard.proto + build wiring (clickhouse_cas_proto)"
```

---

### Task 2: Protobuf codec — encode + first-byte-dispatch decode (preserve JSON legacy + invariants)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp`

The current file (read it first) has `encodeRootShard` (JSON) and `decodeRootShard` (JSON via `decodeJsonGuarded`). Keep all JSON helpers; the JSON decode body becomes the legacy path.

- [ ] **Step 1: Add protobuf + UInt128↔bytes helpers at the top of the anon namespace**

Add includes:
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Proto/cas_root_shard.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
```
Add error code `NOT_IMPLEMENTED` to the `ErrorCodes` block (alongside `CORRUPTED_DATA`).
Add to the anon namespace:
```cpp
constexpr uint32_t CODEC_VERSION_PROTOBUF = 2;   // JSON is the implicit v1

/// UInt128 <-> 16 raw bytes, big-endian. Mirrors the hex convention (u128ToHex) byte order.
std::string u128ToBytes(const UInt128 & v)
{
    std::string out(16, '\0');
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<char>(static_cast<UInt8>(v >> (8 * (15 - i))));
    return out;
}

UInt128 u128FromBytes(const std::string & b, std::string_view what)
{
    if (b.size() != 16)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: tree_id must be 16 bytes, got {}", what, b.size());
    UInt128 v = 0;
    for (int i = 0; i < 16; ++i)
        v = (v << 8) | static_cast<UInt8>(b[i]);
    return v;
}

Cas::Proto::JournalOp journalOpToProto(JournalRecord::Op op)
{
    switch (op)
    {
        case JournalRecord::Op::Add: return Cas::Proto::JOURNAL_OP_ADD;
        case JournalRecord::Op::Remove: return Cas::Proto::JOURNAL_OP_REMOVE;
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: invalid journal op {}", static_cast<int>(op));
}

JournalRecord::Op journalOpFromProto(Cas::Proto::JournalOp op, std::string_view what)
{
    switch (op)
    {
        case Cas::Proto::JOURNAL_OP_ADD: return JournalRecord::Op::Add;
        case Cas::Proto::JOURNAL_OP_REMOVE: return JournalRecord::Op::Remove;
        default: throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS {}: invalid journal op {}", what, static_cast<int>(op));
    }
}
```

- [ ] **Step 2: Rename the JSON decode body to `decodeRootShardJson`**

Change the current `RootShard decodeRootShard(std::string_view data)` to `static RootShard decodeRootShardJson(std::string_view data)` (body unchanged — it keeps `decodeJsonGuarded`, `checkNoUnknownKeys`, and the two `at_version` invariant checks).

- [ ] **Step 3: Replace `encodeRootShard` with the protobuf encoder**

```cpp
String encodeRootShard(const RootShard & root)
{
    Cas::Proto::RootShardManifest msg;
    msg.set_codec_version(CODEC_VERSION_PROTOBUF);
    msg.set_shard_version(root.shard_version);
    msg.set_fence_round(root.fence_round);

    auto & refs = *msg.mutable_refs();
    for (const auto & [name, payload] : root.refs)
    {
        Cas::Proto::RefPayload p;
        p.set_tree_id(u128ToBytes(payload.tree_id));
        p.set_tree_size(payload.tree_size);
        auto & mf = *p.mutable_mutable_files();
        for (const auto & [k, v] : payload.mutable_files)
            mf[k] = v;
        refs[name] = std::move(p);
    }

    for (const auto & rec : root.journal)
    {
        auto * r = msg.add_journal();
        r->set_op(journalOpToProto(rec.op));
        r->set_ref_name(rec.ref_name);
        r->set_tree_id(u128ToBytes(rec.tree_id));
        r->set_at_version(rec.at_version);
    }

    /// Deterministic serialization (sorts map<> entries) so golden tests are stable. Correctness
    /// does not need it — the manifest is CAS-by-token, not content-addressed.
    std::string out;
    {
        google::protobuf::io::StringOutputStream zos(&out);
        google::protobuf::io::CodedOutputStream cos(&zos);
        cos.SetSerializationDeterministic(true);
        if (!msg.SerializeToCodedStream(&cos))
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf serialize failed");
    }
    return out;
}
```

- [ ] **Step 4: Add the protobuf decoder + first-byte-dispatch `decodeRootShard`**

```cpp
static RootShard decodeRootShardProto(std::string_view data)
{
    Cas::Proto::RootShardManifest msg;
    if (!msg.ParseFromArray(data.data(), static_cast<int>(data.size())))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: protobuf parse failed");
    if (msg.codec_version() > CODEC_VERSION_PROTOBUF)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS root shard: codec_version {} is from a newer writer", msg.codec_version());

    RootShard root;
    root.shard_version = msg.shard_version();
    root.fence_round = msg.fence_round();

    for (const auto & [name, p] : msg.refs())
    {
        RefPayload payload;
        payload.tree_id = u128FromBytes(p.tree_id(), "root shard ref");
        payload.tree_size = p.tree_size();
        for (const auto & [k, v] : p.mutable_files())
            payload.mutable_files[k] = v;
        root.refs[name] = std::move(payload);
    }

    for (const auto & r : msg.journal())
    {
        JournalRecord rec;
        rec.op = journalOpFromProto(r.op(), "root shard journal");
        rec.ref_name = r.ref_name();
        rec.tree_id = u128FromBytes(r.tree_id(), "root shard journal");
        rec.at_version = r.at_version();

        /// Same semantic invariants the JSON decoder enforces (corruption -> fail closed).
        if (rec.at_version > root.shard_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal at_version {} exceeds shard_version {}",
                rec.at_version, root.shard_version);
        if (!root.journal.empty() && rec.at_version < root.journal.back().at_version)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS root shard: journal at_version {} after {} - the journal must be non-decreasing",
                rec.at_version, root.journal.back().at_version);

        root.journal.push_back(std::move(rec));
    }
    return root;
}

RootShard decodeRootShard(std::string_view data)
{
    if (data.empty())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS root shard: empty manifest");
    /// Dispatch: a JSON manifest starts with '{' (0x7B); a RootShardManifest starts with field-1's
    /// tag byte 0x08. They never collide.
    if (data.front() == '{')
        return decodeRootShardJson(data);
    return decodeRootShardProto(data);
}
```

- [ ] **Step 5: Build**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja clickhouse_cas_proto dbms > build_b164_codec.log 2>&1`
Expected: builds clean. (Subagent summarizes the log.)

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.cpp
git commit -m "CA B164a: encode RootShard as protobuf; decode dispatches JSON(legacy)|protobuf"
```

---

### Task 3: Unit tests (round-trip, golden, legacy-JSON, dispatch, fail-closed)

**Files:**
- Create: `src/Disks/tests/gtest_cas_rootshard_codec.cpp`
- Verify: `src/Disks/tests/CMakeLists.txt` globs `gtest_*.cpp` (most CH test dirs do via `grep_gtest_sources`); if it lists files explicitly, add the new file.

- [ ] **Step 1: Write the tests**

```cpp
#include <gtest/gtest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.h>
#include <Common/Exception.h>

using namespace DB::Cas;

namespace
{
RootShard sample()
{
    RootShard r;
    r.shard_version = 42;
    r.fence_round = 7;
    RefPayload p;
    p.tree_id = UInt128(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
    p.tree_size = 1142;
    p.mutable_files = {{".ca_mtime", "1781588451"}, {"metadata_version.txt", "0"}};
    r.refs["part_a"] = p;
    r.journal.push_back(JournalRecord{JournalRecord::Op::Add, "part_a", p.tree_id, 41});
    r.journal.push_back(JournalRecord{JournalRecord::Op::Remove, "part_a", p.tree_id, 42});
    return r;
}

bool eq(const RootShard & a, const RootShard & b)
{
    if (a.shard_version != b.shard_version || a.fence_round != b.fence_round) return false;
    if (a.refs.size() != b.refs.size() || a.journal.size() != b.journal.size()) return false;
    for (const auto & [name, pa] : a.refs)
    {
        auto it = b.refs.find(name);
        if (it == b.refs.end()) return false;
        const auto & pb = it->second;
        if (pa.tree_id != pb.tree_id || pa.tree_size != pb.tree_size || pa.mutable_files != pb.mutable_files) return false;
    }
    for (size_t i = 0; i < a.journal.size(); ++i)
    {
        const auto & x = a.journal[i]; const auto & y = b.journal[i];
        if (x.op != y.op || x.ref_name != y.ref_name || x.tree_id != y.tree_id || x.at_version != y.at_version) return false;
    }
    return true;
}
}

TEST(CasRootShardCodec, RoundTripProtobuf)
{
    RootShard r = sample();
    String bytes = encodeRootShard(r);
    ASSERT_FALSE(bytes.empty());
    EXPECT_NE(bytes.front(), '{');                 // protobuf, not JSON
    RootShard back = decodeRootShard(bytes);
    EXPECT_TRUE(eq(r, back));
}

TEST(CasRootShardCodec, EmptyManifestRoundTrips)
{
    RootShard r;                                    // version 0, no refs, no journal
    RootShard back = decodeRootShard(encodeRootShard(r));
    EXPECT_TRUE(eq(r, back));
}

TEST(CasRootShardCodec, DeterministicEncoding)
{
    EXPECT_EQ(encodeRootShard(sample()), encodeRootShard(sample()));   // stable bytes
}

TEST(CasRootShardCodec, LargeJournalRoundTrips)
{
    RootShard r; r.shard_version = 5000;
    for (uint64_t v = 0; v < 2430; ++v)
        r.journal.push_back(JournalRecord{JournalRecord::Op::Add, "p" + std::to_string(v % 38),
                                          UInt128(v, v), v});
    EXPECT_TRUE(eq(r, decodeRootShard(encodeRootShard(r))));
}

TEST(CasRootShardCodec, LegacyJsonStillDecodes)
{
    // A real JSON manifest (the pre-B164 format) must still decode (back-compat).
    std::string json = R"({"format":"cas_root_shard","version":1,"shard_version":42,"fence_round":7,)"
        R"("refs":{"part_a":{"tree":"0123456789abcdeffedcba9876543210","tree_size":1142,)"
        R"("mutable_files":{"metadata_version.txt":"0"}}},)"
        R"("journal":[{"op":"add","ref":"part_a","tree":"0123456789abcdeffedcba9876543210","at_version":42}]})";
    RootShard back = decodeRootShard(json);
    EXPECT_EQ(back.shard_version, 42u);
    ASSERT_EQ(back.refs.size(), 1u);
    EXPECT_EQ(back.refs.at("part_a").tree_size, 1142u);
    ASSERT_EQ(back.journal.size(), 1u);
    EXPECT_EQ(back.journal[0].op, JournalRecord::Op::Add);
}

TEST(CasRootShardCodec, FailClosedOnGarbage)
{
    EXPECT_THROW(decodeRootShard(std::string_view("")), DB::Exception);          // empty
    EXPECT_THROW(decodeRootShard(std::string_view("\xff\xff\xff\xff")), DB::Exception); // not protobuf, not JSON
}

TEST(CasRootShardCodec, FailClosedOnJournalAtVersionBeyondShard)
{
    RootShard r; r.shard_version = 10;
    r.journal.push_back(JournalRecord{JournalRecord::Op::Add, "p", UInt128(1, 1), 11});   // > shard_version
    EXPECT_THROW(decodeRootShard(encodeRootShard(r)), DB::Exception);
}
```

- [ ] **Step 2: Build the test target**

Run: `cd /home/mfilimonov/workspace/ClickHouse/master/build && ninja unit_tests_dbms > build_b164_test.log 2>&1`
Expected: builds clean. (Subagent summarizes.)

- [ ] **Step 3: Run the tests**

Run: `build/src/unit_tests_dbms --gtest_filter='CasRootShardCodec*' > build/test_b164.log 2>&1`
Expected: all `CasRootShardCodec.*` tests PASS. (Subagent summarizes.)

- [ ] **Step 4: Run the full CA suite (no regression)**

Run: `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*' > build/test_b164_ca.log 2>&1`
Expected: same green/red set as before the change (only the pre-existing B140 `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` may be red). (Subagent summarizes.)

- [ ] **Step 5: Commit**

```bash
git add src/Disks/tests/gtest_cas_rootshard_codec.cpp
git commit -m "CA B164a: codec tests (round-trip, golden, legacy-JSON, fail-closed)"
```

---

### Task 4 (optional, last): `clickhouse-disks ca-decode-manifest` introspection tool

Offline introspection is ALREADY available via `protoc --decode DB.Cas.Proto.RootShardManifest cas_root_shard.proto < manifest.bin` (the portability win of protobuf). This task adds an on-disk convenience command only if time permits.

**Files:**
- Create/modify under `programs/disks/` following `CommandFsck.cpp` (the #5 precedent).

- [ ] **Step 1: Add a `ca-decode-manifest <disk> <path>` command** that reads the object via the disk, calls `decodeRootShard`, and prints a JSON dump (reuse the existing JSON `encodeRootShard`-style writer, or `google::protobuf::util::MessageToJsonString` on the parsed message). Defer if the build/soak queue is tight; the `protoc --decode` path covers the introspection requirement.

- [ ] **Step 2: Commit** (if implemented):
```bash
git add programs/disks/
git commit -m "CA B164a: clickhouse-disks ca-decode-manifest (manifest introspection)"
```

---

## Notes for the implementer
- **No `-j`/`nproc`** with ninja; redirect every build/test to a log under `build/` and have a subagent summarize it (CLAUDE.md).
- **Allman braces**, `exception` not `crash`, `ASan` spelling — style check enforces.
- The proto `package DB.Cas.Proto` puts generated types in `DB::Cas::Proto::`. The codec is in `DB::Cas`, so refer to `Cas::Proto::...` or `Proto::...`.
- If `src/Disks/tests/CMakeLists.txt` globs gtest sources, Task 3 needs no CMake edit; verify before adding one.

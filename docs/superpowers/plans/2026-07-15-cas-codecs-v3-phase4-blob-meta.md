---
description: 'Implementation plan for CAS codecs v3 phase 4: converting the fixed 22-byte binary blob-meta sidecar (cas_blob_meta) to one-line Control JSON on the phase-1 text file shape, with the dedup/resurrect condemned-state token semantics provably unchanged.'
sidebar_label: 'CAS codecs v3 phase 4 plan'
sidebar_position: 64
slug: /superpowers/plans/2026-07-15-cas-codecs-v3-phase4-blob-meta
title: 'CAS Codecs V3 — Phase 4: Blob-Meta Text Cutover'
doc_type: 'guide'
---

# CAS Codecs V3 — Phase 4: Blob-Meta Text Cutover Implementation Plan {#cas-codecs-v3-phase4}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Base assumption (explicit):** this plan is written against **the phase-2 draft integrated** (`docs/superpowers/plans/2026-07-15-cas-codecs-v3-phase2-control-plane.md`, draft in `tmp/worktrees/draft-codecs2/DRAFT.patch`) as the assumed-landed foundation — it relies on the phase-1/phase-2 `Core/Formats/` surface: `CasTextFormat` (with the `escape_forward_slashes=false` **pinned** `jsonWriteSettings` and `writeBoolValue`), the `FormatId::BlobMeta = 21` registry entry, and the `cas_blob_meta` `TRAITS` row (`Control`, `Tolerant`, `Never`, cap 1 MiB / line 64 KiB) — all already present. When drafting (task-2 message), branch from current mainline HEAD and **apply the phase-2 `DRAFT.patch` first** as the base layer (documented in the draft's DRAFT.md).

**Goal:** convert the blob-meta sidecar `cas_blob_meta` from the fixed 22-byte `CAMT` binary codec to a one-line `Control` JSON object on the phase-1 text shape. The wire struct (`MetaState`, `BlobMeta`) and its codec (`encodeBlobMeta`/`decodeBlobMeta`) move to `Core/Formats/CasBlobMetaFormat`; the CAS lifecycle helpers (`loadMeta`, `putMetaIfAbsent`, `casMeta`, `deleteMetaExact`, `LoadedMeta`) STAY in `Core/CasBlobMeta`. The dedup/resurrect **condemned-state token semantics are provably unchanged** — that is the single risk this plan discharges (see [§condemned-semantics](#condemned-semantics)).

**Architecture:** `cas_blob_meta` is the smallest object in the whole migration — three scalars (`state`, `condemn_round`, `size`) with no embedded `Token`/`BlobRef` (the blob identity is the KEY, not the body). It has **no shared-vocabulary dependency** (the `MetaState` word-map is blob-meta-local, not `CasWireVocab`), so it is fully parallel-draftable after phase 1 (DAG §dag-phase4). `Never` compression → stored bytes ARE the text (no zstd). `blobMetaKey` is unchanged (no `.zst` suffix; its Phase-3-T5 key **parser** needs no change — unlike `cas_gc_outcomes`). This was never a protobuf object (it was the `CAMT` fixed binary), so there is **no proto graveyard** work here.

**Tech Stack:** C++ (ClickHouse `dbms`), the phase-1 `CasTextFormat` helpers, `ReadHelpers`/`WriteHelpers` JSON primitives, gtest (`unit_tests_dbms`, auto-globbed).

## Global Constraints {#global-constraints}

- **Allman braces** everywhere (CI style check).
- **Layering (physical, phase-1 rule):** `Core/Formats/CasBlobMetaFormat.h` may include only `Formats/CasFormat.h`, `base/`, `<cstdint>`, `<string_view>` — NEVER `CasBackend.h`, `CasLayout.h`, or any subsystem header. The lifecycle ops that need a `Backend`/`Layout` STAY in `Core/CasBlobMeta.{h,cpp}`, which includes the new `Formats/` header.
- **Pre-release, hard cutover, no dual-read:** the `CAMT` binary codec is deleted and replaced by text in one commit; no persisted data exists (`feedback` 2026-06-24). No "try binary then text" fallback.
- **Pinned JSON write settings (already landed in phase 2):** `writeStringValue` emits `escape_forward_slashes=false`. Blob-meta values contain no `/`, so the pin does not change these goldens — but every golden below is written in the pinned form regardless.
- **Error taxonomy (phase-1):** malformed / truncated / wrong-type / unknown-`st` / over-cap → `CORRUPTED_DATA`; future `v` / unknown `!`-key → `UNKNOWN_FORMAT_VERSION`. The header gate (`expectHeaderLine`) replaces the old length/magic/version checks.
- **`v` stamping stays at `G_BUILD` = 3.** No new format generation; no `changePoints` append. The struct's own `version` field is NOT the format version — see [§version-field](#version-field).
- **JSON value conventions:** `state` → full word (`clean`/`condemned`); `condemn_round` and `size` → decimal **strings** (unbounded `u64` counters / blob sizes, per the proto3-JSON convention). No numbers in this body.
- **`blobMetaKey` is UNCHANGED.** `cas_blob_meta` is `Never`-compression → `storedSuffix(FormatId::BlobMeta)` is `""`. Do NOT touch `blobMetaKey` or its inverse key parser (`CasLayout` Phase-3-T5). (Contrast `cas_gc_outcomes`, which gained `.zst`.)
- **Battery is a hard gate:** `cas_blob_meta` gains exactly one `FormatBatteryCase` row.
- **Build/commit discipline** (as phase 2): `flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p4t<N>.log 2>&1; echo "NINJA_EXIT=$?"`, foreground only, no `-j`/`nproc`, subagent-analyze the log; commit after every task, never rebase/amend, explicit-path `git add`, verify `git log -1 --stat`; trailer on every commit:

  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```

## Interfaces consumed {#interfaces}

From `Core/Formats/CasTextFormat.h` (phase 1/2, real signatures): `writeHeaderLine(WriteBuffer&, FormatId)`, `writeKey`, `writeStringValue`, `writeU64StringValue`, `closeObject`, `expectHeaderLine(ReadBuffer&, FormatId)→TextHeader`, `readLine(ReadBuffer&, uint64_t, std::string_view)`, `class JsonObjectReader` (`nextKey`/`readString`/`readU64String`/`skipUnknown`), `sealObject`/`openObject` (identity for `Never`). From `Core/Formats/CasFormat.h`: `FormatId::BlobMeta`, `traitsFor(FormatId)`, `KeyStrictness`. From the battery (`src/Disks/tests/cas_format_test_battery.h`): `struct FormatBatteryCase{id, encode, decode, golden}`, `runFormatBattery`.

## The `version` field {#version-field}

The current `BlobMeta` carries `uint8_t version = 1` (the old codec's own version guard). In v3 the header line `v` is the **only** version field (spec §one-file-shape), so the body does NOT serialize `version`. But the struct field STAYS: `CasInspect.cpp:426` renders `.add("version", jsonUInt(m.version))`, so removing it would break introspection. Resolution: keep `BlobMeta.version` (default `1`, vestigial); `encodeBlobMeta` does not write it; `decodeBlobMeta` leaves it at the default `1` — identical to the old codec, which rejected anything but `1` anyway, so `m.version` was always `1`. Introspection is unchanged. (Follow-up, out of scope: `CasInspect` could surface the header `v` instead; not done here to keep the cutover minimal.)

## Condemned-state semantics: the proof obligation {#condemned-semantics}

The one risk: the dedup/resurrect gate must behave identically after the encoding change. Proof, in two independent parts:

1. **The gate never reads raw meta bytes.** Verified against the code: `casMeta(backend, layout, ref, expected, meta)` does `backend.casPut(key, encodeBlobMeta(meta), expected)` — the conditional is the **etag `Token`**, not a byte-compare; `loadMeta` returns `decodeBlobMeta(got->bytes)` + the etag; every consumer (`CasBuild` writer dedup, `CasGc` condemn/resurrect) branches on the **decoded** `meta.state` / `meta.condemn_round` and CAS-swaps by the etag. A grep confirms no `encodeBlobMeta`-bytes equality/compare exists anywhere (the only `->bytes != …` in the subsystem is the reviewed `cas_gc_outcomes` adopt path). So the on-disk byte pattern of a `Condemned` meta is never load-bearing — only its decoded fields and the backend etag are.
2. **The codec round-trips every field.** A `Condemned` meta with any `condemn_round` and `size` must satisfy `decodeBlobMeta(encodeBlobMeta(m)) == m` (state + condemn_round + size), and the `Clean < Condemned` state ordering + fail-closed rejection of an unknown state must be preserved.

Part 1 is discharged by keeping the lifecycle ops byte-agnostic (they already are — this plan does not touch them) and by the kept lifecycle test `CasBlobMeta.PutIfAbsentThenCasTransitions` passing unchanged on the text codec (it drives `Clean → Condemned` via `casMeta`+`loadMeta` through the etag gate). Part 2 is discharged by the new round-trip + fail-closed unit tests in Task 1. Together: **the resurrect gate is codec-independent.** The plan's Task 2 states this as an explicit exit gate.

---

### Task 1: `Formats/CasBlobMetaFormat` — text codec + battery + unit tests {#task1}

**Files:**
- Create: `Core/Formats/CasBlobMetaFormat.h`, `Core/Formats/CasBlobMetaFormat.cpp`
- Test: `src/Disks/tests/gtest_cas_blob_meta_format.cpp` (new)

**Interfaces:**
- Produces: `enum class MetaState`, `struct BlobMeta` (moved verbatim), `String encodeBlobMeta(const BlobMeta&)`, `BlobMeta decodeBlobMeta(std::string_view)` (signatures identical to today, so all call sites — `CasBlobMeta.cpp` ops, `CasInspect.cpp:491` — compile unchanged). Golden text (Clean) and (Condemned):

```
{"type":"cas_blob_meta","v":3}
{"st":"clean","cr":"0","sz":"12345"}
```
```
{"type":"cas_blob_meta","v":3}
{"st":"condemned","cr":"7","sz":"4096"}
```

- [ ] **Step 1: Failing test** — `src/Disks/tests/gtest_cas_blob_meta_format.cpp`:

```cpp
#include "cas_format_test_battery.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobMetaFormat.h>
#include <IO/ReadBufferFromMemory.h>

using namespace DB::Cas;

namespace DB::ErrorCodes { extern const int CORRUPTED_DATA; }

TEST(CasFormatBattery, BlobMeta)
{
    BlobMeta m;
    m.state = MetaState::Clean;
    m.condemn_round = 0;
    m.size = 12345;
    runFormatBattery(FormatBatteryCase{
        .id = FormatId::BlobMeta,
        .encode = [&] { return sealObject(FormatId::BlobMeta, encodeBlobMeta(m)); },
        .decode = [](std::string_view s) { decodeBlobMeta(std::string(openObject(FormatId::BlobMeta, s))); },
        .golden = "{\"type\":\"cas_blob_meta\",\"v\":3}\n"
                  "{\"st\":\"clean\",\"cr\":\"0\",\"sz\":\"12345\"}\n"});
}

TEST(CasBlobMetaFormat, CondemnedRoundTripAllFields)
{
    BlobMeta m;
    m.state = MetaState::Condemned;
    m.condemn_round = 7;
    m.size = 4096;
    const BlobMeta back = decodeBlobMeta(encodeBlobMeta(m));
    EXPECT_EQ(back.state, MetaState::Condemned);
    EXPECT_EQ(back.condemn_round, 7u);
    EXPECT_EQ(back.size, 4096u);
    EXPECT_EQ(encodeBlobMeta(m),
        "{\"type\":\"cas_blob_meta\",\"v\":3}\n{\"st\":\"condemned\",\"cr\":\"7\",\"sz\":\"4096\"}\n");
}

TEST(CasBlobMetaFormat, FailsClosedOnUnknownStateAndTruncation)
{
    /// Unknown state word -> CORRUPTED_DATA (mirrors the old `state > Condemned` reject).
    const String bad_state = "{\"type\":\"cas_blob_meta\",\"v\":3}\n{\"st\":\"zombie\",\"cr\":\"0\",\"sz\":\"0\"}\n";
    EXPECT_THROW(decodeBlobMeta(bad_state), DB::Exception);
    /// Missing state key -> CORRUPTED_DATA.
    const String no_state = "{\"type\":\"cas_blob_meta\",\"v\":3}\n{\"cr\":\"0\",\"sz\":\"0\"}\n";
    EXPECT_THROW(decodeBlobMeta(no_state), DB::Exception);
    /// Truncated (header only) -> CORRUPTED_DATA.
    EXPECT_THROW(decodeBlobMeta("{\"type\":\"cas_blob_meta\",\"v\":3}\n"), DB::Exception);
}
```

- [ ] **Step 2: Verify compile failure** (`CasBlobMetaFormat.h` missing). `flock … ninja … ; echo NINJA_EXIT=$?` → `1`.

- [ ] **Step 3: Implement** — `Core/Formats/CasBlobMetaFormat.h` (struct + enum move here, with their doc comments):

```cpp
#pragma once
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasFormat.h>
#include <base/types.h>
#include <cstdint>
#include <string_view>

namespace DB::Cas
{

/// The per-hash meta descriptor lifecycle (spec 2026-07-09 §raw-body-refinement, v3). The meta is a
/// 2-state FRESHNESS MARKER, not a linearization point: the body's in-body incarnation_tag plus
/// exact-token delete is the safety core, and the meta is only a point-read for the writer's dedup gate.
enum class MetaState : uint8_t
{
    Clean = 0,       /// referenceable; body present (INV-META-BODY)
    Condemned = 1,   /// GC marked in-degree 0; body STILL present (a writer may resurrect by CAS)
};

/// The durable meta body. v3 text form: header line + {"st":"<state word>","cr":"<condemn_round>",
/// "sz":"<size>"}. `size` is the raw body size (introspection/fsck/GC accounting — reads never consult
/// the meta). The meta is CAS-swapped by its backend etag; its on-disk bytes are never compared.
struct BlobMeta
{
    /// Vestigial: the header line `v` is the authoritative format version. Kept (default 1) because
    /// CasInspect renders it; NOT serialized to the body, and decode leaves it at 1 (was always 1).
    uint8_t version = 1;
    MetaState state = MetaState::Clean;
    uint64_t condemn_round = 0;   /// the GC round that condemned this blob (M4: guards a
                                  /// condemned-etag ABA after spare->re-condemn)
    uint64_t size = 0;
};

/// Text codec. encode is total; decode fails closed (CORRUPTED_DATA) on bad header/type/unknown state.
String encodeBlobMeta(const BlobMeta & meta);
BlobMeta decodeBlobMeta(std::string_view bytes);

}
```

`Core/Formats/CasBlobMetaFormat.cpp`:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobMetaFormat.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasTextFormat.h>
#include <Common/Exception.h>
#include <IO/ReadBufferFromMemory.h>
#include <IO/WriteBufferFromString.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int CORRUPTED_DATA;
}
}

namespace DB::Cas
{

namespace
{

std::string_view metaStateToWord(MetaState s)
{
    switch (s)
    {
        case MetaState::Clean:     return "clean";
        case MetaState::Condemned: return "condemned";
    }
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS blob meta: unknown MetaState {}", static_cast<int>(s));
}

MetaState metaStateFromWord(std::string_view w)
{
    if (w == "clean")     return MetaState::Clean;
    if (w == "condemned") return MetaState::Condemned;
    throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS blob meta: unknown state '{}'", w);
}

}

String encodeBlobMeta(const BlobMeta & meta)
{
    WriteBufferFromOwnString out;
    writeHeaderLine(out, FormatId::BlobMeta);
    bool first = true;
    writeKey(out, "st", first); writeStringValue(out, metaStateToWord(meta.state));
    writeKey(out, "cr", first); writeU64StringValue(out, meta.condemn_round);
    writeKey(out, "sz", first); writeU64StringValue(out, meta.size);
    closeObject(out, first);
    writeChar('\n', out);
    out.finalize();
    return out.str();
}

BlobMeta decodeBlobMeta(std::string_view bytes)
{
    ReadBufferFromMemory in(bytes.data(), bytes.size());
    expectHeaderLine(in, FormatId::BlobMeta);
    const String body = readLine(in, traitsFor(FormatId::BlobMeta).line_cap, "blob meta");
    ReadBufferFromMemory body_in(body.data(), body.size());
    JsonObjectReader r(body_in, KeyStrictness::Tolerant, "blob meta");

    BlobMeta m;
    bool saw_state = false;
    String key;
    while (r.nextKey(key))
    {
        if (key == "st") { m.state = metaStateFromWord(r.readString()); saw_state = true; }
        else if (key == "cr") m.condemn_round = r.readU64String();
        else if (key == "sz") m.size = r.readU64String();
        else r.skipUnknown(key);
    }
    if (!saw_state)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS blob meta: missing st");
    if (!body_in.eof() || !in.eof())
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS blob meta: trailing bytes");
    return m;
}

}
```

Note (implementer): `decodeBlobMeta` reads the raw stored bytes (they ARE the text — `Never` compression, no `openObject` needed inside the codec; the lifecycle ops pass `got->bytes` directly, exactly as today). The battery `decode` wraps `openObject` only for harness uniformity.

- [ ] **Step 4: Verify PASS** — `unit_tests_dbms --gtest_filter='CasFormatBattery.BlobMeta:CasBlobMetaFormat*'` → green.

- [ ] **Step 5: Commit** — `git add` the two `Formats/CasBlobMetaFormat.*` + `gtest_cas_blob_meta_format.cpp`. Message `cas: formats v3 phase 4 — cas_blob_meta text codec` + trailer.

---

### Task 2: Split `Core/CasBlobMeta`, migrate codec tests, prove the gate {#task2}

**Files:**
- Modify: `Core/CasBlobMeta.h` (drop the moved struct/enum/codec decls, include the Formats header, keep the ops), `Core/CasBlobMeta.cpp` (drop the `CAMT` binary codec, keep the four ops)
- Modify: `src/Disks/tests/gtest_cas_blob_meta.cpp` (move the two codec tests out; keep lifecycle + inspect tests)
- Include-rewrite: none expected (`CasBlobMeta.h` re-exports the struct via the Formats include; `CasInspect.cpp` already includes `CasBlobMeta.h`) — verify with the grep below.

**Interfaces:**
- Consumes: Task 1's `Formats/CasBlobMetaFormat.h`.
- Produces: `Core/CasBlobMeta.{h,cpp}` retaining `LoadedMeta`, `loadMeta`, `putMetaIfAbsent`, `casMeta`, `deleteMetaExact` — signatures and behavior byte-identical to today; only the codec they call changed file + encoding.

- [ ] **Step 1: Edit `Core/CasBlobMeta.h`** — replace the `MetaState`/`BlobMeta`/`encode`/`decode` block with an include of the Formats header; keep everything else:

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobDigest.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobRef.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/Formats/CasBlobMetaFormat.h>
#include <base/types.h>
#include <Core/Types.h>

#include <optional>
#include <string_view>

namespace DB::Cas
{

/// A loaded meta plus its backend etag — the etag is the conditional token for the next CAS/delete.
struct LoadedMeta
{
    BlobMeta meta;
    Token etag;
};

/// The shared meta-ops layer used by BOTH Build (writer) and Gc. Key-agnostic across all backends.
/// Requires strong read-after-write consistency for the 1-GET adopt (S3 since 2020, RustFS yes).
///
/// Phase 3 T2/T3 (mixed-algo pools): keyed by the full `BlobRef` pair -- the codec is derived INSIDE
/// via `codecFor(ref.algo)` (never a pool-wide width), so callers never thread a `DigestCodec`.
std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const BlobRef & ref);
CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta);
CasResult casMeta(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected, const BlobMeta & meta);
DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected);

}
```

- [ ] **Step 2: Edit `Core/CasBlobMeta.cpp`** — delete the `MAGIC`/`BODY_LEN`/`putU64LE`/`getU64LE`/`encodeBlobMeta`/`decodeBlobMeta` block (moved to Task 1); keep the four ops verbatim. Result:

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>

#include <Common/ProfileEvents.h>

namespace ProfileEvents
{
    extern const Event CasMetaPut;
    extern const Event CasMetaCas;
    extern const Event CasMetaDelete;
}

namespace DB::Cas
{

std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const BlobRef & ref)
{
    const String key = layout.blobMetaKey(ref);
    auto got = backend.get(key);
    if (!got)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(got->bytes), .etag = got->token};
}

CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const BlobRef & ref, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CasMetaPut);
    const String key = layout.blobMetaKey(ref);
    return backend.casPut(key, encodeBlobMeta(meta), std::nullopt);
}

CasResult casMeta(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected, const BlobMeta & meta)
{
    ProfileEvents::increment(ProfileEvents::CasMetaCas);
    const String key = layout.blobMetaKey(ref);
    return backend.casPut(key, encodeBlobMeta(meta), expected);
}

DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const BlobRef & ref, const Token & expected)
{
    ProfileEvents::increment(ProfileEvents::CasMetaDelete);
    const String key = layout.blobMetaKey(ref);
    return backend.deleteExact(key, expected);
}

}
```

(The old `#include <…/CasIds.h>` and `#include <Common/Exception.h>` are dropped — the codec that used them moved. `decodeBlobMeta` throwing lives in the Formats `.cpp` now.)

- [ ] **Step 3: Migrate the codec tests** — in `src/Disks/tests/gtest_cas_blob_meta.cpp`, DELETE `CasBlobMeta.CodecRoundTripsBothStates` (now covered by `CasFormatBattery.BlobMeta` + `CasBlobMetaFormat.CondemnedRoundTripAllFields` in the new file) and `CasBlobMeta.DecodeRejectsBadMagic` (binary-specific; replaced by `CasBlobMetaFormat.FailsClosedOnUnknownStateAndTruncation`). KEEP all lifecycle/inspect tests — they test the Core ops + `CasInspect` against the stable signatures and must pass unchanged: `PutIfAbsentThenCasTransitions`, `DeleteMetaExactMatchesEtag`, `PutLoadCasDeleteRoundTripAtWidth32`, `DedupCacheAdmitsWidth32Digest`, `InspectRendersCondemnedMeta`, `InspectRendersCleanMeta`. (If the file's top includes referenced the codec directly, no change: it includes `CasBlobMeta.h`, which re-exports the struct.)

- [ ] **Step 4: Grep gate + build + the condemned-semantics proof**

```bash
cd <worktree>
D=src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed
# No lingering CAMT binary codec, no meta byte-compare anywhere:
grep -rn 'CAMT\|BODY_LEN' $D/ ; echo "EXPECT: none"
grep -rn 'encodeBlobMeta' $D/Core/*.cpp | grep -iE '== |!=' ; echo "EXPECT: none (gate never byte-compares meta)"
flock /tmp/cas_build.lock ninja -C build_debug unit_tests_dbms > build_debug/build_p4t2.log 2>&1; echo "NINJA_EXIT=$?"
build_debug/src/unit_tests_dbms --gtest_filter='CasBlobMeta*:CasFormatBattery.BlobMeta' 2>&1 | tail -5
```
Expected: `NINJA_EXIT=0`; both greps empty; ALL `CasBlobMeta.*` green — in particular **`CasBlobMeta.PutIfAbsentThenCasTransitions` passes unchanged**, which is the Part-1 discharge of [§condemned-semantics](#condemned-semantics): the `Clean → Condemned` transition via `casMeta`+`loadMeta` through the etag gate is codec-independent. Subagent-analyze the build log; return a summary.

- [ ] **Step 5: Commit** — `git add` `Core/CasBlobMeta.{h,cpp}` + `gtest_cas_blob_meta.cpp`. Message `cas: formats v3 phase 4 — split Core/CasBlobMeta; keep the etag-gated lifecycle ops` + trailer.

---

## Phase-4 exit criteria {#exit-criteria}

- `unit_tests_dbms --gtest_filter='Cas*'` fully green.
- `cas_blob_meta` is one-line text; `MetaState`/`BlobMeta`/`encodeBlobMeta`/`decodeBlobMeta` live in `Core/Formats/CasBlobMetaFormat`; the four lifecycle ops stay in `Core/CasBlobMeta`.
- One `FormatBatteryCase.BlobMeta` row; golden pinned; `Never` (no `.zst`); `blobMetaKey` + its parser untouched.
- **Condemned-state semantics proven unchanged:** the gate never byte-compares meta (grep-verified), the codec round-trips every field (unit test), and `CasBlobMeta.PutIfAbsentThenCasTransitions` passes unchanged on the text codec (etag gate = codec-independent).
- `BlobMeta.version` retained (vestigial, default 1) for `CasInspect`; the header `v` is the authoritative version.
- No protobuf involved (blob meta was the `CAMT` binary, not proto) — nothing added to or removed from the proto graveyard.

## Draft packaging note {#draft-note}

When implementing the draft (task-2 message from the lead): branch `tmp/worktrees/draft-codecs4` from mainline HEAD, **apply the phase-2 `DRAFT.patch` first** (base layer), then implement Tasks 1–2 on top. Record the layering order in DRAFT.md so the integrator applies phase-2 then phase-4. Same no-build contract; generate `DRAFT.patch` with `git add -A && git diff --cached -M --binary` (exclude `DRAFT.md`/`DRAFT.patch` themselves).

# CAS per-hash meta-descriptor (raw-body) Implementation Plan

> **⛔ SUPERSEDED / REJECTED (2026-07-10).** This raw-body / terminal-tombstone plan recreated the
> already-rejected generation-in-key design (per the user + `docs/superpowers/cas/01-architecture.md`
> §"Approaches tested and REJECTED"). Use **`2026-07-10-cas-freshness-meta-v3.md`** instead, which keeps the
> settled one-key-per-hash + in-body `incarnation_tag` + exact-token BODY delete and adds only a freshness
> meta. Kept here for the record.



> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the writer-distributed condemned-list freshness machinery with a per-hash `.meta` descriptor (a three-state `{clean, condemned, tombstone}` register whose etag is the sole linearization token), drop the blob envelope so bodies are raw immutable content, and delete the writer-side `RetireView`/syncer/`observed_gc_round`/ack-floor — preserving `INV_NO_DANGLE`, `INV_NO_LOSS`, `INV_NO_RETURN` under a re-run TLA+ gate.

**Architecture:** Per blob hash there are now two objects: the **body** (`blobs/xx/<hash>`, raw payload, write-once via `PUT If-None-Match`, etag = content) and the **meta** (`blobs/xx/<hash>.meta`, ~a few dozen bytes, the ONLY conditionally-operated object). `INV-META-BODY` (meta present ⇒ body present) is maintained by ordering — **create bottom-up** (body then meta), **delete top-down** (meta then body) — and is what makes the writer's dedup path a single meta GET. The writer sources condemned status from a point-read of the meta (intrinsically fresh under strong read-after-write consistency) instead of a periodically-synced list, so the ack-floor and the whole writer-side view distribution disappear. GC condemns/spares/deletes by CASing the meta; deletion is a tombstone handshake (meta condemned→tombstone, then body, then tombstone meta) run on a bounded parallel pool.

**Tech Stack:** ClickHouse C++ (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`), gtest (`unit_tests_dbms`), TLC/TLA+ (`docs/superpowers/models/`), CA-s3 stateless + `utils/ca-soak` for integration.

## v2 CORRECTIONS (consult 2026-07-10 — read before implementing Tasks 2–7)

A fresh-model consult found a CRITICAL hole in the first cut and the design + model were corrected (spec
§raw-body-refinement "v2 CORRECTION"; `CaMetaDescriptorRaw.tla` v2 re-run GREEN + `sab_resurrect_tomb` RED).
The binding changes, folded into the tasks below:

1. **Tombstone is TERMINAL.** A writer that observes a `tombstone` meta MUST NOT `CAS tombstone→clean` — under
   raw immutable bodies that dangles a committed ref (GC's already-committed body delete still hits the
   unchanged-token body). Instead the writer **waits (bounded) for the meta to reach `absent`, then
   fresh-uploads**; bounded exhaustion ⇒ `ABORTED`. Resurrect is legal **only from `condemned`**.
2. **Birth-completion is from `absent`-meta only** (a crashed pre-meta birth), never from `tombstone`.
3. **The meta carries a fresh `incarnation` (u128) nonce on EVERY write** (Task 1B) — S3 etags are
   content-derived, so a `clean` meta would ABA without it.
4. **GC delete = terminal tombstone handshake** (Task 5): CAS condemned→tombstone; on win HEAD+delete body
   (safe: terminal tombstone means no writer re-established the body after the win); delete tombstone meta.
5. Task-specific fixes: keep the large-body head-first guard (Task 3); ca-inspect raw-body branch + schedule
   `CasEnvelope` removal (Task 2); three extra removal sites in Task 6; a deterministic GC-delete-vs-writer
   race test (Task 7). Each is noted in its task.

## Global Constraints

- **Branch discipline:** work on `cas-gc-rebuild`; add new commits, never rebase/amend/force; never commit to `master`. (CLAUDE.md)
- **Design source of truth:** `docs/superpowers/specs/2026-07-09-cas-writer-gc-simplification-design.md`, Part II (§meta-descriptor, §raw-body-refinement, §meta-protocols, §phase-b-deletions) + consult findings C1/C2/I1/I2/I3/M2/M4/M5. The meta protocol table (spec lines 139-148) is normative — every case handled there must be handled in code.
- **Formal gate:** the TLA+ model `docs/superpowers/models/CaMetaDescriptorRaw.tla` is the authority for the writer↔GC state machine. Code must match the model's transitions (`FreshUpload`, `Adopt`, `Resurrect`, `BirthCompletion`, `GcCondemn`, `GcSpare`, `GcDeletePhaseA/B/C`). No code before Gate B evidence is persisted (Task 0).
- **No compat scaffolding, no paranoid mode:** CAS is pre-release; the pool layout changes (metas added, envelope removed) — existing dev pools are recreated, no migration, no dual read/write path, no config flag preserving old behavior. (spec §non-goals; CLAUDE.md "avoid fallback paths")
- **Fail-close on the destructive path:** GC never deletes a body except under a won tombstone claim; GC never throws/fail-closes on a 404 during fold (record + continue). (`feedback_ca_gc_never_throw_on_404`, `feedback_ca_resurrect_invariant`)
- **Never read/GET a condemned object to revive it** — revival = fresh re-upload/recreate from source, EXCEPT the one documented INV-1 exception (tokenless copy-forward of a committed-source dep reads its still-present condemned body). (`feedback_ca_resurrect_invariant`)
- **Backend requirement:** strong read-after-write consistency (S3 since 2020, RustFS yes) — record as a pool requirement in code comments where the 1-GET adopt relies on it.
- **C++ style:** Allman braces; say "exception" not "crash" for logical errors; write function names as `f` not `f()` in prose; ASan (two words: Address Sanitizer). (CLAUDE.md)
- **Build/test hygiene:** never pass `-j`/`nproc` to ninja; always redirect ninja output to `build/<name>_build.log` and each test run to a uniquely-named `build/test_<name>.log`; hand every build/test log to a subagent to summarize. (CLAUDE.md)
- **Mass-DROP requirement:** GC condemn/spare/delete meta operations MUST run on a bounded parallel pool — 1M condemns sequential at ~20 ms/op ≈ 5.5 h/pass is unacceptable (spec §meta-budget).
- **Test tags:** do not add `no-*` tags unless strictly necessary; prefer a new test over extending one; use `./tests/queries/0_stateless/add-test` for new stateless tests. (CLAUDE.md)

---

## File Structure

**New files:**
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h` — the `BlobMeta` struct, `MetaState` enum, codec (`encodeBlobMeta`/`decodeBlobMeta`), and the shared meta-ops layer (`loadMeta`/`putMetaIfAbsent`/`casMeta`/`deleteMetaExact`, `LoadedMeta`). One responsibility: the meta object and its conditional operations, shared by `Build` and `Gc`.
- `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp` — codec + meta-ops implementation.
- `src/Disks/tests/gtest_cas_blob_meta.cpp` — codec round-trip + meta-ops (CAS/If-None-Match/delete) unit tests.
- `docs/superpowers/models/CaManifestSweepWindow.tla` + `.cfg` family — the wedge (committed-removal-scoping) TLA+ gate (Task 0b).

**Modified files (by task):**
- `Core/CasLayout.h` — add `blobMetaKey` (Task 1).
- `Core/CasInspect.cpp` — `.meta` dispatch before the `blobs/` branch (Task 2).
- `Core/CasFsck.{h,cpp}` — INV-META-BODY pairing + partition `.meta` keys out of the body LIST (Task 2).
- `Core/CasStore.{h,cpp}` — read-path `locate` offset→0 (Task 3); delete `RetireView`/syncer/`observedGcRound` (Task 6).
- `Core/CasBuild.{h,cpp}` — raw body write (drop envelope) + meta lifecycle in `putBlob`/`uploadFromSource`; K3 + copy-forward re-sourced to meta (Tasks 3, 4).
- `Core/CasEnvelope.{h,cpp}` — the blob-envelope encode path becomes unused for bodies; keep the codec for `ca-inspect` legacy/none — see Task 3 note (Task 3).
- `Core/CasGc.cpp`, `Core/CasGcFormats.h`, `Core/CasBlobInDegree.{h,cpp}` — condemn/spare/delete as meta ops; `RetiredEntry.token` = meta etag; `peek_head`→`peek_meta`; tombstone-handshake delete on a parallel pool; `graduationDue` re-key; `rebuildBaseline` meta capture (Tasks 4, 5).
- `Core/CasServerRoot.{h,cpp}` — drop `observed_gc_round` from the beat + `MountLease`; drop `min_ack` graduation gating (Task 6).
- `src/Disks/tests/cas_test_helpers.h` — meta-aware helpers (`writeRawBlobBody`, `writeMetaClean`, `condemnMeta`, `loadMetaForTest`) (Task 1) and migration of existing helpers (Tasks 3-6).
- Existing gtests migrated per task; `gtest_cas_retire_view.cpp` **deleted** (Task 6).

---

## Task 0: TLA+ gates — persist Gate B evidence + the wedge committed-removal gate

**Files:**
- Run/persist: `docs/superpowers/models/CaMetaDescriptorRaw.tla` + its 5 `.cfg` files (already authored); logs to `tmp/tlc_CaMetaDescriptorRaw_*.log`.
- Create: `docs/superpowers/models/CaManifestSweepWindow.tla`, `docs/superpowers/models/CaManifestSweepWindow_reduced.cfg`, `docs/superpowers/models/CaManifestSweepWindow_sab_sweep_committed.cfg`.
- Runner: `docs/superpowers/models/run_metaraw.sh` (exists; takes cfg basename WITHOUT `.cfg`).

**Interfaces:**
- Consumes: `tmp/tla2tools.jar`; `run_metaraw.sh <basename>` runs `CaMetaDescriptorRaw.tla`.
- Produces: persisted green/red logs establishing the formal gate; a new self-contained model + runner for the manifest-sweep window. No C++ symbols.

**Why this task is first:** the design's formal gate (spec §tla-gates Gate B) is a precondition for touching code, and the persisted green log does not currently exist on disk (only a failed mis-invocation `tmp/tlc_CaMetaDescriptorRaw.log`). The wedge gate is the backlogged "committed-removal scoping" debt (`gtest_cas_orphan_manifest_sweep.cpp::PendingCommittedRemovalBodyIsSkipped` has the C++ regression; the TLA+ gate was never written).

- [x] **Step 1: Re-run the Gate B raw-body positive config and confirm green** — DONE 2026-07-10: `No error has been found` (`tmp/tlc_CaMetaDescriptorRaw_reduced.log`).

Run (from repo root):
```bash
cd docs/superpowers/models && ./run_metaraw.sh CaMetaDescriptorRaw_reduced; cd -
```
Expected: `tmp/tlc_CaMetaDescriptorRaw_reduced.log` ends with `Model checking completed. No error has been found.` (Hand the log to a subagent to confirm the success signature — do not eyeball a 100k-line log.)

- [x] **Step 2: Re-run all four sabotage configs and confirm each stays RED** — DONE 2026-07-10: `sab_meta_first`/`sab_blind_adopt`/`sab_adopt_tomb` → `INV_NO_DANGLE violated`; `sab_del_notomb` → `INV_META_BODY violated`.

Run:
```bash
cd docs/superpowers/models
for s in sab_meta_first sab_blind_adopt sab_adopt_tomb sab_del_notomb; do
  ./run_metaraw.sh "CaMetaDescriptorRaw_$s"
done
cd -
```
Expected (verify via subagent summarizing each `tmp/tlc_CaMetaDescriptorRaw_sab_*.log`):
- `sab_meta_first` → `INV_META_BODY` or `INV_NO_DANGLE` violated (meta written before body).
- `sab_blind_adopt` → `INV_NO_DANGLE` violated (adopt over condemned/tombstone).
- `sab_adopt_tomb` → `INV_NO_DANGLE` violated (adopt over tombstone mid-delete).
- `sab_del_notomb` → `INV_META_BODY` violated (body deleted under condemned without a tombstone claim).

If any positive/sabotage result disagrees with the above, STOP — the model or the design has drifted; escalate before writing code.

- [ ] **Step 3: Write the wedge model `CaManifestSweepWindow.tla`**

Model a single committed manifest body and the orphan-manifest sweep vs the removal-fold, capturing the GC-WEDGE-2026-07-10 window. Create `docs/superpowers/models/CaManifestSweepWindow.tla`:

```tla
---------------------- MODULE CaManifestSweepWindow ----------------------
(* Wedge gate (2026-07-10): a COMMITTED manifest ref is dropped; its removal `-1` is appended to the
   shard journal but NOT yet sealed by the GC fold. A promoted build retired its build_seq, so the
   prefix is watermark-eligible for the orphan-manifest sweep. The sweep must NOT delete the committed
   body in the dropRef->fold-seal window: the removal-fold still needs the body present to emit the
   decrement. SabSweepCommitted drops the pending-committed-removal protection and MUST break the gate:
   the sweep deletes the body, the fold then can never decrement -> INV_FOLD_PROGRESS violated forever. *)
EXTENDS Naturals

CONSTANTS SabSweepCommitted   \* sweep ignores pending-committed-removals (the pre-fix bug)

VARIABLES
  body,            \* BOOLEAN: the committed manifest body object present?
  ownerState,      \* "committed" | "removing" (removal appended, unsealed) | "removed"
  sealedRemoval,   \* BOOLEAN: the -1 removal has been folded/sealed
  swept            \* BOOLEAN: the orphan sweep ran on this eligible prefix

vars == <<body, ownerState, sealedRemoval, swept>>

Init ==
  /\ body = TRUE
  /\ ownerState = "committed"
  /\ sealedRemoval = FALSE
  /\ swept = FALSE

\* Drop the committed ref: append the -1 removal (unsealed). Prefix becomes sweep-eligible.
DropRef ==
  /\ ownerState = "committed"
  /\ ownerState' = "removing"
  /\ UNCHANGED <<body, sealedRemoval, swept>>

\* The removal-fold: requires the body present to emit the decrement, then seals. If the body is gone
\* it CANNOT proceed (the real clamp: "edge-bearing committed body missing at removal-fold").
FoldSealRemoval ==
  /\ ownerState = "removing" /\ ~sealedRemoval /\ body
  /\ sealedRemoval' = TRUE /\ ownerState' = "removed"
  /\ UNCHANGED <<body, swept>>

\* The orphan-manifest sweep on the eligible prefix. CORRECT: skip a body with a pending (unsealed)
\* committed removal. SABOTAGE: delete it anyway.
Sweep ==
  /\ ~swept /\ body
  /\ swept' = TRUE
  /\ IF SabSweepCommitted
     THEN body' = FALSE                                     \* pre-fix: deletes the still-needed body
     ELSE IF ownerState = "removing" /\ ~sealedRemoval
          THEN body' = body                                 \* fix: protect the pending committed removal
          ELSE IF ownerState = "removed"
               THEN body' = FALSE                           \* fully sealed -> orphan, deletable
               ELSE body' = body                            \* committed & live -> owned, skip
  /\ UNCHANGED <<ownerState, sealedRemoval>>

Next == DropRef \/ FoldSealRemoval \/ Sweep

Spec == Init /\ [][Next]_vars /\ WF_vars(FoldSealRemoval)

TypeOK ==
  /\ body \in BOOLEAN /\ sealedRemoval \in BOOLEAN /\ swept \in BOOLEAN
  /\ ownerState \in {"committed","removing","removed"}

(* A removal that has begun must eventually seal — impossible if the body was swept away first. *)
INV_FOLD_PROGRESS == (ownerState = "removing") => (body \/ sealedRemoval)

=========================================================================
```

- [ ] **Step 4: Write the two configs**

Create `docs/superpowers/models/CaManifestSweepWindow_reduced.cfg` (cfg comments use `\*`, not `*`):
```
\* Wedge gate (reduced): the fix (protect pending-committed-removals) holds INV_FOLD_PROGRESS.
SPECIFICATION Spec
CONSTANTS SabSweepCommitted = FALSE
INVARIANT TypeOK
INVARIANT INV_FOLD_PROGRESS
```
Create `docs/superpowers/models/CaManifestSweepWindow_sab_sweep_committed.cfg`:
```
\* Wedge sabotage: sweep deletes the committed body pre-seal -> INV_FOLD_PROGRESS violated.
SPECIFICATION Spec
CONSTANTS SabSweepCommitted = TRUE
INVARIANT TypeOK
INVARIANT INV_FOLD_PROGRESS
```

- [ ] **Step 5: Add a runner and run both configs**

`run_metaraw.sh` hardcodes `CaMetaDescriptorRaw.tla`, so add a sibling runner `docs/superpowers/models/run_sweepwindow.sh` (copy `run_metaraw.sh`, change the target `.tla` to `CaManifestSweepWindow.tla`):
```bash
#!/usr/bin/env bash
set -u
CFG="$1"
LOG="../../../tmp/tlc_${CFG}.log"
java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
  -metadir ../../../tmp/tlc-meta -workers auto -deadlock \
  -config "${CFG}.cfg" CaManifestSweepWindow.tla >"$LOG" 2>&1 || true
grep -E "No error has been found|is violated|Parse Error|Error:" "$LOG"
```
Run:
```bash
cd docs/superpowers/models && chmod +x run_sweepwindow.sh
./run_sweepwindow.sh CaManifestSweepWindow_reduced
./run_sweepwindow.sh CaManifestSweepWindow_sab_sweep_committed
cd -
```
Expected: `reduced` → `No error has been found`; `sab_sweep_committed` → `INV_FOLD_PROGRESS is violated`.

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/models/CaManifestSweepWindow.tla \
        docs/superpowers/models/CaManifestSweepWindow_reduced.cfg \
        docs/superpowers/models/CaManifestSweepWindow_sab_sweep_committed.cfg \
        docs/superpowers/models/run_sweepwindow.sh
git commit -m "test(cas): TLA+ gate for the committed-removal sweep window (wedge regression)

Persist Gate B raw-body evidence (CaMetaDescriptorRaw green + 4 sabotages red) and add a
standalone model proving the orphan-manifest sweep must spare a committed body until its
removal is sealed (GC-WEDGE-2026-07-10). Precedes the meta-descriptor code.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 1: `BlobMeta` codec + shared meta-ops layer

**Files:**
- Create: `Core/CasBlobMeta.h`, `Core/CasBlobMeta.cpp`
- Modify: `Core/CasLayout.h` (add `blobMetaKey`)
- Modify: `src/Disks/tests/cas_test_helpers.h` (add meta helpers)
- Test: `src/Disks/tests/gtest_cas_blob_meta.cpp` (new)

**Interfaces:**
- Consumes: `Cas::Backend` (`casPut(key, bytes, optional<Token> expected)`, `get`, `deleteExact`), `Cas::Layout`, `Cas::Token`, `Cas::u128ToHex`, `Cas::BlobId`.
- Produces:
  - `enum class Cas::MetaState : uint8_t { Clean = 0, Condemned = 1, Tombstone = 2 };`
  - `struct Cas::BlobMeta { uint8_t version = 1; MetaState state = MetaState::Clean; uint64_t condemn_round = 0; uint64_t size = 0; };`
  - `String Cas::encodeBlobMeta(const BlobMeta &);`
  - `BlobMeta Cas::decodeBlobMeta(std::string_view);`  (fail-closed on bad magic/version/length → `CORRUPTED_DATA`)
  - `String Layout::blobMetaKey(const BlobId & id) const;`  (= `blobKey(id) + ".meta"`)
  - `struct Cas::LoadedMeta { BlobMeta meta; Token etag; };`
  - `std::optional<LoadedMeta> Cas::loadMeta(Backend &, const Layout &, const UInt128 & hash);`  (nullopt = meta absent)
  - `CasResult Cas::putMetaIfAbsent(Backend &, const Layout &, const UInt128 & hash, const BlobMeta &);`  (create; `casPut` with `expected == nullopt`)
  - `CasResult Cas::casMeta(Backend &, const Layout &, const UInt128 & hash, const Token & expected, const BlobMeta &);`  (transition; `casPut` If-Match)
  - `DeleteOutcome Cas::deleteMetaExact(Backend &, const Layout &, const UInt128 & hash, const Token & expected);`
  - Test helpers in `cas_test_helpers.h`: `writeRawBlobBody(backend, layout, hash, payload)`, `writeMetaClean(backend, layout, hash, size)`, `condemnMeta(backend, layout, hash, condemn_round)`, `std::optional<LoadedMeta> loadMetaForTest(backend, layout, hash)`.

- [ ] **Step 1: Write the failing codec round-trip test**

Create `src/Disks/tests/gtest_cas_blob_meta.cpp`:
```cpp
#include <gtest/gtest.h>

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include "cas_test_helpers.h"

using namespace DB::Cas;
using namespace DB::Cas::tests;

TEST(CasBlobMeta, CodecRoundTripsAllStates)
{
    for (MetaState s : {MetaState::Clean, MetaState::Condemned, MetaState::Tombstone})
    {
        BlobMeta m{.version = 1, .state = s, .condemn_round = 42, .size = 1 << 20};
        const BlobMeta back = decodeBlobMeta(encodeBlobMeta(m));
        EXPECT_EQ(static_cast<uint8_t>(back.state), static_cast<uint8_t>(s));
        EXPECT_EQ(back.condemn_round, 42u);
        EXPECT_EQ(back.size, 1u << 20);
        EXPECT_EQ(back.version, 1u);
    }
}

TEST(CasBlobMeta, DecodeRejectsBadMagic)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [] { decodeBlobMeta("not-a-meta-object"); });
}
```

- [ ] **Step 2: Run to verify it fails (compile error — symbols undefined)**

```bash
ninja -C build unit_tests_dbms > build/cas_meta_codec_build.log 2>&1
```
Expected: FAIL — `CasBlobMeta.h` not found / `encodeBlobMeta` undefined. (Summarize the log via subagent.)

- [ ] **Step 3: Write `CasBlobMeta.h`**

```cpp
#pragma once

#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBackend.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasToken.h>
#include <base/types.h>
#include <Core/Types.h>

#include <optional>
#include <string_view>

namespace DB::Cas
{

/// The per-hash meta descriptor lifecycle (spec 2026-07-09 §raw-body-refinement). The meta is the SOLE
/// linearization point for a content hash: its backend etag (the "gen") is the only conditional token.
/// The body is raw immutable content (etag == content); it carries no state.
enum class MetaState : uint8_t
{
    Clean = 0,       /// referenceable; body present (INV-META-BODY)
    Condemned = 1,   /// GC marked in-degree 0; body STILL present (a writer may resurrect by CAS)
    Tombstone = 2,   /// GC won the delete race; body deletion in progress / done — never adoptable
};

/// The durable meta body (fixed codec, ~a few dozen bytes). `version` guards codec evolution; `size` is
/// the raw body size (introspection/fsck/GC accounting — reads never consult the meta). No incarnation
/// field: under raw bodies the meta etag is the incarnation.
struct BlobMeta
{
    uint8_t version = 1;
    MetaState state = MetaState::Clean;
    uint64_t condemn_round = 0;   /// the GC round that condemned this incarnation (M4: guards a
                                  /// condemned-etag ABA after spare->re-condemn)
    uint64_t size = 0;
};

/// Fixed codec. encode is total; decode fails closed (CORRUPTED_DATA) on bad magic/version/length.
String encodeBlobMeta(const BlobMeta & meta);
BlobMeta decodeBlobMeta(std::string_view bytes);

/// A loaded meta plus its backend etag — the etag is the conditional token for the next CAS/delete.
struct LoadedMeta
{
    BlobMeta meta;
    Token etag;
};

/// The shared meta-ops layer used by BOTH Build (writer) and Gc. Key-agnostic across all backends.
/// Requires strong read-after-write consistency for the 1-GET adopt (S3 since 2020, RustFS yes).
std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const UInt128 & hash);
CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const UInt128 & hash, const BlobMeta & meta);
CasResult casMeta(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected, const BlobMeta & meta);
DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected);

}
```

- [ ] **Step 4: Write `CasBlobMeta.cpp`**

Use the codebase's little-endian fixed-width writers (mirror `CasGcFormats.cpp`/`CasEnvelope.cpp` style: a 4-byte magic, 1-byte version, 1-byte state, two 8-byte little-endian u64s). Magic `"CAMT"`.
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasIds.h>

#include <Common/Exception.h>

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace DB::Cas
{

namespace
{
constexpr std::string_view MAGIC = "CAMT";
constexpr size_t BODY_LEN = 4 + 1 + 1 + 8 + 8;   /// magic + version + state + condemn_round + size

void putU64LE(String & out, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

uint64_t getU64LE(std::string_view b, size_t off)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(static_cast<uint8_t>(b[off + i])) << (8 * i);
    return v;
}
}

String encodeBlobMeta(const BlobMeta & meta)
{
    String out;
    out.reserve(BODY_LEN);
    out.append(MAGIC);
    out.push_back(static_cast<char>(meta.version));
    out.push_back(static_cast<char>(static_cast<uint8_t>(meta.state)));
    putU64LE(out, meta.condemn_round);
    putU64LE(out, meta.size);
    return out;
}

BlobMeta decodeBlobMeta(std::string_view bytes)
{
    if (bytes.size() != BODY_LEN || bytes.substr(0, 4) != MAGIC)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: bad magic or length ({} bytes)", bytes.size());
    BlobMeta m;
    m.version = static_cast<uint8_t>(bytes[4]);
    if (m.version != 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: unknown version {}", m.version);
    const uint8_t s = static_cast<uint8_t>(bytes[5]);
    if (s > static_cast<uint8_t>(MetaState::Tombstone))
        throw Exception(ErrorCodes::CORRUPTED_DATA, "decodeBlobMeta: bad state {}", s);
    m.state = static_cast<MetaState>(s);
    m.condemn_round = getU64LE(bytes, 6);
    m.size = getU64LE(bytes, 14);
    return m;
}

std::optional<LoadedMeta> loadMeta(Backend & backend, const Layout & layout, const UInt128 & hash)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    auto got = backend.get(key);
    if (!got)
        return std::nullopt;
    return LoadedMeta{.meta = decodeBlobMeta(got->bytes), .etag = got->token};
}

CasResult putMetaIfAbsent(Backend & backend, const Layout & layout, const UInt128 & hash, const BlobMeta & meta)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.casPut(key, encodeBlobMeta(meta), std::nullopt);
}

CasResult casMeta(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected, const BlobMeta & meta)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.casPut(key, encodeBlobMeta(meta), expected);
}

DeleteOutcome deleteMetaExact(Backend & backend, const Layout & layout, const UInt128 & hash, const Token & expected)
{
    const String key = layout.blobMetaKey(BlobId(u128ToHex(hash)));
    return backend.deleteExact(key, expected);
}

}
```

- [ ] **Step 5: Add `blobMetaKey` to `CasLayout.h`**

Immediately after `blobKey` (`CasLayout.h:45-48`):
```cpp
    /// The per-hash meta descriptor sibling of the blob body (spec §raw-body-refinement).
    String blobMetaKey(const BlobId & id) const
    {
        return blobKey(id) + ".meta";
    }
```

- [ ] **Step 6: Register `CasBlobMeta.cpp` in the build if needed**

The CAS core sources are glob-collected. Confirm by grepping the CMake for how `CasGcFormats.cpp` is listed:
```bash
grep -rn "CasGcFormats.cpp\|GLOB.*ContentAddressed" src/Disks/CMakeLists.txt src/CMakeLists.txt 2>/dev/null
```
If sources are globbed (no explicit list), no change is needed. If `CasGcFormats.cpp` is explicitly listed, add `CasBlobMeta.cpp` beside it in the same list.

- [ ] **Step 7: Add the meta test helpers to `cas_test_helpers.h`**

After `writeBlobBody` (`cas_test_helpers.h:475`), add (raw body = payload written verbatim, no envelope; meta via the ops layer):
```cpp
inline void writeRawBlobBody(DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
                             const DB::UInt128 & hash, const String & payload)
{
    backend.casPut(layout.blobKey(DB::Cas::BlobId(DB::Cas::u128ToHex(hash))), payload, std::nullopt);
}

inline void writeMetaClean(DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
                           const DB::UInt128 & hash, uint64_t size)
{
    DB::Cas::putMetaIfAbsent(backend, layout, hash,
        DB::Cas::BlobMeta{.state = DB::Cas::MetaState::Clean, .condemn_round = 0, .size = size});
}

inline void condemnMeta(DB::Cas::Backend & backend, const DB::Cas::Layout & layout,
                        const DB::UInt128 & hash, uint64_t condemn_round)
{
    const auto lm = DB::Cas::loadMeta(backend, layout, hash);
    ASSERT_TRUE(lm.has_value());
    DB::Cas::BlobMeta c = lm->meta;
    c.state = DB::Cas::MetaState::Condemned;
    c.condemn_round = condemn_round;
    DB::Cas::casMeta(backend, layout, hash, lm->etag, c);
}

inline std::optional<DB::Cas::LoadedMeta> loadMetaForTest(DB::Cas::Backend & backend,
                                                          const DB::Cas::Layout & layout, const DB::UInt128 & hash)
{
    return DB::Cas::loadMeta(backend, layout, hash);
}
```
Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>` to the header's include block.

- [ ] **Step 8: Add meta-ops tests to `gtest_cas_blob_meta.cpp`**

Append:
```cpp
TEST(CasBlobMeta, PutIfAbsentThenCasTransitions)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("hash-a");

    const CasResult created = putMetaIfAbsent(*backend, store->layout(), h,
        BlobMeta{.state = MetaState::Clean, .size = 10});
    EXPECT_EQ(created.outcome, CasOutcome::Committed);

    const CasResult dup = putMetaIfAbsent(*backend, store->layout(), h, BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(dup.outcome, CasOutcome::Conflict);   // If-None-Match rejects a second create

    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);

    const CasResult condemned = casMeta(*backend, store->layout(), h, lm->etag,
        BlobMeta{.state = MetaState::Condemned, .condemn_round = 5, .size = 10});
    EXPECT_EQ(condemned.outcome, CasOutcome::Committed);

    const CasResult stale = casMeta(*backend, store->layout(), h, lm->etag,   // stale etag loses
        BlobMeta{.state = MetaState::Clean});
    EXPECT_EQ(stale.outcome, CasOutcome::Conflict);
}

TEST(CasBlobMeta, DeleteMetaExactMatchesEtag)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("hash-b");
    putMetaIfAbsent(*backend, store->layout(), h, BlobMeta{.state = MetaState::Tombstone});
    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(deleteMetaExact(*backend, store->layout(), h, lm->etag).kind, DeleteOutcome::Kind::Deleted);
    EXPECT_FALSE(loadMeta(*backend, store->layout(), h).has_value());
}
```

- [ ] **Step 9: Build and run the meta codec + ops tests**

```bash
ninja -C build unit_tests_dbms > build/cas_meta_codec_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.*' > build/test_cas_blob_meta.log 2>&1
```
Expected: all `CasBlobMeta.*` pass. (Summarize both logs via subagent.)

- [ ] **Step 10: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/tests/gtest_cas_blob_meta.cpp src/Disks/tests/cas_test_helpers.h
git commit -m "feat(cas): BlobMeta three-state codec + shared meta-ops layer

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 1B: Add the fresh `incarnation` nonce to `BlobMeta` (v2 consult finding #8)

**Files:**
- Modify: `Core/CasBlobMeta.h`, `Core/CasBlobMeta.cpp`
- Modify: `src/Disks/tests/cas_test_helpers.h` (helpers set the nonce), `src/Disks/tests/gtest_cas_blob_meta.cpp`

**Interfaces:**
- Consumes: the Task 1 `BlobMeta`/codec/ops layer (committed `ba883680114`); `Cas::UInt128`, `Cas::mintU128` (the fresh-nonce minter used by `CasBuild.cpp`'s `header.incarnation_tag = mintU128()`).
- Produces: `BlobMeta` gains `UInt128 incarnation{};` (bump `version` to 2, append the 16 bytes to the codec, widen `BODY_LEN` by 16; decode fails closed on the old length). Every code path that WRITES a meta mints a fresh `incarnation` (`putMetaIfAbsent`/`casMeta` callers pass a `BlobMeta` whose `incarnation = mintU128()`). This makes each meta object's bytes — and its S3 etag — globally unique, matching the model's fresh-`gen`-per-write.

**Why:** S3 ETags are content-derived; without a per-write nonce a `clean` meta re-encodes to an identical etag across incarnations → a latent ABA on the `clean→condemned` precondition. The nonce makes "meta etag = incarnation" literally true.

- [ ] **Step 1: Write the failing test — two clean metas for the same hash differ**

Append to `gtest_cas_blob_meta.cpp`:
```cpp
TEST(CasBlobMeta, FreshIncarnationMakesEachWriteUnique)
{
    BlobMeta a{.state = MetaState::Clean, .size = 1};
    BlobMeta b{.state = MetaState::Clean, .size = 1};
    a.incarnation = DB::Cas::mintU128();
    b.incarnation = DB::Cas::mintU128();
    EXPECT_NE(a.incarnation, b.incarnation);
    EXPECT_NE(encodeBlobMeta(a), encodeBlobMeta(b));   // distinct bytes -> distinct S3 etag
    EXPECT_EQ(decodeBlobMeta(encodeBlobMeta(a)).incarnation, a.incarnation);   // round-trips
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/cas_meta_incarnation_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.FreshIncarnationMakesEachWriteUnique' > build/test_cas_meta_incarnation.log 2>&1
```
Expected: FAIL — `BlobMeta` has no `incarnation` member.

- [ ] **Step 3: Add the field + extend the codec**

In `CasBlobMeta.h`, add to `BlobMeta` (after `state`): `UInt128 incarnation{};` and bump the doc/`version` default to 2. Add `#include ".../CasIds.h"` if `mintU128`/`UInt128` need it (mintU128 is declared where `CasBuild.cpp` gets it — reuse that header). In `CasBlobMeta.cpp`: `constexpr size_t BODY_LEN = 4 + 1 + 1 + 8 + 8 + 16;` (append 16 for the u128); write the u128 after `size` via two `putU64LE` of its halves (mirror how `CasEnvelope.cpp`/`CasGcFormats.cpp` serialize a `UInt128` — use the SAME byte order helper the codebase uses for u128, e.g. `writeBinaryLittleEndian`/the envelope's u128 writer); read it back in `decodeBlobMeta`; keep `if (bytes.size() != BODY_LEN ...)` fail-closed (an old 22-byte meta now mismatches and throws CORRUPTED_DATA — correct, pre-release, no compat).

- [ ] **Step 4: Mint the nonce at every write site in the helpers**

In `cas_test_helpers.h`, update `writeMetaClean`/`condemnMeta` to set `.incarnation = DB::Cas::mintU128()` on the `BlobMeta` they write. (Production write sites in Tasks 3-5 will do the same — noted in those tasks.)

- [ ] **Step 5: Build and run the meta tests**

```bash
ninja -C build unit_tests_dbms > build/cas_meta_incarnation_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.*' > build/test_cas_meta_incarnation.log 2>&1
```
Expected: all `CasBlobMeta.*` pass (round-trip now includes the incarnation).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.cpp \
        src/Disks/tests/cas_test_helpers.h src/Disks/tests/gtest_cas_blob_meta.cpp
git commit -m "feat(cas): BlobMeta carries a fresh incarnation nonce (S3 etag uniqueness)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 2: `ca-inspect` `.meta` dispatch + `ca-fsck` INV-META-BODY pairing

**Files:**
- Modify: `Core/CasInspect.cpp:399-428` (add `.meta` branch before the `blobs/` prefix branch)
- Modify: `Core/CasFsck.cpp` (partition `.meta` keys out of the body LIST at ~`:188`; add INV-META-BODY pairing pass)
- Modify: `Core/CasFsck.h` (add `meta_orphans` / `body_without_meta` counters to `FsckReport`)
- Test: `src/Disks/tests/gtest_cas_fsck.cpp` (extend), and inline in `gtest_cas_blob_meta.cpp` for inspect

**Interfaces:**
- Consumes: `decodeBlobMeta`, `Layout::blobMetaKey`, `Backend::head`/`list`, the `FsckReport` struct (`CasFsck.h:44-69`), `caInspectToJson` (`CasInspect.h:25`).
- Produces:
  - `ca-inspect`: a `.meta` key renders its decoded `BlobMeta` (JSON) instead of throwing.
  - `FsckReport` gains `uint64_t meta_without_body = 0;` and `uint64_t body_without_meta = 0;`; `clean()` unchanged (still `dangling == 0`); `body_without_meta` is a benign/expected transient (debris/mid-create) reported but not a dangle; `meta_without_body` is an INV-META-BODY violation (reported, and it implies a dangle risk — see step).

- [ ] **Step 1: Write the failing `ca-inspect` `.meta` test**

Append to `gtest_cas_blob_meta.cpp`:
```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.h>

TEST(CasBlobMeta, InspectRendersMetaNotEnvelope)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("hash-c");
    const String meta_key = store->layout().blobMetaKey(BlobId(u128ToHex(h)));
    const String bytes = encodeBlobMeta(BlobMeta{.state = MetaState::Condemned, .condemn_round = 9, .size = 3});

    const String json = caInspectToJson(store->layout(), meta_key, bytes);
    EXPECT_NE(json.find("condemned"), String::npos);   // rendered as meta, not mis-decoded as an envelope
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/cas_inspect_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.InspectRendersMetaNotEnvelope' > build/test_cas_inspect_meta.log 2>&1
```
Expected: FAIL — `caInspectToJson` throws `BAD_ARGUMENTS`/`CORRUPTED_DATA` (a `.meta` key matches `blobsPrefix()` and is mis-decoded as an envelope at `CasInspect.cpp:423`).

- [ ] **Step 3: Add the `.meta` dispatch branch**

In `CasInspect.cpp`, insert BEFORE the `blobs/` prefix check (`if (key.starts_with(layout.blobsPrefix()))`, line 422). Add a small renderer next to the other `render*` helpers and the branch:
```cpp
    if (key.starts_with(layout.blobsPrefix()) && key.ends_with(".meta"))
        return renderBlobMeta(decodeBlobMeta(bytes));
```
`renderBlobMeta` (add beside the other renderers, mirroring `renderRetiredSet`'s JSON style):
```cpp
namespace
{
String renderBlobMeta(const BlobMeta & m)
{
    Poco::JSON::Object obj;
    obj.set("object", "blob_meta");
    obj.set("version", m.version);
    obj.set("state", m.state == MetaState::Clean ? "clean"
                   : m.state == MetaState::Condemned ? "condemned" : "tombstone");
    obj.set("condemn_round", m.condemn_round);
    obj.set("size", m.size);
    std::ostringstream oss;
    obj.stringify(oss);
    return oss.str();
}
}
```
Add `#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobMeta.h>` to `CasInspect.cpp`. (Match the file's existing JSON idiom — if it does not use `Poco::JSON`, mirror whatever `renderRetiredSet` uses.)

**Also fix the raw-body branch (v2 finding #5):** the existing `blobs/`-non-`.meta` branch (`CasInspect.cpp:422-423`) decodes an `EnvelopeHeader`, which under raw bodies (Task 3 drops the envelope) throws `CORRUPTED_DATA` on a valid body. Change that branch to render a raw body — size + a re-hash confirming the key:
```cpp
    if (key.starts_with(layout.blobsPrefix()))   // (non-.meta reached here — the .meta branch is above)
        return renderRawBody(key, bytes);
```
with `renderRawBody` reporting `{object: "raw_blob", size, hash_matches_key: poolContentHash(bytes)==keyHash}` (reuse the hash-parse idiom from `CasGc.cpp:1838-1849`). Since the envelope decode is now unused for bodies (the only other caller, `copyForwardFromCondemned`, is re-shaped in Task 4), add a note to Task 3 to delete `CasEnvelope.{h,cpp}` (keeping only the `ObjectKind` enum if still referenced) once no callers remain.

- [ ] **Step 4: Run the inspect test to verify it passes**

```bash
ninja -C build unit_tests_dbms > build/cas_inspect_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobMeta.InspectRendersMetaNotEnvelope' > build/test_cas_inspect_meta.log 2>&1
```
Expected: PASS.

- [ ] **Step 5: Write the failing fsck INV-META-BODY test**

Append to `gtest_cas_fsck.cpp`:
```cpp
TEST(CasFsck, MetaWithoutBodyIsFlagged)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("orphan-meta");
    // Meta present, body ABSENT — an INV-META-BODY violation (must never arise by construction).
    putMetaIfAbsent(*backend, store->layout(), h, BlobMeta{.state = MetaState::Clean, .size = 5});

    const FsckReport rep = runFsck(*store, /*detail*/ true);
    EXPECT_GE(rep.meta_without_body, 1u);
}

TEST(CasFsck, BodyWithoutMetaIsBenignDebris)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const DB::UInt128 h = u128Of("debris-body");
    // Body present, meta ABSENT — a crashed pre-meta birth: benign debris, NOT a dangle.
    writeRawBlobBody(*backend, store->layout(), h, "payload");

    const FsckReport rep = runFsck(*store, /*detail*/ true);
    EXPECT_GE(rep.body_without_meta, 1u);
    EXPECT_EQ(rep.dangling, 0u);   // debris is not a loss
}
```
Add `#include ".../Core/CasBlobMeta.h"` to `gtest_cas_fsck.cpp` if not already present.

- [ ] **Step 6: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/cas_fsck_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasFsck.MetaWithoutBodyIsFlagged:CasFsck.BodyWithoutMetaIsBenignDebris' > build/test_cas_fsck_meta.log 2>&1
```
Expected: FAIL — `FsckReport` has no `meta_without_body`/`body_without_meta`; the fields don't compile / are always 0.

- [ ] **Step 7: Add the counters and the pairing pass**

In `CasFsck.h`, add to `FsckReport` (after `unaccounted`):
```cpp
    uint64_t meta_without_body = 0;   /// INV-META-BODY violation: a .meta with no body (must not arise)
    uint64_t body_without_meta = 0;   /// benign pre-meta-birth debris (swept by GC's claim-first pass)
    /// (v2 finding #12) fold the invariant violation into clean(): a clean/condemned meta with no body is a
    /// latent loss the 1-GET adopt would trust. Update `bool clean() const { return dangling == 0 && meta_without_body == 0; }`.
```
In `CasFsck.cpp`, the body LIST at `:188` (`listAll(backend, layout.blobsPrefix(), present_blobs, ...)`) now also returns `.meta` keys (same prefix). Partition them: build two sets from the single LIST — `present_bodies` (keys NOT ending in `.meta`) and `present_metas` (keys ending in `.meta`, mapped to their hash). Then after the existing present/reachable passes, add the pairing check:
```cpp
    // INV-META-BODY pairing (spec §raw-body-refinement): every meta must have a body; a body without a
    // meta is benign pre-birth debris. Only meaningful in the full (non-scoped) pass that lists blobs/.
    for (const auto & [hash, meta_key] : present_metas)
    {
        const String body_key = layout.blobKey(BlobId(u128ToHex(hash)));
        if (!present_bodies.contains(body_key) && !backend.head(body_key).exists)
            ++report.meta_without_body;
    }
    for (const String & body_key : present_bodies)
    {
        const UInt128 hash = /* parse hash from body_key (rfind('/')+1), as in rebuildBaseline */;
        if (!present_metas.contains(hash))
            ++report.body_without_meta;
    }
```
Reuse the hash-parse idiom from `CasGc.cpp:1838-1849` (`rfind('/')`, `hexToU128`, skip on parse failure). Ensure `.meta` keys are excluded from `present_blobs`/`reachable-must-be-present` so a body's own `.meta` sibling is not itself flagged dangling.

- [ ] **Step 8: Run the fsck tests to verify they pass**

```bash
ninja -C build unit_tests_dbms > build/cas_fsck_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasFsck.*' > build/test_cas_fsck_meta.log 2>&1
```
Expected: the two new tests pass; the pre-existing `CasFsck.*`/`CasFsckScoped.*`/`CasFsckPartial.*` still pass (the `.meta` partition must not disturb existing body classification — if any regress, the partition is leaking `.meta` keys into `present_blobs`).

- [ ] **Step 9: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasInspect.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFsck.cpp \
        src/Disks/tests/gtest_cas_blob_meta.cpp src/Disks/tests/gtest_cas_fsck.cpp
git commit -m "feat(cas): ca-inspect .meta dispatch + ca-fsck INV-META-BODY pairing

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 3: Writer — raw body + meta lifecycle in `putBlob`/`uploadFromSource`; read-path offset 0

**Files:**
- Modify: `Core/CasBuild.cpp` (`putBlob:130-209`, `uploadFromSource:311-504`, `observeAndAdmit:227-309`)
- Modify: `Core/CasBuild.h` (helper decls if needed)
- Modify: `Core/CasStore.cpp` (`locate:1039-1055` → raw offset 0)
- Modify: `Core/CasStore.h` / pool meta (`blob_header_len` semantics for new pools)
- Test: `src/Disks/tests/gtest_cas_build.cpp` (migrate + add), `gtest_cas_store.cpp` (read-path)

**Interfaces:**
- Consumes: the meta-ops layer (`loadMeta`, `putMetaIfAbsent`, `casMeta`, `LoadedMeta`, `BlobMeta`, `MetaState`) from Task 1; `Backend::casPut`/`get`/`head`; `BlobSource` (`CasBuild.h:16`, re-readable); `depIsTokened` (`CasBuild.cpp:217`); `chassert(precommitted)`.
- Produces (the writer state machine per spec §meta-protocols, matching `CaMetaDescriptorRaw.tla`):
  - `putBlob` writes a **raw** body (`casPut(bodyKey, payload, std::nullopt)` — no envelope) then a meta (`putMetaIfAbsent`); a body-412 or a dedup-cache hit takes the meta-GET path.
  - Dedup-adopt: `loadMeta`; `clean` ⇒ adopt (reference the body, no body HEAD); `condemned` ⇒ resurrect; `tombstone`/`absent`-with-body ⇒ birth-completion.
  - Resurrect: `casMeta(condemned-etag → clean)` — NO body re-upload (raw body already present, INV-1: dying body never read).
  - Birth-completion (body present ∧ meta ∈ {tombstone, absent}): re-upload body from the writer's OWN source (idempotent, content-addressed) then `putMetaIfAbsent`/`casMeta → clean`; NEVER adopt the orphan body.
  - The etag recorded into `deps[...]` becomes the **meta etag** (the linearization token), not a body token.
- Ordering guard: `chassert(precommitted)` stays on the adopt paths; a fresh raw upload before precommit stays legal.

- [ ] **Step 1: Write the failing writer tests (fresh upload + dedup adopt)**

Add to `gtest_cas_build.cpp` (new suite section). These assert the meta layout and the no-envelope body:
```cpp
TEST(CasBuild, PutBlobWritesRawBodyAndCleanMeta)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    auto build = /* begin a build + precommit as existing CasBuild tests do */;
    const String payload = "hello-raw";
    const DB::UInt128 h = store->poolContentHash(payload);   // content hash of the RAW payload

    build->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload));

    // Body is the raw payload verbatim (no envelope header).
    const auto body = backend->get(store->layout().blobKey(BlobId(u128ToHex(h))));
    ASSERT_TRUE(body.has_value());
    EXPECT_EQ(body->bytes, payload);
    // Meta present and clean.
    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);
    EXPECT_EQ(lm->meta.size, payload.size());
}

TEST(CasBuild, PutBlobDedupAdoptsViaSingleMetaGet)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const String payload = "dup";
    const DB::UInt128 h = store->poolContentHash(payload);
    // First writer establishes body+meta.
    { auto b1 = /* build+precommit */; b1->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload)); }
    backend->resetCounts();
    // Second writer dedup-adopts: ONE meta GET, NO body PUT.
    { auto b2 = /* build+precommit */; b2->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload)); }
    EXPECT_EQ(backend->putTotal(), 0u);                      // adopted, nothing written
    EXPECT_GE(backend->ioCountForKeysContaining(".meta"), 1u); // the meta GET happened
}
```
(Use the existing `gtest_cas_build.cpp` build/precommit boilerplate — copy the setup from `CasBuild.PutBlob*` tests already in the file.)

- [ ] **Step 2: Write the failing resurrect + birth-completion tests**

```cpp
TEST(CasBuild, PutBlobResurrectsCondemnedByMetaCasNoBodyReupload)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const String payload = "resurrect-me";
    const DB::UInt128 h = store->poolContentHash(payload);
    writeRawBlobBody(*backend, store->layout(), h, payload);
    writeMetaClean(*backend, store->layout(), h, payload.size());
    condemnMeta(*backend, store->layout(), h, /*round*/ 3);   // meta = condemned, body present
    backend->resetCounts();

    { auto b = /* build+precommit */; b->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload)); }

    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);              // resurrected
    EXPECT_EQ(backend->ioCountForKeysContaining(/*body key, not .meta*/), 0u); // NO body re-upload
}

TEST(CasBuild, PutBlobBirthCompletionFromAbsentMetaReestablishesFromSource)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const String payload = "complete-my-birth";
    const DB::UInt128 h = store->poolContentHash(payload);
    // Crashed pre-meta birth: body present, meta ABSENT -> re-establish from source (never adopt blind).
    writeRawBlobBody(*backend, store->layout(), h, payload);

    { auto b = /* build+precommit */; b->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload)); }

    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Clean);              // clean meta created
    EXPECT_TRUE(backend->get(store->layout().blobKey(BlobId(u128ToHex(h)))).has_value());
}

TEST(CasBuild, PutBlobWaitsThenAbortsOnTerminalTombstone)
{
    // v2 terminal tombstone: a writer must NEVER un-tombstone. With the meta stuck at tombstone (no GC to
    // clear it in this deterministic test), putBlob exhausts its bounded wait and fails closed (ABORTED),
    // never CASing tombstone->clean. (A live GC would clear it to absent and the retry would fresh-upload.)
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const String payload = "being-deleted";
    const DB::UInt128 h = store->poolContentHash(payload);
    writeRawBlobBody(*backend, store->layout(), h, payload);
    putMetaIfAbsent(*backend, store->layout(), h,
        BlobMeta{.incarnation = mintU128(), .state = MetaState::Tombstone, .size = payload.size()});

    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] {
        auto b = /* build+precommit */; b->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload));
    });
    // The meta was NOT resurrected to clean (terminal).
    const auto lm = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Tombstone);
}
```

- [ ] **Step 3: Run to verify the writer tests fail**

```bash
ninja -C build unit_tests_dbms > build/cas_build_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBuild.PutBlob*' > build/test_cas_build_meta.log 2>&1
```
Expected: FAIL — the body still carries an envelope, no meta is written, dedup does the HEAD+412 dance not a meta GET.

- [ ] **Step 4: Rewrite `uploadFromSource` to write a raw body + meta (bottom-up)**

Replace the envelope-building `uploadFromSource` (`CasBuild.cpp:311-504`) with the raw-body + meta protocol. Drop the `EnvelopeHeader`/`encodeEnvelopeHeader` machinery for the body; the body is `source`'s bytes verbatim. New shape (Allman braces):
```cpp
void Build::uploadFromSource(ObjectKind kind, const UInt128 & hash, const String & key, const BlobSource & source)
{
    chassert(kind == ObjectKind::Blob);
    /// Bottom-up create (INV-META-BODY): raw body first (If-None-Match), then meta (If-None-Match).
    /// The body is content-addressed & immutable — a 412 means it already exists (dedup or a racing
    /// writer wrote identical bytes). The source is re-readable; we never GET the dying object (INV-1).
    auto stream_body = [&]() -> PutResult
    {
        auto sink = store->backend().putIfAbsentStream(key);
        source.write_payload(sink->buffer());
        return sink->finalize();
    };
    PutResult body_put = stream_body();
    if (body_put.outcome == PutOutcome::PreconditionFailed)
    {
        /// Body already present. Consult the meta (the linearization) to decide adopt/resurrect/complete.
        observeAndAdmitByMeta(kind, hash, key, source);
        return;
    }
    /// Body freshly written; create the clean meta (fresh incarnation nonce — Task 1B).
    const CasResult meta_put = putMetaIfAbsent(store->backend(), store->layout(), hash,
        BlobMeta{.incarnation = mintU128(), .state = MetaState::Clean, .condemn_round = 0, .size = source.size});
    if (meta_put.outcome == CasOutcome::Conflict)
    {
        /// A racing writer created the meta first (identical body). Adopt its state.
        observeAndAdmitByMeta(kind, hash, key, source);
        return;
    }
    deps[{static_cast<uint8_t>(kind), hash}] = DepEntry{.kind = kind, .token = meta_put.token, .size = source.size};
    recordDoneAndEmit(kind, hash, key, source.size, "fresh raw upload + clean meta");
}
```
(Adapt `recordDoneAndEmit`/event fields to the existing helpers; keep the ProfileEvents. `observeAndAdmitByMeta` is defined in the next step.)

- [ ] **Step 5: Add `observeAndAdmitByMeta` — the dedup/adopt/resurrect/birth-completion state machine**

Add a private method (declare in `CasBuild.h` beside `observeAndAdmit`). **v2 TERMINAL-TOMBSTONE** — it implements the spec §meta-protocols v2 "Dedup-adopt / Resurrect / Wait-on-tombstone / Birth-completion" rows and matches `CaMetaDescriptorRaw.tla` v2's `Adopt`/`Resurrect`(condemned only)/`BirthCompletion`(absent only). A writer NEVER un-tombstones. Every meta write mints a fresh `incarnation` nonce (Task 1B). First add a private helper to keep DRY:
```cpp
/// Ensure the raw body is present from OUR re-readable source (idempotent; content-addressed).
/// A 412 (already present) is success — never a GET/read of a possibly-dying body (INV-1).
void Build::ensureRawBody(const String & key, const BlobSource & source)
{
    auto sink = store->backend().putIfAbsentStream(key);
    source.write_payload(sink->buffer());
    sink->finalize();   // PutOutcome::PreconditionFailed tolerated: body already present is fine
}
```
Then the state machine:
```cpp
uint64_t Build::observeAndAdmitByMeta(ObjectKind kind, const UInt128 & hash, const String & key, const BlobSource & source)
{
    /// EDGE-BEFORE-OBSERVE: adopting an existing incarnation requires a durable closure edge.
    chassert(precommitted);
    constexpr int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        const auto lm = loadMeta(store->backend(), store->layout(), hash);
        if (!lm)
        {
            /// Meta absent. Either nothing exists, or a crashed pre-meta birth (body present, no meta), or
            /// GC finished a delete. In ALL cases: re-establish the body from OUR source (idempotent) then
            /// create a clean meta If-None-Match. NEVER adopt an orphan body blind. (Birth-completion +
            /// fresh-upload collapse to the same act here.)
            ensureRawBody(key, source);
            const CasResult put = putMetaIfAbsent(store->backend(), store->layout(), hash,
                BlobMeta{.incarnation = mintU128(), .state = MetaState::Clean, .condemn_round = 0, .size = source.size});
            if (put.outcome == CasOutcome::Conflict)
                continue;   /// a racing writer created the meta -> re-load and follow
            deps[{static_cast<uint8_t>(kind), hash}] = DepEntry{.kind = kind, .token = put.token, .size = source.size};
            return source.size;
        }
        switch (lm->meta.state)
        {
            case MetaState::Clean:
            {
                /// Adopt: reference the body directly (INV-META-BODY => body present; no body HEAD).
                deps[{static_cast<uint8_t>(kind), hash}] = DepEntry{.kind = kind, .token = lm->etag, .size = lm->meta.size};
                store->dedupCacheAdd(hash);
                return lm->meta.size;
            }
            case MetaState::Condemned:
            {
                /// Resurrect (from condemned ONLY): CAS condemned->clean (fresh incarnation). Body present &
                /// immutable -> NO body re-upload (raw-body). This CAS races GC's condemned->tombstone on the
                /// same etag; the loser re-loads and follows (if GC won, we next observe tombstone -> wait).
                const CasResult cas = casMeta(store->backend(), store->layout(), hash, lm->etag,
                    BlobMeta{.incarnation = mintU128(), .state = MetaState::Clean, .condemn_round = 0, .size = lm->meta.size});
                if (cas.outcome == CasOutcome::Conflict)
                    continue;
                deps[{static_cast<uint8_t>(kind), hash}] = DepEntry{.kind = kind, .token = cas.token, .size = lm->meta.size};
                return lm->meta.size;
            }
            case MetaState::Tombstone:
            {
                /// TERMINAL: the content is being deleted. NEVER CAS tombstone->clean (v2: that dangles a
                /// committed ref — GC's committed body delete still hits the immutable body). WAIT (bounded
                /// re-GET; each iteration is a network round-trip, no sleep/busy-spin fix) for GC to reach
                /// `absent`, then re-drive the fresh-upload path above. Exhaustion -> ABORTED (build restarts;
                /// the INSERT-level retry lands after GC clears the tombstone).
                continue;
            }
        }
    }
    throw Exception(ErrorCodes::ABORTED,
        "putBlob: meta for {} still tombstone (GC deleting) after {} attempts — fail closed, build restarts", key, max_attempts);
}
```
Declare `void ensureRawBody(const String & key, const BlobSource & source);` and `uint64_t observeAndAdmitByMeta(ObjectKind, const UInt128 &, const String &, const BlobSource &);` in `CasBuild.h`. `mintU128` is the same fresh-nonce minter used today at `CasBuild.cpp`'s envelope builder (`header.incarnation_tag = mintU128()`).

- [ ] **Step 6: Simplify `putBlob` to drive the raw+meta path**

Replace `putBlob` (`CasBuild.cpp:130-209`). The dedup-cache/large-body HEAD-first fast path becomes a **meta** GET fast-path (cheaper than the HEAD+412 dance):
```cpp
BlobRef Build::putBlob(const BlobId & id, BlobSource source)
{
    requireAlive();
    const UInt128 logical_hash = hexToU128(id.string());
    const String key = store->layout().blobKey(id);
    const PoolConfig & cfg = store->poolConfig();

    /// Fast path: a likely dedup hit OR a large body (where a wasted body-PUT that 412s is expensive —
    /// B168 P2 / B187 broken-pipe storm) -> one meta GET (adopt/resurrect/wait/complete without streaming
    /// the body). KEEP the large-body guard (v2 finding #6): keying only on the dedup cache would stream a
    /// large not-yet-cached dedup hit just to 412.
    if (store->dedupCacheContains(logical_hash)
        || (cfg.dedup_head_first_min_bytes > 0 && source.size >= cfg.dedup_head_first_min_bytes))
    {
        ProfileEvents::increment(ProfileEvents::CasBlobHeadFirst);
        const uint64_t admitted = observeAndAdmitByMeta(ObjectKind::Blob, logical_hash, key, source);
        store->dedupCacheAdd(logical_hash);
        return BlobRef{id, admitted};
    }

    constexpr int max_attempts = 8;
    for (int attempt = 0; attempt < max_attempts; ++attempt)
    {
        try
        {
            uploadFromSource(ObjectKind::Blob, logical_hash, key, source);
            store->dedupCacheAdd(logical_hash);
            return BlobRef{id, source.size};
        }
        catch (const Exception & e)
        {
            if (e.code() != ErrorCodes::ABORTED || attempt + 1 == max_attempts)
                throw;
        }
    }
    throw Exception(ErrorCodes::LOGICAL_ERROR, "putBlob: exhausted retries for {}", key);
}
```
Update `observeAndAdmit` (the two old HEAD-based overloads, `CasBuild.cpp:227-309`): they are superseded by `observeAndAdmitByMeta`. Remove them and their callers, OR repoint the HEAD-first admit to the meta path. Keep exactly one code path (no dead HEAD-based admit). Preserve both `chassert(precommitted)` guards by placing the single guard at the top of `observeAndAdmitByMeta`.

- [ ] **Step 7: Change the read path to raw offset 0**

In `CasStore.cpp` `locate` (`:1039-1055`), the `EntryPlacement::Blob` case returns `.offset = meta.blob_header_len`. For raw bodies the payload starts at 0:
```cpp
        case EntryPlacement::Blob:
            return BlobLocation{
                .key = pool_layout.blobKey(BlobId(u128ToHex(entry.blob_hash))),
                .offset = 0,   /// raw immutable body: payload starts at byte 0 (no envelope; spec §raw-body-refinement)
                .length = entry.blob_size,
            };
```
Set the new-pool `blob_header_len` to 0 where the pool meta is initialized (find via `grep -n "blob_header_len" Core/*.cpp`), and drop `blob_header_len` from `retiredLogicalSize` accounting (raw size == object size) — a body's on-disk size now equals its logical size. Leave the pool-meta field present (value 0) to avoid a codec change; comment it as "legacy, 0 for raw-body pools".

- [ ] **Step 8: Migrate the affected `gtest_cas_build.cpp` tests**

The tests that assert an envelope (`PutBlobWritesEnvelopeWithFixedHeader`, copy-forward-re-wrap assertions, resurrect-re-uploads-body) must move to the new contract. Per spec §implementation-phases Phase-B: envelope-shape assertions become raw-body assertions; resurrect asserts a meta CAS with NO body re-upload; any test that hand-condemns via the retire view is re-sourced to `condemnMeta`. Delete/rewrite each in place, keeping the test's intent. Also update `writeBlobBody` callers that assumed a header offset.

- [ ] **Step 9: Build and run the writer + read-path tests**

```bash
ninja -C build unit_tests_dbms > build/cas_build_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBuild*:CasStore.*Read*:CasStore.Resolve*' > build/test_cas_build_meta.log 2>&1
```
Expected: the new `CasBuild.PutBlob*` pass; migrated tests pass; the read-path resolve tests pass with offset 0. (Cross-suite tests touching GC/retire-view will still be red until Tasks 4-6 — that is expected; scope this run to `CasBuild*` + read-path.)

- [ ] **Step 10: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_build.cpp src/Disks/tests/gtest_cas_store.cpp
git commit -m "feat(cas): writer writes raw bodies + meta lifecycle; read path at offset 0

putBlob/uploadFromSource: raw body PUT If-None-Match then meta PUT; dedup = 1 meta GET;
resurrect = meta CAS (no body re-upload); birth-completion from source on tombstone/absent-meta.
Drops the blob envelope from the write path; read path resolves at offset 0.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 4: Promote — K3 presence + copy-forward re-sourced to the meta

**Files:**
- Modify: `Core/CasBuild.cpp` (`promote:785-954`, esp. the K3 loop `:878-909`; `copyForwardFromCondemned:506-609`; `isCopyForwardableTokenless:211-215`)
- Test: `src/Disks/tests/gtest_cas_build.cpp`, `gtest_cas_protocol_scenarios.cpp`

**Interfaces:**
- Consumes: `loadMeta`/`casMeta`/`MetaState` (Task 1); `depIsTokened`, `isCopyForwardableTokenless` (existing); `BlobSource` is NOT available at promote (deps are tokened/tokenless leaves), so copy-forward's INV-1 body read stays for the tokenless committed-source case.
- Produces (spec §kept K3 + §meta-protocols copy-forward row):
  - Promote's per-leaf check for a **non-tokened** leaf becomes: `loadMeta(hash)`. `absent` ⇒ `ABORTED` (the hash is not referenceable). `clean` ⇒ OK. `condemned` + copy-forwardable ⇒ copy-forward (meta CAS condemned→clean; body already present & immutable). `condemned` + no-dep ⇒ `ABORTED`. `tombstone` + copy-forwardable ⇒ re-establish from the still-present condemned body (the documented INV-1 exception) then CAS→clean; if body gone ⇒ `ABORTED`.
  - `copyForwardFromCondemned` re-shaped: no envelope re-wrap. Under raw bodies the body is immutable, so copy-forward is a **meta CAS** (condemned→clean); the body GET is needed only to re-establish under a tombstone (body maybe mid-delete). Returns the new meta etag.

- [ ] **Step 1: Write the failing promote-K3 tests**

Add to `gtest_cas_build.cpp` (or `gtest_cas_protocol_scenarios.cpp` for the end-to-end shape). Tokened leaves must be skipped (EDGE-BEFORE-OBSERVE); a non-tokened absent-meta leaf must abort; a condemned copy-forwardable leaf must be resurrected via meta CAS:
```cpp
TEST(CasBuild, PromoteSkipsTokenedLeavesNoMetaGet)
{
    // A build whose leaf is tokened (putBlob'd under the durable edge). Promote must NOT re-check it.
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    /* build: stageManifest, precommit, putBlob (tokened leaf), then: */
    backend->resetCounts();
    /* build->promote(...); */
    EXPECT_EQ(backend->ioCountForKeysContaining(".meta"), 0u); // tokened leaf => zero meta GETs (INSERT case)
}

TEST(CasBuild, PromoteAbortsOnAbsentMetaForNonTokenedLeaf)
{
    // A no-dep / tokenless leaf whose meta is absent => not referenceable => ABORTED (fail-closed).
    /* set up a manifest naming a hash with NO meta and NO dep entry; expect promote to throw ABORTED */
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] { /* build->promote(...) */ });
}

TEST(CasBuild, PromoteCopyForwardsCondemnedTokenlessViaMetaCas)
{
    // A tokenless copy-forwardable leaf whose meta is condemned + body present => meta CAS -> clean, commit ok.
    /* condemnMeta on the leaf; expect promote to succeed and the meta to be clean afterward */
}
```
(Fill the build boilerplate from existing `CasBuild`/`CasProtocol` tests; keep them behavioral.)

- [ ] **Step 2: Run to verify they fail**

```bash
ninja -C build unit_tests_dbms > build/cas_promote_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBuild.Promote*:CasProtocol.*' > build/test_cas_promote_meta.log 2>&1
```
Expected: FAIL — promote still calls `retireView().isCondemnedToken` and HEADs the body.

- [ ] **Step 3: Re-source the K3 loop to the meta**

Replace the K3 loop body (`CasBuild.cpp:878-909`). Tokened leaves still `continue` at `depIsTokened` (`:882-883`). For non-tokened leaves, replace the `head` + `retireView().isCondemnedToken` logic with a meta load:
```cpp
        for (const ManifestEntry & e : body.entries)
        {
            if (e.placement != EntryPlacement::Blob)
                continue;
            if (depIsTokened(e.blob_hash))
                continue;   /// edge-protected (EDGE-BEFORE-OBSERVE); putBlob validated under the durable edge
            constexpr int max_reval_attempts = 8;
            bool validated = false;
            for (int attempt = 0; attempt < max_reval_attempts; ++attempt)
            {
                const auto lm = loadMeta(store->backend(), store->layout(), e.blob_hash);
                if (!lm)
                    throw Exception(ErrorCodes::ABORTED,
                        "promote: blob {} meta absent at commit revalidation — not referenceable, failing closed",
                        u128ToHex(e.blob_hash));
                if (lm->meta.state == MetaState::Clean)
                {
                    validated = true;
                    break;
                }
                if (!isCopyForwardableTokenless(e.blob_hash))
                    throw Exception(ErrorCodes::ABORTED,
                        "promote: blob {} condemned/tombstone at commit revalidation, no source — failing closed (INV-1)",
                        u128ToHex(e.blob_hash));
                copyForwardFromCondemned(e.blob_hash, *lm);   // meta CAS -> clean (+ INV-1 body re-establish on tombstone)
                /// loop: re-load and confirm clean (idempotent under CAS retry)
            }
            if (!validated)
                throw Exception(ErrorCodes::ABORTED,
                    "promote: blob {} not clean after {} copy-forward attempts — failing closed (INV-1)",
                    u128ToHex(e.blob_hash), max_reval_attempts);
        }
```

- [ ] **Step 4: Re-shape `copyForwardFromCondemned` to a meta CAS**

Change its signature to `Token copyForwardFromCondemned(const UInt128 & hash, const LoadedMeta & lm);` (declare in `CasBuild.h`). **v2 TERMINAL-TOMBSTONE:** under raw immutable bodies, copy-forward of a `condemned` leaf is a pure meta CAS (body present & immutable — no body touch). A `tombstone` (or `absent`) meta means the content is genuinely dying (or gone) → fail closed; the build restarts (the tokenless leaf has no source to recreate it, and un-tombstoning is forbidden). This is the raw-body replacement for the old envelope re-wrap:
```cpp
Token Build::copyForwardFromCondemned(const UInt128 & hash, const LoadedMeta & lm)
{
    /// Only a `condemned` leaf is copy-forwardable: CAS the meta condemned->clean (fresh incarnation) on the
    /// observed etag. The body is present (condemn never deletes it) and immutable -> no body GET/PUT.
    if (lm.meta.state != MetaState::Condemned)
        throw Exception(ErrorCodes::ABORTED,
            "promote: copy-forward leaf {} is {} (not condemned) — content dying/gone, failing closed",
            u128ToHex(hash), lm.meta.state == MetaState::Tombstone ? "tombstone" : "clean/absent");
    const CasResult cas = casMeta(store->backend(), store->layout(), hash, lm.etag,
        BlobMeta{.incarnation = mintU128(), .state = MetaState::Clean, .condemn_round = 0, .size = lm.meta.size});
    /// PreconditionFailed => a racing resurrect/GC moved it; the K3 loop re-loads and re-decides.
    EventEmitter{*store}.emit(/* CasEventType::BlobCopyForward, reason "meta CAS condemned->clean" */);
    ProfileEvents::increment(ProfileEvents::CasBlobCopyForward);
    return cas.token;
}
```
Remove the old envelope-decode/re-wrap/`putOverwrite` body of `copyForwardFromCondemned` (`:535-576`). Note the K3 loop (step 3) already ABORTs on `absent` meta before calling this; a `tombstone` observed inside the loop falls here and ABORTs — both fail closed, correct for terminal tombstone.

- [ ] **Step 5: Build and run the promote tests**

```bash
ninja -C build unit_tests_dbms > build/cas_promote_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBuild.Promote*:CasProtocol.*' > build/test_cas_promote_meta.log 2>&1
```
Expected: the new promote tests pass; migrated `CasProtocol.*` scenarios pass (fence-conflict, evidence-hit copy-forward, displaced-token commit re-expressed over meta). GC-dependent scenarios stay red until Task 5.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/tests/gtest_cas_build.cpp src/Disks/tests/gtest_cas_protocol_scenarios.cpp
git commit -m "feat(cas): promote K3 + copy-forward re-sourced to the blob meta

Non-tokened leaf revalidation loads the meta (absent=>ABORT, clean=>ok, condemned/tombstone+
copy-forwardable => meta CAS to clean, INV-1 body re-establish only under a tombstone). Tokened
leaves stay skipped (EDGE-BEFORE-OBSERVE).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 5: GC — condemn/spare/delete as meta ops on a parallel pool; `peek_meta` supersede; graduation re-key; rebuild

**Files:**
- Modify: `Core/CasGc.cpp` (condemn `head_blob:629-671`; delete site `:269-303`; `peek_head:679-686`; `graduationDue:1523`; `rebuildBaseline:1553`, blob LIST `:1824-1865`)
- Modify: `Core/CasGcFormats.h` (`RetiredEntry.token` semantics → meta etag; comment)
- Modify: `Core/CasBlobInDegree.{h,cpp}` (`peek_head`→`peek_meta`; `settleEntry` graduation gate `:196-217`)
- Test: `src/Disks/tests/gtest_cas_gc_ack_floor.cpp`, `gtest_cas_gc_leak.cpp`, `gtest_cas_blob_indegree.cpp`, `gtest_cas_b140_dangle.cpp`

**Interfaces:**
- Consumes: `loadMeta`/`casMeta`/`deleteMetaExact`/`MetaState`/`BlobMeta` (Task 1); `Backend::head`/`deleteExact` (body); a bounded `ThreadPool` (net-new — none exists).
- Produces (spec §phase-b-deletions + §meta-protocols GC rows, matching `CaMetaDescriptorRaw.tla` `GcCondemn`/`GcSpare`/`GcDeletePhaseA/B/C`):
  - **Condemn** (fold `d=0`): `loadMeta`; if `clean` ⇒ `casMeta(clean-etag → condemned, condemn_round)`; store `RetiredEntry{hash, token = condemned-meta-etag, condemn_round, size}`. If meta absent (debris body) ⇒ handled by the claim-first debris sweep, not condemned here.
  - **Spare** (`d>0` recovered): `casMeta(condemned-etag → clean)`.
  - **Delete** (graduated, tombstone handshake): `casMeta(condemned-etag → tombstone)`; on win ⇒ HEAD body + `deleteExact(body, body-etag)` ⇒ `deleteMetaExact(meta, tombstone-etag)`. Idempotent redelete (I3): a meta already-absent still proceeds to the body delete. On a CAS/etag mismatch (a racing resurrect won) ⇒ abort the body delete, drop the entry.
  - **`peek_meta`** (I1 supersede): re-read the meta for net-zero-touched entries; on a generation (etag) mismatch, supersede the stale `RetiredEntry` and re-condemn the current meta generation.
  - **Parallel pool:** condemn/spare/delete meta+body ops submitted to a bounded `ThreadPool` (mass-DROP requirement).
  - **`graduationDue`** re-keyed to `condemn_round < current_round` (M5) — no `min_ack`.
  - **`rebuildBaseline`**: the `blobs/` LIST skips `.meta` keys; for each body it captures/repairs the meta (INV-META-BODY), and emits meta condemns for zero-in-degree bodies.

- [ ] **Step 1: Write the failing GC condemn→delete meta tests**

Add to `gtest_cas_gc_ack_floor.cpp` (the condemn→graduate→delete pipeline). Assert the tombstone handshake and that the body survives condemn but not the final delete:
```cpp
TEST(CasGcRetire, CondemnMarksMetaKeepsBody)
{
    /* build a blob with in-degree 0; run one GC round */
    // meta condemned, body still present (resurrectable)
    const auto lm = loadMetaForTest(*backend, store->layout(), h);
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(lm->meta.state, MetaState::Condemned);
    EXPECT_TRUE(backend->get(store->layout().blobKey(BlobId(u128ToHex(h)))).has_value());
}

TEST(CasGcRetire, GraduatedDeleteTombstoneHandshakeRemovesBothObjects)
{
    /* condemn in round R, then run rounds until delete (condemn_round < current_round) */
    EXPECT_TRUE(runRoundsUntilAbsent(store, gc, *backend, store->layout(), h));
    EXPECT_FALSE(loadMetaForTest(*backend, store->layout(), h).has_value());        // tombstone meta gone
    EXPECT_FALSE(backend->get(store->layout().blobKey(BlobId(u128ToHex(h)))).has_value()); // body gone
}

TEST(CasGcRetire, ResurrectWinsAgainstDeleteRace)
{
    /* condemn; a writer resurrects (meta CAS condemned->clean) before graduation; delete must NOT fire */
    // after the race, meta clean, body present
}
```

- [ ] **Step 2: Run to verify they fail**

```bash
ninja -C build unit_tests_dbms > build/cas_gc_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGcRetire.*:CasGcAckFloor.*' > build/test_cas_gc_meta.log 2>&1
```
Expected: FAIL — condemn still HEADs the body and deletes via body token; no meta transitions.

- [ ] **Step 3: Re-source condemn to a meta CAS**

Replace the `head_blob` lambda's body-token capture (`CasGc.cpp:640/658`) with a meta condemn. The condemn creates the `RetiredEntry` with the **condemned-meta-etag** as its token. Where `closeBlob` builds `RetiredEntry` (`CasBlobInDegree.cpp:271-283`), the `head_blob` callback now returns the condemned meta etag (adapt the callback's return type or capture). Concretely, condemn = `loadMeta`; then:
- meta `clean` ⇒ `casMeta(lm->etag → {incarnation = mintU128(), Condemned, condemn_round, size = lm->meta.size})`. On `Committed` ⇒ store `RetiredEntry{hash, token = cas.token (condemned-etag), condemn_round, size = lm->meta.size}`. On `Conflict` (a writer resurrected or GC raced) ⇒ return `nullopt` (not condemned this pass; retried next fold when `d` is re-checked).
- meta already `condemned` ⇒ idempotent: reuse the observed etag as the ledger token (no CAS), store the `RetiredEntry` (a prior pass condemned it; this pass just re-records — keeps the ledger token fresh for the `peek_meta` supersede).
- meta `tombstone` ⇒ a delete is already in flight; return `nullopt` (the graduated entry from the prior condemn owns the delete).
- meta `absent` (debris body) ⇒ return `nullopt` (the claim-first debris sweep owns it, not condemn).

Keep the three condemn-trail events (`IndegZero`, `GcRetireObserve`, `BlobRetire`) but change `e.token` to the condemned-meta etag and `e.reason` to reflect the meta CAS. `size` comes from `lm->meta.size` (no body HEAD needed — `retiredLogicalSize`/`blob_header_len` accounting is dropped, since a raw body's size == its object size).

- [ ] **Step 4: Re-shape the delete site to the tombstone handshake on a parallel pool**

Replace the R3 redelete loop (`CasGc.cpp:269-303`). Introduce a bounded `ThreadPool` member on `Gc` (or construct one per round sized to a config, e.g. `gc_delete_pool_size`, default 16). For each `redelete` entry, submit a task:
```cpp
// Per graduated entry (submitted to the delete pool; results collected before advancing the round):
// v2 TERMINAL-TOMBSTONE handshake. Phase A: CAS condemned->tombstone on the ledger's condemned etag.
const CasResult claim = casMeta(backend, layout, entry.hash, entry.token /*condemned etag*/,
    BlobMeta{.incarnation = mintU128(), .state = MetaState::Tombstone,
             .condemn_round = entry.condemn_round, .size = entry.size});
if (claim.outcome == CasOutcome::Conflict)
{
    // A resurrect (condemned->clean) or a superseding re-condemn won on this etag: do NOT delete the body.
    // Drop the entry (spared/superseded). records OutcomeKind::Replaced.
    return;
}
// Won the tombstone -> TERMINAL. No writer can un-tombstone (v2), so no writer re-established the body
// after this win; a fresh HEAD therefore cannot observe a live resurrected body (the v2 correctness
// point). Phase B: delete the body top-down. Idempotent redelete (I3): an already-absent body is fine.
const HeadResult hr = backend.head(blobKeyOf(layout, entry.hash));
if (hr.exists)
    backend.deleteExact(blobKeyOf(layout, entry.hash), hr.token);
// Phase C: delete the tombstone meta -> absent.
deleteMetaExact(backend, layout, entry.hash, claim.token /*tombstone etag*/);
```
Wrap submission so a per-task exception is caught, logged, and the entry recorded as an anomaly (never throw out of the pool — `feedback_ca_gc_never_throw_on_404`). Collect all futures before the round advances. Keep `created_delete_marker` fail-closed (throw LOGICAL_ERROR) since versioning-on is a store misconfig.

- [ ] **Step 5: Re-key graduation to `condemn_round < current_round`**

In `settleEntry` (`CasBlobInDegree.cpp:196-217`), replace the `e.condemn_round < min_ack` graduation gate (`:208`) with `e.condemn_round < current_round`. **Off-by-one (v2 finding #13):** `current_round` MUST be `new_round = state.round + 1` — the SAME basis as `condemn_round` (condemn stamps `condemn_round = state.round + 1`, `CasGc.cpp:609`). Passing `state.round` would make `condemn_round < current_round` never true → nothing ever graduates → leak. Pass `new_round` where `min_ack` is passed today. Remove `min_ack` from `foldDeltasIntoGeneration`'s signature (`CasBlobInDegree.h:122`) and all call sites; thread `current_round` (= `new_round`) instead. Update `graduationDue` (`CasGc.cpp:1523`) correspondingly (defers never delete — M5, verified safe).

- [ ] **Step 6: Rename `peek_head`→`peek_meta` and re-shape the supersede**

In `CasBlobInDegree.cpp:245-265`, the supersede peeks the object token for net-zero-touched entries. Change it to peek the **meta**: `loadMeta(hash)`; if present and `lm->etag != prior_retired[ri].token` (etag = the stored condemned-meta-etag) ⇒ supersede (the entry was resurrected + re-condemned to a new generation). Rename the parameter and the `CasGc.cpp:679-686` lambda instance to `peek_meta`, returning the current meta's etag+size. `ReplacedEntry.old_token`/`fresh.token` now hold meta etags.

- [ ] **Step 7: Update `rebuildBaseline` for meta capture**

In the `blobs/` LIST (`CasGc.cpp:1824-1865`): skip keys ending in `.meta` (`if (k.key.ends_with(".meta")) continue;`). For each body with zero in-degree, `loadMeta`; if the meta is absent, repair INV-META-BODY by creating a condemned meta (so GC can then delete both top-down) — or, if policy is to treat a meta-less body as debris, leave it for the debris sweep; follow spec I2 (capture meta state + INV-META-BODY repair). Store the condemned-meta-etag in the rebuilt `RetiredEntry.token` (not the body token). Emit `GcRebuild` events reflecting the meta capture.

- [ ] **Step 8: Migrate GC gtests to the meta contract**

`gtest_cas_gc_leak.cpp` (RESURRECT-REUPLOAD-ORPHAN — I1 supersede), `gtest_cas_blob_indegree.cpp` (three-cursor merge + `peek_meta`), `gtest_cas_b140_dangle.cpp` (shared-blob survival), `gtest_cas_gc_ack_floor.cpp` (rename/re-key). Rewrite each to manufacture meta state via `writeMetaClean`/`condemnMeta` and assert meta transitions instead of retire-list/body-token behavior. Delete assertions on `min_ack`.

- [ ] **Step 9: Build and run the GC tests**

```bash
ninja -C build unit_tests_dbms > build/cas_gc_meta_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGc*:CasThreeCursorMerge.*:CasBlobInDegree.*:CasReuseGcRace.*' > build/test_cas_gc_meta.log 2>&1
```
Expected: the GC meta pipeline tests pass; three-cursor merge + supersede pass with `peek_meta`.

- [ ] **Step 10: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGcFormats.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_gc_ack_floor.cpp src/Disks/tests/gtest_cas_gc_leak.cpp \
        src/Disks/tests/gtest_cas_blob_indegree.cpp src/Disks/tests/gtest_cas_b140_dangle.cpp
git commit -m "feat(cas): GC condemn/spare/delete via blob meta; tombstone handshake on a parallel pool

Condemn = meta CAS clean->condemned (ledger stores the condemned-meta-etag); delete = tombstone
handshake (meta->body->meta) on a bounded pool (mass-DROP); peek_meta supersede; graduation
re-keyed to condemn_round<current_round (no min_ack); rebuild captures meta state.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 6: Delete the writer-side `RetireView` + syncer + `observed_gc_round` + ack-floor

**Files:**
- Delete: `Core/CasRetireView.h`, `Core/CasRetireView.cpp`, `src/Disks/tests/gtest_cas_retire_view.cpp`
- Modify: `Core/CasStore.h` (`observedGcRound:290`, `retireView:380`, `syncRetiredView:410`, `start/stopRetiredViewSync:419-420`, `retire_view:622`, syncer thread state `:657-661`), `Core/CasStore.cpp` (all sites listed below)
- Modify: `Core/CasServerRoot.{h,cpp}` (`observed_gc_round` in `MountLease:87-103`, `prepareRenew:629`, `encodeBody:639`, `computeHeartbeatFloor` `min_ack`/`max_ack` `:479-562`)
- Modify: `Core/CasGc.cpp` (round-recovery numbering `rebuildBaseline:1867-1872` — no longer folds `floor.max_ack` from acks)
- Test: `gtest_cas_heartbeat.cpp`, `gtest_cas_store.cpp` (beat), `gtest_cas_mount.cpp` (`CasHeartbeatFloor`)

**Interfaces:**
- Consumes: nothing new — this task is pure removal of the last view consumers (spec §phase-b-deletions).
- Produces: `Store` no longer has `retireView()`/`observedGcRound()`/`syncRetiredView()`/`RetireView retire_view`/the syncer thread; `MountLease` loses `observed_gc_round`; the beat publishes only `{now, min_active}`; `computeHeartbeatFloor` no longer computes `min_ack`/`max_ack` from acks; GC round numbering advances on its own monotone round (`state.round + 1`, plus the fence/gen maxima that remain).
- Precondition: Tasks 3-5 removed every `retireView()` call — verify with grep before deleting (`CasBuild.cpp:270/283/302/389/449/528/586/894` are all gone).

- [ ] **Step 1: Confirm no remaining `retireView`/`observedGcRound` consumers**

```bash
grep -rn "retireView\|retire_view\|observedGcRound\|observed_gc_round\|syncRetiredView\|RetireView\|min_ack" \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v "CasRetireView\.\|gtest_cas_retire_view"
```
Expected: only comment references remain (spec/design). Any live call site means a prior task missed a re-source — fix it there first. This is the failing "test" for this task (a red grep = not done).

- [ ] **Step 2: Delete the `RetireView` files and its gtest**

```bash
git rm src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.h \
       src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRetireView.cpp \
       src/Disks/tests/gtest_cas_retire_view.cpp
```

- [ ] **Step 3: Remove the syncer + view members from `CasStore`**

In `CasStore.h`, delete: `observedGcRound()` (`:290`), `retireView()` (`:380`), `syncRetiredView()` (`:410`), `startRetiredViewSync`/`stopRetiredViewSync` (`:419-420`), `RetireView retire_view` (`:622`), `retiredViewSyncLoop` + `retired_view_sync_mutex`/`_cv`/`_stop`/`_thread` (`:657-661`), and the `#include ".../CasRetireView.h"`.
In `CasStore.cpp`, delete: `observedGcRound` (`:459-462`), `startRetiredViewSync` (`:605-612`), `stopRetiredViewSync` (`:614-626`), `retiredViewSyncLoop` (`:628-647`), `syncRetiredView` (`:649-714`); the `retire_view.refresh()` prime (`:144-145`), the syncer start (`:326-327`), `stopRetiredViewSync()` in the dtor (`:348`), the `syncRetiredView()` calls in `tryRemountOnce` (`:540`) and `renewWatermarkOnce` (`:718`), and the `retire_view.round()` event fields (`:553/567`). In `renewWatermarkOnce`, drop the `syncRetiredView()` line; it becomes just `mount_keeper->renewOnce();`.

- [ ] **Step 4: Drop `observed_gc_round` from the beat + `MountLease`**

In `CasServerRoot.h`, remove `observed_gc_round` from `MountLease` (`:87-103`) and the `observed_round_fn` ctor param + member of `MountLeaseKeeper` (`:310-314`, `:336-337`). In `CasServerRoot.cpp`, `prepareRenew` (`:629-637`) returns `{.value = now_ms_fn(), .value2 = min_active_fn()}` (drop `value3`); `encodeBody` (`:639-654`) drops the `.observed_gc_round` field. Update `decodeMountLease`/`encodeMountLease` codec to the new field set (CAS is pre-release; no compat). In `CasStore.cpp`, remove the `observed_round_fn` wiring (`:277`, `:527`).

- [ ] **Step 5: Remove `min_ack`/`max_ack` from `computeHeartbeatFloor` and re-source GC round numbering**

In `CasServerRoot.{h,cpp}`, drop `min_ack`/`max_ack` from `HeartbeatFloor` (`:246-264`) and the `m.observed_gc_round` reads (`:516/533/535`). The floor now only classifies live/terminated/fenced mounts and computes `min_active` (the precommit-reclaim floor, which stays — K4). In `CasGc.cpp` round recovery (`:1867-1872`), replace `std::max({floor.max_ack, max_fence_round, state.round, max_gen}) + 1` with `std::max({max_fence_round, state.round, max_gen}) + 1` (the ack term is gone; the GC round advances on its own state + fence/gen maxima). Confirm the regular-round path already increments `state.round` each pass (`new_round = state.round + 1` at `CasGc.cpp:132`, committed `:428`) so `condemn_round < current_round` graduation makes progress.

**Three additional residual consumers (v2 finding #4) — remove/re-source these too, or the build breaks:**
- `RoundReport::min_ack` — the field decl (`CasGc.h:80`), its assignment `report.min_ack = floor.min_ack;` (`CasGc.cpp:143`), and its log line (`CasGc.cpp:191`). Delete the field and both uses (it was a `floor.min_ack` introspection echo).
- `Gc::graduationDueForTest(state, min_ack)` — the test-only overload (`CasGc.h:389-391`): drop its `min_ack` parameter (it now derives `current_round` internally, matching `graduationDue`).
- `CasInspect.cpp:224` — the mount-lease renderer's `.add("observed_gc_round", jsonUInt(m.observed_gc_round))`: remove it (the `MountLease` field is gone).

- [ ] **Step 6: Migrate the heartbeat/beat/floor tests**

`gtest_cas_heartbeat.cpp` + `gtest_cas_store.cpp::CasStoreBeat`/`CasLeaseViewDecouple` + `gtest_cas_mount.cpp::CasHeartbeatFloor`: remove `observed_gc_round`/`min_ack` assertions; keep `min_active` + expiry + fence-out coverage. `CasLeaseViewDecouple` (which tested the lease-vs-view decoupling) is likely obsolete — delete it if its entire premise was the view sync; keep any part that still asserts lease behavior.

- [ ] **Step 7: Build and run the store/heartbeat/mount suite**

```bash
ninja -C build unit_tests_dbms > build/cas_delete_view_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasStore*:CasHeartbeat.*:CasMount*:CasHeartbeatFloor.*' > build/test_cas_delete_view.log 2>&1
```
Expected: PASS. The `RetireView`/`observed_gc_round` symbols are gone and nothing references them.

- [ ] **Step 8: Commit**

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ src/Disks/tests/
git commit -m "refactor(cas): delete writer-side RetireView + syncer + observed_gc_round + ack-floor

The meta point-read replaced list-delivery; graduation paces on GC rounds. Removes the
retired-view download term entirely (writers never read gc/state). Beat keeps lease + min_active.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Task 7: Integration validation — full gtest suite, CA-s3 lane, soak + mass-DROP

**Files:**
- Test-only: full `Ca*/Cas*` gtest suite; CA-s3 stateless lane; `utils/ca-soak`.
- Create: `utils/ca-soak/scenarios/cards/mass_drop_meta_throughput.py` (mass-DROP scenario)

**Interfaces:**
- Consumes: the complete Task 1-6 implementation.
- Produces: a green full CAS gtest run; a green CA-s3 lane (0 promote aborts, 0 fsck dangles); a clean soak run with the fsck gate; a mass-DROP throughput measurement sizing the parallel pool.

- [ ] **Step 0: Deterministic GC-delete-vs-writer-resurrect race test (v2 finding #7)**

The per-task tests manufacture meta state; none exercises a real writer resurrect racing a real GC delete — the exact interleaving of the consult's CRITICAL finding. Add a deterministic test in `gtest_cas_gc_leak.cpp` (or a new `gtest_cas_meta_race.cpp`) that drives the race by hand on the `InMemoryBackend` (single-threaded, explicit step ordering — no sleeps, no real threads):
```cpp
TEST(CasReuseGcRace, TerminalTombstoneNoDangleWhenWriterRacesDelete)
{
    auto backend = std::make_shared<InMemoryBackend>();
    auto store = openStoreForTest(backend);
    const String payload = "raced";
    const DB::UInt128 h = store->poolContentHash(payload);
    writeRawBlobBody(*backend, store->layout(), h, payload);
    writeMetaClean(*backend, store->layout(), h, payload.size());
    condemnMeta(*backend, store->layout(), h, /*round*/ 5);   // meta = condemned(E1), body present

    // GC wins the tombstone claim FIRST (condemned -> tombstone).
    const auto before = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(before && before->meta.state == MetaState::Condemned);
    const CasResult claim = casMeta(*backend, store->layout(), h, before->etag,
        BlobMeta{.incarnation = mintU128(), .state = MetaState::Tombstone, .condemn_round = 5, .size = payload.size()});
    ASSERT_EQ(claim.outcome, CasOutcome::Committed);

    // NOW a writer tries putBlob: it must observe tombstone and WAIT/ABORT — NEVER un-tombstone.
    expectThrowsCode(DB::ErrorCodes::ABORTED, [&] {
        auto b = /* build+precommit */; b->putBlob(BlobId(u128ToHex(h)), BlobSource::fromString(payload));
    });
    // GC completes the delete (body then tombstone meta). No committed ref exists -> no dangle.
    // Assert: had the writer been allowed tombstone->clean (the bug), the body delete below would strand a
    // clean meta with no body. Terminal tombstone forbids it, so the meta stayed tombstone.
    const auto after = loadMeta(*backend, store->layout(), h);
    ASSERT_TRUE(after && after->meta.state == MetaState::Tombstone);
}
```
This is the C++ analog of `CaMetaDescriptorRaw.tla`'s `sab_resurrect_tomb` red run — it proves the code, not just the model, honors terminal tombstone.

- [ ] **Step 1: Full CAS gtest suite**

```bash
ninja -C build unit_tests_dbms > build/cas_full_build.log 2>&1
build/src/unit_tests_dbms --gtest_filter='*Cas*:*Ca*' > build/test_cas_full.log 2>&1
```
Expected: all pass (the known-flaky `CaWiring*` set not grown). Summarize via subagent; investigate any failure with systematic-debugging before proceeding.

- [ ] **Step 2: Full CAS gtest suite under ASan**

```bash
ninja -C build_asan unit_tests_dbms > build_asan/cas_full_build.log 2>&1
build_asan/src/unit_tests_dbms --gtest_filter='*Cas*:*Ca*' > build_asan/test_cas_full.log 2>&1
```
Expected: no ASan reports. (Note: the pre-existing CA-ASAN-SUITE skip debt — some tests provoke `LOGICAL_ERROR` which aborts under `DEBUG_OR_SANITIZER_BUILD`; if such aborts appear, they are the known debt, not a Task-1-6 regression — confirm each abort predates this branch's meta work.)

- [ ] **Step 3: CA-s3 stateless lane**

Run the CA-s3 stateless lane per `reference_praktika_local_runs` (binary symlinked at `ci/tmp/clickhouse`). Confirm 0 promote-abort exceptions in the server log and `ca-fsck` dangling=0 after the run. Summarize via subagent.

- [ ] **Step 4: Write the mass-DROP soak scenario**

Create `utils/ca-soak/scenarios/cards/mass_drop_meta_throughput.py` (follow the existing scenario-card structure in that dir): create N (e.g. 1e5) small parts across many tables, DROP them all, and measure GC condemn+delete wall-clock and the meta-op rate. Assert the delete pool keeps pace (no unbounded retired-list growth; fsck dangling=0 at the end). This sizes `gc_delete_pool_size`.

- [ ] **Step 5: Soak run with the fsck gate**

Run a short soak (per `reference_ca_soak_fresh_restart`): `python3 -m soak.run --seed <N> --phase 3 --duration 1200`, then the mass-DROP scenario. Gate on fsck: dangling=0, `meta_without_body`=0. Archive logs before teardown.

- [ ] **Step 6: Commit the scenario + a worklog note**

```bash
git add utils/ca-soak/scenarios/cards/mass_drop_meta_throughput.py \
        docs/superpowers/worklogs/2026-07-08-unattended-cas-cache-and-stabilization.md
git commit -m "test(cas): mass-DROP meta-throughput soak scenario + Phase B validation notes

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk"
```

---

## Self-Review

**1. Spec coverage** (spec §implementation-phases Phase B items 1-6 + §phase-b-deletions + §kept):
- Gate B (item 1) → Task 0. Wedge TLA+ debt → Task 0b. ✓
- Meta codec + layout + `ca-inspect`/`ca-fsck` (item 2) → Tasks 1, 2. ✓ (`.meta` dispatch before `blobs/`; INV-META-BODY pairing; rebuild meta capture in Task 5.)
- `putBlob` rewrite: raw body + meta, dedup=1 GET, resurrect=meta CAS, birth-completion (item 3) → Task 3. K3 + copy-forward re-source → Task 4. ✓
- GC: condemn=GET+CAS, spare, delete=tombstone handshake on a parallel pool, `peek_meta` supersede, `graduationDue` re-key, meta event-log (item 4) → Task 5. ✓
- Delete `RetireView`/syncer/`observed_gc_round`/ack-floor (item 5) → Task 6. ✓
- Validation gtests → CA-s3 → soak + mass-DROP (item 6) → Task 7. ✓
- Deletions table (RetireView, syncer, observed_gc_round, resurrect-supersede re-shape, K1/K3 re-source) → Tasks 3-6. ✓
- Kept (K1 re-sourced, K2 owner-liveness untouched, K3 re-sourced, K4 beat minus observed_gc_round, K5 merge/pacing/barriers, K6 read path — offset changes only) → Tasks 3-6. ✓
- Raw-body refinement (envelope elimination, three-state meta, resurrect skips body re-upload, tombstone delete, raw read) → Tasks 1, 3, 4, 5. ✓
- Consult findings: C1 birth-completion-from-source (Task 3 step 5), C2 body delete keyed on won-tombstone not a blind fresh HEAD — note the raw-body nuance that a fresh HEAD is safe *after* winning the tombstone because bodies are immutable (Task 5 step 4), I1 `peek_meta`+ledger-stores-meta-etag (Task 5 steps 3/6), I2 rebuild meta capture (Task 5 step 7), I3 idempotent redelete (Task 5 step 4), M4 `condemn_round` in the meta (Task 1), M5 graduation re-key (Task 5 step 5), M2 fsck tolerance of benign body/meta debris (Task 2). ✓

**2. Placeholder scan:** Two intentional `/* ... */` boilerplate markers remain in test steps (build/precommit setup in Tasks 3-5) — these point the implementer to copy existing same-file test boilerplate rather than inventing it, and name exactly what to set up; acceptable because the file already contains the pattern. The `hash-parse from body_key` in Task 2 step 7 and Task 5 step 7 explicitly reference the concrete idiom at `CasGc.cpp:1838-1849`. No `TBD`/`implement later`/"add error handling" placeholders.

**3. Type consistency:** `BlobMeta`/`MetaState`/`LoadedMeta`/`loadMeta`/`putMetaIfAbsent`/`casMeta`/`deleteMetaExact`/`blobMetaKey` are defined in Task 1 and used with identical signatures in Tasks 2-5. `RetiredEntry.token` semantics (body token → meta etag) is stated in Task 5 and consumed by `peek_meta`/delete consistently. `observeAndAdmitByMeta` is defined in Task 3 and not referenced elsewhere. `copyForwardFromCondemned(hash, LoadedMeta)` new signature is defined and called only within Task 4.

**Open judgment calls for the controller to resolve during execution (not blockers):**
- Task 3 step 5: the `absent-meta ∧ absent-body` retry mechanism (sentinel vs re-throw-ABORTED-and-let-`putBlob`-loop) — pick one and keep `putBlob`/`observeAndAdmitByMeta` consistent.
- Task 5 step 4: the delete `ThreadPool` sizing (`gc_delete_pool_size` default) is validated/tuned in Task 7 step 4; start at 16.
- Task 5 step 7 / Task 2: whether a meta-less body in `rebuildBaseline` is repaired (create condemned meta) or left for the debris sweep — spec I2 says capture+repair; the debris sweep (claim-first) is the runtime path. Prefer repair in rebuild (disaster-recovery), claim-first sweep at runtime.

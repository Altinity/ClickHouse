---
description: 'Implementation plan: fold the GC retired list into the per-shard source-edge run (3-cursor to 2-cursor settlement merge) — TLA+ gate first, then run format, merge/round, consumers, validation.'
sidebar_label: 'Plan: CAS retired-in-snapshot'
sidebar_position: 10
slug: /superpowers/plans/cas-retired-in-snapshot
title: 'CAS Retired-in-Snapshot — Implementation Plan'
doc_type: 'reference'
---

# CAS Retired-in-Snapshot Implementation Plan {#title}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One artifact family for GC settlement — the per-shard source-edge run carries the condemned
state; `RetiredSet`/`CART`/`retiredKey`/`GcState::retired_refs` are deleted; the settlement merge goes
from 3 cursors to 2 with settlement semantics byte-for-byte unchanged.

**Architecture:** A new carried `kCondemned` row (at the existing zero-sentinel key) rides the
write-once deterministic run; the fold seal gains a per-shard `CondemnedSummary` so `graduationDue`
and ref-carry decisions are zero-I/O; every consumer (round, dryrun, `fsck`, `ca-inspect`, rebuild)
reads condemned state from seal-named runs. Adoption rules do not change (byte-equal deterministic
artifacts; attempt-pinning makes collisions byte-identical resends).

**Tech Stack:** C++ (ClickHouse `src/Disks/.../ContentAddressed/Core`), protobuf (`cas_format.proto`),
gtest (`src/Disks/tests/`), TLA+/TLC (`docs/superpowers/models/`, `tmp/tla2tools.jar`), praktika
stateless lane, ca-soak.

**Spec:** `docs/superpowers/specs/2026-07-10-cas-retired-in-snapshot-design.md` — the requirements
document. Where this plan and the spec disagree, the spec governs; stop and flag it.

## Global constraints {#global-constraints}

- **Settlement semantics byte-for-byte unchanged** (spec §1): condemn → carry → graduate
  (`delete_pending`) → redelete; graduation gate `condemn_round < current_round`; two-phase
  graduation; clamp suppression; pre-CAS single delete site; resurrect-supersede; `.meta` writes;
  B170 events; GC-log counters. `RetiredMergeResult` keeps its exact shape.
- **`05008_ca_gc_snap_prune` must pass UNMODIFIED** (spec §7). Never edit that test.
- **No compat scaffolding** (spec §1): `key_schema = 0` source-edge runs fail closed; old pools are
  recreated; deleted proto fields become `reserved`.
- **Fail-closed rules** (spec §2.1/§2.2): unknown `token_type`, truncated payload, edge row at
  `source_id = 0`, sentinel row at `source_id != 0`, unknown row type, duplicate sentinel per blob,
  `key_schema != 1`, `kind != SourceEdge` → `CORRUPTED_DATA` (or `NOT_IMPLEMENTED` for future
  versions). Missing adopted seal / missing `condemned_summary` at `snap_generation > 0` →
  `graduationDue` returns `true`, never a silent defer.
- **Carried sentinel is settlement-only** (spec §2.1): it never sets the merge's touch bit — no
  zero-marker emission, no `peek_head` from carry alone.
- **Adoption unchanged** (spec §4): `putDeterministicArtifact` byte-equal for runs and seal.
- Branch `cas-gc-rebuild`; new commits only (no rebase/amend); Allman braces; never `sleep` in C++
  to fix races; commit trailers:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.
- Builds: `ninja -C build <target> > build/<log> 2>&1` (no `-j`); analyze logs via a subagent;
  test runs redirect to unique log files in `build/`.
- Code lives under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` (below: `$CAS`);
  tests under `src/Disks/tests/`. The unit-test binary is `build/src/unit_tests_dbms`.

---

## File map {#file-map}

| File | Role in this plan |
|---|---|
| `docs/superpowers/models/CaRetiredInRun.tla` (+`.cfg`, sabotage cfgs, `run_retiredinrun.sh`) | Task 1: TLA+ gate |
| `$CAS/Core/CasBlobInDegree.h/.cpp` | Tasks 2-3: row codec, typed open, `sourceEdgeId` guard, 2-cursor merge |
| `$CAS/Core/CasRunFile.h/.cpp` | Task 2 (read-only reference; no change expected — validation lives in the typed open helper) |
| `$CAS/Core/CasGenerationSeal.h/.cpp`, `$CAS/Core/Proto/cas_format.proto` | Task 4: `CondemnedSummary` |
| `$CAS/Core/CasGc.h/.cpp` | Tasks 4-6: round wiring, `graduationDue`, dryrun, rebuild |
| `$CAS/Core/CasFsck.cpp`, `$CAS/Core/CasInspect.cpp` | Task 5: consumers |
| `$CAS/Core/CasGcFormats.h/.cpp`, `$CAS/Core/CasLayout.h`, `$CAS/Core/CasFormat.h` | Task 7: deletions (`RetiredSet`, `CART`, `retiredKey`, `retired_refs`) |
| `src/Disks/tests/gtest_cas_blob_indegree.cpp` | Tasks 2-3 tests |
| `src/Disks/tests/gtest_cas_gc_round.cpp`, `gtest_cas_gc_round_defer.cpp`, `gtest_cas_gc_formats.cpp`, `gtest_cas_gc_leak.cpp`, `gtest_cas_truncate_reclaim.cpp`, `gtest_cas_gc_resume.cpp`, `gtest_cas_gc_attempt.cpp` | Tasks 4-7: adapt round/format tests |
| `docs/superpowers/cas/04-gc-protocol.md`, `05-formats-and-backend.md`, `07-s3-budget.md`, `ROADMAP.md` | Task 7: doc updates |

Execution order is strict: Task 1 (gate) must be GREEN before Tasks 2+ start (spec §6). Tasks 2→7
are sequential (each builds on the previous); Task 8 is the validation checkpoint.

---

### Task 1: TLA+ gate — `CaRetiredInRun` green + sabotage red {#task-1}

**Files:**
- Create: `docs/superpowers/models/CaRetiredInRun.tla`
- Create: `docs/superpowers/models/CaRetiredInRun.cfg`
- Create: `docs/superpowers/models/CaRetiredInRun_sab_inmem_token.cfg`
- Create: `docs/superpowers/models/CaRetiredInRun_sab_attempt_reuse.cfg`
- Create: `docs/superpowers/models/CaRetiredInRun_sab_no_pacing.cfg`
- Create: `docs/superpowers/models/run_retiredinrun.sh`

**Interfaces:**
- Consumes: `tmp/tla2tools.jar` (already present), the runner pattern from
  `docs/superpowers/models/run_tlc.sh`.
- Produces: a green gate (all invariants hold) + three red sabotage configs. Tasks 2+ may start
  only after this task's acceptance criteria are met.

**What the model covers (spec §6).** One gc-shard, blobs `{b1, b2}`, writers add/remove edges via a
journal; the GC leader folds a journal prefix (the cut) into an attempt-scoped artifact
`{edges, condemned}` (one atom = the merged run), seals it, and adopts via one `gc/state` CAS.
Settlement rules are spec §3 (pending→redelete at d=0; d>0→spare; d=0 &
`condemn_round < round`→pending; else carry). Physical blobs have incarnation tokens; a writer
resurrect installs a fresh token and a journalled edge (EDGE-BEFORE-OBSERVE). Meta effects are a
separate advisory variable no destructive transition reads. A `Sabotage` constant switches the three
required red flips.

- [ ] **Step 1: Write the runner script** (exact copy of the `run_tlc.sh` pattern, retargeted):

```bash
#!/usr/bin/env bash
# Run one TLC config against CaRetiredInRun.tla. Usage: ./run_retiredinrun.sh <cfg-file> [extra TLC args]
set -uo pipefail
if [[ $# -lt 1 ]]; then
  echo "usage: $0 <cfg-file> [extra TLC args]" >&2
  exit 2
fi
cd "$(dirname "$0")"
JAR=../../../tmp/tla2tools.jar
[[ -f "$JAR" ]] || { echo "jar not found: $JAR" >&2; exit 3; }
CFG="$1"; shift || true
LOG="../../../tmp/tlc_$(basename "$CFG" .cfg).log"
/usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$JAR" tlc2.TLC -metadir ../../../tmp/tlc-meta -workers auto -config "$CFG" "$@" CaRetiredInRun.tla >"$LOG" 2>&1
RC=$?
grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in" "$LOG" | tail -8
echo "exit=$RC log=$LOG"
exit $RC
```

`chmod +x docs/superpowers/models/run_retiredinrun.sh`.

- [ ] **Step 2: Write the model.** Complete draft below — the implementer iterates on it until the
acceptance criteria hold (that iteration IS the gate; if an invariant fails on the honest config,
STOP and report the counterexample — it may be a real spec bug):

```tla
---------------------------- MODULE CaRetiredInRun ----------------------------
(* Retired-list-inside-the-run settlement gate (spec 2026-07-10-cas-retired-in-snapshot §6).
   One gc-shard. The adopted per-shard artifact is one atom {edges, condemned rows}; adoption
   is byte-equal deterministic under attempt-pinned keys; settlement follows spec §3 exactly.
   Sabotage \in {"none","inmem_token","attempt_reuse","no_pacing"} flips the three red gates. *)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Blobs,          \* e.g. {"b1","b2"}
          MaxRound,       \* bound, e.g. 4
          MaxToken,       \* bound on incarnation tokens per blob, e.g. 3
          Sabotage        \* "none" | "inmem_token" | "attempt_reuse" | "no_pacing"

VARIABLES
  journal,     \* Seq of <<blob, op>> ; op \in {"add","rm"} — writer edge events (abstract source ids)
  phys,        \* [Blobs -> 0..MaxToken] physical incarnation token; 0 = absent
  liveRef,     \* [Blobs -> BOOLEAN] writer holds a live reference (edge added, not removed)
  adopted,     \* the durable adopted artifact:
               \*   [gen |-> Nat, cut |-> Nat, edges |-> [Blobs -> Nat],
               \*    cond |-> [Blobs -> [st: {"none","cond","pend"}, tok: Nat, round: Nat]], round |-> Nat]
  artifacts,   \* attempt-keyed store: [Nat -> artifact] — putIfAbsent byte-equal domain
  nextAttempt, \* monotone attempt counter (lease.seq abstraction)
  meta         \* [Blobs -> {"clean","cond"}] advisory freshness meta (never read destructively)

vars == <<journal, phys, liveRef, adopted, artifacts, nextAttempt, meta>>

TokOf(b) == phys[b]

EdgeCount(b, cut) ==
  Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "add"})
    - Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "rm"})

Init ==
  /\ journal = <<>>
  /\ phys = [b \in Blobs |-> 0]
  /\ liveRef = [b \in Blobs |-> FALSE]
  /\ adopted = [gen |-> 0, cut |-> 0, edges |-> [b \in Blobs |-> 0],
                cond |-> [b \in Blobs |-> [st |-> "none", tok |-> 0, round |-> 0]], round |-> 0]
  /\ artifacts = <<>>
  /\ nextAttempt = 1
  /\ meta = [b \in Blobs |-> "clean"]

(* Writer uploads (or resurrects) an incarnation and journals the edge FIRST (EDGE-BEFORE-OBSERVE). *)
WriterAdd(b) ==
  /\ phys[b] < MaxToken
  /\ ~liveRef[b]
  /\ journal' = Append(journal, <<b, "add">>)
  /\ phys' = [phys EXCEPT ![b] = IF phys[b] = 0 THEN 1 ELSE phys[b]]  \* fresh upload if absent
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ meta' = [meta EXCEPT ![b] = "clean"]   \* resurrect path flips meta back
  /\ UNCHANGED <<adopted, artifacts, nextAttempt>>

(* Writer resurrect over a condemned-but-present incarnation: fresh token (etag changes). *)
WriterResurrect(b) ==
  /\ phys[b] > 0 /\ phys[b] < MaxToken
  /\ meta[b] = "cond"          \* the meta point-read told it to re-upload
  /\ ~liveRef[b]
  /\ phys' = [phys EXCEPT ![b] = phys[b] + 1]
  /\ journal' = Append(journal, <<b, "add">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ meta' = [meta EXCEPT ![b] = "clean"]
  /\ UNCHANGED <<adopted, artifacts, nextAttempt>>

WriterRemove(b) ==
  /\ liveRef[b]
  /\ journal' = Append(journal, <<b, "rm">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = FALSE]
  /\ UNCHANGED <<phys, adopted, artifacts, nextAttempt, meta>>

(* Deterministic fold at a chosen cut >= adopted.cut: settle per spec §3. *)
Settle(b, d, prior, newRound) ==
  LET pc == prior.cond[b] IN
  IF pc.st = "pend"
    THEN IF d = 0 THEN [st |-> "none", tok |-> 0, round |-> 0]           \* redelete (delete executes)
                  ELSE [st |-> "none", tok |-> 0, round |-> 0]           \* structurally-impossible spare
  ELSE IF d > 0 THEN [st |-> "none", tok |-> 0, round |-> 0]             \* spared
  ELSE IF pc.st = "cond" /\ (Sabotage = "no_pacing" \/ pc.round < newRound)
    THEN [st |-> "pend", tok |-> pc.tok, round |-> pc.round]             \* graduated
  ELSE IF pc.st = "cond" THEN pc                                          \* carried
  ELSE IF phys[b] > 0 THEN [st |-> "cond", tok |-> phys[b], round |-> newRound]  \* fresh condemn (head)
  ELSE [st |-> "none", tok |-> 0, round |-> 0]                            \* absent-at-condemn

FoldRound ==
  /\ adopted.round < MaxRound
  /\ \E cut \in adopted.cut..Len(journal) :
     LET newRound == adopted.round + 1
         attempt  == IF Sabotage = "attempt_reuse" THEN nextAttempt - 1 ELSE nextAttempt
         art == [gen  |-> adopted.gen + 1, cut |-> cut,
                 edges |-> [b \in Blobs |-> EdgeCount(b, cut)],
                 cond  |-> [b \in Blobs |-> Settle(b, EdgeCount(b, cut), adopted, newRound)],
                 round |-> newRound]
         collide == attempt \in DOMAIN artifacts
         durable == IF collide THEN artifacts[attempt] ELSE art
     IN /\ (collide /\ Sabotage /= "attempt_reuse") => (artifacts[attempt] = art)  \* byte-equal or CORRUPTED_DATA (halt = disallow)
        /\ artifacts' = IF collide THEN artifacts ELSE artifacts @@ (attempt :> art)
        (* redelete executes pre-CAS on prior-adopted pending rows at d=0 in THIS fold; the token
           used is the durable prior one (or, under sabotage, an in-memory re-observation). *)
        /\ LET delTok(b) == IF Sabotage = "inmem_token" THEN phys[b] ELSE adopted.cond[b].tok IN
           phys' = [b \in Blobs |->
                     IF adopted.cond[b].st = "pend" /\ durable.edges[b] = 0 /\ phys[b] = delTok(b)
                       THEN 0 ELSE phys[b]]
        /\ adopted' = durable
        /\ meta' = [b \in Blobs |-> IF durable.cond[b].st \in {"cond","pend"} THEN "cond" ELSE meta[b]]
        /\ nextAttempt' = nextAttempt + 1
        /\ UNCHANGED <<journal, liveRef>>

Next ==
  \/ \E b \in Blobs : WriterAdd(b) \/ WriterRemove(b) \/ WriterResurrect(b)
  \/ FoldRound

Spec == Init /\ [][Next]_vars

(* INV_NO_LOSS: a blob the writer holds a live journalled reference to is never physically absent
   once its edge has been folded (folded live edge => present). *)
INV_NO_LOSS ==
  \A b \in Blobs :
    (liveRef[b] /\ EdgeCount(b, adopted.cut) > 0) => phys[b] > 0

(* INV_NO_RETURN: a delete never removes an incarnation newer than the condemn-time token —
   equivalently, phys only transitions to 0 from exactly the condemned token (checked in FoldRound
   by construction; restated as: a pending row's token never exceeds the live incarnation). *)
INV_NO_RETURN ==
  \A b \in Blobs : adopted.cond[b].st \in {"cond","pend"} => adopted.cond[b].tok <= MaxToken

(* One-pass adoption: the adopted artifact always equals some stored attempt artifact. *)
INV_ONE_PASS ==
  adopted.round = 0 \/ \E a \in DOMAIN artifacts : artifacts[a] = adopted

THEOREM Spec => [](INV_NO_LOSS /\ INV_NO_RETURN /\ INV_ONE_PASS)
===============================================================================
```

- [ ] **Step 3: Write the honest config** `CaRetiredInRun.cfg`:

```
SPECIFICATION Spec
CONSTANTS
  Blobs = {b1, b2}
  MaxRound = 4
  MaxToken = 3
  Sabotage = "none"
INVARIANTS
  INV_NO_LOSS
  INV_NO_RETURN
  INV_ONE_PASS
```

And the three sabotage configs — identical except `Sabotage = "inmem_token"` /
`"attempt_reuse"` / `"no_pacing"` respectively (file names from **Files** above).

- [ ] **Step 4: Run the honest config — must be GREEN:**

Run: `docs/superpowers/models/run_retiredinrun.sh CaRetiredInRun.cfg`
Expected: `Model checking completed. No error has been found.` If TLC reports a violation, analyze
the trace: either fix a modeling bug, or — if the trace maps to a real protocol hole — STOP and
report it (do not weaken the invariant).

- [ ] **Step 5: Run each sabotage config — each must be RED** (an invariant violation or a
byte-equal conflict deadlock reported by TLC):

Run: `docs/superpowers/models/run_retiredinrun.sh CaRetiredInRun_sab_inmem_token.cfg` → expected
`Error: Invariant INV_NO_LOSS is violated` (a resurrected incarnation is deleted with a stale
in-memory token). Same for `_sab_attempt_reuse.cfg` (stale edges silently adopted → INV_NO_LOSS)
and `_sab_no_pacing.cfg` (graduation without the round gate closes the racing-writer spare window
→ INV_NO_LOSS). If a sabotage config comes back green, the model is too weak — strengthen it
before proceeding (this is a hard gate).

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/models/CaRetiredInRun* docs/superpowers/models/run_retiredinrun.sh
git commit -m "tla(cas): CaRetiredInRun gate — retired-in-run settlement green + 3 sabotage reds"
```

---

### Task 2: Run format — `kCondemned` codec, typed open, `source_id = 0` guard {#task-2}

**Files:**
- Modify: `$CAS/Core/CasBlobInDegree.h` (public surface: row constants, `CondemnedRow`,
  encode/decode, typed open helper)
- Modify: `$CAS/Core/CasBlobInDegree.cpp` (implementations; `sourceEdgeId` zero-guard at `:101`;
  row constants near `:25`)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: `RunFileWriter::append(std::string_view key, std::string_view payload)`,
  `RunFileReader` + `keySchema()`/`kind()` (`$CAS/Core/CasRunFile.h`), `Token`/`TokenType`
  (`$CAS/Core/CasToken.h`), `srcEdgeRunKey`/`parseSrcEdgeRunKey`.
- Produces (Tasks 3-6 rely on these exact names):

```cpp
/// $CAS/Core/CasBlobInDegree.h
constexpr char kEdgeActive = 0x01;      // moved up from the .cpp (now part of the format surface)
constexpr char kZeroMarker = 0x00;
constexpr char kCondemned  = 0x02;
constexpr uint8_t kSourceEdgeKeySchema = 1;

struct CondemnedRow
{
    bool delete_pending = false;
    Token token;                 // {value, type} — full token, spec §2.1
    uint64_t size = 0;
    uint64_t condemn_round = 0;
    bool operator==(const CondemnedRow &) const = default;
};

String encodeCondemnedRow(const CondemnedRow & row);          // [0x02][flags][token_type][round][size][len][value]
CondemnedRow decodeCondemnedRow(std::string_view payload);    // throws CORRUPTED_DATA per spec §2.1

/// Typed open (spec §2.1): validates kind == SourceEdge and key_schema == kSourceEdgeKeySchema,
/// fails closed otherwise. ALL source-edge run readers go through this.
RunFileReader openSourceEdgeRun(std::string_view bytes);
```

- [ ] **Step 1: Write the failing tests** (append to `src/Disks/tests/gtest_cas_blob_indegree.cpp`,
matching the file's existing includes/namespaces):

```cpp
TEST(CasCondemnedRow, RoundTripAllTokenTypes)
{
    for (auto type : {DB::Cas::TokenType::ETag, DB::Cas::TokenType::Generation, DB::Cas::TokenType::Emulated})
    {
        DB::Cas::CondemnedRow row;
        row.delete_pending = (type == DB::Cas::TokenType::Generation);
        row.token = DB::Cas::Token{.value = "etag-abc-123", .type = type};
        row.size = 4096;
        row.condemn_round = 7;
        const auto bytes = DB::Cas::encodeCondemnedRow(row);
        ASSERT_EQ(bytes[0], DB::Cas::kCondemned);
        EXPECT_EQ(DB::Cas::decodeCondemnedRow(bytes), row);
    }
}

TEST(CasCondemnedRow, UnknownTokenTypeFailsClosed)
{
    DB::Cas::CondemnedRow row;
    row.token = DB::Cas::Token{.value = "t", .type = DB::Cas::TokenType::ETag};
    auto bytes = DB::Cas::encodeCondemnedRow(row);
    bytes[2] = 99;   // token_type byte (offset: [0]=0x02 [1]=flags [2]=token_type)
    EXPECT_THROW(DB::Cas::decodeCondemnedRow(bytes), DB::Exception);
}

TEST(CasCondemnedRow, TruncatedPayloadFailsClosed)
{
    DB::Cas::CondemnedRow row;
    row.token = DB::Cas::Token{.value = "0123456789", .type = DB::Cas::TokenType::ETag};
    auto bytes = DB::Cas::encodeCondemnedRow(row);
    bytes.resize(bytes.size() - 3);   // token bytes shorter than declared token_len
    EXPECT_THROW(DB::Cas::decodeCondemnedRow(bytes), DB::Exception);
}

TEST(CasSourceEdgeRun, TypedOpenRejectsWrongSchemaAndKind)
{
    /// A run written with today's writer carries key_schema 0 -> the typed open must fail closed.
    /// Build a minimal run body via RunFileWriter with a deliberately wrong header.
    DB::WriteBufferFromOwnString out;
    DB::Cas::RunHeader header;
    header.kind = DB::Cas::RunKind::SourceEdge;
    header.key_schema = 0;                                    // pre-refactor schema
    DB::Cas::RunFileWriter writer(out, header);
    writer.finish();
    EXPECT_THROW(DB::Cas::openSourceEdgeRun(out.str()), DB::Exception);

    DB::WriteBufferFromOwnString out2;
    DB::Cas::RunHeader h2;
    h2.kind = DB::Cas::RunKind::ManifestEntries;              // wrong kind, right schema
    h2.key_schema = DB::Cas::kSourceEdgeKeySchema;
    DB::Cas::RunFileWriter w2(out2, h2);
    w2.finish();
    EXPECT_THROW(DB::Cas::openSourceEdgeRun(out2.str()), DB::Exception);
}

TEST(CasSourceEdgeRun, SourceEdgeIdZeroIsReserved)
{
    /// The zero source_id is the sentinel namespace; producers fail closed on a zero hash
    /// (probability 2^-128 — the check documents the reservation).
    EXPECT_THROW(DB::Cas::assertValidSourceEdgeId(UInt128{0}), DB::Exception);
    EXPECT_NO_THROW(DB::Cas::assertValidSourceEdgeId(UInt128{1}));
}
```

Note: if `RunHeader`/`RunFileWriter` are not visible from the test TU, include
`Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRunFile.h` (they are public
there).

- [ ] **Step 2: Build and run — verify the new tests FAIL to compile** (missing symbols):

```bash
ninja -C build unit_tests_dbms > build/build_task2_red.log 2>&1
```
Expected: compile error mentioning `CondemnedRow` / `encodeCondemnedRow` / `openSourceEdgeRun` /
`assertValidSourceEdgeId`. (Analyze the log with a subagent; do not paste it.)

- [ ] **Step 3: Implement.** In `$CAS/Core/CasBlobInDegree.h` add the interface block from
**Produces** above (plus `void assertValidSourceEdgeId(const UInt128 & source_id);`). In
`$CAS/Core/CasBlobInDegree.cpp`:

```cpp
String encodeCondemnedRow(const CondemnedRow & row)
{
    String out;
    out.push_back(kCondemned);
    out.push_back(static_cast<char>(row.delete_pending ? 1 : 0));
    out.push_back(static_cast<char>(row.token.type));
    auto beU64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) out += static_cast<char>((v >> (8 * i)) & 0xFF); };
    beU64(row.condemn_round);
    beU64(row.size);
    if (row.token.value.size() > 0xFFFF)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: token too long ({})", row.token.value.size());
    out += static_cast<char>((row.token.value.size() >> 8) & 0xFF);
    out += static_cast<char>(row.token.value.size() & 0xFF);
    out += row.token.value;
    return out;
}

CondemnedRow decodeCondemnedRow(std::string_view p)
{
    /// [0]=0x02 [1]=flags [2]=token_type [3..10]=round [11..18]=size [19..20]=len [21..]=value
    constexpr size_t kFixed = 21;
    if (p.size() < kFixed || p[0] != kCondemned)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: malformed header");
    CondemnedRow row;
    const uint8_t flags = static_cast<uint8_t>(p[1]);
    if (flags > 1)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: unknown flags 0x{:02x}", flags);
    row.delete_pending = flags & 1;
    const uint8_t type = static_cast<uint8_t>(p[2]);
    if (type < 1 || type > 3)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: unknown token_type {}", type);
    row.token.type = static_cast<TokenType>(type);
    auto beU64 = [&](size_t off) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint8_t>(p[off + i]); return v; };
    row.condemn_round = beU64(3);
    row.size = beU64(11);
    const size_t len = (static_cast<uint8_t>(p[19]) << 8) | static_cast<uint8_t>(p[20]);
    if (p.size() != kFixed + len)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS condemned row: declared token_len {} vs payload {}", len, p.size() - kFixed);
    row.token.value = String(p.substr(kFixed, len));
    return row;
}

void assertValidSourceEdgeId(const UInt128 & source_id)
{
    if (source_id == UInt128{0})
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS source edge: source_id 0 is the reserved sentinel key (spec §2.1)");
}

RunFileReader openSourceEdgeRun(std::string_view bytes)
{
    RunFileReader reader(bytes);
    if (reader.kind() != RunKind::SourceEdge)
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS source-edge run: wrong kind {}", static_cast<int>(reader.kind()));
    if (reader.keySchema() != kSourceEdgeKeySchema)
        throw Exception(ErrorCodes::NOT_IMPLEMENTED,
            "CAS source-edge run: key_schema {} (this build reads only {})", reader.keySchema(), kSourceEdgeKeySchema);
    return reader;
}
```

Also in `sourceEdgeId` (`.cpp:101` area) add before `return`:
`if (result == UInt128{0}) throw Exception(ErrorCodes::LOGICAL_ERROR, "CAS source edge: hash collided with the reserved sentinel id 0");`
(compute into a local `result` first). Match the constructor semantics `RunFileReader` actually has
(if it takes `(bytes)` vs `(bytes, expected)` — adapt mechanically, keeping the two checks).

- [ ] **Step 4: Build and run the new tests — verify PASS:**

```bash
ninja -C build unit_tests_dbms > build/build_task2_green.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasCondemnedRow*:CasSourceEdgeRun*' > build/test_task2.log 2>&1
```
Expected: all new tests PASS. Then the full pre-existing battery must be untouched:
`build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree*:CasThreeCursorMerge*' > build/test_task2_regress.log 2>&1` — all PASS
(the writer still emits `key_schema = 0` until Task 3; `openSourceEdgeRun` has no callers yet).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_blob_indegree.cpp
git commit -m "cas: kCondemned row codec + typed source-edge open + source_id=0 reservation (retired-in-snapshot T2)"
```

---

### Task 3: 2-cursor merge — condemned rows through `foldDeltasIntoGeneration` {#task-3}

**Files:**
- Modify: `$CAS/Core/CasBlobInDegree.h:114-124` (signature), `$CAS/Core/CasBlobInDegree.cpp`
  (writer `key_schema`, `PriorEdgeCursor`, `closeBlob`, `settleRetiredBelow`, row emission)
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp` (adapt `CasThreeCursorMerge` suite + new tests)

**Interfaces:**
- Consumes: Task 2 (`CondemnedRow`, codec, `openSourceEdgeRun`, `kSourceEdgeKeySchema`).
- Produces: the new signature (Tasks 4-6 call it):

```cpp
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              const std::vector<RunRef> & prior_runs,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              /* prior_retired REMOVED — the prior run IS the retired input */
                              uint64_t current_round = 0, uint64_t condemn_round = 0,
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & head_blob = {},
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & peek_head = {},
                              RetiredMergeResult * out_retired = nullptr,
                              bool suppress_destructive = false);
```

`RetiredMergeResult` (`still_retired`/`graduated`/`spared`/`redelete`/`replaced`) is unchanged —
`still_retired` mirrors exactly the `kCondemned` rows written into the output run (same order).

**Behavioral requirements (all from spec §2.1/§3 — the reviewer gates on these):**
1. Prior-run `kCondemned` rows are decoded at blob open and settled at close-out with today's rule
   order (pending→redelete@d=0; d>0→spare; d=0 & `condemn_round < current_round`→graduate;
   else carry).
2. The sentinel row NEVER sets `cur_touched` — no zero-marker emission and no `peek_head` for a
   generation that only carries the row.
3. Fresh condemns (`head_blob`) write `kCondemned` rows; absent-at-condemn writes `kZeroMarker`
   exactly as today. Prior-gen zero markers are still dropped on carry.
4. Row/key invariants enforced while streaming (`kEdgeActive` never at `source_id = 0`; sentinel
   rows only there; unknown row type / duplicate sentinel → `CORRUPTED_DATA`).
5. Writer emits `header.key_schema = kSourceEdgeKeySchema` (the `= 0` at `.cpp:182` — this flips
   the run reader requirement; ALL run readers must now use `openSourceEdgeRun` — done here for
   `PriorEdgeCursor`, `zeroInDegree`, `inDegreeInGeneration`).
6. Under `suppress_destructive`, pending rows carry unchanged and floor-passed rows stay
   condemned-only (byte-identical carry of the row).

- [ ] **Step 1: Adapt the existing `CasThreeCursorMerge` tests** to the new signature: everywhere a
test passes a `prior_retired` vector, instead build a prior GENERATION whose run contains the
equivalent `kCondemned` rows (fold once with `head_blob` returning the desired token to mint the
row, or write the run directly with `RunFileWriter` + `encodeCondemnedRow`). Keep every assertion
on `RetiredMergeResult` byte-identical. Add the new tests:

```cpp
TEST(CasTwoCursorMerge, CarriedSentinelIsNotATouch)
{
    /// Gen1: condemn b (head_blob present). Gen2: NO deltas at all — the carried kCondemned row
    /// must (a) survive byte-identically, (b) emit no zero-marker, (c) never call peek_head.
    /// (Build gen1 via foldDeltasIntoGeneration with one add+rm delta and a head_blob stub;
    /// then fold gen2 with empty deltas and a peek_head that aborts the test if called.)
    size_t peek_calls = 0;
    auto peek = [&](const UInt128 &) -> std::optional<DB::Cas::HeadResult> { ++peek_calls; return {}; };
    /// ... gen1 setup as in CasThreeCursorMerge.PendingRedeletesAndDrops, then:
    /// foldDeltasIntoGeneration(..., /*scattered=*/{}, ..., current_round, condemn_round,
    ///                          /*head_blob=*/{}, peek, &result, false);
    EXPECT_EQ(peek_calls, 0u);
    /// decode the gen2 run: exactly one kCondemned row for b, zero kZeroMarker rows.
}

TEST(CasTwoCursorMerge, MalformedRunFailsClosed)
{
    /// Handcraft a run with kEdgeActive at source_id=0 -> the merge cursor must throw CORRUPTED_DATA.
    /// And a run with two sentinel rows for one blob -> CORRUPTED_DATA.
}
```

(Write the full bodies following the construction helpers already present in the file — e.g.
`FoldStartsFromEmptyPriorGeneration` shows the Backend/Layout fixture pattern to copy.)

- [ ] **Step 2: Build — the suite must FAIL** (signature mismatch + missing behavior):
`ninja -C build unit_tests_dbms > build/build_task3_red.log 2>&1` — compile errors are the expected
red state here.

- [ ] **Step 3: Implement the merge changes** in `$CAS/Core/CasBlobInDegree.cpp`:
  - Writer header: `header.key_schema = kSourceEdgeKeySchema;` (was `0`, `.cpp:182`).
  - `PriorEdgeCursor`: opens via `openSourceEdgeRun`; on a sentinel key (`source_id == 0`) with
    value `kCondemned`, decode via `decodeCondemnedRow` and expose it as
    `std::optional<CondemnedRow> pending_condemned_for(cur_blob)` instead of yielding it as an
    edge; enforce invariant 4 (throw on `kEdgeActive` at sentinel key, on unknown value byte, on a
    second sentinel for the same blob).
  - Replace the third cursor: delete the `prior_retired` parameter, `settleRetiredBelow` iterates
    the cursor-exposed condemned rows (they arrive in hash order — same order the old sorted
    vector had); `closeBlob` settles using the stashed `CondemnedRow` exactly where it used
    `prior_retired[ri]`.
  - Touch rule: the stash does NOT set `cur_touched`.
  - Emission: `still_retired` entries → `writer.append(srcEdgeRunKey(hash, kZeroSourceId), encodeCondemnedRow(...))`
    at the blob's close-out (sentinel-first order is preserved because the sentinel key sorts below
    the blob's edges and blobs are processed in ascending hash order — emit the sentinel BEFORE the
    blob's surviving edge rows).
  - `RetiredEntry` ⇄ `CondemnedRow` conversion: `RetiredEntry{kind=Blob, hash, token, size,
    condemn_round, delete_pending}` maps 1:1 (the `kind` field stays in the in-memory
    `RetiredMergeResult` for now; it dies with `RetiredSet` in Task 7).
  - `zeroInDegree` / `inDegreeInGeneration`: open via `openSourceEdgeRun`; skip `kCondemned`
    values where they skip zero markers today (`inDegreeInGeneration` counts only `kEdgeActive`
    rows; `zeroInDegree` returns only `kZeroMarker` rows — semantics unchanged).

- [ ] **Step 4: Build + run the full merge battery — PASS:**

```bash
ninja -C build unit_tests_dbms > build/build_task3_green.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasBlobInDegree*:CasThreeCursorMerge*:CasTwoCursorMerge*:CasCondemnedRow*:CasSourceEdgeRun*' > build/test_task3.log 2>&1
```
Expected: all PASS. NOTE: `gtest_cas_gc_round*.cpp` and friends will NOT compile until Task 4 (they
call the old signature) — keep the build green by updating their call sites mechanically in this
task if the build breaks (pass-through change only: drop the `prior_retired` argument, feed the
prior state through runs in fixtures), or coordinate Tasks 3+4 in one build. Prefer the former.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBlobInDegree.cpp \
        src/Disks/tests/gtest_cas_blob_indegree.cpp src/Disks/tests/gtest_cas_gc_*.cpp
git commit -m "cas: 2-cursor settlement merge — condemned rows ride the source-edge run (retired-in-snapshot T3)"
```

---

### Task 4: Seal summary + round wiring + `graduationDue` {#task-4}

**Files:**
- Modify: `$CAS/Core/Proto/cas_format.proto` (`FoldSealProto` + new `CondemnedSummaryProto`)
- Modify: `$CAS/Core/CasGenerationSeal.h:64-76` (+`.cpp` codec)
- Modify: `$CAS/Core/CasGc.cpp` — prior-retired read (`:715-741` — delete), retired write
  (`:519-541` — replace with summary fill), `graduationDue` (`:1643-1658` — rewrite), pure
  ref-carry (`:1100-1119` + `carryParentRefs` `:1069` — carry parent summary; condition from
  summary), `hasInFlightRetired` (the `retired_refs` loop near `:1724`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp`, `gtest_cas_gc_round_defer.cpp`,
  `gtest_cas_gc_formats.cpp`

**Interfaces:**
- Consumes: Task 3 signature; `CasFoldSeal`/`encodeFoldSeal`/`decodeFoldSeal`.
- Produces:

```cpp
/// $CAS/Core/CasGenerationSeal.h
struct CondemnedSummary
{
    uint64_t condemned_total = 0;
    uint64_t pending_total = 0;
    uint64_t oldest_nonpending_condemn_round = UINT64_MAX;   /// UINT64_MAX = none
    bool operator==(const CondemnedSummary &) const = default;
};
struct CasFoldSeal { /* existing fields... */ std::map<uint64_t, CondemnedSummary> condemned_summary; };
```

```proto
// cas_format.proto — append INSIDE FoldSealProto: 
//   repeated CondemnedSummaryProto condemned_summary = 7;  // sorted by shard; TOTAL over gc_shards
message CondemnedSummaryProto {
  uint64 shard = 1;
  uint64 condemned_total = 2;
  uint64 pending_total = 3;
  uint64 oldest_nonpending_condemn_round = 4;   // UINT64_MAX = none
}
```

**Behavioral requirements (spec §2.2):**
1. Every newly written seal's `condemned_summary` is TOTAL over `0..gc_shards-1` — folding shards
   compute it from the rows they wrote (`still_retired`); pure-carry shards copy the parent seal's
   entry verbatim (which is the explicit zero entry, since pure carry requires nothing-to-settle).
2. `graduationDue(state, current_round)`: `snap_generation == 0` → `false`; otherwise read the
   adopted seal (`readFoldSeal(state.snap_generation, state.snap_attempt)`); missing / undecodable /
   summary map not total over `gc_shards` → `true` (fail-closed, forces the fold); else
   `∃ shard: pending_total > 0 || oldest_nonpending_condemn_round < current_round`. Zero backend
   GETs beyond the seal read (which `changedShardCount` already performs — share the read if the
   call sites allow, else keep one `readFoldSeal`).
3. Pure ref-carry condition becomes `!folded_any && summary_of(shard).condemned_total == 0`
   (replaces `prior_retired[0].empty()` at `:1104`; the sharded path analog too).
4. The round no longer reads retired lists (`:715-741` deleted) nor writes retired objects
   (`:519-541`): the summary is computed alongside the run emission in the same loop that today
   builds `RetiredSet`; `next.retired_refs` is set to `{}` (the field itself dies in Task 7).
5. `hasInFlightRetired` = summary check (`∃ shard: condemned_total > 0`) via the adopted seal.

- [ ] **Step 1: Write failing tests.** In `gtest_cas_gc_formats.cpp`: seal codec round-trip with a
non-empty `condemned_summary` (2 shards, one zero entry) — assert `decodeFoldSeal(encodeFoldSeal(s)) == s`.
In `gtest_cas_gc_round_defer.cpp`: (a) `graduationDue` returns `true` when the adopted seal object
is deleted from the fake backend (fail-closed); (b) returns `false` on a total all-zero summary;
(c) returns `true` when a pending entry exists. In `gtest_cas_gc_round.cpp`: after a round that
condemns one blob, the adopted seal's summary shows `condemned_total == 1, pending_total == 0`; a
carry round preserves the summary entry verbatim. Use the fixtures those files already have (they
drive `Gc` rounds against the in-memory backend).

- [ ] **Step 2: Build — verify RED:** `ninja -C build unit_tests_dbms > build/build_task4_red.log 2>&1`
(compile errors on `condemned_summary`, then assertion failures).

- [ ] **Step 3: Implement** per the behavioral requirements. Key anchors: summary fill goes where
`:519-541` builds `RetiredSet set` today (same loop over `folded.retired_merge[shard]` — count
`still_retired`, `delete_pending`, min non-pending round); `carryParentRefs` gains
`result.fold_seal.condemned_summary[shard] = parent_seal.condemned_summary.at(shard)` (throw
`CORRUPTED_DATA` if the parent lacks the entry — totality); the proto codec follows the existing
`FoldShardCoverageProto` encode/decode pattern in `CasGenerationSeal.cpp`.

- [ ] **Step 4: Build + run — PASS:**

```bash
ninja -C build unit_tests_dbms > build/build_task4_green.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGcRound*:CasGcFormats*:CasGcRoundDefer*:CasGcAttempt*:CasGcResume*' > build/test_task4.log 2>&1
```
Expected: all PASS (adapt fixture call sites mechanically where they referenced retired objects).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/ src/Disks/tests/
git commit -m "cas: seal condemned_summary + zero-I/O graduationDue + round wiring off retired objects (retired-in-snapshot T4)"
```

---

### Task 5: Consumers — `previewDeletes` contract, `fsck`, `ca-inspect` {#task-5}

**Files:**
- Modify: `$CAS/Core/CasGc.h:149-165` (`PreviewEntry`), `$CAS/Core/CasGc.cpp:2055-2096`
  (`previewDeletes`)
- Modify: `$CAS/Core/CasFsck.cpp:270-300` (the `retired_refs` loop at `:277`)
- Modify: `$CAS/Core/CasInspect.cpp:235-255` (the `retired_refs` dump at `:239`)
- Test: `src/Disks/tests/gtest_cas_gc_round.cpp` (preview), `src/Disks/tests/gtest_cas_gc_leak.cpp`
  (fsck fixture compiles/passes)

**Interfaces:**
- Consumes: Tasks 3-4 (`openSourceEdgeRun`, `decodeCondemnedRow`, seal summaries).
- Produces (spec §5 dryrun contract):

```cpp
struct PreviewEntry
{
    ObjectKind kind = ObjectKind::Blob;
    UInt128 hash{};
    String key;
    uint64_t size = 0;
    String reason;            /// "unreachable" | "delete_pending" | "awaiting_graduation"
    Token token;              /// stored condemn-time token (empty for "unreachable")
    uint64_t condemn_round = 0;
};
```

**Behavioral requirements:**
1. `previewDeletes` streams the adopted seal's runs per shard via `openSourceEdgeRun`; for every
   `kCondemned` row emit a `PreviewEntry` with the STORED token/type/round and
   `reason = delete_pending ? "delete_pending" : "awaiting_graduation"` — NO HEAD for these; the
   existing `zeroInDegree` + HEAD path stays for fresh zero-markers (`reason = "unreachable"`).
   Output is a superset of today's.
2. `fsck` collects condemned rows in the SAME streaming pass it already does over the runs for
   reachability (replace the `retired_refs` loop; the checks it ran on `RetiredSet` entries run on
   decoded `CondemnedRow`s instead).
3. `ca-inspect` prints per-shard `condemned_summary` from the seal instead of the `retired_refs`
   map (field names in the JSON: `condemned_total`, `pending_total`,
   `oldest_nonpending_condemn_round`, keyed by shard).

- [ ] **Step 1: Write failing tests** (in `gtest_cas_gc_round.cpp`, same fixture style): drive a
round that condemns one present blob, then `previewDeletes` must contain exactly one entry with
`reason == "awaiting_graduation"` and the condemn-time token; drive graduation (next round), then
one entry `reason == "delete_pending"`; after the redelete round, zero entries.

- [ ] **Step 2: RED:** `ninja -C build unit_tests_dbms > build/build_task5_red.log 2>&1`.

- [ ] **Step 3: Implement** the three consumers per requirements (mechanical; `previewDeletes`
keeps its "no writes ever" contract — assert no put/delete calls in the fake backend during the
preview test).

- [ ] **Step 4: GREEN:**

```bash
ninja -C build unit_tests_dbms > build/build_task5_green.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_task5.log 2>&1
```

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/ src/Disks/tests/
git commit -m "cas: dryrun/fsck/inspect read condemned state from seal-named runs (retired-in-snapshot T5)"
```

---

### Task 6: Rebuild — reorder (blob LIST before run flush) + condemned rows in rebuilt runs {#task-6}

**Files:**
- Modify: `$CAS/Core/CasGc.cpp:1900-2035` (the rebuild: journal traversal, `flush_shard`,
  pipeline-blindness LIST, seal + CAS)
- Test: `src/Disks/tests/gtest_cas_gc_resume.cpp` (or wherever the existing rebuild/convergence
  test lives — `grep -l "rebuild" src/Disks/tests/gtest_cas_*.cpp` and extend THAT file)

**Interfaces:**
- Consumes: Tasks 3-4 (row emission, summaries).
- Produces: rebuild emits `kCondemned` rows + total summaries; no `RetiredSet` minting.

**Behavioral requirements (spec §5, REORDERING REQUIRED):** new order = traverse journals
(accumulate per-shard deltas + `edge_bearing`) → LIST physical blobs, HEAD zero-edge ones,
mint their `CondemnedRow{token, size, condemn_round = minted round}` — grouped per shard →
`flush_shard` (folds deltas AND appends the orphan condemned rows into the shard's run) → seal
(with total `condemned_summary`) → `gc/state` CAS. The `zero_condemned` retired-set minting block
(`:2006-2025`) is deleted. Graduation pacing for these minted entries is unchanged (they graduate
once `condemn_round < current_round`, i.e. the next round).

- [ ] **Step 1: Failing test:** in the rebuild/convergence gtest: create a pool state with one
physically-present blob that has zero edges (orphan), run the rebuild, assert the adopted seal's
runs contain a `kCondemned` row for it (decode via `openSourceEdgeRun` + `decodeCondemnedRow`) and
the summary counts it; assert a subsequent regular round graduates and then redeletes it (the
pipeline-blindness repair works end-to-end).

- [ ] **Step 2: RED** → **Step 3: implement the reorder** → **Step 4: GREEN:**

```bash
ninja -C build unit_tests_dbms > build/build_task6.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasGc*' > build/test_task6.log 2>&1
```

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp src/Disks/tests/
git commit -m "cas: rebuild reorder — orphan kCondemned rows enter the rebuilt runs before sealing (retired-in-snapshot T6)"
```

---

### Task 7: Delete the retired artifact family + docs {#task-7}

**Files:**
- Modify: `$CAS/Core/CasGcFormats.h` (delete `RetiredEntry`, `RetiredSet`, `encodeRetiredSet`,
  `decodeRetiredSet`; delete `GcState::retired_refs`), `$CAS/Core/CasGcFormats.cpp` (codecs; the
  `retired_refs` proto encode/decode at `:87-136`)
- Modify: `$CAS/Core/Proto/cas_format.proto` (`RetiredEntryProto`/`RetiredSetProto` deleted with
  `reserved` markers; `GcStateProto.retired_refs` field number `reserved`)
- Modify: `$CAS/Core/CasLayout.h:194-198` (delete `retiredKey`), `$CAS/Core/CasFormat.h`
  (`FormatId::RetiredSet` → comment "retired 2026-07-10, magic CART freed — never reuse")
- Modify: `$CAS/Core/CasGc.cpp` (retention prune of retired objects — delete that branch)
- Note: `RetiredMergeResult` KEEPS using a struct with `{hash, token, size, condemn_round,
  delete_pending}` — rename the element type to `CondemnedRow` usage or keep a slim in-memory
  `RetiredEntry` WITHOUT `kind` (choose the smaller diff; update `ReplacedEntry` accordingly).
- Docs: `docs/superpowers/cas/04-gc-protocol.md` (retired-list sections → in-run rows),
  `05-formats-and-backend.md` (`CART` removed — same style as `CATR`; run row table; seal summary),
  `07-s3-budget.md` (drop retired GET/PUT rows; `graduationDue` cost row), `ROADMAP.md` (item DONE).
- Test: whole battery.

- [ ] **Step 1: Delete + fix compile errors mechanically** (the compiler is the checklist — every
remaining reference is a consumer missed by Tasks 4-6; investigate each, do not stub).
- [ ] **Step 2: Full battery GREEN:**

```bash
ninja -C build unit_tests_dbms > build/build_task7.log 2>&1
build/src/unit_tests_dbms --gtest_filter='*Cas*:*Ca*' > build/test_task7.log 2>&1
```
Expected: same pass set as before this plan (784+/792 baseline — the 8 pre-existing order-pollution
fails are known; ZERO new fails).
- [ ] **Step 3: Update the four docs** (each edit anchored to the sections named above; keep the
`{#anchor}` header convention).
- [ ] **Step 4: Commit**

```bash
git add -A src/Disks docs/superpowers/cas
git commit -m "cas: delete RetiredSet/CART/retiredKey/retired_refs — one artifact family (retired-in-snapshot T7)"
```

---

### Task 8: Validation checkpoint — server build, lane point-runs, soak {#task-8}

**Files:** none (validation only).

- [ ] **Step 1: Server build:** `ninja -C build clickhouse > build/build_task8.log 2>&1` (subagent
analyzes the log). `ln -sf $(pwd)/build/programs/clickhouse ci/tmp/clickhouse` if the symlink is
stale.
- [ ] **Step 2: CA-s3 lane point-run** (one praktika job; verdicts in `ci/tmp/test_result.txt`):

```bash
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed s3 storage, parallel)" \
  --test "04286_content_addressed_remote_data_paths 05008_ca_gc_snap_prune 05009_content_addressed_event_log" \
  > build/test_task8_lane.log 2>&1
```
Expected: all three `[ OK ]` — **`05008` UNMODIFIED is the settlement-semantics oracle.**
- [ ] **Step 3: Phase-1 soak:** `utils/ca-soak/scripts/run_phase1.sh` (2-replica; binary
bind-mounted from `build/programs/clickhouse`). Expected: `PHASE1 OK`, every checkpoint
`fsck dangling=0 / unreachable=0`. This runs ~1h — run in background, verify the final line.
- [ ] **Step 3b: S30/S33-class scenarios** (spec §7): from `utils/ca-soak/scenarios/`, run the S30
(resurrect-churn / recurring-hash) and S33 (concurrent-leader) cards against the new binary —
both must PASS (S33 is the concurrent-leader reclaim-leak regression guard; S30 pins the
resurrect-supersede path this refactor touches via `ReplacedEntry`).
- [ ] **Step 4: Report** — summarize gate results (Task 1 TLA green+3 red, unit battery, lane 3/3,
soak) in the session worklog and mark the ROADMAP entry DONE. Commit any log/doc deltas.

---

## Plan self-review notes {#self-review}

- Spec coverage: §2.1 rows/invariants → T2-T3; §2.2 summary/graduationDue/carry → T4; §2.3 +
  deletions → T7; §3 merge → T3; §4 adoption (no code change — guarded by T1 sabotage (b) and the
  T3 byte-determinism regression tests); §5 consumers → T5 (dryrun/fsck/inspect) + T6 (rebuild) +
  T4 (`hasInFlightRetired`); §6 → T1; §7 → T2-T8 per task + T8 checkpoint; §8 phases map T1..T8.
- The only intentionally-deferred detail: exact `RunFileReader` constructor shape in
  `openSourceEdgeRun` (T2 Step 3 note) — adapt mechanically at implementation time.
- 05008 is never edited; if it goes red at T8, the settlement semantics drifted — treat as a
  blocking defect, bisect against T3/T4.

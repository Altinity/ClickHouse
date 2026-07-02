# CAS GC Ack-Floor Fence Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the O(universe) per-round fence/recheck GC phases with a causal acknowledgement
floor (merged per-server heartbeat + current sorted retired list + one-pass three-cursor merge), per
`docs/superpowers/specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md`.

**Architecture:** Phase 0 is a TLA+ gate (new focused module `CaGcAckFloorCore`) — no production code
before it is GREEN. Then bottom-up: heartbeat body/codec → keeper merge (watermark ∪ mount) → writer
beat (view load + drain + ack) → current retired list → GC heartbeat gate (`min_ack`, fence-out) →
three-cursor merge → round rewiring (drop `fence`/`recheck`) → schema cleanup → observability.

**Tech Stack:** C++ (ClickHouse tree, Allman braces), gtest (`unit_tests_dbms`, `InMemoryBackend`,
injected clocks — never real sleeps for correctness), TLA+/TLC (`tmp/tla2tools.jar`), strict-JSON CAS
metadata codecs.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md`. TLA+ gate (Tasks 1–2)
  MUST be GREEN before any production task starts.
- Graduation rule verbatim: an entry graduates only when `condemn_round < min_ack`; `min_ack` is the
  minimum `observed_gc_round` over heartbeats that are **live or expired-but-not-fenced**; terminated
  and fenced heartbeats are excluded.
- Drain invariant: `observed_gc_round` is advertised only after every in-flight `mutateShard` that
  started under the older view has received its CAS response.
- Open ordering: claim heartbeat → load `gc/state` + retired list → stamp ack → only then enable
  mutations.
- Publish order: retired-list objects (and snapshot runs) durable **before** the `gc/state` CAS that
  publishes the round.
- Pre-release: NO compatibility shims ([[feedback_ca_no_compat_scaffolding_predev]]). Schema versions
  bump; old versions are rejected fail-closed.
- Fail-closed decode everywhere (`CORRUPTED_DATA` on malformed, `NOT_IMPLEMENTED` on future version).
- Never use sleep in C++ to fix race conditions. Tests drive fake clocks / explicit ordering.
- Build: `ninja` in an existing `build*` dir, output redirected to a log file, log analyzed by a
  subagent. Unit tests: `./build/src/unit_tests_dbms --gtest_filter=...` redirected to a unique log.
- Say "exception" (not "crash") for logical errors; ASan not ASAN; function names as `f` not `f()`.

## File map

| File | Role in this plan |
|---|---|
| `docs/superpowers/models/CaGcAckFloorCore.tla` (new) | Phase-0 model |
| `docs/superpowers/models/CaGcAckFloorCore_*.cfg` (new, 11) | positive + 7 sabotage + 3 witness stages |
| `docs/superpowers/models/run_ackfloor.sh` (new) | TLC runner (mirrors `run_mount.sh`) |
| `Core/CasServerRoot.{h,cpp}` | `MountLease` body extension; `MountLeaseKeeper` payload callbacks; fence-out helper |
| `Core/CasWatermark.{h,cpp}` (deleted) | merged into the heartbeat |
| `Core/CasOrphanManifestSweep.cpp` | reads floors from the heartbeat body |
| `Core/CasStore.{h,cpp}` | beat protocol, drain lock, open ordering, `mayMutate` boottime clock |
| `Core/CasRetireView.{h,cpp}` | loads the current retired list via `gc/state` refs |
| `Core/CasGcFormats.{h,cpp}` | `RetiredEntry.condemn_round`; `GcState` v4 (drop `fence_version`, add retired refs) |
| `Core/CasBlobInDegree.{h,cpp}` | three-cursor merge |
| `Core/CasGc.{h,cpp}` | heartbeat gate; one-pass round; delete `fence`/`recheck` |
| `src/Common/ProfileEvents.cpp` | new counters |
| `src/Disks/tests/gtest_cas_{mount,heartbeat,retire_view,blob_indegree,gc_fence_recheck,gc_round,gc_resume,orphan_manifest_sweep}.cpp` | tests |

(`Core/` = `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/`.)

---

### Task 1: TLA+ model `CaGcAckFloorCore` — positive stage

**Files:**
- Create: `docs/superpowers/models/CaGcAckFloorCore.tla`
- Create: `docs/superpowers/models/CaGcAckFloorCore_stage1.cfg`
- Create: `docs/superpowers/models/run_ackfloor.sh`

**Interfaces:**
- Produces: the module + runner used by Task 2 (`./run_ackfloor.sh <cfg-basename>`); sabotage
  constants named exactly as in Task 2.

- [ ] **Step 1: Write the module**

`docs/superpowers/models/CaGcAckFloorCore.tla`:

```tla
-------------------- MODULE CaGcAckFloorCore --------------------
(* Ack-floor GC fence core — spec 2026-07-02-cas-gc-ack-floor-fence-redesign.md.
   Focused model (companion regime to CaCasMountCore): writers advertise observed_gc_round through
   heartbeats; GC graduates a retired entry only when condemn_round < min_ack computed over
   heartbeats that are live OR expired-but-not-fenced; a fenced heartbeat can never renew or land a
   commit again. Commits are two-step (prepare = gate evaluation, land = CAS response) so the
   in-flight window is a real interleaving. The pass is three steps (begin/fold/complete) so a
   commit landing between the fold cut and the deletes is a real interleaving.
   Ref removal is modeled as immediate (no fold lag): removal lag only delays condemnation
   (safe side); the hazard under test is the racing ADD.
   Each Sabotage* flag breaks exactly one load-bearing rule and MUST yield a counterexample. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    Writers, Blobs, MaxRound, MaxTok,
    SabotageIgnoreAckFloor,      \* graduate ignoring the floor entirely
    SabotageAckWithoutRead,      \* ack advances without installing the view
    SabotageAckBeforeDrain,      \* ack advances while an old-view commit is in flight
    SabotageSleeperRearm,        \* floor excludes expired-UNFENCED heartbeats (assumes dead without fence-out)
    SabotageSkipChangedShard,    \* the fold cut leaves one landed ref unconsumed
    SabotageAdoptRetiredToken,   \* the gate references a visibly-retired token instead of recreating
    SabotageOpenWriteBeforeLoad  \* a fresh mount starts with an unloaded (round-0) view

None == "none"
Toks == 1..MaxTok
Rounds == 0..MaxRound

VARIABLES
    round,       \* published gc round (gc/state)
    present,     \* [Blobs -> BOOLEAN]
    tok,         \* [Blobs -> Toks] current incarnation token
    nextTok,     \* [Blobs -> Toks] next fresh token to mint
    deadTok,     \* [Blobs -> SUBSET Toks] deleted incarnations (INV_NO_RETURN oracle)
    retired,     \* current retired list: SUBSET [b: Blobs, t: Toks, r: Rounds]
    landed,      \* landed-but-unfolded refs: SUBSET [b: Blobs, t: Toks, w: Writers]
    folded,      \* folded refs (the in-degree source): SUBSET [b: Blobs, t: Toks, w: Writers]
    wStatus,     \* [Writers -> {"unmounted","live","expired","fenced","terminated"}]
    wView,       \* [Writers -> Rounds] installed view (retired-list version loaded)
    wAck,        \* [Writers -> Rounds] advertised observed_gc_round
    wPending,    \* [Writers -> {None} \cup [b: Blobs, t: Toks]] in-flight commit
    gcPhase,     \* {"idle","running","folded"}
    minAckL,     \* the floor latched by GBegin
    sparedEver, recreatedEver, deletedEver   \* witness history flags

vars == << round, present, tok, nextTok, deadTok, retired, landed, folded,
           wStatus, wView, wAck, wPending, gcPhase, minAckL,
           sparedEver, recreatedEver, deletedEver >>

Indeg(b) == Cardinality({ rf \in folded : rf.b = b })
FloorSet == { w \in Writers : wStatus[w] \in
                (IF SabotageSleeperRearm THEN {"live"} ELSE {"live", "expired"}) }
MinAck == IF FloorSet = {} THEN MaxRound + 1
          ELSE CHOOSE m \in Rounds : (\E w \in FloorSet : wAck[w] = m)
                                     /\ (\A w \in FloorSet : wAck[w] >= m)

Init ==
    /\ round = 0
    /\ present = [b \in Blobs |-> TRUE] /\ tok = [b \in Blobs |-> 1]
    /\ nextTok = [b \in Blobs |-> 2] /\ deadTok = [b \in Blobs |-> {}]
    /\ retired = {} /\ landed = {} /\ folded = {}
    /\ wStatus = [w \in Writers |-> "unmounted"]
    /\ wView = [w \in Writers |-> 0] /\ wAck = [w \in Writers |-> 0]
    /\ wPending = [w \in Writers |-> None]
    /\ gcPhase = "idle" /\ minAckL = 0
    /\ sparedEver = FALSE /\ recreatedEver = FALSE /\ deletedEver = FALSE

(* ---- writer actions ---- *)

WOpen(w) ==
    /\ wStatus[w] = "unmounted"
    /\ wStatus' = [wStatus EXCEPT ![w] = "live"]
    /\ wView' = [wView EXCEPT ![w] = IF SabotageOpenWriteBeforeLoad THEN 0 ELSE round]
    /\ wAck' = [wAck EXCEPT ![w] = IF SabotageOpenWriteBeforeLoad THEN 0 ELSE round]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WBeat(w) ==
    /\ wStatus[w] = "live"
    /\ (SabotageAckBeforeDrain \/ wPending[w] = None)          \* drain-before-advertise
    /\ wView' = IF SabotageAckWithoutRead THEN wView ELSE [wView EXCEPT ![w] = round]
    /\ wAck' = [wAck EXCEPT ![w] = round]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wStatus, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* Gate evaluation. `visible` = the entry for the blob's CURRENT token is in the writer's loaded
   list version. Visible + honest => recreate (fresh incarnation referenced, never the listed one).
   Absent blob => re-upload from source (same recreate shape). *)
WPrepare(w, b) ==
    /\ wStatus[w] = "live" /\ wPending[w] = None /\ nextTok[b] <= MaxTok
    /\ LET visible == \E e \in retired : e.b = b /\ e.t = tok[b] /\ e.r <= wView[w]
           mustRecreate == (~present[b]) \/ (visible /\ ~SabotageAdoptRetiredToken)
       IN IF mustRecreate
          THEN /\ present' = [present EXCEPT ![b] = TRUE]
               /\ tok' = [tok EXCEPT ![b] = nextTok[b]]
               /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
               /\ wPending' = [wPending EXCEPT ![w] = [b |-> b, t |-> nextTok[b]]]
               /\ recreatedEver' = TRUE
          ELSE /\ present[b]                                    \* adopt current incarnation
               /\ wPending' = [wPending EXCEPT ![w] = [b |-> b, t |-> tok[b]]]
               /\ UNCHANGED << present, tok, nextTok, recreatedEver >>
    /\ UNCHANGED << round, deadTok, retired, landed, folded, wStatus, wView, wAck,
                    gcPhase, minAckL, sparedEver, deletedEver >>

WLand(w) ==
    /\ wStatus[w] = "live" /\ wPending[w] # None
    /\ landed' = landed \cup { [b |-> wPending[w].b, t |-> wPending[w].t, w |-> w] }
    /\ wPending' = [wPending EXCEPT ![w] = None]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, folded, wStatus,
                    wView, wAck, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WDropRef(rf) ==
    /\ rf \in folded
    /\ folded' = folded \ {rf}
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, wStatus,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WExpire(w) ==
    /\ wStatus[w] = "live"
    /\ wStatus' = [wStatus EXCEPT ![w] = "expired"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* A live-again sleeper: an EXPIRED (never fenced) heartbeat renews successfully. Honest and safe —
   the honest floor still counts expired heartbeats. Becomes lethal only when the floor excludes
   expired-unfenced writers (SabotageSleeperRearm). The pending commit survives the nap. *)
WSleeperRenew(w) ==
    /\ wStatus[w] = "expired"
    /\ wStatus' = [wStatus EXCEPT ![w] = "live"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

WTerminate(w) ==
    /\ wStatus[w] = "live" /\ wPending[w] = None                \* graceful stop drains first
    /\ wStatus' = [wStatus EXCEPT ![w] = "terminated"]
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

(* ---- GC actions ---- *)

GFenceOut(w) ==
    /\ wStatus[w] = "expired"
    /\ wStatus' = [wStatus EXCEPT ![w] = "fenced"]              \* token-guarded fence-out: renew dead forever
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wView, wAck, wPending, gcPhase, minAckL, sparedEver, recreatedEver, deletedEver >>

GBegin ==
    /\ gcPhase = "idle" /\ round < MaxRound
    /\ minAckL' = MinAck
    /\ gcPhase' = "running"
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, landed, folded,
                    wStatus, wView, wAck, wPending, sparedEver, recreatedEver, deletedEver >>

GFold ==
    /\ gcPhase = "running"
    /\ IF SabotageSkipChangedShard /\ landed # {}
       THEN \E skip \in landed : /\ folded' = folded \cup (landed \ {skip})
                                 /\ landed' = {skip}
       ELSE /\ folded' = folded \cup landed
            /\ landed' = {}
    /\ gcPhase' = "folded"
    /\ UNCHANGED << round, present, tok, nextTok, deadTok, retired, wStatus, wView,
                    wAck, wPending, minAckL, sparedEver, recreatedEver, deletedEver >>

(* Merge + graduate + condemn + publish, over the folded cut. Refs landing after GFold sit in
   `landed` and are invisible here — exactly the implementation's cut. Exact-token delete: a
   graduated entry whose token no longer matches (recreated meanwhile) is dropped without delete. *)
GComplete ==
    /\ gcPhase = "folded"
    /\ LET grads == { e \in retired : Indeg(e.b) = 0
                                      /\ (SabotageIgnoreAckFloor \/ e.r < minAckL) }
           spares == { e \in retired : Indeg(e.b) > 0 }
           kills == { e \in grads : tok[e.b] = e.t }            \* exact-token delete lands
           newly == { [b |-> b, t |-> tok[b], r |-> round + 1] :
                        b \in { bb \in Blobs : present[bb] /\ Indeg(bb) = 0
                                /\ ~(\E e \in retired : e.b = bb) } }
       IN /\ present' = [b \in Blobs |-> IF \E e \in kills : e.b = b THEN FALSE ELSE present[b]]
          /\ deadTok' = [b \in Blobs |-> IF \E e \in kills : e.b = b
                                         THEN deadTok[b] \cup {tok[b]} ELSE deadTok[b]]
          /\ retired' = (retired \ (grads \cup spares)) \cup newly
          /\ sparedEver' = sparedEver \/ spares # {}
          /\ deletedEver' = deletedEver \/ kills # {}
    /\ round' = round + 1
    /\ gcPhase' = "idle"
    /\ UNCHANGED << tok, nextTok, landed, folded, wStatus, wView, wAck, wPending, minAckL,
                    recreatedEver >>

Next ==
    \/ \E w \in Writers : WOpen(w) \/ WBeat(w) \/ WLand(w) \/ WExpire(w)
                          \/ WSleeperRenew(w) \/ WTerminate(w) \/ GFenceOut(w)
    \/ \E w \in Writers, b \in Blobs : WPrepare(w, b)
    \/ \E rf \in folded : WDropRef(rf)
    \/ GBegin \/ GFold \/ GComplete

Spec == Init /\ [][Next]_vars

(* ---- invariants ---- *)

TypeOK ==
    /\ round \in Rounds /\ gcPhase \in {"idle", "running", "folded"}
    /\ \A w \in Writers : wAck[w] \in Rounds /\ wView[w] \in Rounds
    /\ \A e \in retired : e.b \in Blobs /\ e.t \in Toks /\ e.r \in Rounds

(* No reference — landed or folded — may point at an absent blob or a dead incarnation. *)
INV_NO_DANGLE == \A rf \in (landed \cup folded) : present[rf.b]
INV_NO_RETURN == \A rf \in (landed \cup folded) : rf.t \notin deadTok[rf.b]
(* The honest ack never runs ahead of the installed view. *)
INV_ACK_LE_VIEW == \A w \in Writers : wAck[w] <= wView[w]

(* ---- witnesses (negated reachability; TLC "violation" = the state is reachable) ---- *)
W_DeleteHappens == ~deletedEver
W_SpareHappens == ~sparedEver
W_RecreateHappens == ~recreatedEver

=============================================================================
```

- [ ] **Step 2: Write the positive cfg and the runner**

`docs/superpowers/models/CaGcAckFloorCore_stage1.cfg`:

```
SPECIFICATION Spec
CONSTANTS
    Writers = {w1, w2}
    Blobs = {b1}
    MaxRound = 4
    MaxTok = 4
    SabotageIgnoreAckFloor = FALSE
    SabotageAckWithoutRead = FALSE
    SabotageAckBeforeDrain = FALSE
    SabotageSleeperRearm = FALSE
    SabotageSkipChangedShard = FALSE
    SabotageAdoptRetiredToken = FALSE
    SabotageOpenWriteBeforeLoad = FALSE
INVARIANTS
    TypeOK
    INV_NO_DANGLE
    INV_NO_RETURN
    INV_ACK_LE_VIEW
```

`docs/superpowers/models/run_ackfloor.sh` (copy `run_mount.sh`, change the module name; `chmod +x`):

```bash
#!/usr/bin/env bash
# Usage: ./run_ackfloor.sh <Cfg-basename-without-.cfg>
set -u
CFG="${1:?usage: run_ackfloor.sh <cfg-basename>}"
LOG="../../../tmp/tlc_${CFG}.log"
shift || true
java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp ../../../tmp/tla2tools.jar tlc2.TLC \
     -metadir ../../../tmp/tlc-meta -workers auto -config "${CFG}.cfg" "$@" \
     CaGcAckFloorCore.tla 2>&1 | tee "$LOG" | \
     grep -E "Model checking completed|Error:|violated|states generated|distinct states|Finished in"
RC=${PIPESTATUS[0]}
echo "exit=$RC log=$LOG"
exit "$RC"
```

Precondition: `tmp/tla2tools.jar` must exist (symlink
`~/Documents/TLAToolbox-1.7.4-linux.gtk.x86_64/toolbox/tla2tools.jar` if missing).

- [ ] **Step 3: Run the positive stage**

```bash
cd docs/superpowers/models && ./run_ackfloor.sh CaGcAckFloorCore_stage1
```

Expected: `Model checking completed. No error has been found.`, exit=0. If TLC reports a violation,
fix the MODEL (or, if the model faithfully exposes a real protocol hole, STOP and escalate — the
spec must change before any implementation).

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaGcAckFloorCore.tla \
        docs/superpowers/models/CaGcAckFloorCore_stage1.cfg \
        docs/superpowers/models/run_ackfloor.sh
git commit -m "TLA+: CaGcAckFloorCore ack-floor fence model, positive stage GREEN"
```

---

### Task 2: TLA+ sabotage + witness stages

**Files:**
- Create: `docs/superpowers/models/CaGcAckFloorCore_sab_{ignorefloor,ackwithoutread,ackbeforedrain,sleeperrearm,skipshard,adopttoken,openbeforeload}.cfg` (7)
- Create: `docs/superpowers/models/CaGcAckFloorCore_witness_{delete,spare,recreate}.cfg` (3)

**Interfaces:**
- Consumes: Task 1's module + runner.

- [ ] **Step 1: Write the 7 sabotage cfgs**

Each is `CaGcAckFloorCore_stage1.cfg` with exactly ONE flag flipped to TRUE and the invariant list
reduced to the one the sabotage must break:

| cfg | flag → TRUE | INVARIANTS kept |
|---|---|---|
| `..._sab_ignorefloor.cfg` | `SabotageIgnoreAckFloor` | `INV_NO_DANGLE` |
| `..._sab_ackwithoutread.cfg` | `SabotageAckWithoutRead` | `INV_NO_DANGLE` |
| `..._sab_ackbeforedrain.cfg` | `SabotageAckBeforeDrain` | `INV_NO_DANGLE` |
| `..._sab_sleeperrearm.cfg` | `SabotageSleeperRearm` | `INV_NO_DANGLE` |
| `..._sab_skipshard.cfg` | `SabotageSkipChangedShard` | `INV_NO_DANGLE` |
| `..._sab_adopttoken.cfg` | `SabotageAdoptRetiredToken` | `INV_NO_RETURN` |
| `..._sab_openbeforeload.cfg` | `SabotageOpenWriteBeforeLoad` | `INV_NO_DANGLE` |

(Note: `..._sab_ackwithoutread.cfg` must also drop `INV_ACK_LE_VIEW` — it trips trivially there;
the point is that the DANGLE becomes reachable.)

- [ ] **Step 2: Write the 3 witness cfgs**

Copies of stage1 with `INVARIANTS` replaced by exactly one of `W_DeleteHappens` /
`W_SpareHappens` / `W_RecreateHappens` (all sabotage flags FALSE). A TLC "Invariant violated"
here is SUCCESS (the lifecycle state is reachable).

- [ ] **Step 3: Run all stages; record the matrix**

```bash
cd docs/superpowers/models
for c in stage1 sab_ignorefloor sab_ackwithoutread sab_ackbeforedrain sab_sleeperrearm \
         sab_skipshard sab_adopttoken sab_openbeforeload witness_delete witness_spare \
         witness_recreate; do ./run_ackfloor.sh CaGcAckFloorCore_${c}; done
```

Expected: `stage1` clean (exit 0); every `sab_*` reports `Invariant ... violated` (counterexample
found — that is the PASS condition); every `witness_*` reports a violation (reachable). ANY sabotage
that completes with no error is a model bug or — worse — a protocol rule that is not load-bearing:
STOP and re-derive before proceeding.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/models/CaGcAckFloorCore_*.cfg
git commit -m "TLA+: CaGcAckFloorCore sabotage (7) + witness (3) stages verified"
```

---

### Task 3: heartbeat body — extend `MountLease`

**Files:**
- Modify: `Core/CasServerRoot.h` (struct `MountLease`, ~line 85)
- Modify: `Core/CasServerRoot.cpp` (`encodeMountLease` / `decodeMountLease`; `claimMount` writes)
- Modify: `Core/Proto/cas_format.proto` (the mount-lease message, if the codec is protobuf-backed —
  follow whatever encoding `encodeMountLease` actually uses; extend it field-for-field in the same
  style as the existing `expires_at_ms` field)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Produces: `MountLease` gains `uint64_t min_active = 0;`, `uint64_t observed_gc_round = 0;`,
  `bool gc_fenced = false;`. Round-trips through encode/decode. All Task 4–7 code reads/writes
  these exact names.
- `min_active` uses `UINT64_MAX` as the retired sentinel (same convention as `ServerWatermark`).

- [ ] **Step 1: Write the failing round-trip test** (append to `gtest_cas_mount.cpp`):

```cpp
TEST(CasMountLease, BodyCarriesFloorAckAndFence)
{
    MountLease m;
    m.server_uuid = UInt128(0xAB);
    m.writer_epoch = 7;
    m.hostname = "h";
    m.pid = 42;
    m.started_at_ms = 1000;
    m.seq = 3;
    m.expires_at_ms = 2000;
    m.min_active = 5;
    m.observed_gc_round = 9;
    m.gc_fenced = true;
    const MountLease d = decodeMountLease(encodeMountLease(m));
    EXPECT_EQ(d.min_active, 5u);
    EXPECT_EQ(d.observed_gc_round, 9u);
    EXPECT_TRUE(d.gc_fenced);
    EXPECT_EQ(d.writer_epoch, 7u);
}

TEST(CasMountLease, RetiredSentinelRoundTrips)
{
    MountLease m;
    m.min_active = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(decodeMountLease(encodeMountLease(m)).min_active,
              std::numeric_limits<uint64_t>::max());
}
```

- [ ] **Step 2: Run to verify it fails**

```bash
ninja -C build unit_tests_dbms > build/build_task3.log 2>&1   # expect compile FAIL: no member 'min_active'
```

- [ ] **Step 3: Implement**

Add to `struct MountLease` in `CasServerRoot.h` after `expires_at_ms`:

```cpp
    /// Merged heartbeat fields (ack-floor redesign): the per-server build-watermark floor and the
    /// GC-round acknowledgement ride the SAME object as the lease, so one beat renews all three.
    uint64_t min_active = 0;          /// oldest in-flight build_seq; UINT64_MAX = retired (farewell)
    uint64_t observed_gc_round = 0;   /// newest gc round whose retired list this server has loaded
    bool gc_fenced = false;           /// set ONLY by GC fence-out of an expired lease; terminal
```

Extend `encodeMountLease` / `decodeMountLease` in `CasServerRoot.cpp` with the three fields in the
same style as the adjacent `expires_at_ms` handling (fail-closed decode; if the codec is the
strict-JSON style, `min_active` must accept the `"retired"` string sentinel exactly as
`CasWatermark.cpp` does — copy that branch). `claimMount`'s freshly-written bodies leave the new
fields at their defaults (a fresh claim has loaded nothing yet; Task 5 stamps the ack immediately
after the view load).

- [ ] **Step 4: Run tests**

```bash
./build/src/unit_tests_dbms --gtest_filter='CasMount*' > build/test_task3.log 2>&1
```

Expected: all PASS (including the 19 pre-existing mount tests — decode must not reject old-field-only
bodies *within this commit's own encode*; there is no cross-version data, pre-release).

- [ ] **Step 5: Commit** — `git commit -m "CAS heartbeat: MountLease carries min_active / observed_gc_round / gc_fenced"`

---

### Task 4: merge `WatermarkKeeper` into `MountLeaseKeeper`; delete the watermark object

**Files:**
- Modify: `Core/CasServerRoot.h/.cpp` (`MountLeaseKeeper`: payload callbacks)
- Modify: `Core/CasStore.cpp` (`Store::open` step 5 removal; keeper wiring), `Core/CasStore.h`
  (drop `std::unique_ptr<WatermarkKeeper> watermark;`, drop `watermark_renew_period` uses)
- Modify: `Core/CasOrphanManifestSweep.cpp` (`watermarkForNamespace` → read the mount key)
- Delete: `Core/CasWatermark.h`, `Core/CasWatermark.cpp`; remove `serverRootWatermarkKey` from
  `Core/CasLayout.h`
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (rewrite against the merged keeper),
  `src/Disks/tests/gtest_cas_orphan_manifest_sweep.cpp` (fixture writes mount bodies, not watermarks)

**Interfaces:**
- Consumes: Task 3 fields.
- Produces: `MountLeaseKeeper` constructor gains
  `std::function<uint64_t()> min_active_fn_, std::function<uint64_t()> observed_round_fn_`;
  `prepareRenew` fetches both OFF the state lock; `encodeBody` writes them + a fresh
  `expires_at_ms`. `terminate` (graceful stop) stamps `expires_at_ms = now` AND
  `min_active = UINT64_MAX` (the watermark farewell folds into the mount release).
  The orphan sweep reads `decodeMountLease(get(mountKey(srid)))` and consumes
  `{writer_epoch, min_active, seq}` exactly where it consumed `ServerWatermark::{epoch, min_active,
  seq}` (the writable-path watermark epoch was already `writer_epoch` — `CasStore.cpp:157` "THE
  BRIDGE" — so sweep semantics are unchanged).

- [ ] **Step 1: Write the failing tests** — rewrite `gtest_cas_heartbeat.cpp`: port each existing
  `CasWatermarkKeeper.*` case to `MountLeaseKeeper` with the two callbacks, asserting via
  `decodeMountLease` that (a) `start` anchors a body with the current `min_active` and
  `observed_gc_round`, (b) `renewOnce` re-reads both callbacks and bumps `seq`, (c) `stop` stamps
  `expires_at_ms <= now` and `min_active == UINT64_MAX`, (d) a foreign touch makes `renewOnce`
  throw (unchanged base behavior). Keep the existing test names with a `CasHeartbeat` prefix.

- [ ] **Step 2: Run to verify failure** (compile error: constructor arity).

- [ ] **Step 3: Implement** — constructor + `prepareRenew` carrying BOTH values (extend
  `SingleWriterSlot::RenewPayload` with a second field `uint64_t value2 = 0;`), `encodeBody`
  filling the Task 3 fields, `terminate` stamping the farewell sentinel. In `Store::open`: delete
  step 5 entirely; pass `[raw] { return raw->minActive(); }` and (until Task 5)
  `[] { return uint64_t{0}; }` as the callbacks. Point `watermarkForNamespace` at
  `mountKey`/`decodeMountLease`. Delete the watermark files, the layout key, and the
  `watermark_renew_period` config plumbing (the merged keeper renews at `mount_renew_period`).

- [ ] **Step 4: Run** `--gtest_filter='CasHeartbeat*:CasOrphan*:CasMount*:CasStore*'` → PASS;
  full `ninja clickhouse` compiles (watermark includes removed everywhere — grep
  `CasWatermark.h` for stragglers).

- [ ] **Step 5: Commit** — `"CAS heartbeat: merge WatermarkKeeper into MountLeaseKeeper (one per-server slot)"`

---

### Task 5: writer beat — view load, drain, ack, open ordering

**Files:**
- Modify: `Core/CasStore.h/.cpp` (`view_gate` lock; `refreshViewForBeat`; `mutateShard` shared
  lock; open ordering; keeper callback wiring), `Core/CasRetireView.h/.cpp` (expose
  `installSnapshot` used by the beat; keep `refresh` for open)
- Test: `src/Disks/tests/gtest_cas_store.cpp`

**Interfaces:**
- Consumes: Task 4 keeper callbacks.
- Produces:
  - `Store` member `std::shared_mutex view_gate;`
  - `uint64_t Store::refreshViewForBeat();` — (1) `GET gc/state` (absent → returns current view
    round unchanged); (2) if `round` advanced: GET the retired-list objects (Task 6 wires the real
    keys; until then delegate to `retire_view.refresh()`); (3) `std::unique_lock view_gate` —
    the DRAIN — swap the view; (4) return the new round. On ANY read failure: log + return the
    UNCHANGED current round (ack must not advance — lease renewal still proceeds; fail-open for
    availability, fail-closed for the ack).
  - `mutateShard` takes `std::shared_lock view_gate` for its ENTIRE body (gate evaluation through
    CAS response) — this is the drain's other half.
  - Keeper `observed_round_fn` = `[raw] { return raw->refreshViewForBeat(); }` (runs in the keeper
    thread, off `SingleWriterSlot::state_mutex` — `prepareRenew` is documented as the off-lock
    hook precisely for this).
  - Open ordering in `Store::open`: `claimMountAwaitingExpiry` → construct keeper →
    `retire_view.refresh()` (the initial load) → `keeper.start()` (anchors the body WITH the
    loaded round via the callback) → `armMountFence` → `startBackground`. Mutations are impossible
    before `open` returns, so this sequence IS the open-ordering invariant.

- [ ] **Step 1: Write the failing tests** (`gtest_cas_store.cpp`):

```cpp
TEST(CasStoreBeat, AckAdvancesOnlyAfterViewLoad)
{
    /// gc/state at round 3 with an empty retired list; beat returns 3 and the mount body
    /// (after renewOnce) carries observed_gc_round = 3.
}
TEST(CasStoreBeat, GcStateReadFailureLeavesAckUnchanged)
{
    /// FaultInjectionBackend (cas_test_helpers.h pattern) fails GET(gc/state): beat returns the
    /// old round; renewOnce still succeeds (lease extended) with the OLD observed_gc_round.
}
TEST(CasStoreBeat, DrainBlocksAckWhileMutationInFlight)
{
    /// Thread A enters mutateShard whose mutate closure parks on a std::promise (test-controlled).
    /// Thread B calls refreshViewForBeat: it must NOT return before A's closure is released
    /// (assert via an atomic sequence counter: order == [A-enter, A-release, B-return]).
    /// No sleeps: pure promise/future ordering.
}
```

(The first two are direct: build a Store over `InMemoryBackend` with `background_watermark=false`,
drive `renewOnce` manually, decode the mount body from the backend.)

- [ ] **Step 2: Run to verify failure** (no `refreshViewForBeat` symbol).

- [ ] **Step 3: Implement** as specified in Interfaces. Monotone guard inside
  `refreshViewForBeat`: never install a round lower than the current one.

- [ ] **Step 4: Run** `--gtest_filter='CasStoreBeat*:CasStore*:CasBuild*'` → PASS.

- [ ] **Step 5: Commit** — `"CAS beat: view load + drain + observed_gc_round ack ride the heartbeat renewal"`

---

### Task 6: current retired list (condemn_round; refs in gc/state; RetireView)

**Files:**
- Modify: `Core/CasGcFormats.h/.cpp` — `RetiredEntry` gains `uint64_t condemn_round = 0;`
  (strict-JSON field `"condemn_round"`); `encodeRetiredSet` sorts entries by `(kind, hash)`
  (byte-deterministic); `GcState` gains
  `std::map<uint64_t, String> retired_refs;   /// gc-shard -> object key of the current retired list`
  and bumps `"cas_gc_state"` to version 4 (v3 rejected fail-closed).
- Modify: `Core/CasRetireView.cpp` — `refresh`: GET `gc/state` → GET each `retired_refs` key
  (absent ref/object = empty shard) → install `(round, entries)`. No LIST.
- Modify: `docs/superpowers/specs/2026-07-02-cas-gc-ack-floor-fence-redesign.md` — two recorded
  amendments: (a) *Retired list encoding*: reuse the strict-JSON `cas_retired_set` codec with
  `condemn_round`, entries sorted by hash — a `RunFile` `RunKind::Retired` is unnecessary at
  candidate-set sizes (the determinism/publish-order properties are what the spec requires, and
  sorted JSON provides them); (b) *Writer recreate*: recreation happens through the EXISTING
  `Build::putBlob` cold-reuse rule (condemned ⇒ `uploadFromSource`, token-conditional) on the
  retried build; the promote gate stays fail-closed `ABORTED` (sources are not retained at promote
  time). Promote-time in-place recreate recorded as a possible follow-up.
- Test: `src/Disks/tests/gtest_cas_gc_formats.cpp`, `src/Disks/tests/gtest_cas_retire_view.cpp`

**Interfaces:**
- Consumes: `retiredKey(generation, attempt, round, shard)` (existing) as the ref target the GC
  pass writes (Task 9 fills `retired_refs` with these).
- Produces: `RetiredEntry::condemn_round`; `GcState::retired_refs`; `RetireView::refresh` contract
  above. `isCondemnedToken` / `findCondemned` signatures unchanged (writer gate untouched).

- [ ] **Step 1: Failing tests** — formats: `condemn_round` round-trips; entries re-ordered before
  encode come back sorted; `GcState` v4 round-trips `retired_refs`; a v3 body decodes to
  `NOT_IMPLEMENTED`/`CORRUPTED_DATA` (fail-closed, pin whichever the codec's version guard
  produces). Retire view: seed `gc/state{round=5, retired_refs={0: key}}` + a retired set with
  entries at rounds 4 and 5 → `refresh` → `round()==5`, `isCondemnedToken` true for both entries;
  absent ref key → empty view, no throw.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement.** Keep `decodeRetiredSet` adoption call sites compiling (Task 9 removes
  the per-round writers).
- [ ] **Step 4: Run** `--gtest_filter='CasGcFormats*:CasRetireView*'` → PASS.
- [ ] **Step 5: Commit** — `"CAS retired list: condemn_round + current-list refs in gc/state v4; RetireView reads refs"`

---

### Task 7: GC heartbeat gate — classification, fence-out, `min_ack`

**Files:**
- Modify: `Core/CasServerRoot.h/.cpp` — free function:

```cpp
struct HeartbeatFloor
{
    uint64_t min_ack = UINT64_MAX;   /// UINT64_MAX = no counted heartbeats
    size_t live = 0, terminated = 0, fenced_now = 0, already_fenced = 0;
};
/// LIST gc/server-roots/ + GET each mount body. live-or-expired-unfenced count into min_ack;
/// terminated (expires stamped by terminate) excluded; expired past `now_ms > expires_at_ms +
/// skew_margin_ms` and not yet gc_fenced -> token-guarded fence-out putOverwrite (body preserved,
/// gc_fenced = true, seq + 1) then excluded; fence-out PreconditionFailed -> re-GET, reclassify.
HeartbeatFloor computeHeartbeatFloor(Backend & b, const Layout & l, uint64_t now_ms,
                                     uint64_t skew_margin_ms);
```

- Test: `src/Disks/tests/gtest_cas_mount.cpp` (new suite `CasHeartbeatFloor`)

**Interfaces:**
- Consumes: Task 3 fields. Terminated detection: `expires_at_ms <= started_at_ms`? NO — pin it
  concretely: `MountLeaseKeeper::terminate` stamps `min_active == UINT64_MAX` (Task 4); that
  sentinel IS the terminated marker (`expires_at_ms` alone cannot distinguish crash from farewell).
- Produces: `computeHeartbeatFloor` consumed by Task 9's pass.

- [ ] **Step 1: Failing tests** — five bodies seeded into `InMemoryBackend` under
  `mountKey("s1..s5")`: live-acked-3, live-acked-7, expired-unfenced (must be fenced-out by the
  call: body re-read shows `gc_fenced`, and it does NOT count), already-fenced (skipped, no PUT),
  terminated (`min_active == UINT64_MAX`, excluded) → `min_ack == 3`. Second test: fence-out
  loses the token race (backend mutated between GET and PUT via a fault hook) → entry re-read and
  counted as live. Third: empty prefix → `min_ack == UINT64_MAX`.
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** (pure Backend+Layout function, fake `now_ms` — no clocks in tests).
- [ ] **Step 4: Run** `--gtest_filter='CasHeartbeatFloor*'` → PASS.
- [ ] **Step 5: Commit** — `"CAS gc: heartbeat floor (min_ack) with token-guarded fence-out of expired mounts"`

---

### Task 8: three-cursor merge in `foldDeltasIntoGeneration`

**Files:**
- Modify: `Core/CasBlobInDegree.h/.cpp`
- Test: `src/Disks/tests/gtest_cas_blob_indegree.cpp`

**Interfaces:**
- Consumes: Task 6 `RetiredEntry` (with `condemn_round`).
- Produces: the extended signature (all Task 9 call sites use it):

```cpp
struct RetiredMergeResult
{
    std::vector<RetiredEntry> still_retired;   /// carried entries + this pass's new condemnations
    std::vector<RetiredEntry> graduated;       /// in-degree 0 and condemn_round < min_ack
    std::vector<RetiredEntry> spared;          /// in-degree recovered (entry dropped)
};
void foldDeltasIntoGeneration(Backend & backend, const Layout & layout,
                              uint64_t prior_generation, uint64_t prior_attempt,
                              uint64_t new_generation, uint64_t attempt,
                              uint64_t shard,
                              std::vector<BlobDelta> scattered, std::vector<RunRef> & out_runs,
                              const std::vector<RetiredEntry> & prior_retired,   /// sorted by hash
                              uint64_t min_ack, uint64_t condemn_round,
                              const std::function<std::optional<HeadResult>(const UInt128 &)> & head_blob,
                              RetiredMergeResult & out_retired);
```

`head_blob` returns the blob's HEAD (token capture for NEW condemnations; `nullopt`/absent ⇒ skip —
nothing to condemn). Injected so unit tests need no blob objects.

- [ ] **Step 1: Failing tests** (extend `gtest_cas_blob_indegree.cpp`; helpers for building runs
  already exist in the file — reuse them):

```cpp
/// Boundary: entries at condemn_round = min_ack - 1 graduate; = min_ack stay.
TEST(CasThreeCursorMerge, FloorBoundary) { /* prior run: blob A,B in-degree 0 zero-markers;
    prior_retired = {A@r=2, B@r=3}; min_ack = 3; expect graduated={A}, still={B} */ }
/// In-degree recovery spares and drops the entry.
TEST(CasThreeCursorMerge, RecoverySpares) { /* delta adds an edge to A; expect spared={A} */ }
/// New zero-transition condemns with head-captured token at condemn_round.
TEST(CasThreeCursorMerge, NewCandidateCondemned) { /* delta removes C's last edge; head_blob(C)
    returns token "t9"; expect still_retired contains {C, t9, condemn_round} */ }
/// Absent blob at condemn time is skipped entirely.
TEST(CasThreeCursorMerge, AbsentBlobNotCondemned) { /* head_blob -> nullopt; no entry */ }
/// Snapshot output bytes are unchanged vs the two-cursor merge for identical edge inputs.
TEST(CasThreeCursorMerge, SnapshotBytesUnchanged) { /* run old expectations */ }
```

- [ ] **Step 2: Run to verify failure** (signature mismatch).
- [ ] **Step 3: Implement** — third cursor `ri` over `prior_retired` advanced inside the existing
  per-blob `openBlobIfNeeded`/`closeBlob` structure: on closing blob `h` with final `cur_edges`,
  consult the retired cursor at `h` and apply the four spec rules; after the main loop drains,
  flush remaining retired entries (blobs with no edges/deltas this pass: in-degree 0 by
  definition → graduate/keep by the floor). Zero-marker emission unchanged (it still feeds
  "condemn new" detection for blobs whose edges all lived in the prior run).
- [ ] **Step 4: Run** `--gtest_filter='CasThreeCursorMerge*:CasBlobInDegree*:CasSourceEdge*'` → PASS.
- [ ] **Step 5: Commit** — `"CAS gc: three-cursor merge (snapshot x deltas x retired) with ack-floor graduation"`

---

### Task 9: one-pass round — rewire `Gc::runRegularRound`, delete `fence`/`recheck`

**Files:**
- Modify: `Core/CasGc.h/.cpp` — remove `fence`, `recheck`, `RetirePhaseComplete`-era logic,
  `zeroInDegree`/`inDegreeInGeneration` call sites (and the functions themselves in
  `CasBlobInDegree.*` once unreferenced); `retire` is replaced by the pass tail.
- Modify: `Core/CasGcShardPlan.cpp` (reducer path forwards the new merge signature; retired input
  scattered per target shard alongside the deltas)
- Test: rewrite `src/Disks/tests/gtest_cas_gc_fence_recheck.cpp` →
  `gtest_cas_gc_ack_floor.cpp`; update `gtest_cas_gc_round.cpp`, `gtest_cas_gc_resume.cpp`,
  `gtest_cas_gc_leak.cpp`, `gtest_cas_gc_attempt.cpp` to the new round shape.

**Interfaces:**
- Consumes: Task 7 `computeHeartbeatFloor`, Task 8 merge, Task 6 `retired_refs`.
- Produces: the new round sequence, exactly:

```cpp
RoundReport Gc::runRegularRound()
{
    /// R0 lease/resume (unchanged).
    /// R1 floor: computeHeartbeatFloor(backend, layout, now_ms(), skew_margin_ms) — BEFORE discovery.
    /// R2 discovery + windows: existing fold() machinery (LIST + token-diff + window manifests),
    ///     unchanged, EXCEPT it now also loads the prior retired list (via state.retired_refs,
    ///     split per target shard by blobShard) and calls the Task 8 merge with
    ///     (prior_retired_shard, floor.min_ack, condemn_round = state.round + 1, head_blob).
    /// R3 deletes: for each graduated entry: deleteExact(blobKey, token);
    ///     Deleted / TokenMismatch / NotFound all terminal-OK (B170 outcome events, distinct
    ///     reasons). Manifest cleanup (mfCleanup) unchanged.
    /// R4 seal: PUT the new retired-list objects at retiredKey(new_generation, attempt,
    ///     state.round + 1, shard) via putDeterministicArtifact; then ONE gc/state CAS:
    ///     {round = state.round + 1, snap_generation = new_generation, snap_attempt = attempt,
    ///      retired_refs = the keys just written}. Retired PUTs strictly precede the CAS
    ///     (publish-order invariant).
}
```

  `Gc` gets a `skew_margin_ms` member (default: `mount_lease_ttl_ms / 2`, from pool config — no
  new user knob). Resume (`tryResumeIncompleteRound`): the completion-seal machinery reduces to
  "fold seal present, gc/state CAS absent → re-run the pass under a fresh attempt" (attempt
  scoping keeps artifacts byte-stable per attempt; already-executed deletes land on `NotFound`).

- [ ] **Step 1: Write the failing protocol tests** (`gtest_cas_gc_ack_floor.cpp`; reuse the
  fixture style of the old fence/recheck file — Store + Build + Gc over `InMemoryBackend`):

```cpp
/// Candidate condemned in round K graduates and is DELETED in round K+1 once every mount acks K.
TEST(CasGcAckFloor, CondemnThenDeleteNextRoundAfterAcks)
/// A mount stuck at ack K-1 blocks graduation (entry carried), but the round itself completes.
TEST(CasGcAckFloor, StaleAckHoldsTheFloorWithoutBlockingTheRound)
/// A publish referencing the candidate lands before the writer's ack: the pass folds it and SPARES.
TEST(CasGcAckFloor, PreAckPublishSpares)
/// An expired mount is fenced out; the floor advances; its writer's renewOnce then throws.
TEST(CasGcAckFloor, ExpiredMountFencedOutAndExcluded)
/// deleteExact TokenMismatch (writer recreated the blob) is a terminal OK outcome.
TEST(CasGcAckFloor, RecreatedBlobDeleteIsTokenMismatchOk)
/// Publish-order: retired list is readable at the round the gc/state CAS publishes (fault hook
/// kills the leader between retired PUT and CAS; a resumed pass adopts byte-equal artifacts).
TEST(CasGcAckFloor, ResumeAfterCrashBetweenRetiredPutAndStateCas)
```

- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement** per Interfaces; delete `Gc::fence` / `Gc::recheck` /
  `zeroInDegree` / `inDegreeInGeneration` and their tests; fold `retire`'s HEAD-token capture into
  the merge's `head_blob` (the retire-token Phase-5 `storedTok` optimization note in the model
  stays future work — HEAD-per-new-candidate is the pinned behavior).
- [ ] **Step 4: Run the full CAS gtest sweep**
  `--gtest_filter='Cas*' > build/test_task9.log 2>&1`, analyze with a subagent → all PASS.
- [ ] **Step 5: Commit** — `"CAS gc: one-pass ack-floor round; fence/recheck phases removed"`

---

### Task 10: schema + dead-code cleanup

**Files:**
- Modify: `Core/CasGcFormats.h/.cpp` (drop `GcState::fence_version` + its codec block — already
  v4 from Task 6, keep v4), `Core/Proto/cas_format.proto` (drop `FenceVersionEntryProto` et al. if
  now unreferenced), `Core/CasGc.h` (dead members: `fence_seq` usages stay — it is the leadership
  counter, NOT fence machinery), `Core/CasStore.cpp` (drop the `fence_round`-triggered refresh?
  NO — keep: it is the newborn birth-floor trigger; only per-round bumps died with `Gc::fence`).
- Test: `src/Disks/tests/gtest_cas_gc_formats.cpp` (fence_version gone), grep-based assert step.

- [ ] **Step 1: Failing test** — `GcState` round-trip with no `fence_version`; encoding a state
  never emits the key (assert the JSON body does not contain `"fence_version"`).
- [ ] **Step 2: Run to verify failure.**
- [ ] **Step 3: Implement**; then `grep -rn "fence_version\|zeroInDegree\|inDegreeInGeneration\|WatermarkKeeper\|serverRootWatermarkKey" src/` must return no production hits.
- [ ] **Step 4: Full build (`ninja clickhouse`, log + subagent) + full `Cas*` gtest sweep** → PASS.
- [ ] **Step 5: Commit** — `"CAS gc: drop fence_version and dead fence-era code"`

---

### Task 11: observability

**Files:**
- Modify: `src/Common/ProfileEvents.cpp` (after `CasManifestHardLimitExceeded`, ~line 804):
  `CasGcRetiredGraduated`, `CasGcRetiredSpared`, `CasGcRetiredCondemned`,
  `CasGcHeartbeatFenceOuts`, `CasGcFloorHeldByStaleAck` (each with a one-line description,
  `ValueType::Number`).
- Modify: `Core/CasGc.cpp` (increment at the pass sites; WARNING log when a live heartbeat's ack
  lags the round by more than 2: server id, ack, round), `Core/CasEvent.*` if a new
  `CasEventType::GcFenceOut` enum member is needed (follow the existing enum/codec pattern; emit
  from `computeHeartbeatFloor`'s caller with hostname/pid/epoch of the fenced body).
- Modify: `Core/CasGc.h` `RoundReport`: `size_t graduated = 0, spared = 0, condemned = 0,
  fence_outs = 0; uint64_t min_ack = 0;`.
- Test: extend `gtest_cas_gc_ack_floor.cpp` and `gtest_cas_gc_log.cpp` — the
  `CondemnThenDeleteNextRoundAfterAcks` scenario asserts the report counters and that a
  `GcFenceOut` event row is emitted by `ExpiredMountFencedOutAndExcluded`.

- [ ] **Step 1: Failing tests** (report counters + event row). **Step 2: verify failure.**
  **Step 3: implement.** **Step 4: run `Cas*` sweep → PASS.**
- [ ] **Step 5: Commit** — `"CAS gc: ack-floor observability (counters, GcFenceOut event, ack-lag warning)"`

---

### Task 12: `mayMutate` boottime hardening + docs

**Files:**
- Modify: `Core/CasStore.h/.cpp` — the mount-fence deadline moves from
  `std::chrono::steady_clock` to a `CLOCK_BOOTTIME`-based reading (helper
  `static uint64_t bootMs()` via `clock_gettime(CLOCK_BOOTTIME, ...)`; store the deadline as
  `std::atomic<uint64_t> deadline_boot_ms`). Rationale comment: `CLOCK_MONOTONIC` does not advance
  across a VM suspend, so a resumed sleeper would think its fence is still armed; boottime does.
- Modify: `docs/superpowers/cas/ROADMAP.md` — mark the fence/recheck line superseded by this
  redesign; add follow-ups (delta-runs compaction link to the O(buffer) backlog note;
  `process_epoch`→`writer_epoch` stamp unification; promote-time recreate).
- Modify: `utils/ca-soak/scenarios/BACKLOG.md` — add scenario cards: (a) SIGSTOP a writer → floor
  holds → SIGCONT → acks → graduation resumes; (b) hard-KILL a writer mid-burst → fence-out after
  TTL → no dangle in `fsck`; (c) request-budget regression guard (round request count stays
  O(delta)+O(servers)).
- Test: `gtest_cas_store.cpp` — fence deadline test updated to inject a fake boot-clock fn
  (constructor-injected `std::function<uint64_t()> boot_ms_fn`, defaulting to the real one).

- [ ] **Step 1: Failing test** (fake boot clock advances past deadline ⇒ `mayMutate` false;
  monotonic-style freeze cannot be simulated ⇒ test the injected-fn seam). **Step 2: verify
  failure. Step 3: implement. Step 4: `Cas*` sweep + full `ninja clickhouse` → PASS.**
- [ ] **Step 5: Commit** — `"CAS write fence: CLOCK_BOOTTIME deadline; roadmap/backlog updates for ack-floor follow-ups"`

---

## Self-review notes (already applied)

- Spec §"merged heartbeat" said keep `process_epoch` — Task 3/4 drop it as a separate field
  because the writable path already sets `process_epoch = writer_epoch` (`CasStore.cpp:157`); the
  sweep consumes `writer_epoch` unchanged. Spec amendment recorded in Task 6 alongside the two
  bigger ones (retired encoding = sorted strict-JSON, recreate lives in `putBlob` cold-reuse).
- Spec's sabotage list names map: `SabotageDeleteAtFloor` → `SabotageIgnoreAckFloor` (an `r <` vs
  `r ≤` off-by-one is provably still safe under the publish-order invariant, so the load-bearing
  control is "ignores the floor", not the boundary).
- Every task ends green-buildable; Tasks 1–2 gate the rest; Tasks 3–8 are independently
  unit-tested components; Task 9 is the only cross-cutting rewire and carries the protocol suite.

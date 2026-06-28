# CA Layout Phase 0 — server_root_id identity + mount safety — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make content-addressed layout identity explicit and safe: a required `server_root_id`, `ServerUUID` demoted to an owner token, a single sticky owner object (identity), a durable-monotone `writer_epoch` in a sticky `epoch` object, a mount heartbeat lease (liveness), and a local write fence — so two servers can never write the same namespace tree and `writer_epoch` can never reset.

**Architecture:** New `gc/server-roots/<server_root_id>/{owner,epoch,mount,watermark}` objects. The mount lease reuses the existing `SingleWriterSlot` base (as `MountLeaseKeeper`, sibling to `WatermarkKeeper`). The startup protocol hooks into `Store::open`'s writable block. A TLA+ model (`CaCasMountCore`, mirroring `CaGcLeaseCore`) proves the discipline before any code lands.

**Tech Stack:** C++ (`DB::Cas`), Protocol Buffers (`Proto/cas_format.proto`), GoogleTest (`unit_tests_dbms`), TLA+/TLC (`tla2tools.jar`).

**Scope:** This is **Phase 0 only** of the layout spec. Phases 1 (relocation), 2 (cursor sweep), 3 (writer_epoch rename + manifest_ordinal) get their own plans once Phase 0's interfaces are concrete. Spec: `docs/superpowers/specs/2026-06-28-cas-layout-hot-cold-split-design.md` (rev5).

## Global Constraints

- Branch: a new branch off the current branch (NOT master). Add new commits only — never rebase/amend.
- Allman braces (opening brace on its own line) for all C++ — CI style check.
- CA is pre-release, no persisted data ⟹ no migration / no compat scaffolding.
- **Avoid fallback paths; fail closed.** Identity errors throw `ErrorCodes::CORRUPTED_DATA`; supersession/foreign-touch fails the disk closed. Never silently re-mint or recreate an identity object.
- Never use `sleep` to coordinate concurrency. The renewer is the existing `SingleWriterSlot` `ThreadFromGlobalPool` background loop; tests drive `renewOnce()` explicitly (`background_watermark=false` style).
- Build: `ninja unit_tests_dbms` from inside the build dir (`/home/mfilimonov/workspace/ClickHouse/master/build`), **no `-j`, no `nproc`**, redirect output to a log in the build dir; analyze the log via a subagent (concise summary).
- Tests: redirect each run to a uniquely-named log under the build dir.
- Commit-message trailers (exact, end of every commit message):
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
  `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`
- **Task 1 (TLA+ Gate) MUST be green before any code task (Task 2+).**

## Governing rules (from the spec)

- **Owner object = identity, clock-free.** Foreign `ServerUUID` → fail closed regardless of lease state. Owner-absent allowed **only if the whole `server_root_id` subtree is provably empty** (`cas/refs/<id>/`, `cas/manifests/<id>/`, `roots/<id>/`); absent-over-non-empty → `CORRUPTED_DATA`.
- **Lease = liveness.** Same `ServerUUID` reclaims **only after expiry**; same-UUID live lease → fail closed (double-start guard); foreign UUID → fail closed regardless of expiry. No automatic cross-UUID takeover.
- **`writer_epoch` = durable monotone counter** in the sticky `epoch` object (never deleted); missing `owner`/`epoch` over a non-empty root → `CORRUPTED_DATA`. Carried in the existing `writer_instance_id` field this phase (Phase 3 renames it).
- **Write fence is local** — cached `(server_uuid, writer_epoch)` + monotonic deadline + latching `lost` flag set by the renewer; **no per-write S3 read**.

## File structure (Phase 0)

- `docs/superpowers/models/CaCasMountCore.tla` + `*.cfg` + `run_mount.sh` + `CaCasMountCore_RESULTS.md` (Task 1).
- `src/Disks/.../Core/CasStore.h` — `PoolConfig::server_root_id`; the mount-lease/owner/epoch state + the write-fence accessor (Tasks 2, 6, 7).
- `src/Disks/.../Core/CasLayout.h` — `serverRootPrefix`/`ownerKey`/`epochKey`/`mountKey` (Task 3).
- `src/Disks/.../Core/CasFormat.h` + `.cpp` — `FormatId::MountLease`, `FormatId::ServerEpoch` + magics (Task 3).
- `src/Disks/.../Core/Proto/cas_format.proto` — `MountLeaseProto`, `ServerEpochProto`, `OwnerProto` (Task 3).
- `src/Disks/.../Core/CasServerRoot.h` + `.cpp` (NEW) — owner/epoch claim logic + `MountLeaseKeeper : SingleWriterSlot` + codecs (Tasks 3,4,5).
- `src/Disks/.../Core/CasStore.cpp` — startup protocol in the writable block (Task 7).
- `src/Disks/.../MetadataStorageFactory.cpp` + `ContentAddressedMetadataStorage.{h,cpp}` — required `server_root_id` config threaded to `PoolConfig` (Task 2).
- `src/Disks/tests/gtest_cas_mount.cpp` (NEW) — all Phase-0 gtests.

---

### Task 1: TLA+ gate — `CaCasMountCore` {#task-1}

**This is the gate. It must be green before any code task.**

**Files:**
- Create: `docs/superpowers/models/CaCasMountCore.tla`
- Create: `docs/superpowers/models/CaCasMountCore_stage1.cfg` (positive) + `CaCasMountCore_sab_foreigntakeover.cfg` + `CaCasMountCore_sab_epochreset.cfg` + `CaCasMountCore_sab_supersededwrites.cfg` (negative controls)
- Create: `docs/superpowers/models/run_mount.sh` (copy `run_gc_partmanifest.sh`, swap the hardcoded `.tla` to `CaCasMountCore.tla`)
- Create: `docs/superpowers/models/CaCasMountCore_RESULTS.md`; add a row to `docs/superpowers/models/INDEX.md`

**Background (prior art — read first):** `docs/superpowers/models/CaGcLeaseCore.tla` (~203 lines) models a clock-free observation-window lease: `CONSTANTS Actors, None, EnableHeartbeat, MaxClock, MaxSeq, MaxFence`; `VARIABLES clock, stOwner, stSeq, stFence, …`; actions `Tick/Create/Renew/Retire/ObserveOrSteal/Die`; a CAS register modeled as scalar vars with a steal as one atomic action; invariants `NoEpochCollision`, `NoFalseSteal`, `TypeOK`. Mirror its shape.

**Model to build.** Three objects per `server_root_id` (one modeled `server_root_id`): `owner` (a `ServerUUID` or `None`), `epoch` (a counter `0..MaxEpoch`), `mount` (a record `[uuid, epoch, deadline]` or `None`). Two `Actors` (two servers) `A`,`B`, each with a fixed distinct `ServerUUID`; a `clock` (`0..MaxClock`); per-actor `localEpoch`, `localLost`, `wrote` (the set of (actor,epoch) that performed a mutation). Actions:
- `ClaimOwnerEmpty(a)` — owner=None ∧ root empty ⇒ owner:=uuid(a);
- `OwnerGate(a)` — owner≠None ∧ owner≠uuid(a) ⇒ actor a is rejected (sets a `rejected[a]` flag; never mutates);
- `AllocEpoch(a)` — owner=uuid(a) ⇒ epoch:=epoch+1, localEpoch[a]:=epoch';
- `ClaimMountFresh(a)` / `ReclaimExpired(a)` — same-uuid ∧ (mount=None ∨ mount.deadline≤clock) ⇒ mount:=[uuid(a), localEpoch[a], clock+TTL]; a different-uuid mount is never claimed; a same-uuid live mount blocks (no action enabled);
- `Renew(a)` — mount.uuid=uuid(a) ∧ mount.epoch=localEpoch[a] ⇒ extend deadline; if mount.epoch≠localEpoch[a] ⇒ localLost[a]:=TRUE (superseded);
- `Tick` — clock+1;
- `Write(a)` — **only if** ¬localLost[a] ∧ mount.uuid=uuid(a) ∧ mount.epoch=localEpoch[a] ∧ mount.deadline>clock ⇒ wrote := wrote ∪ {(a, localEpoch[a])}.

**Invariants (the gate):**
- `NoTwoServerUuidsOwnSameServerRoot` — `owner` only ever transitions None→uuid once and never to a different uuid: `[](owner # None => owner is the first claimant's uuid)`.
- `ForeignUuidNeverAutoTakesOver` — `[](mount # None => mount.uuid = owner)` (a mount is never held by a non-owner, for any clock/deadline).
- `WriterEpochMonotoneUnique` — `epoch` is non-decreasing and every `AllocEpoch` strictly increases it; no two `Write`s by different `(actor,epoch)` share an epoch value (model `wrote` ⇒ `\A x,y \in wrote : x.epoch = y.epoch => x.actor = y.actor`).
- `SupersededWriterMakesNoMutation` — a `Write(a)` is impossible once `localLost[a]` (encoded by the `Write` guard; assert `[](localLost[a] => no new (a,*) enters wrote)` via an auxiliary "last writer epoch is always the live mount epoch").
- (liveness witness, separate cheap cfg) `SameUuidRestartCanReclaimExpiredMount` — `<>` the same uuid reclaims after a `Die`+`Tick`-past-deadline.

**Negative controls (each MUST produce a counterexample):**
- `_sab_foreigntakeover` — add a `BadForeignReclaim(a)` action enabled on an expired mount regardless of uuid ⇒ must violate `ForeignUuidNeverAutoTakesOver`.
- `_sab_epochreset` — add a `BadEpochResetOnMountDelete` action (mount deleted ⇒ epoch:=0) ⇒ must violate `WriterEpochMonotoneUnique`.
- `_sab_supersededwrites` — drop the `mount.epoch=localEpoch[a]` conjunct from `Write` ⇒ must violate `SupersededWriterMakesNoMutation`.

- [ ] **Step 1: Write `run_mount.sh`**

```bash
cp docs/superpowers/models/run_gc_partmanifest.sh docs/superpowers/models/run_mount.sh
# edit run_mount.sh: change the hardcoded model filename to CaCasMountCore.tla
chmod +x docs/superpowers/models/run_mount.sh
```

- [ ] **Step 2: Write `CaCasMountCore.tla` + the positive `stage1` cfg** (mirror `CaGcLeaseCore.tla`'s structure; scalar-var CAS register; the actions + invariants above). The `stage1.cfg`: `SPECIFICATION Spec`, `CONSTANTS Actors={A,B} None=NoneVal MaxClock=4 MaxEpoch=3 TTL=2`, the four safety `INVARIANT`s, `CHECK_DEADLOCK FALSE`.

- [ ] **Step 3: Run the positive stage**

Run: `cd docs/superpowers/models && ./run_mount.sh CaCasMountCore_stage1`
Expected: "Model checking completed. No error has been found." Record distinct-state count.

- [ ] **Step 4: Write + run the three sabotage cfgs (+ the liveness witness cfg)**

Run each via `./run_mount.sh <cfg>`.
Expected: each `_sab_*` reports the named invariant **VIOLATED** with a counterexample; the witness cfg reports the liveness witness reachable (VIOLATED on the `~reachable` form). If a sabotage does NOT violate, the model is too weak — strengthen until it counterexamples.

- [ ] **Step 5: Record results + commit**

Write `CaCasMountCore_RESULTS.md` (header / what is modelled / invariants / results table / counterexamples / reproduce); add an `INDEX.md` row.
```bash
git add docs/superpowers/models/CaCasMountCore.tla docs/superpowers/models/CaCasMountCore_*.cfg \
        docs/superpowers/models/run_mount.sh docs/superpowers/models/CaCasMountCore_RESULTS.md \
        docs/superpowers/models/INDEX.md
git commit -m "CA mount-safety TLA+ gate: CaCasMountCore (owner/epoch/lease/write-fence discipline)
<trailers>"
```

**Gate to proceed:** stage1 green; all three sabotages counterexample; witness reachable.

---

### Task 2: Required `server_root_id` config + `PoolConfig` field {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h:28-72` (add `PoolConfig::server_root_id`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp:210-260` (required read)
- Modify: `src/Disks/.../ContentAddressed/ContentAddressedMetadataStorage.h:179`-area + `.cpp:140-169, 356-369` (thread through)
- Create: `src/Disks/.../Core/CasServerRoot.h` (add `validateServerRootId(const String &) -> void` here; reused by later tasks)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Produces: `PoolConfig::server_root_id` (`String`); `DB::Cas::validateServerRootId(const String & id)` — throws `ErrorCodes::BAD_ARGUMENTS` on an invalid id (clean relative path: non-empty, no leading/trailing `/`, no `//`, no `.`/`..` segment, length ≤ 255, no segment equal to `_files`/`_manifests`).

- [ ] **Step 1: Write the failing test** in `gtest_cas_mount.cpp`:

```cpp
#include <gtest/gtest.h>
#include "cas_test_helpers.h"
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasServerRoot.h>
using namespace DB::Cas;

TEST(CasServerRootId, ValidationAcceptsCleanPathsRejectsBad)
{
    EXPECT_NO_THROW(validateServerRootId("replica-a"));
    EXPECT_NO_THROW(validateServerRootId("shard-01/replica-a"));
    EXPECT_THROW(validateServerRootId(""), DB::Exception);
    EXPECT_THROW(validateServerRootId("/replica"), DB::Exception);
    EXPECT_THROW(validateServerRootId("replica/"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a//b"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a/../b"), DB::Exception);
    EXPECT_THROW(validateServerRootId("a/_files/b"), DB::Exception);
}
```

- [ ] **Step 2: Run to verify it fails** — `cd build && ninja unit_tests_dbms > build_t2.log 2>&1` — expect compile FAIL (`validateServerRootId` / `CasServerRoot.h` not found).

- [ ] **Step 3: Implement `validateServerRootId`** in `CasServerRoot.h` (header-only is fine; follow `CasLayout.h::checkNamespace` style — read it for the exact segment-rejection idiom). Add `PoolConfig::server_root_id` (`String server_root_id;`) to `CasStore.h`.

- [ ] **Step 4: Thread the required config** — in `MetadataStorageFactory.cpp` add `const std::string server_root_id = config.getString(config_prefix + ".server_root_id");` (no default ⟹ Poco throws on absence) near line 248; pass it to the `ContentAddressedMetadataStorage` ctor (extend the ctor param list + the `.h` `const std::string server_root_id;` member); assign `pool_config.server_root_id = server_root_id;` at `ContentAddressedMetadataStorage.cpp:~360`; call `Cas::validateServerRootId(server_root_id)` once at config read.

- [ ] **Step 5: Run + build** — `ninja unit_tests_dbms > build_t2b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasServerRootId.*' > test_t2.log 2>&1` — expect PASS. Analyze via subagent.

- [ ] **Step 6: Commit** (stage only the touched files; NOT `git add -A`).

---

### Task 3: Layout keys + `MountLeaseProto`/`ServerEpochProto`/`OwnerProto` + codecs {#task-3}

**Files:**
- Modify: `src/Disks/.../Core/CasLayout.h` (after `serverWatermarkKey`, ~L225)
- Modify: `src/Disks/.../Core/Proto/cas_format.proto` (new messages)
- Modify: `src/Disks/.../Core/CasFormat.h:14-54` + `CasFormat.cpp:31-67` (new `FormatId`s + magics in BOTH switches)
- Modify: `src/Disks/.../Core/CasServerRoot.h` + create `CasServerRoot.cpp` (codecs)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Produces (in `CasLayout`): `serverRootPrefix(const String & srid)` → `<prefix>/gc/server-roots/<srid>/`; `ownerKey(srid)` → `…/owner`; `epochKey(srid)` → `…/epoch`; `mountKey(srid)` → `…/mount`; `serverRootWatermarkKey(srid)` → `…/watermark`.
- Produces (in `CasServerRoot.h`): structs `OwnerObject{UInt128 server_uuid;}`, `ServerEpoch{uint64_t next_writer_epoch;}`, `MountLease{UInt128 server_uuid; uint64_t writer_epoch; String hostname; uint64_t pid; uint64_t started_at_ms; uint64_t seq; uint64_t expires_at_ms;}` + `encode*/decode*` (mirror `encodeServerWatermark`/`decodeServerWatermark`, `CasWatermark.cpp:24-68`).
- Produces (in `CasFormat`): `FormatId::Owner`, `FormatId::ServerEpoch`, `FormatId::MountLease` with distinct magics (e.g. `"CAOW"`,`"CAEP"`,`"CAML"`).

- [ ] **Step 1: Failing test** (codec round-trips + key strings):

```cpp
TEST(CasServerRoot, KeysAndCodecsRoundTrip)
{
    Layout layout("p");
    EXPECT_EQ(layout.ownerKey("shard/r-a"), "p/gc/server-roots/shard/r-a/owner");
    EXPECT_EQ(layout.epochKey("shard/r-a"), "p/gc/server-roots/shard/r-a/epoch");
    EXPECT_EQ(layout.mountKey("shard/r-a"), "p/gc/server-roots/shard/r-a/mount");

    MountLease m{.server_uuid = UInt128(7), .writer_epoch = 42, .hostname = "h", .pid = 9,
                 .started_at_ms = 1000, .seq = 3, .expires_at_ms = 2000};
    EXPECT_EQ(decodeMountLease(encodeMountLease(m)).writer_epoch, 42u);
    EXPECT_EQ(decodeMountLease(encodeMountLease(m)).server_uuid, UInt128(7));
    EXPECT_EQ(decodeServerEpoch(encodeServerEpoch(ServerEpoch{.next_writer_epoch = 5})).next_writer_epoch, 5u);
    EXPECT_EQ(decodeOwner(encodeOwner(OwnerObject{.server_uuid = UInt128(11)})).server_uuid, UInt128(11));
}
```

- [ ] **Step 2: Run to verify fail** — compile FAIL (keys/codecs absent).

- [ ] **Step 3: Implement** — add the proto messages (each with `CasHeader header = 1;` field-1, mirroring `WatermarkProto`); add the three `FormatId`s + magics to BOTH switches in `CasFormat.cpp`; add the `CasLayout` key helpers; implement the codecs in `CasServerRoot.cpp` mirroring `encodeServerWatermark`/`decodeServerWatermark` (magic check + `checkCompatibility` before field reads; `u128ToBytesBE`/`u128FromBytesBE` for UInt128).

- [ ] **Step 4: Run** — `ninja unit_tests_dbms` then `--gtest_filter='CasServerRoot.*'` — expect PASS.

- [ ] **Step 5: Commit.**

---

### Task 4: Owner + epoch claim (identity + durable-monotone allocator) {#task-4}

**Files:**
- Modify: `src/Disks/.../Core/CasServerRoot.h` + `.cpp`
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `Backend`, `Layout`, the codecs (Task 3); `validateServerRootId` (Task 2).
- Produces: `void claimOwnerOrThrow(Backend & b, const Layout & l, const String & srid, UInt128 our_uuid)` — `putIfAbsent` owner; on `PreconditionFailed` `get` + compare (equal→ok; differ→`CORRUPTED_DATA`); **owner-absent claim requires `serverRootSubtreeEmpty(b,l,srid)`** else `CORRUPTED_DATA`. And `uint64_t allocateWriterEpoch(Backend & b, const Layout & l, const String & srid)` — CAS-bump `epoch.next_writer_epoch`, returning the allocated value; a missing `epoch` over a non-empty subtree → `CORRUPTED_DATA`. And `bool serverRootSubtreeEmpty(Backend & b, const Layout & l, const String & srid)` — `list(limit=1)` over each of `cas/refs/<srid>/`, `cas/manifests/<srid>/`, `roots/<srid>/`; empty iff all three are empty. (Phase 0 note: `cas/refs/`/`cas/manifests/` prefixes don't carry data yet — Phase 1 relocates into them — so today only `roots/<srid>/` can be non-empty; list all three so the check is correct once Phase 1 lands.)

- [ ] **Step 1: Failing tests:**

```cpp
TEST(CasServerRootClaim, OwnerStickyAndForeignFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    EXPECT_NO_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(1)));     // fresh empty root → claim
    EXPECT_NO_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(1)));     // same uuid → ok
    EXPECT_THROW(claimOwnerOrThrow(*b, l, "r", UInt128(2)), DB::Exception);  // foreign → fail closed
}

TEST(CasServerRootEpoch, AllocatorIsMonotoneAndSurvivesMountConcept)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    const uint64_t e1 = allocateWriterEpoch(*b, l, "r");
    const uint64_t e2 = allocateWriterEpoch(*b, l, "r");
    EXPECT_GT(e2, e1);                                             // strictly increasing
    // deleting the (separate) mount object must NOT reset the epoch:
    b->deleteExact(l.mountKey("r"), b->head(l.mountKey("r")).token);   // (no mount yet → NotFound, fine)
    EXPECT_GT(allocateWriterEpoch(*b, l, "r"), e2);
}

TEST(CasServerRootClaim, MissingOwnerOverNonEmptyRootIsCorrupted)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    // simulate existing data without an owner (identity lost):
    b->putIfAbsent(l.mountpointObjectKey("data") /* under roots/ */ , "x");  // make roots/<...> non-empty for srid
    // (adjust the planted key to fall under roots/<srid>/ for the chosen srid)
    EXPECT_THROW(claimOwnerOrThrow(*b, l, "<srid matching the planted key>", UInt128(1)), DB::Exception);
}
```
(Adjust the planted-key path so it lands under `roots/<srid>/`; use the real `CasLayout` helper for a roots-relative key.)

- [ ] **Step 2: Run → fail.**
- [ ] **Step 3: Implement** `serverRootSubtreeEmpty`, `claimOwnerOrThrow`, `allocateWriterEpoch` per the interfaces (CAS-bump pattern from `acquireOrRenewLease` `CasGc.cpp:1742-1768`: read-or-create, mutate, `casPut` with the observed token, retry on `Conflict` up to a bounded count).
- [ ] **Step 4: Run → pass** (`--gtest_filter='CasServerRoot*'`).
- [ ] **Step 5: Commit.**

---

### Task 5: `MountLeaseKeeper` (heartbeat lease) {#task-5}

**Files:**
- Modify: `src/Disks/.../Core/CasServerRoot.h` + `.cpp` (the keeper)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: `SingleWriterSlot` (`CasSingleWriterSlot.h`) — extend it exactly like `WatermarkKeeper` (`CasWatermark.h:45-72`): implement `prepareRenew`, `encodeBody`, `claim`, `terminate`.
- Produces: `class MountLeaseKeeper final : public SingleWriterSlot` with ctor `(BackendPtr, const Layout &, const String & srid, UInt128 server_uuid, uint64_t writer_epoch, std::chrono::milliseconds ttl, std::function<uint64_t()> now_ms_fn)`; `void start()` (= `doStart`), `void stop()` (= `doTerminate`). A free function `MountClaimResult claimMount(Backend &, const Layout &, const String & srid, UInt128 our_uuid, uint64_t our_epoch, uint64_t now_ms, uint64_t ttl_ms)` implementing the startup decision: `absent`→claim; `same uuid + expired`→reclaim; `same uuid + live`→`LiveDoubleStart` (fail); `foreign uuid`→`ForeignOwner` (fail). `MountClaimResult` = `{enum {Claimed, LiveDoubleStart, ForeignOwner} kind; MountLease body;}`.

**Note:** `claim`/`renew` go through `SingleWriterSlot`, whose `renewOnce` already **fails closed (throws `LOGICAL_ERROR`) on any foreign touch** — reuse that for the foreign/superseded renew case. The renew body must carry our `writer_epoch`; the keeper detects supersession by comparing the observed mount's `writer_epoch` to ours (Task 6 wires the `lost` flag).

**Adopt rule (critical — the normal flow is `claimMount(...)` writes the mount, THEN `keeper.start()`):** the keeper's `claim` hook must **ADOPT a live mount that is already ours** instead of seeing it as a double-start. The discriminator is `(server_uuid, writer_epoch)`:
- **same `server_uuid` + same `writer_epoch`** → it is *our own* just-written claim (or a replay) → **adopt**: use the observed token and `putOverwrite` to refresh `seq`/`expires_at_ms` (no fail);
- **same `server_uuid` + a *different* live `writer_epoch`** → a newer incarnation superseded us (or a concurrent double-start) → **fail closed**;
- **foreign `server_uuid`** → **fail closed**.
So `claimMount` (the startup decision) returning `Claimed` after writing the mount, followed by `keeper.start()`, is the steady path and must not self-trip the live-double-start guard. (The same-uuid+same-epoch adopt is exactly why `claimMount` and the keeper share the `(uuid, epoch)` identity rather than only checking `uuid`.)

- [ ] **Step 1: Failing tests** (template from `gtest_cas_heartbeat.cpp:17-35`; drive the clock via `now_ms_fn`):

```cpp
TEST(CasMountLease, AbsentClaimThenRenewBumpsSeq)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    uint64_t now = 1000;
    auto r = claimMount(*b, l, "r", UInt128(1), /*epoch*/7, now, /*ttl*/100);
    EXPECT_EQ(r.kind, MountClaimResult::Claimed);
    MountLeaseKeeper k(b, l, "r", UInt128(1), 7, std::chrono::milliseconds(100), [&]{ return now; });
    k.start();
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).seq, 1u);
    k.renewOnce();
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).seq, 2u);
}

TEST(CasMountLease, SameUuidLiveFailsForeignFailsExpiredReclaims)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    claimMount(*b, l, "r", UInt128(1), 7, /*now*/1000, /*ttl*/100);    // A live until 1100
    // same uuid, lease still live → double-start guard:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(1), 8, 1050, 100).kind, MountClaimResult::LiveDoubleStart);
    // foreign uuid, even after expiry → fail closed:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(2), 1, 1200, 100).kind, MountClaimResult::ForeignOwner);
    // same uuid after expiry → reclaim:
    EXPECT_EQ(claimMount(*b, l, "r", UInt128(1), 9, 1200, 100).kind, MountClaimResult::Claimed);
}

TEST(CasMountLease, KeeperStartAdoptsOurOwnClaimNotDoubleStart)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    uint64_t now = 1000;
    // The normal flow: claimMount writes the live mount under (uuid=1, epoch=7), THEN keeper.start().
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), /*epoch*/7, now, /*ttl*/100).kind, MountClaimResult::Claimed);
    MountLeaseKeeper k(b, l, "r", UInt128(1), /*epoch*/7, std::chrono::milliseconds(100), [&]{ return now; });
    EXPECT_NO_THROW(k.start());     // adopts our own live (uuid=1,epoch=7) mount — NOT a double-start
    EXPECT_EQ(decodeMountLease(b->get(l.mountKey("r"))->bytes).writer_epoch, 7u);

    // A keeper for the SAME uuid but a DIFFERENT live epoch must fail closed (superseded/double-start):
    MountLeaseKeeper k2(b, l, "r", UInt128(1), /*epoch*/8, std::chrono::milliseconds(100), [&]{ return now; });
    EXPECT_ANY_THROW(k2.start());
}
```

- [ ] **Step 2: Run → fail.**
- [ ] **Step 3: Implement** `claimMount` (the four-way decision; uses `get(mountKey)` + the body's `expires_at_ms` vs `now_ms`) and `MountLeaseKeeper` (the four `SingleWriterSlot` hooks; `encodeBody` → `encodeMountLease` with the seq + fresh `expires_at_ms = now_ms_fn() + ttl`; `claim` does the HEAD→putIfAbsent/putOverwrite dance like `WatermarkKeeper::claim`).
- [ ] **Step 4: Run → pass.**
- [ ] **Step 5: Commit.**

---

### Task 6: Local write fence {#task-6}

**Files:**
- Modify: `src/Disks/.../Core/CasStore.h` + `CasStore.cpp` (a `MountFence` holder + the gate; the renewer sets `lost`)
- Modify: `src/Disks/.../Core/CasServerRoot.cpp` (renewer wires supersession → `lost`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Produces: `struct MountFence { UInt128 server_uuid; uint64_t writer_epoch; std::atomic<std::chrono::steady_clock::time_point> deadline; std::atomic<bool> lost; }` (owned by `Store`); `bool Store::mayMutate() const` — `!fence.lost.load() && steady_clock::now() < fence.deadline.load()`; `void Store::tripMountLost()` (latching). The `MountLeaseKeeper`'s renew updates `deadline` on success and calls `tripMountLost()` on a superseded/foreign observation.
- Consumes: every existing mutable write path (`mutateShard`, manifest/blob PUTs) calls `mayMutate()` first (or a single chokepoint) and throws `ErrorCodes::ABORTED` "mount lost / lease expired" when false.

- [ ] **Step 1: Failing test:**

```cpp
TEST(CasMountFence, SupersededWriterRefusedNoS3Read)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    auto store = Store::open(b, PoolConfig{.pool_prefix="p", .root_shards=1, .server_root_id="r"});
    EXPECT_TRUE(store->mayMutate());
    store->tripMountLost();
    EXPECT_FALSE(store->mayMutate());
    // a mutate attempt fails closed:
    EXPECT_THROW(store->renewWatermarkOnce() /* or a mutateShard call */, DB::Exception);  // pick a real mutate entrypoint
}
```
(Use a real mutating entrypoint that now consults `mayMutate()`.)

- [ ] **Step 2: Run → fail.**
- [ ] **Step 3: Implement** the `MountFence` + `mayMutate`/`tripMountLost`; gate the mutate chokepoint(s); wire `MountLeaseKeeper` renew → `deadline`/`tripMountLost`. The deadline is a `steady_clock` instant computed at renew time from `now_ms + ttl` (monotonic; wall-clock changes can't extend it).
- [ ] **Step 4: Run → pass.**
- [ ] **Step 5: Commit.**

---

### Task 7: Startup protocol wiring in `Store::open` {#task-7}

**Files:**
- Modify: `src/Disks/.../Core/CasStore.cpp:106-115` (the writable W-ANCHOR block) + `CasStore.h` (hold `MountLeaseKeeper`, `MountFence`, allocated `writer_epoch`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

**Interfaces:**
- Consumes: Tasks 2–6. **Strict order, in the `if (!config.read_only)` block, after `PoolMeta::createOrValidate`.** The existing `WatermarkKeeper` construction (currently `CasStore.cpp:106-115`, fed the random `process_epoch`) **MOVES to after epoch allocation** — it now consumes the durable `writer_epoch`, so it cannot anchor first. **owner / epoch / mount / watermark are bootstrap-control writes**: they run here during open, *before* the write fence is armed and *before* any ordinary data/ref/manifest write; the [write fence](#task-6) gates only **ordinary mutations**, never these bootstrap writes (which establish the very right to write). Order:
  1. `validateServerRootId(config.server_root_id)`;
  2. `claimOwnerOrThrow(backend, layout, config.server_root_id, config.server_id)` — **identity**;
  3. `uint64_t writer_epoch = allocateWriterEpoch(backend, layout, config.server_root_id)` — **durable monotone epoch** (replaces the random `process_epoch` for identity; the [bridge](#bridge));
  4. `auto claim = claimMount(backend, layout, srid, config.server_id, writer_epoch, now_ms, ttl_ms)`; if `LiveDoubleStart` / `ForeignOwner` → throw `ErrorCodes::ABORTED` with the actionable error text (spec §mount-safety); else construct + `start()` the `MountLeaseKeeper` (which **adopts** the just-written same-`(uuid,epoch)` mount — Task 5) and arm `MountFence{server_uuid = config.server_id, writer_epoch, deadline, lost = false}`;
  5. construct + `start()` the `WatermarkKeeper` with the durable **`writer_epoch`** (NOT the random `process_epoch`) — the watermark must be durable *before* ordinary writes;
  6. only now are ordinary data/ref/manifest mutable writes allowed (each gated by `mayMutate()`).
  - Read-only open: do NONE of steps 2–5 (no mutation); may `get(ownerKey)` to validate if present, never `putIfAbsent`.
- Produces: a `Store` whose `writer_epoch` (durable monotone) is the value carried in the watermark + the manifest `writer_instance_id` field this phase; `mayMutate()` reflects the live mount; `Store::writerEpoch()` accessor.

> **Bridge note {#bridge}:** Phase 0 makes the watermark + the `writer_instance_id`-field epoch the **durable monotone** `writer_epoch` (dropping the random `process_epoch` for identity). Phase 2's epoch-aware sweep reads this value; Phase 3 only renames the field + adds `manifest_ordinal`.

- [ ] **Step 1: Failing integration tests:**

```cpp
TEST(CasMountStartup, SecondServerSameRootFailsClosed)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    auto s1 = Store::open(b, PoolConfig{.pool_prefix="p", .root_shards=1, .server_id=UInt128(1), .server_root_id="r"});
    // a second server (different uuid) on the SAME server_root_id + same backend → fail closed:
    EXPECT_THROW(Store::open(b, PoolConfig{.pool_prefix="p", .root_shards=1, .server_id=UInt128(2), .server_root_id="r"}),
                 DB::Exception);
}

TEST(CasMountStartup, WriterEpochStrictlyIncreasesAcrossReopen)
{
    auto b = std::make_shared<InMemoryBackend>(); Layout l("p");
    auto s1 = Store::open(b, PoolConfig{.pool_prefix="p", .root_shards=1, .server_id=UInt128(1), .server_root_id="r"});
    const uint64_t e1 = s1->writerEpoch();
    s1.reset();   // simulate shutdown (mount lease lapses; owner+epoch sticky)
    // same server reopen → reclaims (owner matches; for the test the lease is treated expired) and gets a higher epoch:
    auto s2 = Store::open(b, PoolConfig{.pool_prefix="p", .root_shards=1, .server_id=UInt128(1), .server_root_id="r"});
    EXPECT_GT(s2->writerEpoch(), e1);
}
```
(The same-server-reopen test may need the lease modeled as expired — drive via the injectable `now_ms_fn` or a test hook; if the live-lease guard blocks immediate reopen, advance the simulated clock past the TTL in the test.)

- [ ] **Step 2: Run → fail.**
- [ ] **Step 3: Implement** the startup block + `Store::writerEpoch()` accessor; replace the `WatermarkKeeper` epoch arg with the allocated durable `writer_epoch`.
- [ ] **Step 4: Run → pass** (`--gtest_filter='CasMount*:CasServerRoot*'`); also run `--gtest_filter='Cas*:Ca*'` to confirm no regressions (existing tests that call `openStoreForTest` now need a `server_root_id` — **update `openStoreForTest` in `cas_test_helpers.h` to set `.server_root_id="test"`** so the broad suite still builds/passes; this is a required companion edit).
- [ ] **Step 5: Commit.**

---

### Task 8: Read-only open + actionable error text + `openStoreForTest` companion {#task-8}

**Files:**
- Modify: `src/Disks/.../Core/CasStore.cpp` (read-only path; finalize error text)
- Modify: `src/Disks/tests/cas_test_helpers.h` (`openStoreForTest` sets `server_root_id`)
- Test: `src/Disks/tests/gtest_cas_mount.cpp`

- [ ] **Step 1: Failing tests** — read-only open over a pool owned by another uuid does NOT throw and does NOT mutate (no owner/epoch/mount writes); the `LiveDoubleStart`/`ForeignOwner` error message contains `server_root_id`, the existing mount's `hostname`/`pid`, and the three remediation lines (assert substrings).
- [ ] **Step 2: Run → fail.**
- [ ] **Step 3: Implement** the read-only branch (validate-not-mutate) + the error text (spec §mount-safety verbatim).
- [ ] **Step 4: Run → pass; full suite `Cas*:Ca*` green.**
- [ ] **Step 5: Commit.**

---

## Self-Review

**Spec coverage (Phase 0 only):** server_root_id required+validated+immutable → Tasks 2,8. ServerUUID demotion → Task 2,7. Single owner object (putIfAbsent + empty-root precondition + foreign-fail) → Task 4. Sticky epoch / durable monotone writer_epoch / missing→CORRUPTED_DATA → Tasks 3,4. Mount lease (same-uuid-expiry reclaim, live→fail, foreign→fail) → Task 5. Local write fence (cached id + monotonic deadline + lost flag, no per-write S3) → Task 6. Startup protocol order + error text + read-only → Tasks 7,8. Watermark relocation + epoch bridge → Task 7. TLA+ gate (all 5 invariants + sabotages) → Task 1. Phases 1–3 are explicitly out of scope (own plans).

**Placeholder scan:** Tests show real code; the two spots with "(adjust …)" / "(pick a real entrypoint)" are bounded by an objective pass condition (the test must compile against the real mutate entrypoint / the real roots key) — the implementer resolves them against concrete signatures, not a TODO. The codec/CAS-bump tasks reference established in-repo patterns (`encodeServerWatermark`, `acquireOrRenewLease`) by exact file:line rather than re-printing them.

**Type consistency:** `server_root_id:String`, `writer_epoch:uint64_t`, `MountClaimResult::{Claimed,LiveDoubleStart,ForeignOwner}`, `MountFence`/`mayMutate`/`tripMountLost`, `claimOwnerOrThrow`/`allocateWriterEpoch`/`serverRootSubtreeEmpty`/`claimMount`/`MountLeaseKeeper` — consistent across Tasks 2–8.

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-29-cas-layout-phase0-mount-safety.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — fresh subagent per task, spec+quality review between tasks, fast iteration. Task 1 (TLA+ gate) MUST pass before any code task.

**2. Inline Execution** — execute in this session via executing-plans, batch with checkpoints.

**Which approach?** (And note: Phases 1–3 get their own plans after Phase 0 lands.)

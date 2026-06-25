# CAS Build-Heartbeat Removal — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Delete the per-build heartbeat object (`builds/<build_id>`) and all its machinery — it is written but never read; in-flight sparing is already done by the watermark `min_active` + precommit-set.

**Architecture:** Staged deletion. Task 1 = pre-flight confirmations (no code). Task 2 = unwire `Build`/`Store` from `HeartbeatKeeper` (keep the files, fix test call sites) → build+sweep green. Task 3 = delete the now-unreferenced heartbeat code (files, `builds/` namespace, `HeartbeatProto`/`CAHB`/`FormatId::Heartbeat`, the `CasEventType::Heartbeat` enumerator, the `builds/` instrumentation, the dedicated gtests) → build+sweep+grep-gate green. Task 4 = rename the watermark-shared config keys (`heartbeat_period`→`watermark_renew_period`, `background_heartbeats`→`background_watermark`) + purge residual heartbeat naming (keep only GC-lease `gc/hb`). Task 5 = the load-bearing `min_active` sparing test. Task 6 = docker-safe scoped soak.

**Tech Stack:** C++ (ClickHouse), protobuf, gtest (`unit_tests_dbms`), ninja. Allman braces, `-Werror`.

**Branch:** `cas-vfs-path-mapping` (not master; new commits only, no amend/rebase).

**Spec:** `docs/superpowers/specs/2026-06-26-cas-build-heartbeat-removal-design.md`.

**Scope guards / KEEP (do NOT touch):** `ServerWatermark` + `WatermarkKeeper`; `SingleWriterSlot` (watermark derives from it); the build_id mint + `retireBuildSeq`/`min_active` machinery; the GC-lease pulse `gc/hb` (`GcHeartbeat`, `CasEventType::GcLeaseHeartbeat`); and `PoolConfig::heartbeat_period` + `background_heartbeats` (**watermark-shared** — `CasStore.cpp:114-115`). Pre-release: clean delete, no migration, no compat scaffolding.

**Build & test conventions (every task):**
- Build dir `/home/mfilimonov/workspace/ClickHouse/master/build`. Build: `cd build && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1` — **no `-j`, no `nproc`**; check `tail -5` for a `[N/N]` link line + no `error:`.
- Full sweep: `build/src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > build/cas_hbrm_sweep.log 2>&1`. The ONLY tolerated red is the baseline `CaWiringOps.FreezeViaHardLinksIntoShadow`.
- Proto change (Task 3) needs `cd build && cmake .` once before `ninja`.

---

## Removal surface (grounded)

- `Core/CasHeartbeat.{h,cpp}` — `Heartbeat`, `encode/decodeHeartbeat`, `HeartbeatKeeper` (whole files).
- `Core/CasBuild.{h,cpp}` — ctor param + `heartbeat` member (`.h:34,186`); `renewHeartbeat()` test hook (`.h:99`); `heartbeat->renewOnce()` (`.cpp:107`); slow-op sanity block (`.cpp:949-956`); `abandon()`'s `stopBackground()`+`discard()` (`.cpp:1109-1110`, KEEP `retireBuildSeq`). The `startBuild`/`abandon` event *text* mentioning heartbeat (reword, don't delete the events).
- `Core/CasStore.{h,cpp}` — `createBuild` keeper construction (`.cpp:267`) + its `startBackground` (`.cpp:269-270`); `Build` ctor call drops the keeper arg. KEEP `heartbeat_period`/`background_heartbeats`.
- `Core/CasLayout.h` — `buildHeartbeatKey` (`:181`) + the `builds/` doc line (`:30`).
- `Core/Proto/cas_root_shard.proto` — `HeartbeatProto`.
- `Core/CasFormat.{h,cpp}` — `FormatId::Heartbeat=10` (`.h:29`); its `magicFor` arm `0x42484143` "CAHB" (`.cpp:61`); its `changePoints` arm (`.cpp:40`). Renumber trailing `FormatId` enumerators (RootsRegistry/GcOutcomes) to stay contiguous.
- `Core/CasEvent.{h,cpp}` — the `Heartbeat` enumerator (`.h:25`) + its `toString` arm (`.cpp:58`); repoint the `CasEvent::type` default (`.h:45`) to `BlobPut`. KEEP `GcLeaseHeartbeat`.
- `Core/CasInstrumentedBackend.{h,cpp}` — the `/builds/` key-routing branch + the `CasBuild*` ProfileEvents (`CasBuildPut`/`Dedup`/`Overwrite`/`Cas`/`CasConflict`/`Head`/`HeadMiss`/`Get`/`Delete`/`List`), incl. their central definitions in `src/Common/ProfileEvents.cpp`.
- The dedicated heartbeat gtests (discovered via grep in Task 3).

---

### Task 1: Pre-flight confirmations (no code change, no commit)

**Files:** none — verification only. These three facts are the foundation of the deletion; confirm them, do not re-derive.

- [ ] **Step 1: Confirm `abandon()`'s seq-retire is NOT entangled with the heartbeat**

Run: `sed -n '1106,1112p' src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp`
Expected: `store->retireBuildSeq(build_seq)` is a SEPARATE statement from `heartbeat->stopBackground()`/`heartbeat->discard()`. Conclusion: removing the two heartbeat calls leaves seq-retire (which feeds `min_active`) intact.

- [ ] **Step 2: Confirm the heartbeat has NO live reader**

Run: `grep -rn "decodeHeartbeat\|buildHeartbeatKey\|\"builds/\"\|/builds/" src/Disks/ --include=*.cpp --include=*.h | grep -v -E "gtest|tests/|encodeHeartbeat|HeartbeatKeeper|buildHeartbeatKey\(.*const|String buildHeartbeatKey|CasInstrumentedBackend|///|//"`
Expected: no live *reader* (only the writer-side keeper + the layout key definition). Conclusion: deleting it changes no GC decision.

- [ ] **Step 3: Confirm the config keys are watermark-shared (must be KEPT)**

Run: `grep -rn "background_heartbeats\|heartbeat_period" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`
Expected: `CasStore.cpp:114-115` uses them for `watermark->startBackground`. Conclusion: KEEP both config keys; only remove the heartbeat-keeper's own `startBackground` at `CasStore.cpp:269-270`.

- [ ] **Step 4: Confirm `FormatId::Heartbeat` is not persisted beyond the (removed) `CAHB` magic**

Run: `grep -rn "FormatId::Heartbeat\|= 10" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasFormat.cpp`
Expected: the enum value drives only the in-memory `magicFor`/`changePoints` tables; the only on-disk artifact is the `CAHB` magic (removed with the codec). Conclusion: renumbering trailing enumerators is safe.

(No commit — this task records the green light for Tasks 2-3.)

---

### Task 2: Unwire `Build`/`Store` from the `HeartbeatKeeper` (keep files)

**Files:** `Core/CasBuild.h`, `Core/CasBuild.cpp`, `Core/CasStore.cpp`, plus any test that constructs `Build` directly with a keeper or calls `renewHeartbeat()` (discovered via build errors).

- [ ] **Step 1: Drop the keeper from `Build` (`CasBuild.h`)**

Remove the `std::unique_ptr<HeartbeatKeeper> heartbeat_` ctor parameter (`:34`), the `heartbeat` member (`:186`), and the `void renewHeartbeat();` test hook (`:99`). Remove the `#include` of `CasHeartbeat.h` if present. The ctor becomes `Build(StorePtr store_, UInt128 build_id_, ...)` (drop only the keeper arg; keep all others).

- [ ] **Step 2: Remove the keeper calls in `CasBuild.cpp`**

- ctor init list (`:68`): drop `, heartbeat(std::move(heartbeat_))`.
- `startBuild` (`:107`): delete `heartbeat->renewOnce();`; reword the `BuildStart` event reason (`:82`) from "heartbeat durable; build in-flight" to "build in-flight".
- slow-op sanity block (`:949-956`): delete the whole `if (heartbeat) { ... renewOnce(); }` block.
- `abandon()` (`:1109-1110`): delete `heartbeat->stopBackground();` and `heartbeat->discard();`. **KEEP** `store->retireBuildSeq(build_seq);` and the rest. Reword the `BuildAbort` event reason (`:1120`) from "heartbeat discarded; uploads become reclaimable debris" to "build abandoned; uploads become reclaimable debris (reaped by full GC via min_active)".
- delete the `renewHeartbeat()` definition if it exists in the .cpp.

- [ ] **Step 3: Stop constructing the keeper in `Store::createBuild` (`CasStore.cpp`)**

Delete the `auto keeper = std::make_unique<HeartbeatKeeper>(...)` (`:267`) and its `if (config.background_heartbeats) keeper->startBackground(config.heartbeat_period);` (`:269-270`). Change the `make_shared<Build>(...)` (`:272`) to drop the `std::move(keeper)` argument. **Do NOT** touch the watermark `startBackground` at `:114-115`.

- [ ] **Step 4: Fix test call sites (build-driven)**

Run: `cd build && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1; tail -30 cas_hbrm_build.log`
For each compile error in `src/Disks/tests/*`: a test that constructs `Build(...)` with a keeper arg, or calls `renewHeartbeat()`, or references `Build::heartbeat` — fix it to the new ctor / delete the heartbeat-specific assertion. Tests that go through `store->createBuild(...)` are unaffected (the factory hides the change). Dedicated heartbeat-codec/keeper tests are left for Task 3 (they still compile because `CasHeartbeat.{h,cpp}` still exist).

- [ ] **Step 5: Build + sweep + commit**

Run: `cd build && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1; tail -3 cas_hbrm_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_hbrm_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_hbrm_sweep.log | tail -6`
Expected: clean build; only the baseline red. `CasHeartbeat.{h,cpp}` now compile but are unreferenced by production code.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp
# plus any test files touched in Step 4
git commit -m "CA: unwire Build/Store from the build HeartbeatKeeper (no behavior consumer)

Build no longer constructs/renews/discards the builds/<build_id> heartbeat;
abandon() keeps retireBuildSeq (the min_active feed) and only drops the
heartbeat calls. Watermark startBackground + heartbeat_period/background_
heartbeats config (watermark-shared) untouched. CasHeartbeat.{h,cpp} are now
unreferenced by production code; deleted in the next commit.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Delete the heartbeat code, namespace, format, events, instrumentation

**Files:** `Core/CasHeartbeat.{h,cpp}` (delete); `Core/CasLayout.h`; `Core/Proto/cas_root_shard.proto`; `Core/CasFormat.{h,cpp}`; `Core/CasEvent.{h,cpp}`; `Core/CasInstrumentedBackend.{h,cpp}`; `src/Common/ProfileEvents.cpp`; the dedicated heartbeat gtests.

- [ ] **Step 1: Delete the codec files**

`git rm src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasHeartbeat.cpp`

- [ ] **Step 2: `CasLayout.h`** — delete `buildHeartbeatKey` (`:181`) and the `///   - build heartbeats: POOL/builds/S/BUILD_ID` doc line (`:30`).

- [ ] **Step 3: proto** — in `cas_root_shard.proto`, delete the `HeartbeatProto` message. Update the file-header magic table comment to drop the `HeartbeatProto "CAHB"` entry.

- [ ] **Step 4: `CasFormat`** — `.h`: delete `Heartbeat = 10,` and renumber `RootsRegistry`/`GcOutcomes` to stay contiguous (e.g. `RootsRegistry = 10, GcOutcomes = 11`). `.cpp`: delete the `case FormatId::Heartbeat:` arms in `magicFor` (`:61`) and `changePoints` (`:40`). Update the `gtest_cas_format.cpp` `ChangePointsExistForEveryClass`/magic tests to drop `Heartbeat`.

- [ ] **Step 5: `CasEvent`** — `.h`: delete the `Heartbeat,` enumerator (`:25`); change the `CasEvent::type` default (`:45`) from `CasEventType::Heartbeat` to `CasEventType::BlobPut`. `.cpp`: delete the `case CasEventType::Heartbeat: return "heartbeat";` arm (`:58`). KEEP `GcLeaseHeartbeat`.

- [ ] **Step 6: `CasInstrumentedBackend` + `ProfileEvents.cpp`** — delete the `if (key.find("/builds/") ...)` routing branch; delete the `CasBuild*` ProfileEvents (extern decls + usages in `CasInstrumentedBackend.{h,cpp}` AND their central definitions in `src/Common/ProfileEvents.cpp` — grep `CasBuild` to find both).

- [ ] **Step 7: Delete the dedicated heartbeat gtests**

Run: `grep -rln "HeartbeatKeeper\|decodeHeartbeat\|encodeHeartbeat\|buildHeartbeatKey\|HeartbeatProto" src/Disks/tests/`
For each hit: delete the heartbeat-specific test case(s) (round-trip, keeper, codec). If a test file is heartbeat-only, `git rm` it. Do NOT delete unrelated tests in a shared file.

- [ ] **Step 8: Build (regen proto) + sweep + grep gate + commit**

Run: `cd build && cmake . && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1; tail -5 cas_hbrm_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_hbrm_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_hbrm_sweep.log | tail -6`
Expected: clean; only the baseline red.

Grep gate: `grep -rn "Heartbeat\|heartbeat\|builds/" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v -E "GcHeartbeat|gc/hb|GcLeaseHeartbeat|gc_lease_heartbeat" | grep -v tests`
Expected: **empty** (the only surviving heartbeat references are the GC-lease ones, which are explicitly kept). If anything else remains, remove it before committing.

```bash
git add -A src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ src/Common/ProfileEvents.cpp src/Disks/tests/
git commit -m "CA: delete the build heartbeat (builds/ namespace, HeartbeatProto/CAHB, events, metrics)

Remove CasHeartbeat.{h,cpp}, buildHeartbeatKey + the builds/ namespace,
HeartbeatProto + the CAHB magic + FormatId::Heartbeat (renumbered), the
CasEventType::Heartbeat enumerator (default repointed to BlobPut), the /builds/
instrumentation + CasBuild* ProfileEvents, and the dedicated heartbeat gtests.
In-flight sparing is unaffected (watermark min_active + precommit). Kept:
watermark, SingleWriterSlot, seq machinery, build_id, GC-lease gc/hb. Pre-release
clean delete, no migration.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Rename the watermark config keys + purge residual heartbeat naming

**Files:** `Core/CasStore.h`, `Core/CasStore.cpp`, `ContentAddressedMetadataStorage.cpp`, `Core/CasSingleWriterSlot.h`. **The keys are code-set, not XML-parsed — no config XML changes.** Do NOT touch the GC-lease names (`GcHeartbeat`, `gc/hb`, `GcLeaseHeartbeat`, `gc_lease_heartbeat`).

- [ ] **Step 1: Rename the `PoolConfig` keys (`CasStore.h:48-49`)**

`std::chrono::milliseconds heartbeat_period{5000};` → `std::chrono::milliseconds watermark_renew_period{5000};`
`bool background_heartbeats = false;` → `bool background_watermark = false;`
Update the `:30` comment `/// provenance + heartbeats` → `/// provenance` and the `:110` comment referencing `background_heartbeats` → `background_watermark`.

- [ ] **Step 2: Update the usages**

`CasStore.cpp:114-115`: `if (config.background_watermark) store->watermark->startBackground(config.watermark_renew_period);`
`ContentAddressedMetadataStorage.cpp:355`: `pool_config.background_watermark = (context != nullptr) && !read_only;`
`ContentAddressedMetadataStorage.cpp:298` comment: reword "run no heartbeats" → "run no background watermark renewal".

- [ ] **Step 3: Simplify `CasSingleWriterSlot.h` doc (now a single user)**

The class doc (`:16-45,84`) contrasts "watermark vs heartbeat" as two users. After the heartbeat is gone, the slot has ONE user (the watermark). Reword the doc to drop the heartbeat contrast (e.g. the `slot_name_`/`terminal_verb_` are still generic, but the examples should reference only the watermark). Keep the class generic (no functional change) — this is a comment-only cleanup.

- [ ] **Step 4: Build + sweep + grep gate + commit**

Run: `cd build && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1; tail -3 cas_hbrm_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_hbrm_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_hbrm_sweep.log | tail -6`
Expected: clean; only the baseline red.

Grep gate: `grep -rni "heartbeat" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ | grep -v -E "GcHeartbeat|gc/hb|GcLeaseHeartbeat|gc_lease_heartbeat"`
Expected: **empty** — the only surviving "heartbeat" mentions are the GC-lease ones.

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasSingleWriterSlot.h
git commit -m "CA: rename watermark config keys + purge residual heartbeat naming

heartbeat_period -> watermark_renew_period, background_heartbeats ->
background_watermark (they drive the watermark renewer, not a heartbeat).
Reword CasSingleWriterSlot doc to its single remaining user (watermark) and the
'run no heartbeats' comment. Keep only the GC-lease names (gc/hb,
GcLeaseHeartbeat). Code-only rename; keys are code-set, no config XML.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Confirm in-flight sparing coverage (the load-bearing test)

**Files:** a CA GC/build gtest (extend an existing one or add a case) — likely `src/Disks/tests/gtest_cas_build.cpp` or `gtest_cas_gc_round.cpp`.

- [ ] **Step 1: Find the existing precommit-reclaim / min_active sparing test**

Run: `grep -rln "min_active\|retireBuildSeq\|precommit.*reclaim\|reclaim.*precommit" src/Disks/tests/`
Read the closest test. If it already asserts "a build's blobs are spared while `build_seq >= min_active` and reclaimed after the seq retires", no new test is needed — note it and proceed.

- [ ] **Step 2: If coverage is thin, add the sparing test**

Add a test (matching the fixture style of the file found) that: creates a build, uploads a blob, does NOT publish, asserts GC does NOT delete the blob while the build's seq is `>= min_active`; then retires the seq (abandon/`retireBuildSeq`), asserts GC reclaims it. This proves the heartbeat was not load-bearing for sparing.

- [ ] **Step 3: Run + commit (if a test was added)**

Run: `cd build && ninja unit_tests_dbms > cas_hbrm_build.log 2>&1; tail -3 cas_hbrm_build.log && ./src/unit_tests_dbms --gtest_filter='Cas*:Ca*' > cas_hbrm_sweep.log 2>&1; grep -E '\[  PASSED  \]|\[  FAILED  \]' cas_hbrm_sweep.log | tail -6`
Expected: clean; only the baseline red.

```bash
git add src/Disks/tests/
git commit -m "CA: test in-flight blob sparing via min_active (heartbeat-independent)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```
(Skip the commit if Step 1 found adequate coverage.)

---

### Task 6: Docker-safe scoped soak (final validation)

**Files:** none — validation only. **DOCKER SAFETY:** another debug session owns containers on this host (compose project `archeology`, container `archeology-clickhouse-1` holds host port 8123). NEVER touch foreign containers; only the `ca-soak` compose project; no `docker system prune`, no unscoped `down`. The harness scripts are scoped to project `ca-soak`. If host port 8123 is occupied, the soak cannot bring up ch1 (known env block) — record and skip rather than fight it.

- [ ] **Step 1: Run the short scoped soak** (if port 8123 is free)

Run the `utils/ca-soak` smoke/phase-1 (scoped to project `ca-soak`), redirecting to a log under `utils/ca-soak/tmp/`. After it settles, assert via the metrics/`system.content_addressed_*`:
- the `builds/` prefix is never created (LIST `roots/`/pool shows no `builds/` keys; `CasBuild*` op counters are gone);
- the aggregate no-loss oracle (`compare_aggregates`, model==node1==node2) stays green (no in-flight over-deletion);
- overall request volume on the dropped namespace is zero.

- [ ] **Step 2: Record results in the work log** (`docs/superpowers/cas-unattended-work-log-2026-06-24.md`) and the backlog B203 entry; if port 8123 is blocked, record "soak deferred — env port conflict" (build + full gtest sweep are the validation to date). No commit needed beyond the log update.

---

## Self-Review (performed inline)

**Spec coverage:** removal map → Tasks 2-3 (every bullet mapped: CasHeartbeat, Build/Store wiring, layout, proto, CasFormat, CasEvent, instrumentation, gtests). The one invariant (in-flight sparing via min_active) → Task 4. Keep-list (watermark/SingleWriterSlot/seq/build_id/gc-hb/config keys) → scope guards + Task 2 Step 3 note. Pre-flight (abandon entanglement, FormatId, config-shared) → Task 1. Grep gate + sweep → Tasks 3/4. Soak → Task 5. ✓
**Placeholder scan:** no TBDs; every edit cites file:line + the concrete change; test fallout is build-driven with explicit grep + rules (mirrors the pack-removal/envelope plans). The CasEvent default repoint and FormatId renumber are spelled out. ✓
**Type/consistency:** `Build` ctor change is consistent across `.h`/`.cpp`/`Store::createBuild`/tests; `FormatId` renumber is contiguous; `CasEventType::BlobPut` is a real enumerator (first in the enum); KEEP-list (`GcLeaseHeartbeat`, watermark config) consistent between scope-guards and the grep gate's exclusions. ✓

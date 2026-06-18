# B140-dangle — handoff dossier (start-here to fix the CA-GC data-loss bug)

**Date:** 2026-06-18 · **Branch:** `cas-mergetree-poc` · **Repo:** `/home/mfilimonov/workspace/ClickHouse/master`
**Status:** root cause **PINNED** (ground-truth decode). Fix **NOT started**. This file is everything needed to get up to speed fast and start fixing. Minimum speculation: each claim is tagged **[CERTAIN]**, **[STRONG]**, **[REJECTED]**, or **[OPEN]**.

---

## 0. TL;DR

In content-addressed (CA) MergeTree, GC **deleted a blob that a live part still references** → `fsck` reports `dangling` = **data loss** (INV-NO-LOSS violation). Root cause **[CERTAIN]**: a **content-shared / deduplicated** blob `B` was referenced by two part-trees, but GC's in-degree snapshot (`gc/snap`) only ever recorded **one** of them. When that one parent tree was stripped, GC computed `inDeg(B)=0` and deleted `B` — while a *different, live* part still references the same content. It is a **cross-node** dedup-vs-GC under-count. It is **NOT P9** (P9 is done and exonerated). The earlier "marker-without-edges" hypothesis was **REJECTED** by decoding real snapshots.

This is the **dangling (data-loss) sibling of B140** (the known, deferred *leak* variant). It is tracked as **`B140-dangle`** in the backlog.

---

## 1. Current state of the tree (what's committed)

- **P9 (separate, prior task): DONE, committed, verified** — eliminated the GC 404-HEAD storm. Commits `86db7fb3e0a`, `4a787a80f0c`, `03e11643db3`, `3243c6aec96`, `6d6c10eff33`, `ec5a41e7f91`, `9275d94607d`. The whole B140-dangle investigation conclusively exonerated P9. Memory: the auto-memory `project_p9_gc_snap_prune.md`.
- **B140-dangle commits (most recent first):**
  - `404aacbb1c9` — **PRODUCER PINNED** (VERDICT B); updated backlog + spec banner.
  - `70433ba6178` — ground-truth correction (marker-without-edges REFUTED).
  - `f5d84d61e0f` — fix spec (now **SUPERSEDED** — its mechanism + fix are wrong).
  - `c976d1741e3` — failing repro gtest + reuse-gate gtest + backlog entry.
  - `0fb10a9548a` — **`CAGCDEL`/`CAREUSE` audit instrumentation** (kept on purpose).
- **Working tree is clean** (only the usual untracked `contrib/*` submodule markers + scratch files in repo root). Cluster torn down; disk free (~414G).

---

## 2. Background you must know (the CA GC model)

Three object kinds in the pool (all keyed under `<pool>/`, here `soak_pool/`):
- **blob** `blobs/xx/<hash>` — file content, addressed by content hash ⇒ identical content = **one** physical blob (**dedup**).
- **tree** `trees/xx/<hash>` — a part's manifest: lists files, each referencing a blob by hash. Edge `tree → blob`.
- **ref/root** in `roots/<ns>/<shard>` manifests — `ref_name (part name) → tree_id`. Edge `root → tree`.

**GC** keeps an **in-degree snapshot** `gc/snap/<generation>/<shard>`:
- `inDeg(tree)` = # of `root→tree` edges; `inDeg(blob)` = # of `tree→blob` edges.
- Built by **`fold`** (svёртка): read the manifest **journal** (`Add(ref→tree)` / `Remove(ref)`); on `Add` add the root edge + **expand the tree once** (`isExpanded` guard → read the tree object → add all `tree→blob` edges → `markExpanded`).
- **Candidate for deletion** = `inDeg==0` AND in `known` (`zeroInDegreeKnown`). Pipeline: retire → fence → recheck (fold-through-fence) → **single content-delete site** (exact-token).
- When a tree is deleted, cascade **`stripTree`** removes its outgoing `tree→blob` edges (decrementing children's in-degree) **and** clears its marker, atomically.

`gc/state` holds `{round, snap_generation, snap_shards(=1), fence_seq, folded_cursor[per-shard], fence_version[round][per-shard]}`. Snap generations are **write-once** (putIfAbsent + byte-equal adoption + probe-upward).

**Key design fact:** the snap stores edges per generation, but **does NOT store the fold cursor** in its bytes. `snap_shards==1` is enforced (fail-closed). 64 **root** shards (`content_addressed_root_shards=64`) for the ref/manifest layer; the blob in-degree lives in the single snap shard 0.

---

## 3. The bug — exact mechanism (VERDICT B) **[CERTAIN, ground-truth-decoded]**

`B` is a **dedup-shared** blob referenced by **two** distinct part-trees: `T_live` (an older part) and `T_cur` (a newer, still-live part). True in-degree of `B` = 2.

Ground-truth trace from decoding the **actual delete-generation snapshots** (gens 1280/1281; `B=ff647af8…`, `T_live=74db26ba…`):
1. **gens 1260–1279 (healthy):** GC folded the old part ⇒ `T_live` is live + `markExpanded`, edge `T_live→B` present, `inDeg(B)=1`. **The edge `T_cur→B` was never in the GC snapshot** (the live part's reference to the shared blob was never represented in GC's in-degree). So GC's view of `inDeg(B)` = 1, true value = 2.
2. **gen 1280:** the old part merges away ⇒ `root→T_live` removed ⇒ `T_live` becomes a zero-in-degree *tree* (its child edge to `B` still present, so `B` still held) ⇒ `T_live` (the tree) retired + deleted.
3. **gen 1281:** cascade `stripTree(T_live)` drops `T_live→B` ⇒ GC sees `inDeg(B)=0` ⇒ `B` retired + **deleted** (exact-token).
4. **`B` is still referenced** by the live part via `T_cur→B`, but that edge was never in GC's snapshot. So GC "correctly" (by its own undercount) deleted a reachable blob.
5. **`fsck`** (authoritative walk of live refs: `resolveRef→readTree→blob`, then HEAD) later finds the live ref → missing `B` ⇒ **`dangling`**.

Verified for **all 304** active (non-detached) live dangles in the decode: at delete time `B` is `known=Y`, `inDeg=0`, **zero** `tree→B` edges; the historical `T_live` is fully gone. **243/243 VERDICT B, 0 VERDICT A.**

**The crisp framing:** in reality there are **two** reachability pictures — (1) GC's in-degree snapshot (`gc/snap`, what GC deletes by) and (2) the authoritative manifest (`roots/`, what readers/`fsck` use). `dangling` is precisely a **divergence**: a live edge exists in (2) but not in (1). Dedup is the necessary condition (a blob with one parent is freed correctly). Cross-node is where the second edge is lost (writer node2 dedup-references the shared blob; GC-leader node1's in-degree never folds that edge before deleting).

---

## 4. What we know FOR CERTAIN

- **[CERTAIN]** It is real data loss reproduced **3×** in the 2-node soak: `dangling=55`, `=25`, `=31` — always at the `gc_checkpoint` stage (~64 min in, seed 20260617), **pre-chaos** (`last_fault=null`).
- **[CERTAIN]** GC deleted the dangling blobs: in the dangling=31 run, **31/31** dangling hashes appear in ch1's `CAGCDEL` audit log (`outcome=Deleted`). ch2 has **0** `CAGCDEL` (single GC leader, by design).
- **[CERTAIN]** The blobs are **content-shared** and referenced by **live and/or detached-broken** parts (`fsck --detail` `reachable_from`).
- **[CERTAIN]** Real snapshots have **zero** `markExpanded` trees missing their child edges (`markers_with_ZERO_child_edges = 0` in every decoded generation) — so the "marker-without-edges" mechanism does not occur.
- **[CERTAIN]** The mechanism is the shared-blob in-degree **under-count** described in §3 (decoded per-blob, per-generation).
- **[CERTAIN]** **NOT P9.** P9 changes `everEdged` *pruning*; this is fold *edge accounting*. P9's GC counters were textbook (`forgotten_on_delete == objects_deleted`, `forgotten_absent == 0`) on the failing runs too.
- **[CERTAIN]** The **local** (single-process) reuse-vs-GC publish gate is correct/fail-closed: gtest `CasReuseGcRace.ReuseOfBlobDeletedBeforePublish` **PASSES**.

---

## 5. What was REJECTED (don't re-derive these)

- **[REJECTED] Marker-without-edges** (a live `markExpanded` tree missing its child edges). Refuted by ground-truth decode (`markers_with_ZERO_child_edges=0`). The committed gtest `CasGcDangle` injects *this* state, so **it tests the WRONG mechanism** and must be rewritten (see §9).
- **[REJECTED] Cross-generation marker incoherence / fold-watermark fix** (the SUPERSEDED spec's proposal). Refuted: snap generations are write-once + byte-equal-adopted, the cascade clears the marker and advances the cursor in the **same** CAS with a fold-through-fence recheck. A faithful TLA+ model exhausts **9.1M states with no dangle**.
- **[REJECTED] Single-process fold bug (candidate A).** The once-per-tree expansion records `T→B` correctly in every healthy generation; `displaced_later` skip never drops a *live* tree's edge (it fires only when the ref was re-pointed, else fail-closes with `FILE_DOESNT_EXIST`). Single- and dual-`Gc`-instance gtests over the real APIs **cannot reproduce** it faithfully (0/8000 with an honest competing-round delete; apparent fails were artifacts of deleting a still-live tree).
- **[REJECTED] Fence/cursor single-Add race (candidate B-narrow), in the modeled form.** Code-vs-model audit: the recheck folds-through-fence for **every** root shard of **every** fence-time-registry namespace; the fence uses the **fence-time** (not fold-time) universe; relink/`adoptTree` goes through the same gate; the evidence-freshness handshake matches the model's `EvOK` and is sound given `ViewableRound` (which `RetireView::refresh` provides). So the modeled cross-node fence race is closed. The real producer is the shared-blob under-count, whose exact loss-of-edge point is **[OPEN]** (§6).

---

## 6. The ONE open question (resolve first — it picks the fix)

**[OPEN]** Was the live part's publish/dedup of `B` **before** or **after** GC deleted `B`?
- **BEFORE** (writer dedup-HEAD'd `B` while present; GC deleted it; the publish committed the ref anyway): the **cross-node publish-gate / retire-view freshness** failed to see the condemnation → fix is in the writer-side gate / retire-view cross-node coverage.
- **AFTER** (writer dedup-HEAD'd a blob that was about to be / had just been deleted): the **dedup HEAD-then-reference window** → fix is in the dedup path.

Either way the invariant to restore is: **a dedup-shared blob must not be deleted while ANY live ref (including a not-yet-folded cross-node publish) references its content.** To answer: compare the live part `T_cur`'s `Add` `at_version` (from the captured/regenerated `roots/` manifest) against the delete round's `fence_version`/`folded_cursor` and the blob's `CAGCDEL round`.

---

## 7. Where everything is

### Specs / design
- **SUPERSEDED fix spec** (read the top banner — mechanism + fix are wrong, but the *pinned producer* is recorded there): `docs/superpowers/specs/2026-06-17-ca-b140-dangle-fix-design.md`
- P9 spec/plan (context): `docs/superpowers/specs/2026-06-17-ca-gc-snap-prune-design.md`, `docs/superpowers/plans/2026-06-17-ca-gc-snap-prune.md`
- Original CA incarnation-store design (the protocol): `docs/superpowers/specs/2026-06-10-ca-incarnation-store-design.md` (§7 regular GC, §8 full GC, invariants INV-NO-LOSS/NO-RETURN/NO-DANGLE/OVER-COUNT-ONLY)

### Backlog
- `docs/superpowers/deferred_backlog/cas-mergetree-integration.md` — search **`B140`** (the leak variant + its full root-cause analysis) and **`B140-dangle`** (this bug; the entry has the pinned producer + the open question). Related: `B160` (GC lease/leadership churn = the cross-leader trigger), `B164` (manifest journal size), `B147` (snap O(pool) cost).

### TLA+ models (`docs/superpowers/models/`)
- **`CaIncarnationCore.tla`** — the canonical GC core model. `GFold` (atomic expand: `marker'` + `treeEdges'` together), single global `marker`/`treeEdges`/`cursor`, `GRecheckDelete`/`FoldedThroughFence`, `GFenceRegistry`/`GFenceShard`, invariants `INV_NO_DANGLE`/`INV_NO_LOSS`/`INV_NO_RETURN`. **Why it never caught B140-dangle:** it has ONE reachability structure used by both GC's delete decision and the `INV_NO_LOSS` check — GC's view and "truth" are identical by construction, so it cannot express the snapshot-vs-manifest divergence that *is* this bug. Its `Children[t]` is a derived uniform function (no real dedup of distinct trees sharing a physical blob via HEAD-before-PUT); no per-node resident snapshots; fold assumed faithful/complete.
- **`CaB140Dangle.tla`** + cfgs `CaB140Dangle_{safe,producer,blob,loss,adopt}.cfg` — Phase-1 model that reached the dangle **only via UNFAITHFUL arms** (`GStripTree` keeping the marker; `GAdoptGeneration` field-mixing). **Artifacts, not the producer.** Kept for contrast.
- **`CaB140DangleFaithful.tla`** + `CaB140DangleFaithful_shared.cfg` — faithful version (strip clears marker; whole-gen byte-equal adoption; faithful displaced_later/resident-snap/probe-upward/relink; two trees sharing a blob). **Exhausts 9.1M states clean** (non-vacuity verified; a control stripping a *live* tree's edges DOES violate INV_NO_LOSS, proving the harness can detect loss). These two `CaB140Dangle*` files + `CaB140Dangle_RESULTS.md` are **UNTRACKED** (not committed).
- **`CaB140Dangle_RESULTS.md`** — full Phase-1 writeup + traces.

### Sources (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/`)
- `Core/CasGc.cpp` — the GC. Key spots: `foldShardRecords` (~774-868: once-per-tree expansion `if(!isExpanded)` ~805, `markExpanded` ~865, the `displaced_later` skip ~830-843); `recheck` (~154-339, fold-through-fence ~182-209); `cascadeAndPersist` (~342+, `stripTree` + probe-upward persist + the marker-clear/cursor-advance CAS); `retire` (~601-701); **the single content-delete site** + the **`CAGCDEL` instrumentation** (~248-266); `fence`/registry-fence (~495-600); resident-snap reuse (~1021); lease/steal (`acquireOrRenewLease`).
- `Core/CasGcSnap.{h,cpp}` — `markExpanded`/`isExpanded`/`addEdge`(`known.insert`)/`addTreeEdge`/`addRootEdge`(last-op-wins)/`removeRootEdge`/`stripTree`(clears edges+marker)/`forget`(P9, erases from `known`)/`zeroInDegreeKnown`; snap codec `encode/decodeSnapFields` (note: **does NOT store the fold cursor**).
- `Core/CasBuild.cpp` — the writer. `observeAndAdmit` (~247: dedup reuse — HEAD, condemned-token check `retireView().isCondemnedToken` ~268 → `resurrect` or adopt) + **`CAREUSE` instrumentation**; `adoptTree`/relink (~339-377); `revalidateDeps` (~582-686, the gate re-HEAD); `gateCheckDeps` (~493-535); `publish` (~688-755, the gate inside `mutateShard`).
- `Core/CasRetireView.cpp` — the writer's view of condemned tokens; `refresh` reads `gc/state` round first then lists retired sets (provides the `ViewableRound` coverage).
- `Core/CasFsck.cpp` — `runFsck` (authoritative ref-walk; HEAD-confirms dangling at ~170-172; this is the oracle).

### Tests (`src/Disks/tests/`)
- **`gtest_cas_b140_dangle.cpp`** — `CasGcDangle.MarkedExpandedWithoutEdgesDeletesLivePinnedBlob`. **RED today**, but **injects the WRONG (rejected) marker-without-edges state**. **Must be rewritten** to the real shared-blob/dedup shape. Helpers in-file: `openTestStore`, `runGcToFixpoint`.
- `gtest_cas_gc_leak.cpp` — `CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` (the B140 **leak** variant, intentionally-RED M-F-deferred guard) **and** `CasReuseGcRace.ReuseOfBlobDeletedBeforePublish` (**PASSES** — local reuse gate is safe). Helpers: `openTestStore`, `publishPart2`, `runGcToFixpoint`, `runFsck`, `rawDeleteTree`.
- `gtest_cas_gc_round.cpp` — round-level GC tests; helpers `openTestStore`, `publishPart`, `rawDeleteTree`, `readSnap`, lease/steal tests use **two `Gc` instances on one Store** (the template for a cross-node repro). `idOf`/`u128Of`/`hexToU128` from `cas_test_helpers.h`.
- `gtest_cas_gc_snap.cpp` — `GcSnap` unit tests (where the temp debug-dump method for decoding goes).

### Soak harness (`utils/ca-soak/`)
- `scripts/run_keepalive.sh` — **3h keep-alive runner (NO teardown on failure — cluster stays UP for forensics)**. `scripts/run_24h.sh` — phase-3 runner (tears down). `scripts/disk_watchdog.sh` — host-safety (60G floor). `soak/run.py` — the phase-3 driver (stages warmup→steady→mutations→ttl_pressure→gc_checkpoint→chaos→cliff→converge; `gc_checkpoint` ~t+3812s is where it dangles). `soak/fsck.py` — wraps `clickhouse disks --disk ca_ro --query "fsck [--detail]"`.
- `docker-compose.yml` — 2 ClickHouse (`ca-soak-ch1-1` GC leader / `ca-soak-ch2-1`) + RustFS (`ca-soak-rustfs1-1`, S3 at `rustfs1:11121`, bucket `test`, creds clickhouse/clickhouse, pool `soak_pool/`) + Keeper. **Mounts the host binary** `../../build/programs/clickhouse` over `/usr/bin/clickhouse`. `configs/storage_conf.xml`: disks `ca` (rw) + `ca_ro` (ro fsck view); 64 root shards.

### Persisted forensic artifacts (PARTIAL — see §11 caveat)
- `utils/ca-soak/logs/p9_capture_FAILURE_dangling31.json`, `p9_capture_fsck.txt` (fresh fsck --detail with reachable_from), `p9_instr_FAILURE_dangling25.json`, `p9_instr_correlation.txt` (the 25-blob CAREUSE@round-vs-CAGCDEL@round token-match table), `p9_instr_fsck_detail.txt`.
- `/tmp/snapdump` (gens 671-673 + outcomes_340 + state — the *detached*-dangle era) and `/tmp/snapdump3` (gens 333-335, 664-666 + state + `roots/` manifests). **NB:** the **decisive live-dangle decode used gens 1280/1281, which were captured to `utils/ca-soak/tmp/snapdump_live/` by a subagent and are NOT persisted now** (cluster gone). Re-generate via a fresh soak.

---

## 8. How the bug was caught (so you trust the chain)

1. P9 soak (3h, chaos) failed at `gc_checkpoint` with `dangling`. Diagnosed structurally NOT-P9 (node2 had +58k *extra* rows — a GC change can't add rows).
2. Added **`CAGCDEL`** (every GC content-delete: key/kind/token/round/fence_seq/gen/shard/outcome) + **`CAREUSE`** (every `observeAndAdmit` adopt/resurrect: key/token/round) instrumentation; ran a **keep-alive** soak (cluster left up on failure).
3. On `dangling`, correlated logs: every dangling blob = ch2 `CAREUSE adopt`@round X → ch1 `CAGCDEL Deleted`@round Y (X<Y), **exact token match** — GC deleted reused blobs.
4. **Decoded the real `gc/snap` with the production codec** (temp `GcSnap` debug accessors + throwaway gtest). First decode REFUTED marker-without-edges. Final decode of the **delete-generation** snaps (1280/1281) produced the per-generation trace in §3 ⇒ VERDICT B.
5. Faithful TLA+ model (9.1M states clean) + a code-vs-model fence audit ruled out the fold/fence/marker mechanisms ⇒ the producer is the shared-blob under-count in the cross-node dedup path.

---

## 9. How to run things

> Per repo rules: redirect build/test output to a log in the build dir and have a **subagent** summarize it; don't pass `-j`/`nproc`; Allman braces; "exception" not "crash".

**Build (default/release; the soak image needs the release binary):**
```
cd build && ninja clickhouse        > build_x.log 2>&1   # binary for the soak (mounted into docker)
cd build && ninja unit_tests_dbms   > build_t.log 2>&1   # gtests
```
**Run gtests (from `build/`):**
```
./src/unit_tests_dbms --gtest_filter='CasGcDangle.*:CasReuseGcRace.*:CasGcLeak.*:CasGcRound.*:CasGcSnap.*'
```
Expected baseline: full `Cas*`/`CaWiring*` green EXCEPT the two intentional reds (`CasGcLeak.DisplacedUnexpandedTreeBlobsLeak` = B140-leak; `CasGcDangle.*` = this bug, currently the wrong-mechanism injection).

**TLC:**
```
cd docs/superpowers/models
./run_tlc.sh CaIncarnationCore_stage4.cfg          # core model (run_tlc.sh hardcodes the core module)
# B140 models use a different module, so invoke TLC directly:
java -XX:+UseParallelGC -cp ../../../tmp/tla2tools.jar tlc2.TLC -workers auto \
     -config CaB140DangleFaithful_shared.cfg CaB140DangleFaithful.tla
```

**Soak (2-node, reproduces in ~64 min, seed 20260617):**
```
cd utils/ca-soak
SEED=20260617 DURATION=3h WORKERS=2 METRICS=soak.db ./scripts/run_keepalive.sh > logs/soak.log 2>&1
# keep-alive => on failure the cluster stays UP. Monitor logs/soak.log for "CHECKPOINT FAILURE" / "dangling=".
# disk watchdog: nohup ./scripts/disk_watchdog.sh >/dev/null 2>&1 &
```

**On a dangling failure — capture the RIGHT artifacts (cluster is up):**
```
# 1. dangling blobs + reachable_from:
docker exec ca-soak-ch1-1 clickhouse disks --config-file /etc/clickhouse-server/config.xml \
   --disk ca_ro --query "fsck --detail" > logs/fsck.txt
# 2. per-blob delete generation (grep includes rotated .gz!):
docker exec ca-soak-ch1-1 sh -c 'zcat -f /var/log/clickhouse-server/clickhouse-server.log* | grep CAGCDEL | grep <hash>'
# 3. capture the DELETE-GEN snap + state + manifests via mc:
docker run --rm --network ca-soak_default -v /tmp/cap:/out --entrypoint sh minio/mc -c \
  'mc alias set rfs http://rustfs1:11121 clickhouse clickhouse;
   mc cp --recursive rfs/test/soak_pool/gc/snap/<GEN>/ /out/snap_<GEN>/;
   mc cp rfs/test/soak_pool/gc/state /out/state;
   mc cp --recursive rfs/test/soak_pool/roots/ /out/roots/'
# 4. writer-side relink timing (ch2):
docker exec ca-soak-ch2-1 sh -c 'zcat -f /var/log/clickhouse-server/clickhouse-server.log* | grep "CAREUSE.*<hash>"'
```

**Decode raw snaps offline (production codec; the proven method):** add temporary public debug accessors to `GcSnap` (`debugIsKnown(kind,hash)`, `debugInDegree(kind,hash)`, `debugIsExpanded(tree)`, `debugHasTreeEdge(parent,childKind,child)`), write a throwaway `gtest_*.cpp` (auto-globbed) that reads the raw files and `decodeGcSnap`/`decodeGcState`/`decodeRootShard` them, build `unit_tests_dbms`, run, then **remove the temp code** (verify `git diff src/` empty). To map a dangling blob → its live tree: take `reachable_from`'s part path, `decodeRootShard` the matching `roots/<uuid>/<uuid>/<shard>`, the `ref_name`'s payload has the `tree_id`.

---

## 10. The fix — likely shape + the TLA+ refinement that must catch it first (proper flow)

Do it via brainstorm→spec→TLA+→plan→implement, TDD. **First** resolve §6 (before/after-delete). Then:

**TLA+ refinement (so the model CAN reproduce it — currently it can't, see §7):**
1. Split GC's reachability into **two** structures: `snapEdges` (GC's folded in-degree, what it deletes by) and `manifestRefs` (authoritative live refs). Today they're one.
2. Add a transition where a (cross-node / dedup) live publish puts `T_cur→B` into `manifestRefs` but **not yet** into `snapEdges`.
3. Check `INV_NO_LOSS` against **`manifestRefs`** (truth) while GC's delete decision reads **`snapEdges`** (its view). Then TLC should reproduce the dangle (delete of a manifest-reachable shared blob).
4. Then design + prove the fix closes it.

**Candidate fixes (preserve INV-NO-LOSS / INV-NO-RETURN / INV-OVER-COUNT-ONLY):**
- **(writer/gate freshness)** ensure a dedup-reused blob's *condemnation* is visible cross-node before publish commits (retire-view freshness coverage across nodes), so the publish-gate `resurrect`s instead of committing a ref to a being-deleted blob.
- **(GC reachability completeness)** ensure the recheck before the content-delete of a shared blob accounts for every live ref to its content (a coherent cross-node cut), not just the folded parents.
- (heavier, documented in the SUPERSEDED spec / B140) edges-in-journal would remove the tree-read/skip class entirely but grows the hot-path manifest (conflicts B164).

**Tests:** rewrite `CasGcDangle` to the real shared-blob/dedup shape (ideally a **natural** 2-`Gc`-instance repro over one Store, not byte-injection); add sibling-scenario tests; keep `CasReuseGcRace` green and the B140-leak red. Then re-soak: quick → **12h with chaos**, verify `dangling=0`.

---

## 11. Honest caveats / gotchas

- The **decisive 1280/1281 decode artifacts are not persisted** (env torn down). The VERDICT-B conclusion is solid (decoded + cross-checked), but to *continue forensics* you'll re-run the soak and re-capture. `/tmp/*` may not survive into a new session — rely on the committed docs + the harness.
- `CasGcDangle` currently **tests the wrong (rejected) mechanism** — do not treat its RED as the target; rewrite it.
- The `CaB140Dangle*.tla` models are **untracked** and the Phase-1 one uses **unfaithful** arms — keep only as contrast; the *faithful* one (no-repro) is the honest baseline to refine.
- The soak's `gc_checkpoint` can also hit a **mutation-drain timeout** ("backlog stuck at N") under load — that's a *different*, documented, non-deterministic harness issue (`soak/run.py:136`), not this bug.
- `CAGCDEL`/`CAREUSE` are INFO-level and **committed** (kept intentionally for this investigation); decide whether to keep or gate them when the fix lands.
- Use `zcat -f …clickhouse-server.log*` (include rotated `.gz`) when grepping `CAGCDEL`/`CAREUSE` — deletes from early rounds are in the rotated log.

---

## 12. One-paragraph brief for the next session

CA-GC deletes a dedup-shared blob because its in-degree snapshot counted only one of two referencing part-trees (the second, live, cross-node-published reference never entered the snapshot); when the counted parent is stripped the blob hits in-degree 0 and is deleted while still live → `fsck dangling` = data loss. Pinned by decoding the real delete-generation snapshots (VERDICT B). Not P9; not marker-without-edges; not a single-process fold bug; the modeled fence race is closed. Open: was the live dedup-publish before or after the delete (picks writer-gate-freshness vs dedup-window fix). Start: read this file + the `B140-dangle` backlog entry + the SUPERSEDED spec banner; resolve §6; refine the TLA+ to split snapshot-vs-manifest reachability so it reproduces; then fix + TDD + 12h soak.

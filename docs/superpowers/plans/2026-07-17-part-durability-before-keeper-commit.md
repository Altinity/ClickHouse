# Part Durability Before Keeper Commit — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close every part disk-storage transaction in `MergeTreeData::Transaction::renameParts`, restoring the invariant that a part is durable before its `block_id`/part-znode is registered in `Keeper` — fixing the acked-then-lost `INSERT` data loss.

**Architecture:** One generic edit in `MergeTreeData::Transaction::renameParts` (rename loop, then a `hasActiveTransaction`-guarded `commitTransaction` loop) plus the header contract comment. Zero CA-specific code: `ContentAddressedTransaction::commit` is simply invoked earlier. Spec: `docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md`. Root-cause report: `docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md`.

**Tech Stack:** C++ (`src/Storages/MergeTree`), stateless SQL test with a failpoint, ca-soak scenario framework (Python).

## Global Constraints

- Branch `cas-gc-rebuild`; new commits only — no rebase, no amend, **never push**.
- Allman braces; comment style of the surrounding code; function names without `()` in prose.
- Builds: run `ninja` from `build/` with NO `-j`, redirect output to a log file in `build/`, analyze the log with a subagent returning a concise summary.
- Tests: redirect output to a uniquely-named log in `build/`, analyze with a subagent.
- The corrected CA gtest battery filter (do not use a shorter one): `Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*`.
- Stateless test: use `./tests/queries/0_stateless/add-test <name>` to allocate the number; only strictly necessary tags.
- The compose repro cluster (`ca-soak-*`) may be recycled (all forensic evidence is committed); `down -v` + `up` remounts the rebuilt binary.

## Plan-time audit (done while writing this plan — record, no action)

Spec §Testing item 4 required a write-after-close audit: mutating part-storage operations that route through the open disk transaction are `createFile`/`moveFile`/`replaceFile`/`removeFile`/`removeFileIfExists`/`createProjection`/`createDirectories`/`removeRecursive`/`removeSharedRecursive`/`renameTo` (via `executeWriteOperation`). Between `renameParts` and `Transaction::commit`, the only such calls across all ten call sites are `renameTo(temporary_part_relative_path)` on the sink's **rollback** branches (`ReplicatedMergeTreeSink.cpp:1059/:1076`), which under the fix intentionally run over a closed transaction through the autocommit route (CA committed-source move) — exactly the spec's rollback semantics. `writeTransactionFile` bypasses the transaction entirely (`DataPartStorageOnDiskBase.cpp:1110-1114`). **PASS — no blocker.**

---

### Task 1: Failing regression test (generic failpoint, plain-S3 shape)

**Files:**
- Create: `tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.sql` (number assigned by `add-test`)
- Create: `tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.reference`

**Interfaces:**
- Produces: the stateless test name `<NNNNN>_insert_dedup_disk_commit_failpoint`, used by Task 2 Step 5 as the fix's pass gate.

- [ ] **Step 1: Allocate the test**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
./tests/queries/0_stateless/add-test insert_dedup_disk_commit_failpoint
```

Note the assigned `<NNNNN>` printed by the tool.

- [ ] **Step 2: Write the test SQL**

Content of `tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.sql`:

```sql
-- Tags: zookeeper, no-fasttest, no-parallel
-- no-fasttest: needs an object-storage disk (storage_policy 's3_cache').
-- no-parallel: enables a server-global failpoint on the object-storage disk commit.

DROP TABLE IF EXISTS t_dedup_disk_commit SYNC;

CREATE TABLE t_dedup_disk_commit (k UInt64, v String)
ENGINE = ReplicatedMergeTree('/clickhouse/tables/{database}/t_dedup_disk_commit', 'r1')
ORDER BY k
SETTINGS storage_policy = 's3_cache';

SYSTEM ENABLE FAILPOINT disk_object_storage_fail_commit_metadata_transaction;

-- The disk-storage commit of the inserted part fails. The part must NOT be registered in Keeper:
-- before the fix the disk commit ran only in MergeTreeData::Transaction::commit, AFTER the Keeper
-- multi had durably created the block_id dedup znode, so this failure left a phantom dedup token.
INSERT INTO t_dedup_disk_commit SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x'); -- { serverError FAULT_INJECTED }

SYSTEM DISABLE FAILPOINT disk_object_storage_fail_commit_metadata_transaction;

-- Byte-identical retry of the failed INSERT: it must really insert. Before the fix it silently
-- deduplicated against the phantom block_id ("already exists ... ignoring it") and was acked with
-- zero rows written — the acked-then-lost data loss.
INSERT INTO t_dedup_disk_commit SETTINGS insert_deduplicate = 1, insert_keeper_fault_injection_probability = 0 VALUES (1, 'x');

SELECT count() FROM t_dedup_disk_commit;

DROP TABLE t_dedup_disk_commit SYNC;
```

Content of `tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.reference`:

```
1
```

- [ ] **Step 3: Run against the CURRENT (pre-fix) binary — must FAIL**

The pre-fix binary already exists at `build/programs/clickhouse` (R3-era, contains the bug).

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "stateless" --test <NNNNN>_insert_dedup_disk_commit_failpoint > build/test_dedup_failpoint_prefix.log 2>&1 || true
```

Analyze `build/test_dedup_failpoint_prefix.log` with a subagent. Expected: the test **FAILS** — actual output `0`, reference `1` (the retry falsely deduplicated). This is the reproduction of the bug in test form. If it unexpectedly PASSES, STOP: the reproduction assumption is wrong — re-verify against the report before touching source.

- [ ] **Step 4: Commit the test (expected-fail state is fine — it documents the bug)**

```bash
git add tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.sql tests/queries/0_stateless/<NNNNN>_insert_dedup_disk_commit_failpoint.reference
git commit -m "cas: failing regression test — INSERT dedup vs disk-commit failure (acked-then-lost, spec 2026-07-17-part-durability-before-keeper-commit)"
```

---

### Task 2: The fix — close disk transactions in renameParts

**Files:**
- Modify: `src/Storages/MergeTree/MergeTreeData.cpp:8770-8778` (`MergeTreeData::Transaction::renameParts`)
- Modify: `src/Storages/MergeTree/MergeTreeData.h:364-368` (declaration comment)

**Interfaces:**
- Consumes: test name from Task 1.
- Produces: the fixed binary `build/programs/clickhouse` used by Tasks 3-4.

- [ ] **Step 1: Replace the renameParts implementation**

In `src/Storages/MergeTree/MergeTreeData.cpp`, replace:

```cpp
void MergeTreeData::Transaction::renameParts()
{
    for (const auto & part_need_rename : precommitted_parts_need_rename)
    {
        LOG_TEST(data.log, "Renaming part to {}", part_need_rename->name);
        part_need_rename->renameTo(part_need_rename->name, true);
    }
    precommitted_parts_need_rename.clear();
}
```

with:

```cpp
void MergeTreeData::Transaction::renameParts()
{
    /// Materialize every part of this transaction: perform the deferred tmp->final renames, then
    /// close each part's disk-storage transaction, making the parts DURABLE on their disks.
    ///
    /// Contract: after renameParts returns, every part of this transaction is durable at its
    /// final name. commit only flips in-memory visibility (its commitTransaction loop remains as
    /// a safety net for paths that do not come through here); rollback compensates with new
    /// operations over committed disk state (removing a rolled-back part reclaims its disk data;
    /// on a content-addressed disk that drops the published ref).
    ///
    /// Ordering is load-bearing: every call site invokes renameParts BEFORE its external Keeper
    /// commit decision. A part must be durable before its block_id/part-znode is registered,
    /// otherwise a fault between the Keeper commit and the disk commit leaves a phantom part whose
    /// surviving block_id silently dedups a byte-identical client retry (acked data loss; see
    /// docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md). This also keeps the
    /// disk commit (network I/O on object storages) off the data_parts lock, which
    /// Transaction::commit holds.
    for (const auto & part_need_rename : precommitted_parts_need_rename)
    {
        LOG_TEST(data.log, "Renaming part to {}", part_need_rename->name);
        part_need_rename->renameTo(part_need_rename->name, true);
    }
    precommitted_parts_need_rename.clear();

    for (const auto & part : precommitted_parts)
        if (part->getDataPartStorage().hasActiveTransaction())
            part->getDataPartStorage().commitTransaction();
}
```

- [ ] **Step 2: Update the declaration comment**

In `src/Storages/MergeTree/MergeTreeData.h`, replace:

```cpp
        /// Rename should be done explicitly, before calling commit(), to
        /// guarantee that no lock held during rename (since rename is IO
        /// bound, while data parts lock is the bottleneck)
        void renameParts();
```

with:

```cpp
        /// Renames should be done explicitly, before calling commit, to
        /// guarantee that no lock is held during the rename and the disk
        /// commit (both are IO bound, while the data parts lock is the
        /// bottleneck). Contract: after renameParts every part of this
        /// transaction is durable on its disk at its final name; commit only
        /// flips in-memory visibility, and rollback compensates via new disk
        /// operations (part removal). Every caller runs this BEFORE its
        /// external Keeper commit decision: a part must be durable before its
        /// block_id/part-znode is registered in Keeper (see
        /// docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md).
        void renameParts();
```

- [ ] **Step 3: Build**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/build
ninja clickhouse unit_tests_dbms > build_fix_renameparts.log 2>&1
```

Analyze `build/build_fix_renameparts.log` with a subagent. Expected: success, no new warnings in `MergeTreeData.*`.

- [ ] **Step 4: Run the CA gtest battery**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*:ContentAddressedLog*:CountingBackendShape*:RefSnapshotCodec*:RefTableCacheEviction*:RefWriter*' > build/test_ca_battery_renameparts.log 2>&1
```

Analyze with a subagent. Expected: all pass (CA-layer publish semantics did not move — only the caller of `commitTransaction` did).

- [ ] **Step 5: Rerun the Task-1 test — must PASS**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "stateless" --test <NNNNN>_insert_dedup_disk_commit_failpoint > build/test_dedup_failpoint_postfix.log 2>&1
```

Analyze with a subagent. Expected: **PASS** (`count() = 1` — the failpoint now fires in `renameParts`, before the `Keeper` multi; the retry really inserts).

- [ ] **Step 6: Sanity — a couple of adjacent stateless tests (rename/commit paths)**

```bash
python3 -m ci.praktika run "stateless" --test "replicated_merge_tree 02446_parallel_replicas" > build/test_sanity_renameparts.log 2>&1 || true
```

Analyze with a subagent; triage per `reference_stateless_test_triage` — only failures NEW relative to the branch baseline block.

- [ ] **Step 7: Commit**

```bash
git add src/Storages/MergeTree/MergeTreeData.cpp src/Storages/MergeTree/MergeTreeData.h
git commit -m "cas: close part disk-storage transactions in renameParts — part durable before Keeper block_id registration

Fixes the acked-then-lost INSERT data loss (split commit: Keeper multi committed the block_id
while the part's disk commit ran only in Transaction::commit and failed under an S3 outage;
the surviving dedup znode silently swallowed the byte-identical client retry). Also moves the
object-storage disk commit off the data_parts lock. Spec:
docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md"
```

- [ ] **Step 8: Code-review subagent**

Dispatch a normal code-review subagent (NOT umbrella) over the two commits (test + fix) with the spec as context. Fix findings inline; re-run Steps 3-5 if source changed.

---

### Task 3: S40 scenario card (CA integration repro as a permanent gate)

**Files:**
- Create: `utils/ca-soak/scenarios/cards/s40_insert_dedup_outage.py`
- Modify: `utils/ca-soak/scenarios/cards/__init__.py` (add the import line)

**Interfaces:**
- Consumes: fixed binary from Task 2 (remounted into the compose cluster).
- Produces: scenario name `S40` runnable via `python3 -m scenarios.run --scenario S40`.

- [ ] **Step 1: Recycle the compose cluster onto the fixed binary**

All forensic evidence from the repro cluster is committed; it may be recycled now.

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
docker compose -f docker-compose.yml down -v
docker compose -f docker-compose.yml up -d
```

Wait for both `ch` nodes to answer `SELECT 1` (ports 8123/8124).

- [ ] **Step 2: Write the card**

Content of `utils/ca-soak/scenarios/cards/s40_insert_dedup_outage.py`:

```python
"""S40: acked-then-lost INSERT under an S3 outage + replica kill (dedup phantom regression gate).

Reproduces the 2026-07-17 CRITICAL data loss (report
docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md): continuous byte-identical-retry
sync inserts while RustFS is paused past the CAS write budget (90s) and the second replica is
killed mid-outage. Before the renameParts durability fix, an insert whose Keeper multi committed
the block_id but whose disk commit then failed left a PHANTOM dedup znode; the client retry
"already exists on other replicas ... ignoring it"-dedup'ed against it and was acked with zero
rows written. The gate: every id the server ever acked (HTTP 200) must be present after recovery.

Fault mechanics are copied from the proven build/dl_probe.py: raw docker pause/unpause of rustfs
(105s > 90s budget) + kill/start of ch2 inside the pause window; inserts run through the whole
window so some are guaranteed mid-commit when the fault bites.
"""

import subprocess
import threading
import time

from ..framework import sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

_TABLE = "s40_dedup_outage"


def _dock(*args):
    subprocess.run(["docker", *args], capture_output=True)


@register
class S40(Scenario):
    name = "S40"
    title = "acked-then-lost INSERT under S3 outage + replica kill"
    priority = "P0"
    expect_exception = True   # inserts DO fail loudly during the outage; CA-log exception rows are expected

    # The pause must exceed the 90s CAS write budget, so there is no meaningfully faster dev preset.
    param_table = {
        "dev": {"insert_window_s": 150, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
                "writers": 6, "payload_bytes": 20000},
        "ci": {"insert_window_s": 150, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
               "writers": 8, "payload_bytes": 20000},
        "full": {"insert_window_s": 300, "pause_s": 105, "kill_after_s": 16, "ch2_down_s": 50,
                 "writers": 8, "payload_bytes": 20000},
    }

    def run(self, ctx, result):
        p = ctx.params
        node = ctx.cluster.node1
        payload = "x" * int(p["payload_bytes"])

        sql.create_ca_table(node, _TABLE, columns="id UInt64, payload String", order_by="id")

        acked = set()
        acked_lock = threading.Lock()
        next_id = [0]
        id_lock = threading.Lock()
        stop_at = time.time() + float(p["insert_window_s"])

        def writer():
            while time.time() < stop_at:
                with id_lock:
                    next_id[0] += 1
                    i = next_id[0]
                deadline = time.time() + 240
                # Byte-identical retry until the SERVER acks — the client behavior that
                # triggers the dedup-phantom loss.
                while time.time() < deadline:
                    try:
                        node.query(
                            f"INSERT INTO {_TABLE} SETTINGS insert_deduplicate=1, "
                            f"async_insert=0 VALUES ({i}, '{payload}')",
                            timeout=100)
                        with acked_lock:
                            acked.add(i)
                        break
                    except Exception:
                        time.sleep(1.5)

        def faults():
            time.sleep(8)
            ctx.log("S40: PAUSE rustfs")
            _dock("pause", "ca-soak-rustfs1-1")
            time.sleep(float(p["kill_after_s"]) - 8)
            ctx.log("S40: KILL ch2")
            _dock("kill", "ca-soak-ch2-1")
            time.sleep(float(p["ch2_down_s"]))
            ctx.log("S40: START ch2")
            _dock("start", "ca-soak-ch2-1")
            time.sleep(float(p["pause_s"]) - float(p["kill_after_s"]) - float(p["ch2_down_s"]))
            ctx.log("S40: UNPAUSE rustfs")
            _dock("unpause", "ca-soak-rustfs1-1")

        ft = threading.Thread(target=faults, daemon=True)
        ft.start()
        threads = [threading.Thread(target=writer, daemon=True) for _ in range(int(p["writers"]))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        ft.join(timeout=float(p["pause_s"]) + 30)

        # Recovery: wait for node1 to answer, then converge replication.
        for _ in range(24):
            try:
                node.query("SELECT 1", timeout=10)
                break
            except Exception:
                time.sleep(5)
        time.sleep(30)
        node.query(f"SYSTEM SYNC REPLICA {_TABLE}", timeout=300)

        present = set(int(x) for x in node.query(
            f"SELECT id FROM {_TABLE} ORDER BY id").split())
        lost = sorted(acked - present)
        ctx.write_json("s40_acked_vs_present.json",
                       {"acked": len(acked), "present": len(present), "lost": lost[:100]})

        result.add(Verdict.check(
            "every acked insert is present", "lost == 0",
            f"acked={len(acked)} present={len(present)} lost={len(lost)} (ids {lost[:10]}...)" if lost
            else f"acked={len(acked)} present={len(present)} lost=0",
            not lost,
            "an acked-but-absent id = the dedup-phantom data loss (report 2026-07-17)"))

        # The smoking-gun log line must not fire for parts that do not exist.
        for n in ctx.cluster.nodes():
            try:
                n.query("SYSTEM FLUSH LOGS", timeout=60)
            except Exception:
                pass
        since = ctx.extra["since_event_time"]
        ghost_dedups = node.scalar(
            f"SELECT count() FROM system.text_log "
            f"WHERE event_time >= '{since}' "
            f"AND message LIKE '%already exists on other replicas as part%'")
        result.add(Verdict.check(
            "no cross-replica dedup against a phantom part", "0 ghost dedups",
            f"text_log ghost-dedup lines: {ghost_dedups}", int(ghost_dedups) == 0,
            "with the fix no block_id can outlive a failed disk commit, so this line "
            "can only fire for parts that really exist on a replica; any hit needs triage"))

        _common.standard_end(ctx, result, [_TABLE])
```

- [ ] **Step 3: Register the card**

Append to `utils/ca-soak/scenarios/cards/__init__.py`:

```python
from . import s40_insert_dedup_outage  # noqa: F401
```

- [ ] **Step 4: Run S40 on the fixed binary**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
python3 -m scenarios.run --scenario S40 --scale ci --seed 1 > ../../build/test_s40_run.log 2>&1
```

Analyze `build/test_s40_run.log` with a subagent. Expected: **pass** — `lost=0`, ghost-dedup verdict green (or, if the line fires for a part that exists, adjust the second verdict's query to exclude parts present in `system.parts` and re-run; the primary verdict is `lost == 0`). If `lost > 0`: STOP — the fix does not close the reproduction; return to the spec with the S40 artifacts.

- [ ] **Step 5: Commit**

```bash
git add utils/ca-soak/scenarios/cards/s40_insert_dedup_outage.py utils/ca-soak/scenarios/cards/__init__.py
git commit -m "cas: S40 scenario — acked-then-lost INSERT gate (S3 outage + replica kill, dedup phantom)"
```

---

### Task 4: Validation gates on the fixed binary

**Files:** none created (logs only, under `build/`).

**Interfaces:**
- Consumes: fixed binary + recycled cluster from Task 3.

- [ ] **Step 1: dl_probe rerun (the original reproduction)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master
python3 build/dl_probe.py > build/test_dl_probe_postfix.log 2>&1
```

Analyze with a subagent. Expected: `LOST(acked-but-absent)=0` (pre-fix: ~198/1314). Non-zero → STOP, same rule as S40 Step 4.

- [ ] **Step 2: S39 (#37 fence tolerance — R3 must stay green)**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
python3 -m scenarios.run --scenario S39 --scale ci --seed 1 > ../../build/test_s39_postfix.log 2>&1
```

Analyze with a subagent. Expected: 12/12 verdicts pass, as on the R3 landing run.

- [ ] **Step 3: S36 + S37 quick (MOVE + multi-disk unaffected)**

```bash
python3 -m scenarios.run --scenario S36,S37 --scale dev --seed 1 > ../../build/test_s36_s37_postfix.log 2>&1
```

Analyze with a subagent. Expected: both pass (S37 with its known pre-existing MOVE-PARTITION-under-kill dup exception if it recurs — pre-existing, not a blocker).

- [ ] **Step 4: 20-minute soak with the row-count oracle**

```bash
cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
python3 -m soak.run --phase 3 --duration 20m --seed 42 --insert-mode sync > ../../build/test_soak_postfix.log 2>&1
```

(Seed 42 = the chaos recipe that originally exposed the loss in R4.) Analyze with a subagent. Expected: checkpoint model == observed (no deficit), fsck clean, no wedges. A checkpoint deficit → STOP and triage before any further landing.

- [ ] **Step 5: Record the gate results**

Append one line per gate to `utils/ca-soak/scenarios/RUN_HISTORY.md` (S40/S39/S36/S37 rows are written by the runner automatically; add the dl_probe + soak lines by hand in the same table format).

```bash
git add utils/ca-soak/scenarios/RUN_HISTORY.md
git commit -m "cas: validation gates for the renameParts durability fix (dl_probe=0 lost, S39/S36/S37, 20m soak)"
```

---

### Task 5: Bookkeeping + upstream submission prep

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG.md` (the `*** CRITICAL` entry)
- Modify: `docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md` (FIX LANDED section)
- Create: `tmp/upstream_issue_dedup_durability.md` (draft only — NOT submitted)
- Modify: worklog `docs/superpowers/worklogs/2026-07-17-unattended-round2.md`

**Interfaces:**
- Consumes: commit hashes from Tasks 1-4, gate results from Task 4.

- [ ] **Step 1: BACKLOG — close the CRITICAL entry**

Append under the entry's UPDATE-3 in `docs/superpowers/cas/BACKLOG.md`:

```markdown
### FIX LANDED <date>: generic renameParts disk-transaction close (spec 2026-07-17-part-durability-before-keeper-commit-design.md)
`<fix commit hash>` closes every part disk-storage transaction in `MergeTreeData::Transaction::renameParts` (part durable BEFORE the Keeper block_id registration). Gates: failpoint stateless test (pre-fix 0 rows / post-fix 1), S40 lost=0, dl_probe lost=0 (was ~15%), S39 12/12, S36/S37, 20m seed-42 soak checkpoint-clean. R3 (#37) ship-readiness RESTORED. Residual (narrower, pre-existing, upstream-accepted): block_id outliving a durably-committed part lost later — tracked separately, verify-on-dedup is the candidate if it ever matters.
```

- [ ] **Step 2: Report — FIX LANDED section**

Append to `docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md`:

```markdown
## FIX LANDED (<date>)
Spec: docs/superpowers/specs/2026-07-17-part-durability-before-keeper-commit-design.md. Fix commit `<hash>`: `MergeTreeData::Transaction::renameParts` closes each part's disk-storage transaction (hasActiveTransaction-guarded) after the deferred renames — part durable before the Keeper multi at every call site; disk commit moved off the data_parts lock. Regression test `<NNNNN>_insert_dedup_disk_commit_failpoint` (failpoint `disk_object_storage_fail_commit_metadata_transaction`: pre-fix count=0, post-fix count=1). Gates: S40 lost=0, dl_probe lost=0, S39 12/12, 20m seed-42 soak clean. R3 ship-readiness restored.
```

- [ ] **Step 3: Upstream issue draft (prepare only — do NOT file/push anything)**

Create `tmp/upstream_issue_dedup_durability.md` with: title `Silent INSERT data loss on object-storage disks: block_id dedup znode is committed to Keeper before the part's local metadata is durable`; body = the invariant, the upstream ordering trace (`ReplicatedMergeTreeSink.cpp` renameParts → multi → commit; `PureMetadataObjectStorageOperation` deferral; `precommitTransaction` no-op), the failpoint reproduction SQL from Task 1 verbatim, the crash-window scenario (kill between multi and disk commit → phantom + surviving block_id → byte-identical retry falsely dedups; `createEmptyPartInsteadOfLost` preserves the loss), and the proposed one-function fix (the Task 2 diff, without the fork-specific report link). Note in the draft that ClickHouse Cloud/SMT may be unaffected (different commit path) — scope is the open-source object-storage disks.

- [ ] **Step 4: Memory + worklog + ledger, final commit**

Update the memory file `project_r3_acked_lost_dataloss_regression.md` (status → FIXED, fix commit, gates, R3 restored; upstream draft at `tmp/upstream_issue_dedup_durability.md` pending user decision) and `MEMORY.md` pointer (drop the 🔴). Append a worklog line with the same summary. Update `.superpowers/sdd/progress.md` position (data-loss fix LANDED; next = resume R5 from S13 via `build/r5_resume.sh` on the FIXED binary — note the S01-S12 results predate the fix and stay valid as pre-fix baseline).

```bash
git add docs/superpowers/cas/BACKLOG.md docs/superpowers/reports/2026-07-17-dataloss-traced-root-cause.md docs/superpowers/worklogs/2026-07-17-unattended-round2.md .superpowers/sdd/progress.md tmp/upstream_issue_dedup_durability.md
git commit -m "cas: bookkeeping — renameParts durability fix landed (backlog/report/worklog/ledger) + upstream issue draft"
```

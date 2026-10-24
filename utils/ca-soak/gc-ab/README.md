# GC before/after measurement on the ca-soak stand

Scripts that run a ca-soak scenario against a given ClickHouse binary, collect the run, and print per-phase GC time and counters. Two sets of runs (for example a base binary and a changed one) are compared by median and spread.

```bash
export GC_AB_OUT=$PWD/tmp/gc-ab && mkdir -p $GC_AB_OUT && cd $GC_AB_OUT     # runs/, logs and tables go here
S=<checkout>/utils/ca-soak/gc-ab
$S/run-stand.sh run before-s11ci-1 /abs/path/clickhouse-before --scenario S11 --seed 20260930 --duration 300s --scale ci
python3 $S/extract.py runs/before-s11ci-1 > before-s11ci-1.txt
$S/run-stand.sh down
```

Everything is mounted through a compose override; nothing in the checkout or the build directory is edited.

## Files {#files}

| File | Purpose |
|---|---|
| `run-stand.sh` | `run <label> <binary> [scenario args]`, `collect <label> <harness-run-dir>`, `down`. Runs the scenario, snapshots the run into `runs/<label>/`, restores `RUN_HISTORY.md` and `BACKLOG.md` |
| `gen_override.py` | Writes `override-current.yml`: mounts the binary (and one optional `config.d` file) into `keeper1`, `ch1`, `ch2` |
| `extract.py` | One run: per-round table, per-phase time and `ProfileEvents` by round class, `metric_log` windows, janitor metrics |
| `compare.py` | Before/after over `runs/before-s11ci-*` and `runs/after-s11ci-*`: median (min..max) per round class |
| `confirm.py` | Several named groups of runs side by side, plus per-run `fold_reduce` |
| `t2.py` | Janitor (`namespace_cleanup`) figures per run: keys per DELETE request, phase time |
| `t3.py` | Per-round condemned, graduated, redeleted counters of a run |
| `small-graduation-budget.xml` | Sets `gc_round_graduation_budget` to 5 on the `ca` disk |
| `longround/gc-long-interval.xml` | Sets `cas_gc_interval_sec` to 300 on the `ca` disk |
| `longround/seq2.sh` | One S11 run with 300 s rounds and optional latency; extracts and tears down |
| `longround/netem.sh` | Adds egress delay on the object store container with `tc netem` |
| `longround/lr.py` | Long-round figures per run, median (min..max) per side |
| `leakrca/probe.sh` | Loops on a live stack: `cas-fsck --detail`, outdated part counts, one forced GC round |

## Parameters {#parameters}

| Name | Where | Meaning |
|---|---|---|
| `GC_AB_OUT` | all scripts | Output directory, default the current directory. Python tools read `runs/` relative to the current directory, so run them from here |
| `GC_EXTRA_CONFIG` | `run-stand.sh` | Absolute path of one XML mounted as `config.d/zz_gc_extra.xml` on `ch1` and `ch2` |
| `NETEM_IMAGE` | `netem.sh` | Image with `tc`, default `series-measure-fault-tools:local` (a local tag, not built by the repo) |

## One run {#one-run}

`run-stand.sh run` refuses to start while any `ca-soak-*` container is up: the runner's reset would tear it down. It leaves the stack up afterwards, so run `run-stand.sh down` (`docker compose down -v`) before the next run to get a clean remount.

Default scenario arguments are `--scenario S11 --seed 20260930 --duration 300s --scale dev`. `--duration` does not change S11; `--scale` does. `dev` takes about 90 s with 6 to 13 rounds per node: enough to check the stand, too few to compare timings. `ci` takes 7 to 10 minutes.

## Before/after pair {#pair}

Alternate the binaries, at least three runs per side. `compare.py` needs the labels `before-s11ci-N` and `after-s11ci-N`.

```bash
for i in 1 2 3; do
  for side in before after; do
    $S/run-stand.sh run $side-s11ci-$i /abs/path/clickhouse-$side --scenario S11 --seed 20260930 --duration 300s --scale ci > $side-s11ci-$i.log 2>&1
    python3 $S/extract.py runs/$side-s11ci-$i > $side-s11ci-$i.txt
    $S/run-stand.sh down
  done
done
python3 $S/compare.py > COMPARISON.md
```

`confirm.py 'BEFORE=before-s11ci-1,before-s11ci-2' 'AFTER=after-s11ci-1,after-s11ci-2'` puts arbitrary groups side by side.

## Long rounds {#long-rounds}

Ten-second rounds split the work of one workload into many small rounds. To get few large rounds, mount `longround/gc-long-interval.xml` through `GC_EXTRA_CONFIG`; `seq2.sh` does that and also runs the extract and the teardown:

```bash
$S/longround/seq2.sh <label> /abs/path/clickhouse 0 ci        # LAT_MS=0: no latency
python3 $S/longround/lr.py 'BEFORE=long-before-a,long-before-a3' 'FINAL=long-final-a,long-final-a2'
```

The first scheduled round fires about 300 s after start and folds most of the workload in one pass. The forced rounds of the end checkpoint follow.

## Latency {#latency}

`longround/netem.sh LAT_MS OUTFILE` waits for both nodes, then adds `LAT_MS` of egress delay on `eth0` of `ca-soak-rustfs1-1` through a helper container that joins its network namespace.

- Needs docker and a container with `NET_ADMIN` and `tc` (`NETEM_IMAGE`). It touches only the object store container.
- Delays every response and every verb by a constant; no jitter, no bandwidth limit.
- `seq2.sh <label> <binary> 20 dev` starts it in the background. Use scale `dev`: `ci` with latency was estimated at over 25 minutes and never run.
- Removed with the stack by `run-stand.sh down`.

## Scenarios {#scenarios}

| Scenario | Reaches |
|---|---|
| S11 | Heavy `ALTER TABLE ... DELETE`: manifest fold and redelete. The table classes below apply to it |
| S30 | Create/drop churn: the namespace janitor. Read with `t2.py` |
| S46 | Restarts under GC. With `GC_EXTRA_CONFIG=small-graduation-budget.xml` the graduation budget runs out across rounds and a marker is carried. Read with `t3.py` |

```bash
GC_EXTRA_CONFIG=$S/small-graduation-budget.xml $S/run-stand.sh run after-s46 /abs/path/clickhouse-after --scenario S46 --seed 20260930 --duration 300s --scale dev
python3 $S/t3.py after-s46
```

S11 does not reach the janitor or the carried marker.

## Reading the output {#output}

- Round class (S11): `before`, `during` or `after` the delete phase, by round start and end against the `MutatePart*` window in `part_log` (`extract.py` prints the window length). Both nodes are pooled. `ALL` is every round.
- Working round: a `Finish` row with `duration_ms >= 20`. Follower no-op rounds take 0 to 1 ms and would dilute per-round means.
- Normalisation: rounds are timer-driven (`gc_interval_sec` 10), so counts and emitted edges differ between runs. Compare `per 1000 emitted edges` for body reads and round time, `per 1000 objects deleted` for delete counters.
- Spread: min..max over the runs of one side. Call an effect real only when the two ranges are disjoint.
- Binaries without `CASRefManifestBodyMemoHits` show 0 hits. Body reads there are `CASRefManifestBodyFoldGets`; on the changed side they are gets plus hits.
- Distinct manifests per round are not exposed; on the changed side they are at most `CASRefManifestBodyFoldGets`.

## Limits and traps {#limits}

- Per-phase and round rows hold only requests made on the round thread. `DiskS3ListObjects` and `DiskS3HeadObject` are not in them: `extract.py` takes them from `system.metric_log` 1 s buckets over the round intervals. Those are process-wide and include foreground traffic unless the workload is stopped.
- Round-row `DiskS3GetObject` is a small share of the window's GETs, because manifest body GETs are not attributed to the round thread. No figure gives all S3 requests of the GC.
- The fault proxy (`s3faultproxy` variant) cannot carry the S11 `ci` load: about 300 to 800 requests/s, the first `ALTER TABLE ... DELETE` starts 4 times later than direct. Use `netem.sh` for latency.
- The end checkpoint can fail its leak and dry-run verdicts when outdated-part cleanup lands between the last GC round and `cas-fsck`. More forced rounds then reclaim every unreachable manifest; `leakrca/probe.sh` on the stack left up shows it.
- With 300 s rounds the content of the one scheduled round depends on where the workload ends.
- `run-stand.sh` snapshots `scenarios/RUN_HISTORY.md` and `scenarios/BACKLOG.md` before the run. The runner appends to the tracked `RUN_HISTORY.md`; the wrapper restores the snapshot when the file only grew. If the wrapper is killed, check `git status utils/ca-soak/scenarios`.
- A run that fails a scenario verdict is still collected. Check the verdict in `runs/<label>/report.md` before using its figures.

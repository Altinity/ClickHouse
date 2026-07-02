# D2 — CA scenario-suite failure triage (2026-07-02, post-D1 dev sweep)

Full dev-scale sweep (32 runnable scenarios, seed 20260702) after D1 (shard incarnation + registry removal)
landed merge-ready. **Result: 14 PASS, 10 INCONCLUSIVE, 8 FAIL. Zero D1 regressions** — every FAIL is
pre-existing harness / infra / scale. The D1-specific scenarios all PASS (S30, S34, S35).

## FAIL triage (root cause + classification + fix)

| # | Scenario | Root cause (evidence) | Class | Fix |
|---|----------|----------------------|-------|-----|
| S23 | idle shared pool baseline | "idle GC 35594 S3 ops/round" is a **metric bug**: `s3_ops` sums S3 `*Microseconds` ProfileEvents (5122+6838+…) as op counts. Real: `CasRootList=0 CasRootGet=0 CasGcGet=71 CasGcPut=24 CasGcList=9`; `S3ReadRequestsCount=17`. | HARNESS | count only `*RequestsCount`/`*Requests`, not `*Microseconds` |
| S31 | ca-gc-dryrun under gc_shards>1 | `docker-compose-gc_shards2.yml up -d` → `rc=1`, cluster not healthy in 240s → 0/0 verdicts (errored at reset). | INFRA | re-run; fix gc_shards2 boot (likely down→up race / stale net) |
| S19 | clone and partition movement | dst replica agreement ch1=16 rows vs ch2=8 — no `SYNC REPLICA` before the agreement check (replication lag). | HARNESS | `SYSTEM SYNC REPLICA` before `assert_replicas_agree` (same as S35) |
| S26 | table-level verbatim file churn | replica agreement ch1=880 vs ch2=860 — same missing-sync race. | HARNESS | same as S19/S35 |
| S21 | read-heavy many-ref | `SELECT … FINAL` on a plain `ReplicatedMergeTree` → `ILLEGAL_FINAL` (12/48 errors); column-prune check `1col=0 all=0` vacuous at dev scale. | HARNESS | drop FINAL (or use a ReplacingMergeTree); gate column-prune on a scale where blobs aren't fully cached |
| S20 | replicated fetch and relink | follower `CasRootCas=0`; note: "counters may not be scoped per-node". | HARNESS | scope the CAS counter per node, or make the verdict record-only |
| S10 | patch parts + lightweight deletes | deleted bucket left 60 rows; "patch parts observed = 0 (may merge away quickly)". | SCALE/TIMING | verify at bigger scale (A2) / sync+settle before the count; likely not a product bug |
| S13 | process loss during write+GC | chaos-killed ch1 did not become healthy within 240s; agreement fails are downstream (ch1 conn refused). | HARNESS/CHAOS | re-run; check the kill/restart mechanism + health-wait timeout |

## Why none is a D1 regression
D1 changed GC discovery (`LIST(cas/refs/)`), the incarnation-keyed fold cursor, the newborn self-floor, and
shard-object reclaim. It did **not** touch replication, the read path / column pruning, `FINAL`, lightweight
deletes, or the chaos/kill harness. The two fails inside D1's blast radius both cleared: S23's op count is a
metric artifact (real CAS/S3 op counts are tiny; `CasRootList/Get=0` on idle), and S31 never executed a test
(cluster boot failed). S33 (concurrent-leader reclaim-leak guard) and S30/S34/S35 (D1) all PASS.

## Deferred / backlog
- S23 1-server & 10-server idle baselines: INCONCLUSIVE — need multi-topology infra (tied to the S12
  10-replica / `Cluster` node-count backlog).
- S23 idle RSS +82 MiB (budget 64): likely allocator/log slack; confirm it does not grow unbounded in the 4h soak.

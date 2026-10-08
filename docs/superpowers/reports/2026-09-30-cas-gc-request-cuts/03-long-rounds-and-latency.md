# Long GC rounds and object-store latency: BEFORE vs FINAL

Date 2026-09-30, 12:26-14:16 local (about 110 min wall). BEFORE = `clickhouse-before-61d24118251`
(sha256 `eecb2cf3...4710`), FINAL = `clickhouse-after-ac48289f453` (sha256 `8c75bd29...bce`); both `26.6.4.20001.altinityantalya`.
All figures are MEASURED from `runs/<label>/` by `longround/lr.py` (wraps `compare.py`) unless marked DERIVED.
Definitions as in `task-6b2-report.md` Method: working round = `Finish` row with `duration_ms >= 20`; all rounds of both nodes pooled (no before/during/after split: the one scheduled round fell in the "during" class by the part_log window and every other round in "after").
Per-run values are in brackets; "median (min..max)" over the runs listed.

## Configurations that ran

| id | scale | GC interval | latency | runs (label, verdict) |
|---|---|---|---|---|
| A: long rounds, no latency | ci | `cas_gc_interval_sec` 300 on both nodes via `GC_EXTRA_CONFIG=longround/gc-long-interval.xml` | 0 | BEFORE `long-before-a` PASS, `long-before-a3` PASS; FINAL `long-final-a` FAIL, `long-final-a2` FAIL |
| B: long rounds + 20 ms | dev | 300 s (same file) | +20 ms on every response of the object store (`tc netem delay 20ms` on the RustFS container's `eth0`) | BEFORE `lat-dev-before-1`, `-2`; FINAL `lat-dev-final-1`, `-2`; all four FAIL only on "GC round duration bounded" (see Concerns) |
| C: 10-s rounds (reference, earlier runs) | ci | 10 s | 0 | BEFORE `before-s11ci-1..5`, FINAL `final-s11ci-1..2`, all PASS |

Order actually run: `long-before-a`; `long-before-a2` (fault-proxy variant, latency 0, killed after 22 min); B x4 alternating; `long-final-a`; `long-final-a2`; `long-before-a3`. Seed 20260930, `--duration 300s` (ignored by S11). No STOP file was hit, `docker ps` was clean before every run, `git status --short utils/ca-soak` was identical to its pre-experiment state after every run (`longround/*.gitstatus` vs `gitstatus.baseline`).

### Why netem and not the harness proxy (latency part)

The `s3faultproxy` variant needs no harness edit for the binary (a `docker` shim in `longround/bin` re-adds the override, because the variant's explicit `-f` makes compose ignore `COMPOSE_FILE`), and `slow` mode arms through the control port (`{"rate":1.0,"modes":["slow"],"slow_ms":20,"methods":[GET,PUT,HEAD,POST,DELETE]}`). But the proxy cannot carry S11 ci even with latency 0 (`long-before-a2`, BEFORE, long rounds, MEASURED from `long-before-a2.proxy.txt` and the harness part_log):

- direct (`long-before-a`): first `ALTER ... DELETE` 161 s after cluster up (part_log `MutatePartStart`), all 20 delete rounds done by 325 s;
- through the proxy: first `ALTER ... DELETE` 679 s after cluster up (4.2x later); by 22 min only 4 of 20 mutations were created and 3 done;
- proxy forwarded 300-800 requests/s (samples every 15 s), 470791 requests in 22 min, `s3proxy` container at 11% CPU: the limit is per-request serialisation in the Python HTTP proxy (new upstream connection per request, no keep-alive), not CPU.

The run was killed at 13:04 (`long-before-a2` has no result). Latency was then injected without a proxy: a helper container joins the RustFS network namespace with `NET_ADMIN` and runs `tc qdisc add dev eth0 root netem delay 20ms limit 100000` (`longround/netem.sh`). Check from inside `ca-soak-ch1-1`: a TCP connect plus request/response to `rustfs1:11121` took 61 ms with netem (the SYN-ACK and the response are both delayed). Latency applies to the RustFS to ClickHouse direction only, to every request and verb; there is no jitter and no bandwidth limit.

Latency at scale ci was NOT run. Dev with 20 ms took 66 s of workload and about 4 min of end checkpoint (MEASURED); the ci run was judged to exceed 25 minutes (ESTIMATE, no ci latency run exists) and the brief says to drop to dev in that case.

## Round shape (MEASURED)

| | A BEFORE (2 runs) | A FINAL (2 runs) | C 10-s BEFORE (5) | C 10-s FINAL (2) | B BEFORE (2) | B FINAL (2) |
|---|---|---|---|---|---|---|
| working rounds per run | 17 (16..18) | 16 (16..16) | 30 (29..39) | 29.5 (27..32) | 5 | 5 |
| emitted edges per run | 135.5k (132.5k..138.5k) | 135.4k (133.9k..136.9k) | 62.2k (46.1k..64.6k) | 55.8k (47.1k..64.5k) | 3271 (2980..3562) | 3138 (2949..3326) |
| edges per working round | 7989 (7695..8283) | 8460 (8367..8553) | 1655 (1536..2198) | 1930 (1471..2388) | 596-712 | 590-665 |
| edges in the largest round | 52.3k (44.5k..60.1k) | 52.4k (44.9k..59.8k) | 9935 (5051..19.2k) | 13.5k (6457..20.5k) | 3271 (2980..3562) | 3138 (2949..3326) |

A: the first scheduled round fires about 300 s after start and folds the whole workload in one round (44.5k-60.1k edges, 39-53 s: 52.7, 42.6, 43.1, 39.1 s in the four runs); every later round is driven by the end checkpoint (`SYSTEM CAS GC RUN`). No per-round budget capped the ref intake: the first round folded 44.5k-60.1k edges in one pass, and `ContentAddressedSettings.cpp` has no intake budget setting. I did not identify which budget paces the 15-17 forced rounds that follow. B: dev has only 3k edges, all in the first forced round (a single 47 s / 43 s round). Edges per run in A (135k) are higher than in C (62k) because the drain rounds re-emit edges for the deleted manifests of the whole run.

## Verdict table

### A: long rounds, ci, no latency (n = 2 per side)

| figure | BEFORE | FINAL |
|---|---|---|
| manifest body GETs | 135.5k (132.5k..138.5k) | 84.4k (81.8k..87.0k) |
| memo hits | 0 | 51.0k (49.9k..52.1k) |
| hits / edges (DERIVED) | 0 | **0.377 (0.364..0.389)** |
| GETs per 1000 edges (DERIVED) | 1000 | 623 (611..636) |
| `fold_ref_intake` ms, total | 18.96k (16.97k..20.96k) | 13.68k (13.66k..13.70k) |
| `fold_ref_intake` ms per 1000 edges (DERIVED) | 139.7 (128.1..151.3) | 101.1 (100.1..102.0) |
| `fold_reduce` ms | 10.63k (10.32k..10.94k) | 10.08k (9.99k..10.16k) |
| `defer_decision` ms | 120.2k (118.0k..122.3k) | 124.2k (116.6k..131.8k) |
| `ref_object_cleanup` ms | 24.6k (24.1k..25.0k) | 24.9k (24.8k..24.9k) |
| `manifest_deletes` ms | 27.0k (25.4k..28.6k) | 26.5k (26.4k..26.5k) |
| `namespace_cleanup` ms | 3681 (3609..3752) | 3585 (3404..3765) |
| whole-round ms, total | 208.6k (201.9k..215.2k) | 206.4k (198.9k..213.8k) |
| whole-round ms per 1000 edges (DERIVED) | 1539 (1524..1553) | 1524 (1486..1562) |
| read-ahead hit / miss / wasted | 221.8k / 3886 / 600 | 176.0k / 28 / 5630 |
| workload end to GC fixpoint (harness log: "quiescing" to "post-graduation-drain residual") | 548 s (545..551) | 533.5 s (521..546) |
| janitor | not reached by S11 | not reached by S11 |

Phase share of the BEFORE round time (fold_ref_intake / fold_reduce / defer_decision / ref_object_cleanup / manifest_deletes / namespace_cleanup): run a 10/5/57/12/13/2 %, run a3 8/5/58/12/13/2 %. The 10-s-round BEFORE runs had 12/12/12/43/14/4 % (ref_object_cleanup dominant).

### B: long rounds + 20 ms, dev (n = 2 per side)

| figure | BEFORE | FINAL |
|---|---|---|
| manifest body GETs | 3271 (2980..3562) | 2054 (2008..2100) |
| memo hits | 0 | 1084 (941..1226) |
| hits / edges (DERIVED) | 0 | **0.344 (0.319..0.369)** |
| GETs per 1000 edges (DERIVED) | 1000 | 656 (631..681) |
| `fold_ref_intake` ms, total | 39.6k (38.3k..40.9k) | 28.0k (27.9k..28.1k) |
| `fold_ref_intake` ms per 1000 edges (DERIVED) | 12180 (11490..12870) | 8959 (8461..9457) |
| `fold_reduce` ms | 7795 (7450..8141) | 7364 (6905..7824) |
| `defer_decision` ms | 1076 (1040..1111) | 942 (834..1049) |
| `ref_object_cleanup` ms | 1699 (1660..1738) | 1640 (1530..1750) |
| `manifest_deletes` ms | 605 (536..674) | 595 (463..727) |
| `namespace_cleanup` ms | 1035 (1023..1048) | 984 (885..1083) |
| whole-round ms, total | 54.0k (52.1k..55.9k) | 41.7k (40.7k..42.7k) |
| whole-round ms per 1000 edges (DERIVED) | 16580 (15680..17470) | 13320 (12850..13790) |
| read-ahead hit / miss / wasted | 10.0k / 7 / 0 | 8356 / 7 / 0 |
| workload end to GC fixpoint | 248.5 s (243..254) | 222.5 s (198..247) |

Phase share, BEFORE: `fold_ref_intake` 74 % and 73 %, `fold_reduce` 14-15 %, the other four 1-3 % each. FINAL: `fold_ref_intake` 66-69 %, `fold_reduce` 17-18 %.
DERIVED: (39.63 - 28.01) s / (3271 - 2054) GETs saved = 9.5 ms of `fold_ref_intake` per GET saved (medians). One GET costs at least 20 ms of added latency, so the fold overlaps about two GETs at a time (3271 edges x 20 ms / 39.6 s = 1.7).

## Verdicts

1. **Memo hit rate with long rounds: 0.377 (0.364..0.389) at ci, no latency (n=2), 0.344 (0.319..0.369) at dev with 20 ms (n=2), against 0.309 (0.295..0.323) with 10-s rounds (`final-s11ci-1..2`).** The gain is small: about +7 points. Round 1 of `long-final-a`: 59827 edges = 37365 GETs + 22462 hits (0.375); the later forced rounds with work sit at 0.40 (e.g. 1996 hits / 4990 edges). The GETs of a drain round equal its `manifests_deleted` (2994 of 2994), so the hit rate there is set by the graph shape, not by round length; halving the GETs, as expected in the audit, does not happen on this workload.
2. **`fold_ref_intake` drops outside the spread in both configurations.** Latency dev: per 1000 edges 12180 (11490..12870) to 8959 (8461..9457), -26 %; total 39.6 s to 28.0 s, -29 %; ranges disjoint. Long rounds ci without latency: per 1000 edges 139.7 (128.1..151.3) to 101.1 (100.1..102.0), -28 %, ranges disjoint (n=2). Absolute saving without latency is 5.3 s in a 208 s run.
3. **Whole-round time: drops outside the spread only under latency.** Latency dev: total 54.0 s (52.1..55.9) to 41.7 s (40.7..42.7), -23 %; per 1000 edges -20 %; ranges disjoint. Long rounds ci, no latency: 208.6 s (201.9..215.2) to 206.4 s (198.9..213.8), -1 %, ranges overlap; no change outside the spread.
4. **What dominates.** Under 20 ms latency at dev the BEFORE round is 73-74 % `fold_ref_intake` and 14-15 % `fold_reduce`: the manifest-body GETs are the round, and the branch changes exactly that phase (-26 %). `fold_reduce` (about 7.5 s, 14-18 % of the round) is the second largest and is not touched by the branch (7795 vs 7364 ms, ranges overlap). Without latency at ci the round is 57-58 % `defer_decision` (about 120 s), then `manifest_deletes` 13 %, `ref_object_cleanup` 12 %; `fold_ref_intake` is 8-10 %. Whether `defer_decision` also dominates under latency at ci was not measured (latency ran only at dev, where it is 2 %).
5. **What moved the wrong way.**
   - **Both FINAL runs at A fail the leak verdict; both BEFORE runs pass.** `long-final-a`: 9 unreachable `_manifests` plus 568 blobs still in the condemn pipeline after the end checkpoint (residual 8022 after the forced rounds, 577 after the drain); `long-final-a2`: 63 unreachable `_manifests`, pipeline empty (residual 2512 then 63). BEFORE: residual 59 then 0, and 1759 then 0. Failed verdicts: "no unbounded leftovers" and "deleted part content reclaimed". FINAL passed 2 of 2 at 10-s rounds (`final-s11ci-1..2`) and BEFORE passed 5 of 5 there. Root cause not found (stand torn down before the failure was seen; the harness `forensics/object_lifetimes.json` samples 9 and 40 keys of the orphans, in two clusters of manifest sequence numbers, 0x3c50-0x4727 and 0x8e59-0x8e82 for `long-final-a2`). n=2 vs n=2: this is a strong signal, not a proof of a regression.
   - Read-ahead: wasted 600 (594..606) to 5630 (4742..6517) at A (10x), hits 221.8k to 176.0k (memo hits replace read-ahead hits), misses 3886 to 28. Whether `CASRefManifestBodyFoldGets` includes the wasted read-aheads was not checked, so the net GET saving may be smaller by up to 5630 per run. At 10-s rounds wasted was 27-144 on both sides.
   - `defer_decision` at A: 120.2k to 124.2k ms, ranges overlap (118.0k..122.3k vs 116.6k..131.8k), no movement.

## Limits

- n = 2 per side in A and B (the second BEFORE in A was added to attribute the FINAL failures); no latency run at ci.
- Latency was only run at scale dev (3k edges, one 43-47 s round). It shows the regime (12 ms of fold per edge) but not the hours-long production round (F4: 6000-6800 s of `fold_ref_intake`). Extrapolation is not measured: at 12 ms/edge, 100k edges are 20 min.
- netem adds a constant 20 ms to responses only; real S3 has 10-100 ms variable first-byte latency, throttling and connection limits. The GET concurrency of the fold (about 2) is a property of the code path and would, if it holds, cap the benefit at the GET count ratio.
- Workload differs between sides in size (edges 132k-138k vs 134k-137k per run at A, 2949-3562 at B); per-1000-edge figures are the comparable ones.
- Timer effects: the one scheduled round in A fires at about 300 s after start, i.e. right at the end of the workload (workload 287-325 s), so its content (44-60k edges) depends on where the workload ends.
- "Workload end to fixpoint" is the harness log (quiesce, fsck settle, forced rounds, graduation drain); it includes fixed waits and is not a GC-only figure.
- `long-before-a2` (proxy) is not a result; the proxy carried the load 4x to 20x slower than direct.

## Concerns

1. The FINAL binary failed the leak verdict in 2 of 2 long-round runs (BEFORE 0 of 2). Needs a reproduction with the stand kept up (do not tear down on FAIL) and a look at whether the unreachable manifests are deleted by one more forced round or stay.
2. The B runs all FAIL "GC round duration bounded" (30 s ceiling at dev; largest round 47.3 s and 50.5 s BEFORE, 37.3 s and 36.3 s FINAL) because of the added latency. It applies to both sides and is not a product finding.
3. Long rounds concentrate all edges into one round, so the memo table for a real hours-long round holds every distinct manifest of that round; memory of the memo was not measured here.

## Files

`tmp/gc-cuts-stand/longround/`: `gc-long-interval.xml`, `netem.sh`, `arm.sh` + `bin/docker` (proxy attempt), `seq1.sh` `seq2.sh` `seqB.sh` (sequences), `lr.py` (figures), `lr-ci-nolat.md`, `lr-dev-lat.md`, `seq.log`, per-run `<label>.log|.txt|.netem.txt|.gitstatus`; run data in `tmp/gc-cuts-stand/runs/<label>/`.

# RCA: S11 `dryrun ⊆ deletable` red on BASE (`before-s11ci-1`)

Verdict class: (a) harness assertion timing. The end checkpoint assumes a quiescent pool, but background outdated-part removal is not part of its quiescence and landed between `fsck` and `cas-gc-dryrun`. The dry-run tool and GC behaved as designed.

## What the verdict compares
- `scenarios/framework/assertions.py:60-90`: candidate keys from `clickhouse disks --query cas-gc-dryrun` must be a subset of fsck-detail keys of class `unreachable | pending-gc | awaiting-gc`.
- Two separate, sequential processes (`scenarios/framework/checkpoint.py:76-84`): `lifecycle.fsck_detail` then `lifecycle.dryrun`. No shared snapshot.
- The dry-run reads the durable sealed generation and `gc/state` (`Gc::previewDeletes`, `CasGc.cpp:4766` at 61d24118251). Its header (`CasGc.h:489-493`) states the subset guarantee holds ONLY at quiescence.

## Timeline (run.log local = UTC+2; gc_log/part_log UTC)
| UTC | Event | Source |
|---|---|---|
| 08:25:35 | quiesce starts (sync, drain, OPTIMIZE FINAL) | run.log 10:25:35 |
| 08:25:39 | last merge of `s11_buckets` | predown/ch1/part_log.tsv |
| 08:25:57-08:26:48 | ch1 GC rounds 21 `Deferred` x7 (skip-unchanged, `changed_shards=0`, `ref_keys_listed=354`); includes the forced `Manual` round at 08:26:23 | raw/gc_log_ca-soak-ch1-1.tsv |
| 08:26:49 | "forced GC residual unreachable=0 (rounds=1)" then "final detailed fsck + dry-run" starts | run.log |
| 08:26:50-53 | cleanup thread removes 3834 outdated parts (ch1 99+1820, ch2 3+1915) | part_log `RemovePart`, table `s11_buckets` |
| 08:26:58 | ch1 round 22 starts; `defer_decision`: `changed_shards=2`, `ref_keys_listed=821` (was 354), `rounds_deferred_before=7` | gc_log |
| 08:27:00.42 | `fold_reduce`: 92376 deltas, `condemned=2965`; `round_commit` 08:27:00.430 (gc/state CAS) | gc_log |
| 08:27:05 | `manifest_deletes` 11511 (= 3 x 3837 parts); Finish: `objects_deleted=0`, `entries_graduated=0`, `entries_condemned=2965` | gc_log |
| 08:27:10 | "collecting observations" (fsck + dry-run done) | run.log |

## Answers
1. Compared: dry-run candidates (post-round-22 condemned rows) vs fsck classes. Fsck saw the pre-removal ledger: `ref_records_walked=335` (end_state.json), matching the 354 ref keys of the deferred rounds, not the 821 after the burst. Cluster state at fsck: replication queue empty (`raw/replication_queue_*` header only), mutation queue 0, but cleanup thread still holding 3834 outdated parts.
2. The 2965 are blobs fsck (pre-burst) found referenced by the outdated parts' manifests. Round 22 folded the burst's ref removals, saw zero in-degree, and condemned them (`CASGCRetiredCondemned=2965`, `entries_condemned=2965`). The dry-run, run after 08:27:00.43, listed exactly those 2965 condemned rows (count equal). Not stale/partial: it read a NEWER state than fsck. The mismatch is fsck being older than the dry-run. The dry-run reason column (`awaiting_graduation`) was not captured; inference from equal counts only.
3. (a). Not (b): the dry-run correctly reports condemned rows. Not (c): round 22 deleted 0 objects and graduated 0 (`objects_deleted=0`, `entries_graduated=0`), and the run ended before any later round, so no reachable-at-fsck blob was physically deleted. Removed-part count x3 = manifests deleted (11502 vs 11511), so the condemned set is the removed parts' content. Not proven: that each of the 2965 has zero in-degree in the ledger at 08:27:00 (no dry-run stdout or ledger dump kept); the fold is the authority, and post-run verdicts (row counts, replica agreement, oracle) pass.
4. 1 of 6: only here did the outdated-part burst land 1 s after the forced-GC/fsck stage began. Other runs (RemovePart bursts, `s11_buckets`, UTC): before-2 08:37:49 vs checkpoint fsck 08:41:27; before-3 08:55:39 vs 08:57:16; after-1 08:31:04 vs 08:34:09; after-2 08:44:58 vs 08:48:47. after-3: last burst 09:07:19, fsck 09:07:29 (10 s gap): dry-run 1606 candidates / 1606 deletable, passed by luck of ordering. The burst is not tied to quiesce end (bursts occur every ~30-60 s at cleanup cadence); this run's quiesce+GC was short (74 s) and hit the cadence tick. Node roles were the same in all runs (ch1 leader, ch2 `NotALeader`). Branch exposed: yes. `git diff 61d24118251 HEAD` touches none of `previewDeletes`, `shouldDeferRound`, `CasFsck`, `CommandCaGcDryRun`, or the harness; the same race exists. after-s11ci-3 shows the same near miss.
5. Fix (harness): before fsck+dry-run, wait for no outdated parts (`system.parts` `active=0` for the checked tables, or run `SYSTEM ... CLEANUP`/wait on `part_log` RemovePart stability) AND drive GC until a `Success` round with no fold pending (`changed_shards=0` and `ref_keys_listed` stable), then take fsck and dry-run back to back and re-run both if `gc/state.round` changed between them. Assertion-side alternative: capture `gc/state.round` before fsck and after dry-run and mark INCONCLUSIVE (not fail) if it moved. Cheapest deterministic repro: S11 small scale, then `ALTER ... DELETE`, `OPTIMIZE FINAL`, and hold the cleanup thread (`SYSTEM STOP CLEANUP`); run fsck detail; `SYSTEM START CLEANUP` + wait for RemovePart; `SYSTEM CAS GC RUN`; run `cas-gc-dryrun`: expect N candidates, all `reachable` in the earlier fsck.

## Not established
- Exact fsck end / dry-run start timestamps (harness does not log them); dry-run stdout with reasons (not saved); ledger dump at 08:27:00.
- Why the cleanup fired ~70 s after the last merge (`old_parts_lifetime` not configured in the soak files; default 480 s does not explain it).
- `raw/ca_events_summary_*` is capped at 100001 rows and ends 08:21:39, so per-ref removal events are unavailable. Capture next time: `cas_log` ref-removal rows for the last 5 min, dry-run stdout, and `gc/state` before/after.

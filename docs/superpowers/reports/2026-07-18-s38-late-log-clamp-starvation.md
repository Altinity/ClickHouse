# S38 late-log detection: starved by clamp suppression (product observation) — 2026-07-18

## Symptom {#symptom}

S38 (`RefLateLogDetected fires for an injected dead-epoch late log`) FAILED twice tonight
(runs `20260718T002659`, `20260718T014616`): 0 `ref_late_log_detected` CA-log events across
the whole 240 s post-injection poll window.

## Evidence (run `20260718T014616`) {#evidence}

- Injection at `01:48:06Z` (dead-epoch ref-log txn, 91 bytes, key
  `.../_log/0000000000000001-fffffffffffffffe`).
- ch2 GC log (`raw/gc_log_ca-soak-ch2-1.tsv`): **40 `Finish/Success` rounds AFTER injection**
  (through `01:52:29Z`). GC leadership and round liveness were HEALTHY — the "no healthy
  leader window" hypothesis from run 1 is refuted.
- Every post-injection round's ProfileEvents show `CasGcClampSuppressedPasses: 1`,
  `DiskS3ReadRequestsErrors: ~284`, `LogError: 1`: the round as a whole finishes `Success`,
  but the orphan-manifest **sweep pass** — the pass that would call `reportLateLogsIfAny`
  (`CasOrphanManifestSweep.cpp`) — is clamp-suppressed on the poisoned key every round.

## Verdict {#verdict}

PRODUCT OBSERVATION (not a card defect): an injected/poison late log itself triggers the
repeated failing reads that CLAMP its key; the clamp then suppresses the very sweep pass
that would report the log as late. Detection is structurally starved for as long as the
clamp persists. Safety is unaffected (fail-closed; nothing revived or deleted), but the
observability contract "a late log is reported" does not hold under clamp.

This is the known deferred BACKLOG item **[clamp liveness] scoped suppression under long
persistent clamps** manifesting — S38 is effectively its designed detector. Fix direction
belongs to that item: a clamped key must still be REPORTABLE (emit `ref_late_log_detected`
or a dedicated clamped-key anomaly event from the clamp path itself), even while the sweep
skips processing it.

## Secondary finding: observe-layer Success under-count {#observe-undercount}

The card polled `observe.gc_log_all(cl, since_inject).summary.success` and got **0** while
the raw GC log shows 40 post-injection Success rows. The poll's exit condition
(`min_real_rounds = 5`) therefore never triggered and the run always burns the full 240 s.
Needs one look at `gc_log_all`'s time filter/summary counting (timestamp format vs
`since` comparison is the prime suspect). Card logic itself is sound.

## Disposition {#disposition}

- Product fix: deferred to BACKLOG `[clamp liveness]` (now with a concrete reproducer: S38).
- Card: unchanged (it did its job); optionally assert `CasGcClampSuppressedPasses > 0` to
  pin the mechanism explicitly instead of failing on the downstream symptom.
- observe.gc_log_all under-count: small harness bug, fix separately.

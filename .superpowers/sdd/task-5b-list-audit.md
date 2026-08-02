# Task 5b recovery `LIST` audit

Date: 2026-08-02

Authority: Task 5b in `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` and the exact-frontier amendment in `docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`.

## Verdict

The current tree contains correctness uses of stream `LIST`, but none is an unresolved dependency. Every such use has one specified replacement: generation-9 `_ckpt.committed_through`, read from an exact checkpoint object and used as the inclusive replay bound. Task 5b must delete the correctness uses rather than preserve a fallback. After the change, `LIST(cas/ns/stream/)` may only offer snapshot candidates, diagnostics, performance hints, or leak-only cleanup nominations.

## Writer-mount recovery: `CasRefLedger::runRecoveryWalkOnce`

| Current use of `LIST` data | Current class | Generation-9 disposition |
|---|---|---|
| `greatest_listed_snapshot` selects the base snapshot | Correctness | A listed snapshot is only a candidate. `chooseRecoveryGrounding` accepts it only at or below exact `committed_through`; otherwise it is ignored and diagnosed. The checkpoint's own snapshot remains an exact candidate. |
| Smallest `hint_log_ids` supplies genesis when `_ckpt.life_epoch` is absent | Correctness | Delete. `Live` and `Removing` require a readable checkpoint with `life_epoch`; absence is corruption. `Creating` and catalog-absent lives are not recovered. |
| `hint_log_ids` supplies a witness above a missing arithmetic slot | Correctness | The inclusive `committed_through` frontier is the authoritative stop and missing-slot witness. Listed ids may add diagnostics but cannot extend or shorten committed history. |
| Sorted `hint_log_ids` avoids exact probes for ids not listed | Performance | Retain only if useful as an exact-key fetch hint. Empty, partial, or reordered listings must reconstruct the same state and next id. |
| Listed snapshot/log ids explain malformed or unexpected stream contents | Diagnostics | Retain. Diagnostic differences across equivalent listing behaviours are allowed. |

The exact-key base fetch and the checkpoint-token revalidation on a missing selected base remain correctness mechanisms; neither depends on `LIST` completeness.

## Read-only recovery: `recoverRefTableDetailed` and `recoverRefTable`

| Current use of `LIST` data | Current class | Generation-9 disposition |
|---|---|---|
| `snapshots.back()` chooses the newest base | Correctness | Replace with `chooseRecoveryGrounding`. A listed snapshot is merely a candidate bounded by `committed_through`; exact checkpoint state supplies lifecycle, genesis, and stop. |
| The sorted `logs` vector defines the complete replay tail | Correctness | Delete as authority. Replay exact arithmetic ids from the grounded successor through inclusive `committed_through`. |
| A selected listed object vanishing triggers a fresh full-list restart | Correctness | Exact frontier replay no longer restarts to rediscover history. Resolve an ambiguous exact read/checkpoint observation through exact checkpoint re-read and fail closed when the same authority still requires a missing object. |
| Parsed listed keys nominate malformed/foreign stream objects | Diagnostics / leak-only cleanup | Retain for reporting and janitor nomination; never install table state from them. |
| Page callback accounts enumeration work | Performance / observability | Retain while the optional hint listing exists. It cannot affect the recovered value. |

Both overload families are bound: bare-name admission must first resolve an exact catalog row, while the exact-life overload must receive the matching catalog entry and checkpoint authority rather than treating a physical life id as self-authorizing.

## Callers whose expectations change

| Caller | Current correctness dependency | Required exact-frontier contract |
|---|---|---|
| `Gc::rebuildBaseline` | `recoverRefTable` derives the owner set and greatest applied id from a complete listed stream. | Pass the immutable catalog cut and exact checkpoint; recover only through `committed_through`. A catalog-named life without usable frontier authority is retain/refuse, never destructive. |
| `CasOrphanManifestSweep::activeManifestKeys` | The recovered owner set and its second listed-log pass protect tail removals; an omitted log can make a manifest look orphaned. | Exact frontier recovery supplies the owner set. Exact arithmetic replay from the fold cursor through `committed_through` supplies removal protection. Until available, catalog-named namespaces remain retain-only. The ownership-tree listing remains leak nomination only. |
| `CasFsck` stream/table checks | Fresh recovery and several revalidation helpers assume a full list supplies the current table and stop. | Recover exact bounded history from catalog + checkpoint. The independently listed stream remains an oracle/diagnostic input, so omissions may change findings about storage quality but not the reconstructed committed table. |
| `CasFsck` snapshot oracle | Listed snapshots/logs choose the independently replayed comparison. | Exact frontier defines the history being checked; listing may offer candidate snapshots and extra witnesses only. |
| Fresh dangle rechecks in `CasFsck` | `recoverRefTable` is described as "a full LIST + replay" and supplies destructive/dangling classification. | Use the exact bounded recovery result; on missing/corrupt authority keep the existing fail-closed verdict. |
| Writer `ensureRefTableRecovered` / `installRecoveryResult` | The private candidate and next id can currently be grounded or stopped by listed ids. | Install and allocate only after exact frontier validation. Writer recovery may adopt exactly one valid unfrontiered successor; read-only callers may not. |

## Residual rule

No remaining stream `LIST` result may decide genesis, committed membership, replay stop, `last_epoch_seal`, next transaction id, owner protection, or permission to delete. If implementation discovers another such consumer, Task 5b stops at that consumer and records it instead of adding a fallback.

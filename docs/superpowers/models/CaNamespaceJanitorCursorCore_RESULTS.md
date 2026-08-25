# `CaNamespaceJanitorCursorCore` — gate results {#canamespacejanitorcursorcore-results}

This dedicated model owns Step 8's cursor/suppression liveness rule. A DEFER
invocation may inspect one bounded janitor page under suppression, but it must
retain a valid cursor so the next authorized fold retries that page. Physical
life capture and stale-delete safety remain in `CaRefNsCleanupStaleLeaderCore`.

Run `./run_namespace_janitor_cursor.sh` from this directory. The runner
asserts every expected verdict and writes raw TLC logs under `tmp/`.

| Config | Expected result | Result |
|---|---|---|
| `_sab_advancesuppressed` | `EventuallyDeadAReclaimed` temporal counterexample | **PASS** — 4 generated / 3 distinct; fair trace `A-defer → B-fold → A` |
| `_safe` | liveness GREEN | **PASS** — 7 generated / 6 distinct / queue 0; depth 6 |
| `_witness_deletion` | `WITNESS_DEAD_A_DELETED` reachable | **PASS** — 3 generated / 3 distinct / queue 0; depth 3 |

The machine has only pages A and B. A owns `deadA`; B is inert. Rounds
alternate DEFER then authorized fold. Honest DEFER retains A, so the fold
deletes A. The sabotage advances `A -> B`; that fold resets `B -> A`, forming
the fair infinite `A-defer -> B-fold -> A` cycle and violating `<> ~deadA`.
There is no free `NoOp`; weak fairness on both page actions rules out a
vacuous stuttering counterexample.

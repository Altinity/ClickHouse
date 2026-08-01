-------------------- MODULE CaRefPreFoldDrainAllRowsCore --------------------
(* Serial all-row portion of the catalog-only pre-fold drain.

   The catalog token covers its whole object. After one exact `Removing` deletion changes that token,
   a later candidate cannot reuse the prior snapshot. This companion to `CaRefPreFoldDrainCore`
   abstracts two independently eligible rows and proves the required loop: select one exact row,
   resolve its CAS, then rescan the complete catalog before either selecting another row or deciding.
   It deliberately does not repeat the two-leader/adopted-proof protocol, which remains owned by
   `CaRefPreFoldDrainCore`. *)
EXTENDS Integers, FiniteSets

CONSTANTS
    SabotageSkipRescan,
    SabotageNonExactDelete

Rows == {"r1", "r2"}
Phases == {"scan", "issued", "done"}

VARIABLES
    remaining,
    token,
    phase,
    observedRow,
    observedToken,
    advancedWithDebt,
    nonExactDelete

vars == << remaining, token, phase, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

Init ==
    /\ remaining = Rows
    /\ token = 1
    /\ phase = "scan"
    /\ observedRow = "none"
    /\ observedToken = 0
    /\ advancedWithDebt = FALSE
    /\ nonExactDelete = FALSE

(* A complete catalog GET selects exactly one currently eligible row. *)
Select ==
    /\ phase = "scan"
    /\ remaining # {}
    /\ \E row \in remaining :
         /\ observedRow' = row
         /\ observedToken' = token
         /\ phase' = "issued"
    /\ UNCHANGED << remaining, token, advancedWithDebt, nonExactDelete >>

(* Exact CAS deletes only the row selected from this full-object token. The next action must rescan. *)
DeleteSelected ==
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ observedToken = token
    /\ remaining' = remaining \ {observedRow}
    /\ token' = token + 1
    /\ phase' = "scan"
    /\ nonExactDelete' = nonExactDelete \/ ~(observedRow \in remaining /\ observedToken = token)
    /\ UNCHANGED << observedRow, observedToken, advancedWithDebt >>

(* Another actor may consume the exact selected row, or mutate another catalog row and invalidate the
   full-object token. This invocation resolves either outcome only by returning to a complete scan. *)
ExternalDeleteSelected ==
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ remaining' = remaining \ {observedRow}
    /\ token' = token + 1
    /\ UNCHANGED << phase, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

(* An unrelated eligible row may change instead. The selected row remains present but its full-object
   token is stale, so this is a rejected CAS only after the following complete rescan. *)
ExternalDeleteOther ==
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ \E other \in remaining \ {observedRow} : remaining' = remaining \ {other}
    /\ token' = token + 1
    /\ UNCHANGED << phase, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

ResolveExternalOutcome ==
    /\ phase = "issued"
    /\ (observedRow \notin remaining \/ observedToken # token)
    /\ phase' = "scan"
    /\ UNCHANGED << remaining, token, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

(* A rejected response or an ambiguous response known not to have landed leaves the row/token intact.
   Neither is completion: their only continuation is a fresh complete scan. *)
RejectSelected ==
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ observedToken = token
    /\ phase' = "scan"
    /\ UNCHANGED << remaining, token, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

UnknownNotLanded ==
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ observedToken = token
    /\ phase' = "scan"
    /\ UNCHANGED << remaining, token, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

(* Sabotage: a stale full-object token is accepted. The sticky consequence is only set by this
   deliberately non-exact mutation, making `ExactCatalogCAS` a red-controlled claim. *)
NonExactDeleteSelected ==
    /\ SabotageNonExactDelete
    /\ phase = "issued"
    /\ remaining # {}
    /\ observedToken # token
    /\ \E victim \in remaining : remaining' = remaining \ {victim}
    /\ token' = token + 1
    /\ phase' = "scan"
    /\ nonExactDelete' = TRUE
    /\ UNCHANGED << observedRow, observedToken, advancedWithDebt >>

(* A decision is legal only after a fresh scan observes no eligible rows. *)
Decide ==
    /\ phase = "scan"
    /\ remaining = {}
    /\ phase' = "done"
    /\ UNCHANGED << remaining, token, observedRow, observedToken, advancedWithDebt, nonExactDelete >>

(* Omission control: advancing directly from one issued candidate leaves the other exact row debt. *)
SkipRescan ==
    /\ SabotageSkipRescan
    /\ phase = "issued"
    /\ observedRow \in remaining
    /\ observedToken = token
    /\ remaining' = remaining \ {observedRow}
    /\ token' = token + 1
    /\ phase' = "done"
    /\ advancedWithDebt' = TRUE
    /\ nonExactDelete' = nonExactDelete \/ ~(observedRow \in remaining /\ observedToken = token)
    /\ UNCHANGED << observedRow, observedToken >>

NoOp == UNCHANGED vars

Next == Select \/ DeleteSelected \/ ExternalDeleteSelected \/ ExternalDeleteOther
        \/ ResolveExternalOutcome \/ RejectSelected \/ UnknownNotLanded \/ NonExactDeleteSelected
        \/ Decide \/ SkipRescan \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ remaining \subseteq Rows
    /\ token \in 1..3
    /\ phase \in Phases
    /\ observedRow \in Rows \cup {"none"}
    /\ observedToken \in 0..3
    /\ advancedWithDebt \in BOOLEAN
    /\ nonExactDelete \in BOOLEAN

AllEligibleRowsResolvedBeforeDecision == ~advancedWithDebt
ExactCatalogCAS == ~nonExactDelete
CompletionDrained == phase = "done" => remaining = {}

=============================================================================

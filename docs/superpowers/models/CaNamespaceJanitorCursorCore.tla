-------------------- MODULE CaNamespaceJanitorCursorCore --------------------
(* A DEFER invocation has no destructive proof, but it must still run one
   bounded janitor page.  A valid suppressed page retains its cursor.  If it
   advances, alternating DEFER/fold rounds phase-lock a dead A page out of
   every authorized fold: A-defer -> B-fold -> A forever. *)
EXTENDS Integers

CONSTANT SabotageAdvanceSuppressed

Pages == {"A", "B"}

VARIABLES cursor, roundKind, deadA, deletedEver, deferEver

vars == << cursor, roundKind, deadA, deletedEver, deferEver >>

Init ==
    /\ cursor = "A"
    /\ roundKind = "defer"
    /\ deadA = TRUE
    /\ deletedEver = FALSE
    /\ deferEver = FALSE

(* DEFER is suppression-only.  Its bounded page may observe A or B, but in
   the honest protocol it publishes no cursor advance. *)
DeferPage ==
    /\ roundKind = "defer"
    /\ cursor' = IF SabotageAdvanceSuppressed
                  THEN IF cursor = "A" THEN "B" ELSE "A"
                  ELSE cursor
    /\ roundKind' = "fold"
    /\ deferEver' = TRUE
    /\ UNCHANGED << deadA, deletedEver >>

(* An authorized fold deletes dead residue on A.  Finishing B resets the
   cursor to A, as a completed ownership-tree cycle does in production. *)
AuthorizedFoldPage ==
    /\ roundKind = "fold"
    /\ deadA' = IF cursor = "A" THEN FALSE ELSE deadA
    /\ deletedEver' = (deletedEver \/ (cursor = "A" /\ deadA))
    /\ cursor' = IF cursor = "A" THEN "B" ELSE "A"
    /\ roundKind' = "defer"
    /\ UNCHANGED deferEver

Next == DeferPage \/ AuthorizedFoldPage

(* No free NoOp exists.  Fairness makes an enabled page run; the sabotage's
   infinite A-defer -> B-fold -> A cycle is still fair and must fail liveness. *)
Spec ==
    Init /\ [][Next]_vars
         /\ WF_vars(DeferPage)
         /\ WF_vars(AuthorizedFoldPage)

TypeOK ==
    /\ cursor \in Pages
    /\ roundKind \in {"defer", "fold"}
    /\ deadA \in BOOLEAN
    /\ deletedEver \in BOOLEAN
    /\ deferEver \in BOOLEAN

EventuallyDeadAReclaimed == <>~deadA

(* Negated reachability witness for the useful honest route. *)
WITNESS_DEAD_A_DELETED == ~(deferEver /\ deletedEver /\ ~deadA)
=============================================================================

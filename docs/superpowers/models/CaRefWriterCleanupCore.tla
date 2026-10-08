-------------------- MODULE CaRefWriterCleanupCore --------------------
(* Ref-table writer-side cleanup core — spec 2026-07-11-cas-ref-table-snapshot-log-design.md,
   sections State Transitions (Add Precommit / Promote / Remove Precommit), Failed Precommit
   Cleanup, Clean Up Old Precommits, and Writer Namespace Removal; plan
   2026-07-12-cas-ref-table-snapshot-log-phase1.md Task 3 (small model).

   SCOPE: active builds and exact precommit ownership for ONE table. The append itself is modeled
   as atomic-durable (Task 1/2's CaRefTableSnapshotLogCore/CaRefDeltaIntakeCore own append
   uncertainty and fold intake; this model owns only the OWNERSHIP LIFECYCLE built on top of a
   durable log).

   MODELED LIFECYCLE:
     - StartBuild: a build is born Active in the current epoch and attempts a precommit for its own
       manifest. The every-attempt wedge may report Adopted or remain Unresolved; unresolved means
       the conditional PUT may already be durable, so the possible owner is modeled explicitly.
     - PromoteBuild: one atomic transaction removes the precommit and installs the committed
       owner (spec Promote: "There is no moment at which the manifest has no owner"). Guarded on
       build.epoch == current_epoch (an old-epoch build can never promote after a Fence).
     - FailBuild / ResolveGrant* / RemoveFailedPrecommit / RetireFailedBuild: failure deposits an
       in-memory cleanup duty while the mount lives. The duty first resolves an uncertain grant;
       Adopted owes an exact owner removal, Rejected proves none exists, and neither arm may retire
       while the outcome is uncertain. The steps are split so both wrong retirement orders are
       reachable, toggle-able behaviors rather than something TLA+ atomicity hides.
     - Fence: a successor mints a new epoch; PromoteBuild's guard freezes every older-epoch build
       in place (no further Fail/Retire either — that machinery belongs to the dead predecessor
       process; only SuccessorCleanupStep may still touch its precommit).
     - SuccessorCleanupStep: bounded (one precommit per step) exact removal of stale
       (epoch < current) precommit bindings (spec Clean Up Old Precommits). Interruption between
       steps needs no extra state: any other action, including another Fence, may interleave.
     - RemoveNamespace / CancelBuild: spec Writer Namespace Removal — "one body transaction
       containing their exact removals ... After the transaction is durable, it applies the same
       operations to memory, cancels local builds." Modeled as one atomic transaction (every
       non-cancelled build's owner binding cleared in the same step the namespace becomes Removed)
       followed by a separate CancelBuild step. The ONLY way CancelBuild can precede a legitimate
       RemoveNamespace clear is the sabotage toggle below (correct order gates CancelBuild on
       namespaceState = "Removed").

   INVARIANTS:
     INV_NO_WRONGFUL_RECLAIM   (I1) — SuccessorCleanupStep never removes the precommit of a build
       that can still promote (Active, epoch == current). Tracked via a ghost (wrongfulReclaim)
       rather than a snapshot predicate, because RemoveNamespace legitimately clears an
       Active-current-epoch build's precommit too (full teardown, not a wrongful reclaim) —
       the ghost distinguishes "cleanup did this" from "namespace removal did this".
     INV_PROMOTE_NEVER_OWNERLESS (I2) — while the namespace is Live, a Promoted build always holds
       the committed owner (PromoteBuild is one atomic step; this is a structural regression
       guard, not sabotage-toggled — RemoveNamespace tearing down a Promoted build's committed
       owner AFTER namespace removal is the expected teardown, not ownerlessness).
     INV_RETIRE_AFTER_REMOVAL  (I3) — a build is Retired only once its exact precommit removal is
       durable.
     INV_NO_RETIRE_UNCERTAIN_GRANT (I4) — a build is never Retired while its owner-grant outcome
       remains unresolved.
     INV_NAMESPACE_REMOVAL_COMPLETE (I5) — namespaceState = "Removed" implies no owner binding
       (precommit or committed) survives, for every build. RemoveNamespace requires zero owners
       durably; CancelBuild running before durability lets a build's binding escape the removal
       transaction's enumeration.

   LIVENESS: StalePrecommitEventuallyGone, under weak fairness of SuccessorCleanupStep and
   CleanupFailedProgress (RemoveFailedPrecommit/RetireFailedBuild) only — Fence and the ordinary
   mutation actions are the unconstrained environment. Delayed cleanup only over-protects
   manifests (safety never requires cleanup to precede ordinary current-epoch mutations).

   SABOTAGE (each MUST produce a counterexample when enabled, one at a time):
     SabotageRetireBeforeRemoval          (S1) -> INV_RETIRE_AFTER_REMOVAL violated (a Retired
       build's precommit is never cleaned up again: also the class of hazard the liveness
       property would expose, since RemoveFailedPrecommit is gated on buildState = "Failed").
     SabotageSuccessorRemovesCurrentEpoch (S2) -> INV_NO_WRONGFUL_RECLAIM violated (successor
       cleanup reclaims a still-promotable current-epoch precommit).
     SabotageCancelBeforeRemovalDurable   (S3) -> INV_NAMESPACE_REMOVAL_COMPLETE violated (a build
       cancelled before the removal transaction is durable is excluded from that transaction's
       owner enumeration, so the namespace-removal marks itself Removed while the build's
       precommit remains a durable, never-cleaned owner).
     SabotageRetireUncertain              (S4) -> INV_NO_RETIRE_UNCERTAIN_GRANT violated (a failed
       build drops its cleanup duty while the attempted owner PUT may already be durable).

   Self-loop NoOp so a fully quiescent terminal state (namespace Removed, no builds left to touch)
   is not a spurious TLC deadlock (house pattern, see CaGcAckFloorCore.tla). *)
EXTENDS Integers

CONSTANTS
    Builds, MaxEpoch,
    SabotageRetireBeforeRemoval,           \* S1: retire a Failed build before its removal is durable
    SabotageRetireUncertain,               \* S4: retire while the owner-grant outcome is unresolved
    SabotageSuccessorRemovesCurrentEpoch,  \* S2: successor cleanup also reclaims epoch == current
    SabotageCancelBeforeRemovalDurable     \* S3: cancel a local build before namespace removal lands

Epochs == 1..MaxEpoch
BuildStates == {"Unborn", "Active", "Failed", "Retired", "Promoted", "Cancelled"}
GrantOutcomes == {"None", "Unresolved", "Adopted", "Rejected"}
GrantRealities == {"None", "Adopted", "Rejected"}

VARIABLES
    currentEpoch,     \* the current writer's fence epoch (Fence only ever increases it)
    namespaceState,    \* "Live" | "Removed" (no recreation in this small model — out of scope here)
    buildEpoch,        \* [Builds -> 0..MaxEpoch]; 0 = not yet started ("Unborn")
    buildState,        \* [Builds -> BuildStates]
    grantOutcome,      \* [Builds -> GrantOutcomes]; outcome observable by the caller
    grantReality,      \* [Builds -> GrantRealities]; actual durable result hidden by Unresolved
    dutyQueued,        \* [Builds -> BOOLEAN]; live-mount cleanup duty awaiting settlement/retirement
    sawUnresolvedDuty, \* reachability ghost: a failed build was queued while its grant was unresolved
    hasPrecommit,      \* [Builds -> BOOLEAN] durable precommit ownership of this build's manifest
    hasCommitted,      \* [Builds -> BOOLEAN] durable committed ownership, installed only by Promote
    wrongfulReclaim    \* ghost: SuccessorCleanupStep ever reclaimed a still-promotable precommit

vars == << currentEpoch, namespaceState, buildEpoch, buildState, grantOutcome, grantReality, dutyQueued,
           sawUnresolvedDuty, hasPrecommit, hasCommitted, wrongfulReclaim >>

Init ==
    /\ currentEpoch = 1
    /\ namespaceState = "Live"
    /\ buildEpoch = [b \in Builds |-> 0]
    /\ buildState = [b \in Builds |-> "Unborn"]
    /\ grantOutcome = [b \in Builds |-> "None"]
    /\ grantReality = [b \in Builds |-> "None"]
    /\ dutyQueued = [b \in Builds |-> FALSE]
    /\ sawUnresolvedDuty = [b \in Builds |-> FALSE]
    /\ hasPrecommit = [b \in Builds |-> FALSE]
    /\ hasCommitted = [b \in Builds |-> FALSE]
    /\ wrongfulReclaim = FALSE

(* ---- current-writer build lifecycle ---- *)

\* Add Precommit: namespace Live, exact binding absent, manifest tuple = the locally active build.
StartBuild(b) ==
    /\ namespaceState = "Live"
    /\ buildState[b] = "Unborn"
    /\ buildEpoch' = [buildEpoch EXCEPT ![b] = currentEpoch]
    /\ buildState' = [buildState EXCEPT ![b] = "Active"]
    /\ \/ /\ grantOutcome' = [grantOutcome EXCEPT ![b] = "Adopted"]
           /\ grantReality' = [grantReality EXCEPT ![b] = "Adopted"]
           /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = TRUE]
       \/ /\ grantOutcome' = [grantOutcome EXCEPT ![b] = "Unresolved"]
           /\ grantReality' = [grantReality EXCEPT ![b] = "Adopted"]
           \* An unresolved conditional PUT may already be durable: the dangerous world.
           /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = TRUE]
       \/ /\ grantOutcome' = [grantOutcome EXCEPT ![b] = "Unresolved"]
           /\ grantReality' = [grantReality EXCEPT ![b] = "Rejected"]
           /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = FALSE]
    /\ UNCHANGED << currentEpoch, namespaceState, dutyQueued, sawUnresolvedDuty,
                    hasCommitted, wrongfulReclaim >>

\* Promote: one atomic transaction removes the exact precommit and installs the committed owner —
\* never a moment with zero owners. Guarded on build.epoch == current_epoch (spec: "promote guarded
\* on build.epoch == current_epoch"); an old-epoch build can never reach this action again once
\* Fence has moved past it.
PromoteBuild(b) ==
    /\ buildState[b] = "Active"
    /\ buildEpoch[b] = currentEpoch
    /\ grantOutcome[b] = "Adopted"
    /\ hasPrecommit[b]
    /\ buildState' = [buildState EXCEPT ![b] = "Promoted"]
    /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = FALSE]
    /\ hasCommitted' = [hasCommitted EXCEPT ![b] = TRUE]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, grantOutcome, grantReality, dutyQueued,
                    sawUnresolvedDuty, wrongfulReclaim >>

\* The current writer observes a failed build: it keeps the build Active (bookkeeping-wise still
\* holding its precommit) until the removal below is durable.
FailBuild(b) ==
    /\ buildState[b] = "Active"
    /\ buildEpoch[b] = currentEpoch
    /\ buildState' = [buildState EXCEPT ![b] = "Failed"]
    /\ dutyQueued' = [dutyQueued EXCEPT ![b] = TRUE]
    /\ sawUnresolvedDuty' = [sawUnresolvedDuty EXCEPT ![b] =
                                @ \/ grantOutcome[b] = "Unresolved"]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, grantOutcome, grantReality,
                    hasPrecommit, hasCommitted, wrongfulReclaim >>

\* The duty queue is the next live-mount caller of the every-attempt wedge. An adopted outcome may
\* owe an exact owner removal; a rejected outcome proves that no owner survived.
ResolveGrantAdopted(b) ==
    /\ buildState[b] = "Failed"
    /\ buildEpoch[b] = currentEpoch
    /\ dutyQueued[b]
    /\ grantOutcome[b] = "Unresolved"
    /\ grantReality[b] = "Adopted"
    /\ grantOutcome' = [grantOutcome EXCEPT ![b] = "Adopted"]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, buildState, grantReality, dutyQueued,
                    sawUnresolvedDuty, hasPrecommit, hasCommitted, wrongfulReclaim >>

ResolveGrantRejected(b) ==
    /\ buildState[b] = "Failed"
    /\ buildEpoch[b] = currentEpoch
    /\ dutyQueued[b]
    /\ grantOutcome[b] = "Unresolved"
    /\ grantReality[b] = "Rejected"
    /\ grantOutcome' = [grantOutcome EXCEPT ![b] = "Rejected"]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, buildState, grantReality, dutyQueued,
                    sawUnresolvedDuty, hasPrecommit, hasCommitted, wrongfulReclaim >>

\* Durable exact precommit removal for a current-epoch failed build.
RemoveFailedPrecommit(b) ==
    /\ buildState[b] = "Failed"
    /\ buildEpoch[b] = currentEpoch
    /\ dutyQueued[b]
    /\ grantOutcome[b] = "Adopted"
    /\ hasPrecommit[b]
    /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = FALSE]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, buildState, grantOutcome, grantReality,
                    dutyQueued, sawUnresolvedDuty, hasCommitted, wrongfulReclaim >>

\* Retire only after the removal above is durable (SabotageRetireBeforeRemoval drops that gate: S1).
RetireFailedBuild(b) ==
    /\ buildState[b] = "Failed"
    /\ buildEpoch[b] = currentEpoch
    /\ dutyQueued[b]
    /\ (SabotageRetireBeforeRemoval \/ ~hasPrecommit[b]
        \/ (SabotageRetireUncertain /\ grantOutcome[b] = "Unresolved"
            /\ grantReality[b] = "Adopted"))
    /\ (SabotageRetireUncertain \/ grantOutcome[b] # "Unresolved")
    /\ buildState' = [buildState EXCEPT ![b] = "Retired"]
    /\ dutyQueued' = [dutyQueued EXCEPT ![b] = FALSE]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, grantOutcome, grantReality, sawUnresolvedDuty,
                    hasPrecommit, hasCommitted, wrongfulReclaim >>

CleanupFailedProgress ==
    \E b \in Builds : ResolveGrantAdopted(b) \/ ResolveGrantRejected(b)
                   \/ RemoveFailedPrecommit(b) \/ RetireFailedBuild(b)

(* ---- epoch fencing and successor maintenance ---- *)

\* A successor mints a new epoch. Every build with buildEpoch < currentEpoch' is now frozen: it can
\* never Promote (guarded), and FailBuild/RemoveFailedPrecommit/RetireFailedBuild are equally
\* guarded on buildEpoch[b] = currentEpoch, so the dead predecessor's own machinery stops touching
\* it too — only SuccessorCleanupStep may still act on its precommit.
Fence ==
    /\ currentEpoch < MaxEpoch
    /\ currentEpoch' = currentEpoch + 1
    /\ UNCHANGED << namespaceState, buildEpoch, buildState, grantOutcome, grantReality, dutyQueued,
                    sawUnresolvedDuty, hasPrecommit, hasCommitted, wrongfulReclaim >>

\* Clean Up Old Precommits: bounded (one binding per step) exact removal of stale bindings.
\* Honest guard: buildEpoch[b] < currentEpoch (strictly older than the fence). Sabotage widens it
\* to <= currentEpoch (S2), reaching a still-promotable current-epoch Active build.
SuccessorCleanupStep ==
    \E b \in Builds :
        /\ hasPrecommit[b]
        /\ IF SabotageSuccessorRemovesCurrentEpoch
           THEN buildEpoch[b] <= currentEpoch
           ELSE buildEpoch[b] < currentEpoch
        /\ hasPrecommit' = [hasPrecommit EXCEPT ![b] = FALSE]
        /\ wrongfulReclaim' = (wrongfulReclaim \/ (buildState[b] = "Active" /\ buildEpoch[b] = currentEpoch))
        /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, buildState, grantOutcome, grantReality,
                        dutyQueued, sawUnresolvedDuty, hasCommitted >>

(* ---- namespace removal ---- *)

\* One atomic body transaction: every currently-tracked (not yet Cancelled) build's owner binding
\* is cleared in the SAME step the namespace becomes Removed (spec: "Earlier operations in the same
\* transaction contain an exact owner_transition ... for every committed ref and precommit").
\* Under the correct order, CancelBuild cannot have fired yet (gated below on namespaceState =
\* "Removed"), so `toClear` always includes every live build here. Under S3, a build cancelled
\* early is excluded from the enumeration and escapes with its precommit intact.
RemoveNamespace ==
    /\ namespaceState = "Live"
    /\ LET toClear == { b \in Builds : buildState[b] # "Cancelled" }
       IN /\ hasPrecommit' = [b \in Builds |-> IF b \in toClear THEN FALSE ELSE hasPrecommit[b]]
          /\ hasCommitted' = [b \in Builds |-> IF b \in toClear THEN FALSE ELSE hasCommitted[b]]
    /\ namespaceState' = "Removed"
    /\ UNCHANGED << currentEpoch, buildEpoch, buildState, grantOutcome, grantReality, dutyQueued,
                    sawUnresolvedDuty, wrongfulReclaim >>

\* Local builds cancelled only AFTER the removal transaction above is durable (SabotageCancel
\* BeforeRemovalDurable drops that gate: S3).
CancelBuild(b) ==
    /\ buildState[b] = "Active"
    /\ buildEpoch[b] = currentEpoch
    /\ (SabotageCancelBeforeRemovalDurable \/ namespaceState = "Removed")
    /\ buildState' = [buildState EXCEPT ![b] = "Cancelled"]
    /\ UNCHANGED << currentEpoch, namespaceState, buildEpoch, grantOutcome, grantReality, dutyQueued,
                    sawUnresolvedDuty, hasPrecommit, hasCommitted, wrongfulReclaim >>

NoOp == UNCHANGED vars

Next ==
    \/ \E b \in Builds : StartBuild(b) \/ PromoteBuild(b) \/ FailBuild(b) \/ CancelBuild(b)
    \/ CleanupFailedProgress
    \/ Fence
    \/ SuccessorCleanupStep
    \/ RemoveNamespace
    \/ NoOp

\* Weak fairness ONLY on the cleanup machinery: Fence, StartBuild, PromoteBuild, FailBuild,
\* CancelBuild, and RemoveNamespace are the unconstrained environment (delayed cleanup only
\* over-protects; safety never requires it to race ahead of ordinary mutations).
Spec ==
    /\ Init
    /\ [][Next]_vars
    /\ WF_vars(SuccessorCleanupStep)
    /\ WF_vars(CleanupFailedProgress)

----------------------------------------------------------------------------
(* ---- invariants ---- *)

TypeOK ==
    /\ currentEpoch \in Epochs
    /\ namespaceState \in {"Live", "Removed"}
    /\ buildEpoch \in [Builds -> 0..MaxEpoch]
    /\ buildState \in [Builds -> BuildStates]
    /\ grantOutcome \in [Builds -> GrantOutcomes]
    /\ grantReality \in [Builds -> GrantRealities]
    /\ \A b \in Builds :
          /\ grantOutcome[b] = "Adopted" => grantReality[b] = "Adopted"
          /\ grantOutcome[b] = "Rejected" => grantReality[b] = "Rejected"
          /\ hasPrecommit[b] => grantReality[b] = "Adopted"
    /\ dutyQueued \in [Builds -> BOOLEAN]
    /\ sawUnresolvedDuty \in [Builds -> BOOLEAN]
    /\ hasPrecommit \in [Builds -> BOOLEAN]
    /\ hasCommitted \in [Builds -> BOOLEAN]
    /\ wrongfulReclaim \in BOOLEAN

\* (I1) Cleanup never removes the precommit of a build that can still promote.
INV_NO_WRONGFUL_RECLAIM == ~wrongfulReclaim

\* (I2) While the namespace is Live, a Promoted build always holds the committed owner (structural
\* regression guard on PromoteBuild's atomicity; RemoveNamespace's own teardown of a Promoted
\* build's committed owner happens only together with namespaceState -> "Removed", so it is
\* excluded here on purpose).
INV_PROMOTE_NEVER_OWNERLESS ==
    \A b \in Builds : (buildState[b] = "Promoted" /\ namespaceState = "Live") => hasCommitted[b]

\* (I3) A build is Retired only once its exact precommit removal is durable.
INV_RETIRE_AFTER_REMOVAL == \A b \in Builds : buildState[b] = "Retired" => ~hasPrecommit[b]

\* An unresolved grant is a possible durable owner. Retirement is forbidden until the every-attempt
\* wedge resolves it; the live duty remains queued in the meantime.
INV_NO_RETIRE_UNCERTAIN_GRANT ==
    \A b \in Builds : buildState[b] = "Retired" => grantOutcome[b] # "Unresolved"

\* (I4) RemoveNamespace requires zero owners durably: once Removed, no binding survives.
INV_NAMESPACE_REMOVAL_COMPLETE ==
    namespaceState = "Removed" => \A b \in Builds : ~hasPrecommit[b] /\ ~hasCommitted[b]

(* ---- liveness ---- *)

\* stale = binding epoch < current epoch (spec Clean Up Old Precommits).
StaleExists == \E b \in Builds : hasPrecommit[b] /\ buildEpoch[b] < currentEpoch
NoStale == \A b \in Builds : ~(hasPrecommit[b] /\ buildEpoch[b] < currentEpoch)

StalePrecommitEventuallyGone == StaleExists ~> NoStale

(* ---- negated reachability witnesses for both duty-queue resolution arms ---- *)

DutyAdoptDrained ==
    \E b \in Builds : sawUnresolvedDuty[b] /\ grantOutcome[b] = "Adopted"
                   /\ buildState[b] = "Retired" /\ ~dutyQueued[b] /\ ~hasPrecommit[b]
DutyRejectDrained ==
    \E b \in Builds : sawUnresolvedDuty[b] /\ grantOutcome[b] = "Rejected"
                   /\ buildState[b] = "Retired" /\ ~dutyQueued[b] /\ ~hasPrecommit[b]

WITNESS_DUTY_ADOPT_DRAIN == ~DutyAdoptDrained
WITNESS_DUTY_REJECT_DRAIN == ~DutyRejectDrained

=============================================================================

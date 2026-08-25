------------------------ MODULE CaRelinkLaneComposition -----------------------
(*
The relink seam composed with the public contract of `CaRefLaneCore`.

This model intentionally treats the ref lane as a six-state component. Relink
does not know about PUT outcomes, resolver verdicts, or recovery walk details.
It may certify a source identity only while the lane is `Ready`; the receiver
may promote only that exact identity; and source deletion is enabled only after
the receiver owns it.
*)
EXTENDS TLC

CONSTANTS
    SabotageConfirmBlocked,
    SabotageSkipIdentity,
    SabotageDeleteUnowned

LaneStates == {"Ready", "Writing", "Wedged", "NeedsRecovery", "Closed", "Faulted"}
Bindings == {"none", "blob", "other"}

VARIABLES
    lane,
    source_exists,
    confirmed_binding,
    receiver_binding,
    promoted,
    bad_confirmation,
    bad_promotion,
    saw_confirmation,
    saw_blocked_refusal,
    saw_recovery,
    saw_promotion,
    saw_delete

vars ==
    << lane, source_exists, confirmed_binding, receiver_binding, promoted,
       bad_confirmation, bad_promotion, saw_confirmation,
       saw_blocked_refusal, saw_recovery, saw_promotion, saw_delete >>

Init ==
    /\ lane = "Ready"
    /\ source_exists = TRUE
    /\ confirmed_binding = "none"
    /\ receiver_binding = "none"
    /\ promoted = FALSE
    /\ bad_confirmation = FALSE
    /\ bad_promotion = FALSE
    /\ saw_confirmation = FALSE
    /\ saw_blocked_refusal = FALSE
    /\ saw_recovery = FALSE
    /\ saw_promotion = FALSE
    /\ saw_delete = FALSE

StartWrite ==
    /\ lane = "Ready"
    /\ lane' = "Writing"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

CommitWrite ==
    /\ lane = "Writing"
    /\ lane' = "Ready"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

WriteUnresolved ==
    /\ lane = "Writing"
    /\ lane' = "Wedged"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

RequireRecovery ==
    /\ lane \in {"Writing", "Wedged"}
    /\ lane' = "NeedsRecovery"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

Recover ==
    /\ lane = "NeedsRecovery"
    /\ lane' = "Ready"
    /\ saw_recovery' = TRUE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal,
                    saw_promotion, saw_delete >>

CloseLane ==
    /\ lane = "Wedged"
    /\ lane' = "Closed"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

FaultLane ==
    /\ lane = "Wedged"
    /\ lane' = "Faulted"
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete >>

ConfirmSource ==
    /\ lane = "Ready"
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ saw_confirmation' = TRUE
    /\ UNCHANGED << lane, source_exists, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete >>

RefuseBlockedConfirmation ==
    /\ lane # "Ready"
    /\ saw_blocked_refusal' = TRUE
    /\ UNCHANGED << lane, source_exists, confirmed_binding,
                    receiver_binding, promoted, bad_confirmation,
                    bad_promotion, saw_confirmation, saw_recovery,
                    saw_promotion, saw_delete >>

ConfirmWhileBlocked ==
    /\ SabotageConfirmBlocked
    /\ lane # "Ready"
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ bad_confirmation' = TRUE
    /\ UNCHANGED << lane, source_exists, receiver_binding, promoted,
                    bad_promotion, saw_confirmation, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete >>

PromoteExactIdentity ==
    /\ confirmed_binding = "blob"
    /\ receiver_binding' = confirmed_binding
    /\ promoted' = TRUE
    /\ saw_promotion' = TRUE
    /\ UNCHANGED << lane, source_exists, confirmed_binding,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_delete >>

PromoteDifferentIdentity ==
    /\ SabotageSkipIdentity
    /\ confirmed_binding = "blob"
    /\ receiver_binding' = "other"
    /\ promoted' = TRUE
    /\ bad_promotion' = TRUE
    /\ UNCHANGED << lane, source_exists, confirmed_binding,
                    bad_confirmation, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion,
                    saw_delete >>

DeleteSource ==
    /\ promoted
    /\ receiver_binding = "blob"
    /\ source_exists' = FALSE
    /\ saw_delete' = TRUE
    /\ UNCHANGED << lane, confirmed_binding, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion >>

DeleteBeforeOwnership ==
    /\ SabotageDeleteUnowned
    /\ ~promoted
    /\ source_exists
    /\ source_exists' = FALSE
    /\ saw_delete' = TRUE
    /\ UNCHANGED << lane, confirmed_binding, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion >>

Next ==
    \/ StartWrite
    \/ CommitWrite
    \/ WriteUnresolved
    \/ RequireRecovery
    \/ Recover
    \/ CloseLane
    \/ FaultLane
    \/ ConfirmSource
    \/ RefuseBlockedConfirmation
    \/ ConfirmWhileBlocked
    \/ PromoteExactIdentity
    \/ PromoteDifferentIdentity
    \/ DeleteSource
    \/ DeleteBeforeOwnership

Spec == Init /\ [][Next]_vars

TypeOK ==
    /\ lane \in LaneStates
    /\ source_exists \in BOOLEAN
    /\ confirmed_binding \in Bindings
    /\ receiver_binding \in Bindings
    /\ promoted \in BOOLEAN
    /\ bad_confirmation \in BOOLEAN
    /\ bad_promotion \in BOOLEAN
    /\ saw_confirmation \in BOOLEAN
    /\ saw_blocked_refusal \in BOOLEAN
    /\ saw_recovery \in BOOLEAN
    /\ saw_promotion \in BOOLEAN
    /\ saw_delete \in BOOLEAN

ConfirmationRequiresReady == ~bad_confirmation
PromotionUsesConfirmedIdentity == ~bad_promotion
DeletedSourceIsOwned ==
    ~source_exists => (promoted /\ receiver_binding = "blob")

W_Confirmation == ~saw_confirmation
W_BlockedRefusal == ~saw_blocked_refusal
W_Recovery == ~saw_recovery
W_Promotion == ~saw_promotion
W_Delete == ~saw_delete

=============================================================================

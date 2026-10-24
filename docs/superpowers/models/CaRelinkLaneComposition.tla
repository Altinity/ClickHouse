------------------------ MODULE CaRelinkLaneComposition -----------------------
(*
The relink seam composed with the public contract of `CaRefLaneCore`.

This model intentionally treats the ref lane as a six-state component. Relink
does not know about PUT outcomes, resolver verdicts, or recovery walk details.
It may certify a source identity while the lane is `Ready`, or while it is `Writing` and the
outstanding mutation does not touch that identity (the touch is a nondeterministic parameter of
`StartWrite`, since this model has no transaction content); the receiver may promote only that
exact identity; and source deletion is enabled only after the receiver owns it.
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
    outstanding_touches,
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
    saw_delete,
    saw_confirmed_outside_ready

vars ==
    << lane, outstanding_touches, source_exists, confirmed_binding, receiver_binding, promoted,
       bad_confirmation, bad_promotion, saw_confirmation,
       saw_blocked_refusal, saw_recovery, saw_promotion, saw_delete,
       saw_confirmed_outside_ready >>

Init ==
    /\ lane = "Ready"
    /\ outstanding_touches = FALSE
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
    /\ saw_confirmed_outside_ready = FALSE

StartWrite(touch) ==
    /\ lane = "Ready"
    /\ lane' = "Writing"
    /\ outstanding_touches' = touch
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

CommitWrite ==
    /\ lane = "Writing"
    /\ lane' = "Ready"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

WriteUnresolved ==
    /\ lane = "Writing"
    /\ lane' = "Wedged"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

RequireRecovery ==
    /\ lane \in {"Writing", "Wedged"}
    /\ lane' = "NeedsRecovery"
    /\ outstanding_touches' = FALSE
    /\ UNCHANGED << source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

Recover ==
    /\ lane = "NeedsRecovery"
    /\ lane' = "Ready"
    /\ saw_recovery' = TRUE
    /\ UNCHANGED << outstanding_touches, source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

CloseLane ==
    /\ lane = "Wedged"
    /\ lane' = "Closed"
    /\ UNCHANGED << outstanding_touches, source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

FaultLane ==
    /\ lane = "Wedged"
    /\ lane' = "Faulted"
    /\ UNCHANGED << outstanding_touches, source_exists, confirmed_binding, receiver_binding,
                    promoted, bad_confirmation, bad_promotion,
                    saw_confirmation, saw_blocked_refusal, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

(* The lane certifies the identity: Ready, or Writing with an outstanding mutation that leaves the
   identity alone.  Wedged and the broken states never certify. *)
Confirmable == lane = "Ready" \/ (lane = "Writing" /\ ~outstanding_touches)

ConfirmSource ==
    /\ Confirmable
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ saw_confirmation' = TRUE
    /\ saw_confirmed_outside_ready' = (saw_confirmed_outside_ready \/ lane = "Writing")
    /\ UNCHANGED << lane, outstanding_touches, source_exists, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete >>

RefuseBlockedConfirmation ==
    /\ ~Confirmable
    /\ saw_blocked_refusal' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, confirmed_binding,
                    receiver_binding, promoted, bad_confirmation,
                    bad_promotion, saw_confirmation, saw_recovery,
                    saw_promotion, saw_delete, saw_confirmed_outside_ready >>

ConfirmWhileBlocked ==
    /\ SabotageConfirmBlocked
    /\ ~Confirmable
    /\ source_exists
    /\ confirmed_binding' = "blob"
    /\ bad_confirmation' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, receiver_binding, promoted,
                    bad_promotion, saw_confirmation, saw_blocked_refusal,
                    saw_recovery, saw_promotion, saw_delete, saw_confirmed_outside_ready >>

PromoteExactIdentity ==
    /\ confirmed_binding = "blob"
    /\ receiver_binding' = confirmed_binding
    /\ promoted' = TRUE
    /\ saw_promotion' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, confirmed_binding,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_delete, saw_confirmed_outside_ready >>

PromoteDifferentIdentity ==
    /\ SabotageSkipIdentity
    /\ confirmed_binding = "blob"
    /\ receiver_binding' = "other"
    /\ promoted' = TRUE
    /\ bad_promotion' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, source_exists, confirmed_binding,
                    bad_confirmation, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion,
                    saw_delete, saw_confirmed_outside_ready >>

DeleteSource ==
    /\ promoted
    /\ receiver_binding = "blob"
    /\ source_exists' = FALSE
    /\ saw_delete' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, confirmed_binding, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion, saw_confirmed_outside_ready >>

DeleteBeforeOwnership ==
    /\ SabotageDeleteUnowned
    /\ ~promoted
    /\ source_exists
    /\ source_exists' = FALSE
    /\ saw_delete' = TRUE
    /\ UNCHANGED << lane, outstanding_touches, confirmed_binding, receiver_binding, promoted,
                    bad_confirmation, bad_promotion, saw_confirmation,
                    saw_blocked_refusal, saw_recovery, saw_promotion, saw_confirmed_outside_ready >>

Next ==
    \/ \E touch \in BOOLEAN : StartWrite(touch)
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
    /\ outstanding_touches \in BOOLEAN
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
    /\ saw_confirmed_outside_ready \in BOOLEAN

ConfirmationRequiresUntouchedIdentity == ~bad_confirmation
PromotionUsesConfirmedIdentity == ~bad_promotion
DeletedSourceIsOwned ==
    ~source_exists => (promoted /\ receiver_binding = "blob")

W_Confirmation == ~saw_confirmation
W_BlockedRefusal == ~saw_blocked_refusal
W_Recovery == ~saw_recovery
W_Promotion == ~saw_promotion
W_Delete == ~saw_delete
W_ConfirmedOutsideReady == ~saw_confirmed_outside_ready

=============================================================================

-------------------- MODULE CaWriterEpochBackfillCore --------------------
(*
  Mandatory numeric writer-epoch backfill for one namespace.

  A namespace was active at epoch 1, then inactive while the pool's global writer epoch advances.
  Before a recovery, fold or destructive consumer may use epoch E, it must materialize every unused
  epoch 2..E as the empty `EpochSeal` at sequence 1.  A seal for E contains the immediately prior
  epoch E - 1, so a direct E -> E + 2 link cannot look like a closed history.

  This deliberately models neither logs nor LIST.  Their correctness is owned by
  `CaRefTableSnapshotLogCore`; this module is the small numeric closure that prevents a consumer from
  treating an inactive interval as an unobservable gap.  `SabotageDirectSkip` admits the forbidden
  direct transition as a negative control.  It must violate `INV_NO_EPOCH_SKIP`.
*)

EXTENDS Integers, FiniteSets

CONSTANTS MaxEpoch, SabotageDirectSkip, SabotageFrontierAfterSeal, SabotageSnapshotBaseAtSeal

Epochs == 1..MaxEpoch
AuthorizationKinds == {"recovery", "fold", "destructive"}

VARIABLES
    global_epoch,       \* pool-global writer epoch
    consumer_epoch,     \* greatest epoch this namespace consumer has closed
    epoch_seal,         \* [epoch -> predecessor epoch], 0 means no seq-1 empty seal
    authorized_at,      \* [authorization kind -> epoch], 0 means not granted
    decoded_terminal,   \* epoch of the terminal seal just decoded, 0 means none
    frontier_claim,     \* none | at decoded seal | same-epoch position after it
    checkpoint_base,    \* none | a real snapshot | an EpochSeal (corrupt)
    owner_set_authorized

vars == <<global_epoch, consumer_epoch, epoch_seal, authorized_at, decoded_terminal,
          frontier_claim, checkpoint_base, owner_set_authorized>>

RequiredSeals(through) == {epoch \in Epochs : epoch > 1 /\ epoch <= through}

HasImmediatePredecessorSeal(epoch) == epoch_seal[epoch] = epoch - 1

BackfilledThrough(through) ==
    \A epoch \in RequiredSeals(through) : HasImmediatePredecessorSeal(epoch)

MayAuthorize == consumer_epoch = global_epoch /\ BackfilledThrough(consumer_epoch)

TypeOK ==
    /\ global_epoch \in Epochs
    /\ consumer_epoch \in Epochs
    /\ epoch_seal \in [Epochs -> 0..MaxEpoch]
    /\ authorized_at \in [AuthorizationKinds -> 0..MaxEpoch]
    /\ decoded_terminal \in 0..MaxEpoch
    /\ frontier_claim \in {"none", "at_decoded_seal", "same_epoch_after_seal"}
    /\ checkpoint_base \in {"none", "snapshot", "epoch_seal"}
    /\ owner_set_authorized \in BOOLEAN

(* A consumer can be at E only after every numeric predecessor has a seq-1 empty seal. *)
INV_NO_EPOCH_SKIP ==
    /\ consumer_epoch <= global_epoch
    /\ BackfilledThrough(consumer_epoch)

(* All three consequential consumers share the same complete-chain gate. *)
INV_AUTHORIZATION_REQUIRES_BACKFILL ==
    \A kind \in AuthorizationKinds :
        authorized_at[kind] = 0 \/ BackfilledThrough(authorized_at[kind])

(* A decoded terminal seal closes its epoch. No consumer may authorize a frontier position after it
   in that SAME epoch; the next candidate is the numeric `{E + 1, 1}` position covered above. *)
INV_NO_SAME_EPOCH_FRONTIER_AUTHORIZATION ==
    frontier_claim # "same_epoch_after_seal" \/
        \A kind \in AuthorizationKinds : authorized_at[kind] = 0

(* `EpochSeal` is a no-op delimiter, never a snapshot body from which an owner set may be recovered. *)
INV_OWNER_SET_BASE_IS_NOT_EPOCH_SEAL ==
    \neg(owner_set_authorized /\ checkpoint_base = "epoch_seal")

(*
  A deliberately NEGATED non-vacuity witness.  TLC reports this invariant violated only after the
  honest path has granted recovery, fold and destructive authorization; it is true in the initial
  state, so an immediate counterexample cannot masquerade as reachability evidence.
*)
W_ALL_AUTHORIZATION_KINDS_REACHABLE ==
    \E kind \in AuthorizationKinds : authorized_at[kind] = 0

Init ==
    /\ global_epoch = 1
    /\ consumer_epoch = 1
    /\ epoch_seal = [epoch \in Epochs |-> 0]
    /\ authorized_at = [kind \in AuthorizationKinds |-> 0]
    /\ decoded_terminal = 0
    /\ frontier_claim = "none"
    /\ checkpoint_base = "none"
    /\ owner_set_authorized = FALSE

AdvanceGlobalEpoch ==
    /\ global_epoch < MaxEpoch
    /\ global_epoch' = global_epoch + 1
    /\ UNCHANGED <<consumer_epoch, epoch_seal, authorized_at, decoded_terminal, frontier_claim,
                  checkpoint_base, owner_set_authorized>>

(* The only honest transition consumes exactly E + 1 and writes its empty sequence-1 seal. *)
BackfillOneEpoch ==
    /\ consumer_epoch < global_epoch
    /\ LET next_epoch == consumer_epoch + 1 IN
       /\ epoch_seal[next_epoch] = 0
       /\ consumer_epoch' = next_epoch
       /\ epoch_seal' = [epoch_seal EXCEPT ![next_epoch] = consumer_epoch]
    /\ UNCHANGED <<global_epoch, authorized_at, decoded_terminal, frontier_claim, checkpoint_base,
                  owner_set_authorized>>

GrantAuthorization(kind) ==
    /\ kind \in AuthorizationKinds
    /\ authorized_at[kind] = 0
    /\ MayAuthorize
    /\ authorized_at' = [authorized_at EXCEPT ![kind] = consumer_epoch]
    /\ UNCHANGED <<global_epoch, consumer_epoch, epoch_seal, decoded_terminal, frontier_claim,
                  checkpoint_base, owner_set_authorized>>

DecodeTerminalSeal ==
    /\ decoded_terminal = 0
    /\ \A kind \in AuthorizationKinds : authorized_at[kind] = 0
    /\ decoded_terminal' = consumer_epoch
    /\ frontier_claim' = "at_decoded_seal"
    /\ UNCHANGED <<global_epoch, consumer_epoch, epoch_seal, authorized_at, checkpoint_base,
                  owner_set_authorized>>

(* Negative control: a consumer advances its frontier after the decoded terminal seal without crossing
   into E+1, then authorizes a recovery consumer from that false frontier. *)
FrontierAfterDecodedTerminal ==
    /\ SabotageFrontierAfterSeal
    /\ decoded_terminal # 0
    /\ frontier_claim = "at_decoded_seal"
    /\ frontier_claim' = "same_epoch_after_seal"
    /\ authorized_at' = [authorized_at EXCEPT !["recovery"] = consumer_epoch]
    /\ UNCHANGED <<global_epoch, consumer_epoch, epoch_seal, decoded_terminal, checkpoint_base,
                  owner_set_authorized>>

(* Negative control: treating a checkpoint's EpochSeal as a snapshot base exposes a fabricated
   recovered owner set. *)
AuthorizeOwnerSetFromEpochSeal ==
    /\ SabotageSnapshotBaseAtSeal
    /\ \neg owner_set_authorized
    /\ checkpoint_base' = "epoch_seal"
    /\ owner_set_authorized' = TRUE
    /\ UNCHANGED <<global_epoch, consumer_epoch, epoch_seal, authorized_at, decoded_terminal,
                  frontier_claim>>

(*
  Negative control only: a regressed caller writes the sequence-1 seal at E + 2 and advances the
  consumer directly, leaving E + 1 absent.  It does not receive authorization because `MayAuthorize`
  remains the complete-chain predicate; `INV_NO_EPOCH_SKIP` makes the admitted skip observable.
*)
DirectSkipEpoch ==
    /\ SabotageDirectSkip
    /\ consumer_epoch + 2 <= global_epoch
    /\ LET skipped_epoch == consumer_epoch + 2 IN
       /\ epoch_seal[skipped_epoch] = 0
       /\ consumer_epoch' = skipped_epoch
       /\ epoch_seal' = [epoch_seal EXCEPT ![skipped_epoch] = consumer_epoch]
    /\ UNCHANGED <<global_epoch, authorized_at, decoded_terminal, frontier_claim, checkpoint_base,
                  owner_set_authorized>>

Next ==
    \/ AdvanceGlobalEpoch
    \/ BackfillOneEpoch
    \/ (\E kind \in AuthorizationKinds : GrantAuthorization(kind))
    \/ DecodeTerminalSeal
    \/ FrontierAfterDecodedTerminal
    \/ AuthorizeOwnerSetFromEpochSeal
    \/ DirectSkipEpoch

Spec == Init /\ [][Next]_vars

=============================================================================

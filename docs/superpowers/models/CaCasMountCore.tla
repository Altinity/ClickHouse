-------------------------- MODULE CaCasMountCore --------------------------
(***************************************************************************)
(* TLA+ safety gate for the content-addressed "mount ownership + server-   *)
(* root identity" subsystem.                                               *)
(*                                                                         *)
(* One modeled server_root_id, three objects + two server Actors (A,B,     *)
(* each a distinct fixed ServerUUID):                                      *)
(*                                                                         *)
(*   owner   : Actors \cup {None}  -- sticky identity; once set never      *)
(*             changes (the server_root claim is first-writer-wins).       *)
(*   epoch   : 0..MaxEpoch         -- durable monotone counter living in   *)
(*             its OWN object (NOT the mount).                             *)
(*   mount   : None, or [uuid, epoch, deadline, fenced] -- a TTL lease.    *)
(*             `fenced` models the GC heartbeat-floor fence-out stamp      *)
(*             (gc_fenced = true): terminal for that (uuid, epoch).        *)
(*   mtoken  : 0..MaxToken         -- the slot's write-version (the S3     *)
(*             ETag/token); every mount write bumps it. Token-guarded      *)
(*             writes (the keeper's adopt CAS) compare against it.        *)
(*                                                                         *)
(* 2026-07-06 P3.1 extension (fence recovery, spec                         *)
(* 2026-07-06-cas-mount-lease-fence-recovery-design.md):                   *)
(*   - GcFence: any expired, unfenced mount may be stamped fenced by the   *)
(*     pool's GC (uuid/epoch preserved, token bumped). The fence is        *)
(*     TERMINAL for that (uuid, epoch): "a fence costs an epoch".          *)
(*   - The keeper's adopt is NON-ATOMIC (AdoptRead then AdoptWrite with an *)
(*     interleaving point) -- the real `MountLeaseKeeper::claim` is        *)
(*     GET-then-CAS, and the S13 wedge lives in that window.               *)
(*   - Late renewal: `Renew` may fire after the deadline passed (the       *)
(*     beat-blocked renewal); on a fenced slot it classifies as            *)
(*     fenced-by-gc and schedules a remount (new epoch), never a wedge.    *)
(*   - Recovery loop: fenced same-epoch is NOT adoptable/refreshable; the  *)
(*     actor re-allocates a fresh epoch and reclaims (remount).            *)
(*                                                                         *)
(* GATE (positive cfg, all Sab*=FALSE): owner is sticky, a mount is never  *)
(* held by a non-owner, epochs are strictly monotone + writer-unique, a    *)
(* superseded writer performs no new mutation, NO live mount body ever     *)
(* exists under a fenced (uuid, epoch) [FenceCostsEpoch], and no actor     *)
(* ever wedges permanently [NoPermanentWedge].                             *)
(*                                                                         *)
(* Sabotage CONSTANT booleans reproduce the guarded-against states:        *)
(*   SabForeignTakeover   -> ForeignUuidNeverAutoTakesOver violated        *)
(*   SabEpochReset        -> WriterEpochMonotoneUnique violated            *)
(*   SabAdoptWedgeOnTouch -> NoPermanentWedge violated (the OLD adopt:     *)
(*     a token mismatch at AdoptWrite fails closed PERMANENTLY -- the      *)
(*     pre-fix `LOGICAL_ERROR` abort during `Store::open`, exit 49)        *)
(*   SabAdoptIgnoresFence -> FenceCostsEpoch violated (the OLD adopt read  *)
(*     that skips the gc_fenced check: same-epoch resurrection of a        *)
(*     fenced incarnation)                                                 *)
(*                                                                          *)
(* 2026-07-14 rev.6 extension (observation-based reclaim, spec              *)
(* 2026-07-13-cas-ref-lease-exclusivity-rev6-design.md, S:unclean-takeover, *)
(* S:gc-fence-out): a reclaimer never trusts a foreign wall clock. The      *)
(* holder's stamp (`mount.deadline`) and its TRUE local-fence expiry        *)
(* (`fenceUntil`) can differ by up to `Drift` ticks (bounded clock-RATE     *)
(* skew, not a clock-sync error). `SabWallClockReclaim` reproduces the OLD  *)
(* bug: a reclaimer that trusts the stamp directly (`clock > mount.deadline`)*)
(* instead of waiting out the full `TTL + Drift` on ITS OWN clock after     *)
(* observing the token hold stable (`ObservedReclaim`).                     *)
(*                                                                          *)
(* 2026-07-14 rev.6 round 8 (reconciled package, two independent           *)
(* consult/review passes converged on this): rounds 2-7 iterated on how a  *)
(* reclaim should be represented (epoch-only bump, then a bolted-on        *)
(* `heldToken` token-chain) and kept surfacing new masked/false verdicts.  *)
(* The root cause both passes converged on: the model's reclaim was        *)
(* BODY-INVISIBLE while the product's reclaim is a token-guarded           *)
(* `putOverwrite` that installs the SUCCESSOR'S BODY                       *)
(* (`CasServerRoot.cpp:331-332`). Round 8 makes the reclaim install that    *)
(* body (P1) and lets the model's PRE-EXISTING body classification         *)
(* (`Renew`/`ClaimMount`/`AdoptRead`, all present since round 0/P3.1) do    *)
(* the re-arm-blocking work -- no compensating field needed, so `heldToken` *)
(* is deleted (P3). `Write` shrinks to a PURE LOCAL check (P2, the         *)
(* product's `Store::mayMutate`/`refAppendFenceOk`, `CasStore.cpp:201-226`, *)
(* reads no shared state at all): `~rejected /\ ~wedged /\ ~crashed[a] /\   *)
(* owner=a /\ clock<fenceUntil` -- `mount#None`, `mount.uuid=a`, `epochOK`, *)
(* `~mount.fenced` all removed as unfaithful per-write body reads the real *)
(* write path never performs. `ClaimMount` gains a STRICT-ORDER guard      *)
(* (P4, `CasStore.cpp:312-316`): an actor may claim/mint/reclaim only with  *)
(* the LATEST durable epoch it holds (`localEpoch[a] >= epoch`), which     *)
(* closes the `ClearExpiredMount` + fresh-mint re-arm path no prior round   *)
(* caught. `SabSupersededWrites` (and `_sab_supersededwrites.cfg`) are      *)
(* RETIRED: with `epochOK` gone from `Write` by construction, the flag has  *)
(* nothing left to toggle. `SupersededWriterMakesNoMutation` (the          *)
(* knowledge-based invariant) and its `localLost`/`lostThenWrote` machinery *)
(* are KEPT -- they still have live readers/checkers (see the task         *)
(* report's round-8 kept-vs-removed table) -- but are no longer trusted    *)
(* in `_sab_wallclockreclaim.cfg`'s invariant list (dropped there: with a   *)
(* faithful reclaim body a `Renew`-then-`Write` drift path COULD trip it,   *)
(* masked only by the shallower `GlobalSupersededWriterMakesNoMutation`     *)
(* violation -- keeping it there invites "why is this green" confusion).    *)
(* `rev6_observe`'s GREEN (with `ObservedReclaim` enabled, no monotone      *)
(* witness bundled) is now THE reclaim-faithfulness regression detector,    *)
(* superseding the round-6 canary role.                                    *)
(*                                                                          *)
(* 2026-07-14 rev.6 round 9 (`GcFence` made Drift-aware, per the design     *)
(* spec's own decision #2 -- "GC fence-out becomes observation-based"):     *)
(* round 8's `rev6_observe.cfg` came back RED on the knowledge-based        *)
(* `SupersededWriterMakesNoMutation` because `GcFence` still used a bare    *)
(* wall-clock check (`mount.deadline <= clock`), unlike `ObservedReclaim`.  *)
(* `GcFence`'s guard is now `mount.deadline + Drift <= clock`, which makes   *)
(* `GcFence` and `Write` mutually exclusive on the same mount BY            *)
(* CONSTRUCTION, for every `Drift` (see `GcFence`'s own comment for the     *)
(* proof) -- byte-identical to the old guard at `Drift = 0`, so every       *)
(* legacy cfg's semantics are unchanged. This is the model's gate for a     *)
(* Task 9 C++ change (`computeHeartbeatFloor`): do not regress the model    *)
(* back to a bare wall-clock `GcFence`.                                     *)
(*                                                                          *)
(* 2026-07-14 rev.6 round 10 (class audit + `ClearExpiredMount` closes the  *)
(* class): round 9's fix stopped hiding a SECOND, previously-masked false   *)
(* alarm on `SupersededWriterMakesNoMutation` via `ClearExpiredMount`       *)
(* (bare `mount.deadline <= clock`, feeding `AdoptWrite`'s "vanished while  *)
(* adopting" fallback). Decision #2's PRINCIPLE generalizes beyond the      *)
(* fence-out flavor specifically named: ANY OBSERVER-side death verdict on *)
(* a mount -- fence it, clear it, reclaim it -- must clear the full        *)
(* rate-bound observation threshold (`TTL + Drift`) before concluding the  *)
(* record is dead; `ClearExpiredMount` is GC bookkeeping on an              *)
(* already-presumed-dead mount and follows the SAME rule `GcFence` does,   *)
(* even though (unlike `GcFence`/`computeHeartbeatFloor`) it is not tied   *)
(* to one specific named product function. Its guard is now                *)
(* `mount.deadline + Drift <= clock`, same mutual-exclusion-with-`Write`    *)
(* proof, byte-identical at `Drift = 0`. A full audit of every clock        *)
(* comparison in this model (see the task report's round-10 table) found   *)
(* exactly these TWO observer-side bare wall-clock sites -- `GcFence`       *)
(* (round 9) and `ClearExpiredMount` (this round) -- and nothing else:      *)
(* `ClaimMount`'s `expired` is HOLDER-side self-reclaim (see its own        *)
(* comment); `Write`'s `clock < fenceUntil` is the holder's own ground      *)
(* truth; `ObservedReclaim` was already Drift-aware by design;              *)
(* `WallClockReclaim` is the deliberate sabotage this suite exists to       *)
(* catch. `rev6_observe.cfg` is now fully GREEN and EXHAUSTIVE -- the       *)
(* matrix is trustworthy against this class BY AUDIT, not by luck.          *)
(***************************************************************************)
EXTENDS Integers, FiniteSets

CONSTANTS
    Actors,               \* server identities (distinct ServerUUIDs), e.g. {A, B}
    None,                 \* "no owner / no mount" sentinel (distinct from any Actor)
    MaxClock,             \* bound on abstract clock (TLC finiteness)
    MaxEpoch,             \* bound on the durable epoch counter
    MaxToken,             \* bound on the mount write-version counter (TLC finiteness)
    TTL,                  \* lease time-to-live added to clock at claim/renew
    SabForeignTakeover,   \* FALSE = honest; TRUE drops owner=a guard on an expired-mount claim
    SabEpochReset,        \* FALSE = honest; TRUE enables an action zeroing epoch when mount cleared
    SabAdoptWedgeOnTouch, \* FALSE = honest; TRUE makes an adopt token-mismatch a PERMANENT wedge
    SabAdoptIgnoresFence, \* FALSE = honest; TRUE lets AdoptRead accept a fenced same-epoch body
    Drift,                \* max extra ticks the holder's TRUE local-fence expiry outlives the stamp
    SabWallClockReclaim,  \* FALSE = honest observation-based reclaim; TRUE = trust the stamp (rev.6 bug)
    SabEpochGuardOff,     \* FALSE = honest (epoch re-mint from 0 requires mount = None); TRUE drops
                          \* that guard -- models the pre-fix allocateWriterEpoch
    SabDecomBlindBypass   \* FALSE = honest (decommission re-mint requires a TERMINAL mount); TRUE =
                          \* the rejected blind bypass -- mint epoch 1 regardless of mount liveness
                          \* (the round-3 finding-1 bug)

VARIABLES
    owner,        \* Actors \cup {None}; sticky once non-None
    epoch,        \* 0..MaxEpoch; durable monotone counter (its own object)
    epochCeiling, \* 2026-07-24 fence-not-rescue gate: history -- monotone high-water mark over
                  \* every value `epoch` has ever held (mirrors `epoch` on every increase, stays
                  \* put on every wipe/reset). Purely derived bookkeeping: drives the
                  \* genesis-vs-wipe guard on `RemintEpoch`/`RemintEpochDecom`/`AllocEpoch`/
                  \* `ObservedReclaim`/`WallClockReclaim` (`epoch = 0 /\ epochCeiling > 0` means
                  \* a genuine prior allocation was WIPED, distinct from bare genesis/never-yet-
                  \* allocated). NOT used by `WriterEpochMonotoneUnique` (an earlier draft tried
                  \* that and it was wrong -- see that invariant's own comment for why).
    epochWiped,   \* 2026-07-24: BOOLEAN, history -- TRUE once `WipeEpoch` has EVER fired on
                  \* this behavior. `WipeEpoch` is a ONE-SHOT-per-trace event: without this
                  \* guard, `WipeEpoch` composes with `AllocEpoch`/`RemintEpoch`/`RemintEpochDecom`
                  \* re-minting and wiping again, repeatedly, multiplying the explored state
                  \* space without adding any new KIND of scenario (a second, third, ... loss
                  \* is not qualitatively different from the first for what this gate's
                  \* invariants can distinguish) -- confirmed empirically: `rev6_observe.cfg`
                  \* reached 179M states generated / 28M distinct with an UNBOUNDED, still-
                  \* growing queue before this guard. Both negative-control cfgs
                  \* (`_sab_epochwipelive`, `_sab_decomblindbypass`) stay reachable with a
                  \* SINGLE wipe (that is what births the same-pair twin); repeated epoch-object
                  \* loss is out of this gate's modeled scope by deliberate, documented choice
                  \* (a state-space bound, not a claim that a second loss is safe or unsafe).
    mount,        \* None, or [uuid |-> Actor, epoch |-> Nat, deadline |-> Nat, fenced |-> BOOLEAN]
    mtoken,       \* 0..MaxToken; bumped by EVERY mount write (claim/renew/fence/adopt/reclaim)
    clock,        \* 0..MaxClock; abstract time
    localEpoch,   \* [Actors -> 0..MaxEpoch]; the epoch an actor last allocated for itself
    localLost,    \* [Actors -> BOOLEAN]; TRUE once the actor learned it was superseded/fenced --
                  \* PURE KNOWLEDGE (round 3): appears in NO safety guard, only in the dedicated
                  \* knowledge-based witness (`lostThenWrote`/`SupersededWriterMakesNoMutation`)
    crashed,      \* rev.6 round 3: [Actors -> BOOLEAN]; TRUE = a MECHANICAL fact -- this actor's
                  \* process is genuinely dead (set by `Die`). A crashed process not writing is
                  \* PHYSICS, not politeness, so `Write` gates on `~crashed[a]` directly -- disjoint
                  \* from `localLost` (knowledge), which a dead process cannot even possess.
    rejected,     \* [Actors -> BOOLEAN]; TRUE once a foreign actor was permanently rejected
    adoptObs,     \* [Actors -> (0..MaxToken) \cup {None}]; the token AdoptRead observed (in-flight adopt)
    fencedEpochs, \* SUBSET (Actors \X (0..MaxEpoch)); every (uuid, epoch) a GcFence ever stamped
    wedged,       \* [Actors -> BOOLEAN]; TRUE = permanent fail-closed abort (the S13 wedge state)
    wrote,        \* SUBSET (Actors \X (0..MaxEpoch)); mutations performed, tagged by live epoch
    rootEmpty,    \* BOOLEAN; FALSE once any Write happened
    firstOwner,   \* history: the first actor to ever own (sticky-owner witness)
    lostThenWrote,\* history: TRUE if a superseded actor ever entered a NEW write (must stay FALSE)
    reclaimed,    \* history: [Actors -> BOOLEAN]; TRUE once a reclaimed its OWN expired mount
    remountedAfterFence, \* history: [Actors -> BOOLEAN]; TRUE once a re-mounted after being fenced
    fenceUntil,   \* rev.6: holder's TRUE local-fence expiry (stamp + nondet skew <= Drift)
    obsToken,     \* rev.6: reclaimer's observation -- mtoken value first seen (None = unarmed)
    obsSince,     \* rev.6: clock tick at which obsToken was first observed
    observedReclaimEver, \* rev.6 history: TRUE once ObservedReclaim has ever completed (witness)
    supersededThenWrote \* rev.6 history: TRUE if Write ever fired while `epoch` (GLOBAL truth,
                        \* the durable counter) had already advanced past the writer's OWN
                        \* `localEpoch[a]` -- i.e. a completed reclaim the writer did not know about

vars == << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
           adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
           lostThenWrote, reclaimed, remountedAfterFence,
           fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

Init ==
    /\ owner = None
    /\ epoch = 0
    /\ epochCeiling = 0
    /\ epochWiped = FALSE
    /\ mount = None
    /\ mtoken = 0
    /\ clock = 0
    /\ localEpoch = [a \in Actors |-> 0]
    /\ localLost = [a \in Actors |-> FALSE]
    /\ crashed = [a \in Actors |-> FALSE]
    /\ rejected = [a \in Actors |-> FALSE]
    /\ adoptObs = [a \in Actors |-> None]
    /\ fencedEpochs = {}
    /\ wedged = [a \in Actors |-> FALSE]
    /\ wrote = {}
    /\ rootEmpty = TRUE
    /\ firstOwner = None
    /\ lostThenWrote = FALSE
    /\ reclaimed = [a \in Actors |-> FALSE]
    /\ remountedAfterFence = [a \in Actors |-> FALSE]
    /\ fenceUntil = 0
    /\ obsToken = None
    /\ obsSince = 0
    /\ observedReclaimEver = FALSE
    /\ supersededThenWrote = FALSE

----------------------------------------------------------------------------
\* Claim an empty, unowned server_root: first-writer-wins, sticky from here on.
ClaimOwnerEmpty(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = None
    /\ rootEmpty
    /\ owner' = a
    /\ firstOwner' = IF firstOwner = None THEN a ELSE firstOwner
    /\ UNCHANGED << epochCeiling, epochWiped, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A foreign uuid hits an already-owned root: fail closed, permanently rejected.
RejectForeignOwner(a) ==
    /\ owner # None
    /\ owner # a
    /\ ~rejected[a]
    /\ rejected' = [rejected EXCEPT ![a] = TRUE]
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* The owner bumps the durable epoch counter and records it as its live epoch.
\* rev.6 round 5: guarded against firing while `a`'s own mount record is still UNFENCED --
\* the product's `allocateWriterEpoch` is invoked exactly once, strictly before any mount
\* exists for this open() attempt (`CasStore.cpp:312-316`'s own "STRICT ORDER: ... claim
\* owner (identity) -> allocate durable writer_epoch -> claim the mount lease (liveness) ..."
\* comment), or after the fence-recovery loop has ALREADY reset the previous (now-abandoned)
\* keeper (`CasStore.cpp:454-455`, `mount_keeper.reset()` before reallocating; the
\* `FencedSelf` retry at `:413` likewise only fires when our own fresh attempt was ALREADY
\* fenced). NOTE: this is NOT gated on wall-clock expiry (`mount.deadline`/`clock`) -- `Renew`
\* is explicitly allowed to fire on a wall-clock-expired-but-unfenced mount ("the beat-blocked
\* renewal", see its own comment), so mere expiry does not mean the actor has abandoned this
\* epoch; only a GENUINE FENCE does ("a fence costs an epoch", the model's own P3.1
\* philosophy). A guard keyed on `mount.deadline > clock` still let `AllocEpoch` fire right
\* at/after the wall-clock deadline while `mount.fenced` was still FALSE, reproducing the
\* SAME false alarm this round already fixed once (verified via TLC after the first,
\* deadline-keyed attempt at this guard came back RED with an identical trace).
AllocEpoch(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch < MaxEpoch
    /\ (epoch > 0 \/ epochCeiling = 0)   \* 2026-07-24: the PRESENT-epoch CAS-bump branch --
                                         \* `epoch = 0 /\ epochCeiling > 0` is a genuine WIPE
                                         \* of a previously-allocated epoch (the object reads
                                         \* ABSENT), routed exclusively through the Phase C
                                         \* absent-epoch branch (`RemintEpoch`/
                                         \* `RemintEpochDecom`, guarded); bare genesis
                                         \* (`epochCeiling = 0`, never yet allocated) still
                                         \* mints here as before (byte-identical to every
                                         \* pre-existing cfg, none of which could ever reach
                                         \* `epoch = 0 /\ epochCeiling > 0` before `WipeEpoch`).
    /\ ~(mount # None /\ mount.uuid = a /\ ~mount.fenced)
    /\ epoch' = epoch + 1
    /\ epochCeiling' = IF epoch' > epochCeiling THEN epoch' ELSE epochCeiling
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch + 1]
    /\ UNCHANGED << owner, epochWiped, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* Claim/reclaim the mount lease (the free-function `claimMount`, ONE atomic CAS in the
\* model: its real get+putOverwrite failure mode is a clean LiveDoubleStart retry, not
\* the wedge -- the wedge lives in the keeper's split adopt below).
\* Branch order mirrors the code:
\*   foreign uuid            -> not claimable here (fail closed; RejectForeignOwner covers identity)
\*   same uuid + SAME epoch  -> "refresh"; FIXED protocol refuses when fenced (a fence
\*                              costs an epoch -- no same-epoch resurrection)
\*   fenced (diff epoch)     -> reclaim IMMEDIATELY, expiry regardless (fence is terminal)
\*   expired                 -> reclaim
\*   mount = None            -> fresh mint
\* A same-uuid DIFFERENT-epoch LIVE unfenced mount is NOT claimable -> double-start blocked.
\* SabForeignTakeover drops the owner=a guard on the expired branch.
\* A successful (re)claim starts a FRESH incarnation: localLost resets, in-flight adopt
\* observation resets. History: reclaimed (own expired), remountedAfterFence (was fenced).
\* rev.6 round 3: `crashed` ALSO resets here -- this model's only rebirth path for a
\* crashed actor IS a successful (re)claim (a process restart), so a fresh incarnation
\* must clear the mechanical "dead" fact along with the knowledge flag, or `witness_reclaim`
\* / `witness_remountafterfence` (which require the SAME uuid to write again after
\* recovering) would become permanently unable to `Write` post-crash.
\* rev.6 round 8 (P4, STRICT-ORDER guard, consult/review-reconciled -- closes round-7's
\* Finding A): `strictOK == localEpoch[a] >= epoch` (same `SabForeignTakeover /\ expired`
\* bypass as `ownerOK`) gates the THREE claim-ESTABLISHING disjuncts -- fresh mint
\* (`mount = None`), `fencedReclaim`, and `expired /\ ~sameEpoch` -- mirroring
\* `CasStore.cpp:312-316`'s STRICT ORDER ("allocate durable writer_epoch -> claim the mount
\* lease"): an actor only ever claims/mints/reclaims with the LATEST durable epoch it holds.
\* `>=`, not `=`: `sab_epochreset`'s post-`SabResetEpoch` re-mint (`localEpoch[a]` unchanged,
\* `epoch` zeroed) must still land to reach its own violation; `=` would wedge that cfg.
\* `refreshOK` (the SAME-epoch continuation) is deliberately NOT strictOK-gated: with P1
\* (below) installing the successor's body on every reclaim, `sameEpoch` (`mount.epoch =
\* localEpoch[a]`) can only hold when `a`'s own body is still exactly what it last wrote,
\* which already implies `localEpoch[a] = epoch` (no reclaim has landed since), making
\* `strictOK` redundant there.
\*
\* rev.6 round 10 (bare-wall-clock audit, HOLDER-side, no `+ Drift` needed): `expired`
\* (`mount.deadline <= clock`, bare wall-clock, deliberately NOT touched this round) looks
\* like `GcFence`/`ClearExpiredMount`'s pattern but is NOT an observer-side death verdict on
\* a DIFFERENT still-alive party -- in the honest protocol `ownerOK` forces `mount.uuid = a =
\* owner` (sticky), so this is `a` examining its OWN prior record, exactly like `Renew`'s
\* late-renewal (which has NO expiry guard at all). Whatever `expired` decides, `ClaimMount`
\* always installs a FRESH `fenceUntil` for `a` itself (below), so there is no second party
\* whose STILL-VALID fence a premature verdict could ignore -- unlike `GcFence`/
\* `ClearExpiredMount`, which act on a mount whose TRUE holder may be a physically distinct,
\* still-alive process with its own independent `fenceUntil`. (The ONE place `expired` lets a
\* DIFFERENT actor take over -- the `SabForeignTakeover` bypass on `ownerOK`/`strictOK` -- is
\* the deliberate, already-flagged SABOTAGE this task exists to catch via
\* `ForeignUuidNeverAutoTakesOver`, not a hidden honest-path gap.) Separately, `expired /\
\* ~sameEpoch /\ strictOK` (the one `canClaim` disjunct that reads `expired` at all) is
\* REACHABILITY-VACUOUS in the honest protocol: `refreshOK` already covers same-epoch+unfenced
\* regardless of `expired`, `fencedReclaim` already covers fenced+diff-epoch regardless of
\* `expired`, and the only way to reach unfenced+diff-epoch is a completed reclaim that has
\* already advanced `epoch` past `a`'s own `localEpoch[a]` -- which `strictOK` then blocks.
ClaimMount(a) ==
    LET expired    == (mount # None) /\ (mount.deadline <= clock)
        ownExpired == expired /\ (mount.uuid = a)
        ownerOK    == IF SabForeignTakeover /\ expired THEN TRUE ELSE owner = a
        strictOK   == IF SabForeignTakeover /\ expired THEN TRUE ELSE localEpoch[a] >= epoch
        sameEpoch  == (mount # None) /\ (mount.uuid = a) /\ (mount.epoch = localEpoch[a])
        refreshOK  == sameEpoch /\ (~mount.fenced \/ SabAdoptIgnoresFence)
        fencedReclaim == (mount # None) /\ (mount.uuid = a) /\ mount.fenced
                         /\ (mount.epoch # localEpoch[a])
        canClaim   == (mount = None /\ strictOK) \/ refreshOK \/ (fencedReclaim /\ strictOK)
                      \/ (expired /\ ~sameEpoch /\ strictOK)
        wasFenced  == \E e \in 0..MaxEpoch : << a, e >> \in fencedEpochs
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ mtoken < MaxToken
    /\ ownerOK
    /\ canClaim
    /\ mount' = [uuid |-> a, epoch |-> localEpoch[a], deadline |-> clock + TTL, fenced |-> FALSE]
    /\ mtoken' = mtoken + 1
    /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6: TRUE local fence, stamp + skew
    /\ localLost' = [localLost EXCEPT ![a] = FALSE]
    /\ crashed' = [crashed EXCEPT ![a] = FALSE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ reclaimed' = IF ownExpired THEN [reclaimed EXCEPT ![a] = TRUE] ELSE reclaimed
    /\ remountedAfterFence' =
         IF wasFenced THEN [remountedAfterFence EXCEPT ![a] = TRUE] ELSE remountedAfterFence
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, clock, localEpoch, rejected, fencedEpochs, wedged,
                    wrote, rootEmpty, firstOwner, lostThenWrote,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* The mount holder renews -- possibly LATE (the beat-blocked renewal: no expiry guard,
\* a renewal may fire after the deadline passed and after a GcFence landed).
\*   own live epoch, not fenced -> extend the deadline (token-guarded write; bumps mtoken)
\*   own live epoch, FENCED     -> the fence took the token; classify by BODY (fixed
\*                                 protocol): fenced-by-gc -> schedule remount (localLost;
\*                                 the actor re-allocates an epoch and reclaims). Never a wedge.
\*   newer epoch on the slot    -> learn we were superseded. rev.6 round 8: this is now the
\*                                 SOLE mechanism blocking re-arm after a reclaim -- P1 (below)
\*                                 installs the successor's new-epoch body, so `mount.epoch #
\*                                 localEpoch[a]` here IS the reclaim (no `heldToken`
\*                                 compensating field needed, round 7's mechanism deleted).
\*                                 Models `renewOnce`'s cached-token CAS failing ->
\*                                 `onRenewMismatch` classifying "superseded by a newer
\*                                 incarnation" (`CasServerRoot.cpp:785-792`).
Renew(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ IF mount.epoch = localEpoch[a]
       THEN IF ~mount.fenced
            THEN /\ mtoken < MaxToken
                 /\ mount' = [mount EXCEPT !.deadline = clock + TTL]
                 /\ mtoken' = mtoken + 1
                 /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6
                 /\ UNCHANGED localLost
            ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                 /\ UNCHANGED << mount, mtoken, fenceUntil >>
       ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
            /\ UNCHANGED << mount, mtoken, fenceUntil >>
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, clock, localEpoch, crashed, rejected, adoptObs, fencedEpochs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* The pool's GC fences an EXPIRED, unfenced mount (computeHeartbeatFloor's token-guarded
\* fence-out): gc_fenced = true, uuid/epoch/deadline preserved, token bumped. The fence is
\* an ENVIRONMENT action (any GC leader; identity irrelevant to this model).
\*
\* rev.6 round 9 (Drift-aware, per the design spec's own decision log): the design spec
\* (`2026-07-13-cas-ref-lease-exclusivity-rev6-design.md`, decision #2) states plainly that
\* "GC fence-out becomes observation-based, and a `gc_fenced` lease is then a transferable
\* certificate of observed death" -- i.e. this model's `GcFence` is the gate for a Task 9
\* C++ change (`computeHeartbeatFloor`), not an already-settled mechanism outside this
\* task's scope. Round 8 exposed exactly why the bare wall-clock form (`mount.deadline <=
\* clock`) was unsound: it let a fence land while the holder's TRUE local fence
\* (`fenceUntil`, bounded by `mount.deadline + Drift` -- see `ClaimMount`/`Renew`/
\* `AdoptWrite`'s `\E d \in 0..Drift : fenceUntil' = clock + TTL + d`) had NOT yet expired,
\* a false alarm previously invisible only because `Write`'s old `~mount.fenced` conjunct
\* (removed round 8, P2) blindly trusted every fence regardless of whether it was premature.
\*
\* The observation-based form reduces algebraically to a single wall-clock comparison here
\* (no new observation-state machinery needed): "the stamp has been silent for the full
\* rate-bound wait (`TTL + Drift`) on the GC's own clock" is exactly "the clock has now
\* passed the holder's MAXIMUM POSSIBLE `fenceUntil`" -- both count `TTL + Drift` ticks
\* forward from the same event (the write that set `mount.deadline`). So the guard becomes
\* `mount.deadline + Drift <= clock`, i.e. `clock >= mount.deadline + Drift`.
\*
\* This makes `GcFence` and `Write` MUTUALLY EXCLUSIVE ON THIS MOUNT BY CONSTRUCTION, for
\* every `Drift`, not merely at `Drift = 0` (round 8 only showed the Drift=0 case): every
\* HOLDER-side write to `mount.deadline` picks its companion `fenceUntil` as `clock'' + TTL
\* + d` for SOME `d \in 0..Drift` at the SAME clock value that produced `mount.deadline =
\* clock'' + TTL`, so `fenceUntil <= mount.deadline + Drift` always holds (the `d = Drift`
\* case is the tight bound; TLC explores it). The two RECLAIM-side writes to
\* `mount.deadline` (`ObservedReclaim`/`WallClockReclaim` installing the successor body)
\* leave the DISPLACED holder's `fenceUntil` untouched while resetting `mount.deadline =
\* clock + TTL` forward, so `fenceUntil <= mount.deadline + Drift` is preserved there too
\* (the fence can only be FURTHER below the new deadline). `GcFence`'s guard `mount.deadline + Drift <= clock` then
\* directly implies `fenceUntil <= mount.deadline + Drift <= clock`, i.e. `clock >=
\* fenceUntil` -- so `Write`'s `clock < fenceUntil` conjunct is false whenever `GcFence`
\* could act. At `Drift = 0` this is byte-identical to the old guard (`d` ranges only over
\* `{0}`, so `fenceUntil = mount.deadline` exactly, and `mount.deadline + 0 <= clock`
\* reduces to `mount.deadline <= clock`) -- every legacy (`Drift = 0`) cfg's semantics are
\* therefore completely unchanged (re-run and re-verified this round regardless).
GcFence ==
    /\ mount # None
    /\ ~mount.fenced
    /\ mount.deadline + Drift <= clock
    /\ mtoken < MaxToken
    /\ mount' = [mount EXCEPT !.fenced = TRUE]
    /\ mtoken' = mtoken + 1
    /\ fencedEpochs' = fencedEpochs \union { << mount.uuid, mount.epoch >> }
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, clock, localEpoch, localLost, crashed, rejected, adoptObs,
                    wedged, wrote, rootEmpty, firstOwner, lostThenWrote, reclaimed,
                    remountedAfterFence, fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* --- The keeper's NON-ATOMIC adopt (MountLeaseKeeper::claim: GET, decide, CAS) --------
\* AdoptRead: observe our own same-epoch slot and remember the token. The FIXED protocol
\* refuses a FENCED body at read (a fence costs an epoch): classify fenced-by-gc ->
\* schedule remount. SabAdoptIgnoresFence models the OLD read that skipped the check.
\* rev.6 round 8: after a reclaim installs the successor body (P1), `mount.epoch #
\* localEpoch[a]` here too, so this guard alone already keeps a superseded `a` from ever
\* observing a token to adopt with -- no `heldToken` needed.
AdoptRead(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ adoptObs[a] = None
    /\ mount # None
    /\ mount.uuid = a
    /\ mount.epoch = localEpoch[a]
    /\ IF mount.fenced /\ ~SabAdoptIgnoresFence
       THEN /\ localLost' = [localLost EXCEPT ![a] = TRUE]   \* refuse: recover via new epoch
            /\ UNCHANGED adoptObs
       ELSE /\ adoptObs' = [adoptObs EXCEPT ![a] = mtoken]
            /\ UNCHANGED localLost
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, clock, localEpoch, crashed, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* AdoptWrite: the CAS against the observed token. The GcFence may land between AdoptRead
\* and AdoptWrite -- THE S13 window.
\*   token unchanged -> adopt succeeds (fresh body, seq/expiry refreshed).
\*   token moved     -> FIXED protocol: re-read and classify by BODY -- our own uuid +
\*                      fenced = fenced-by-gc, recoverable (schedule remount, retry with a
\*                      new epoch). SabAdoptWedgeOnTouch models the OLD behavior: the
\*                      mismatch throws LOGICAL_ERROR out of Store::open -> PERMANENT wedge.
\* rev.6 round 4: the "classify by BODY" this comment has always claimed is now actually
\* implemented -- re-reading `mount` (already directly visible; no extra model step needed)
\* and checking uuid/epoch/fenced BEFORE concluding loss. If the body still shows
\* `mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced`, the token moved for a
\* provably benign, self-caused reason -- no loss, no wedge (matches the product's real
\* `MountLeaseKeeper::claim`, `CasServerRoot.cpp:711-737`, which performs exactly this
\* re-read-and-classify; see the task report's round-4 section for the product-vs-model
\* reachability caveat -- this exact interleaving is NOT reachable in the current product
\* because `state_mutex` serializes `claim()` against `renewOnce()` and the renewal thread
\* does not exist until AFTER `claim()` returns, `CasStore.cpp:444-473` -- this action still
\* encodes the target DESIGN semantics the comment above has always documented).
\* rev.6 round 7 (review M1, stated explicitly): the `selfCaused` branch below is a
\* PRODUCT-UNREACHABLE branch -- the product's actual behavior for this exact body pattern
\* ("same uuid, same epoch, unfenced, yet the token moved") is a HARD, FAIL-CLOSED
\* `LOGICAL_ERROR`, not a benign continue (`CasServerRoot.cpp:729-733`, "touched while
\* adopting our own mount slot -- failing closed"; the analogous `:768-769` explicitly says
\* "same (uuid, epoch) unfenced -- no plausible classification -- falls through to the
\* base's generic throw"). That fail-closed response is INTENDED DESIGN (the product authors
\* already considered this exact case), not a gap Tasks 3-8 should read as license to soften.
\* This branch is strictly MORE PERMISSIVE than the product and is retained only because it
\* is product-unreachable (round 4's `state_mutex` argument) and therefore can only suppress
\* a false-positive model witness, never add a reachable unsafe state.
\* rev.6 round 8: round 7's `heldToken` conjunct on the success branch is DELETED -- with P1
\* installing the successor's body on every reclaim, a reclaim also changes `mount.epoch`,
\* so `AdoptRead`'s OWN guard (`mount.epoch = localEpoch[a]`) already refuses to observe a
\* token in that case, and this action's success branch reverts to the faithful CAS against
\* the OBSERVED token alone (`mtoken = adoptObs[a]`, `CasServerRoot.cpp:711`) -- this also
\* fixes round 7's Finding B: a plain `GcFence` token bump (unrelated to any reclaim) no
\* longer spuriously blocks `sab_fenceresurrect`'s adopt.
AdoptWrite(a) ==
    \* `mount # None` guards the record access below -- `ClearExpiredMount` can clear `mount`
    \* without touching `adoptObs` (a reachable state even pre-round-4), so a re-read CAN
    \* legitimately find nothing there; that is the product's "vanished while adopting"
    \* branch (`CasServerRoot.cpp:735-736`), which also fails closed (not self-caused).
    LET selfCaused == mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ adoptObs[a] # None
    /\ IF mtoken = adoptObs[a]
       THEN /\ mtoken < MaxToken
            /\ mount' = [uuid |-> a, epoch |-> localEpoch[a],
                         deadline |-> clock + TTL, fenced |-> FALSE]
            /\ mtoken' = mtoken + 1
            /\ \E d \in 0..Drift : fenceUntil' = clock + TTL + d  \* rev.6
            /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ UNCHANGED << localLost, wedged >>
       ELSE /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
            /\ IF SabAdoptWedgeOnTouch
               THEN /\ wedged' = [wedged EXCEPT ![a] = TRUE]
                    /\ UNCHANGED localLost
               ELSE IF selfCaused
                    THEN UNCHANGED << localLost, wedged >>  \* rev.6: benign, self-caused -- no loss
                    ELSE /\ localLost' = [localLost EXCEPT ![a] = TRUE]
                         /\ UNCHANGED wedged
            /\ UNCHANGED << mount, mtoken, fenceUntil >>
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, clock, localEpoch, crashed, rejected, fencedEpochs, wrote,
                    rootEmpty, firstOwner, lostThenWrote, reclaimed, remountedAfterFence,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* Abstract time advances (so a deadline can pass -> fence + reclaim reachable).
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A server process crashes: later Ticks pass the deadline without the holder renewing.
\* An in-flight adopt observation dies with the process. rev.6 round 3: this sets
\* `crashed[a]`, a MECHANICAL fact -- NOT `localLost` (a dead process cannot "learn"
\* anything; knowledge and physical death are disjoint concepts kept in disjoint vars).
Die(a) ==
    /\ ~rejected[a]
    /\ mount # None
    /\ mount.uuid = a
    /\ crashed' = [crashed EXCEPT ![a] = TRUE]
    /\ adoptObs' = [adoptObs EXCEPT ![a] = None]
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, clock, localEpoch, localLost, rejected,
                    fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* A mutation under the mount. rev.6 round 8 (P2, consult/review-reconciled): `Write` is now
\* a PURE LOCAL check, matching the product's actual write path exactly --
\* `Store::mayMutate`/`refAppendFenceOk` (`CasStore.cpp:201-226`) reads NO shared state at
\* all: `!mount_fence.lost && bootMsNow() < deadline_boot_ms`, two LOCAL atomics, never a
\* re-read of the mount object. So `mount # None`, `mount.uuid = a`, `epochOK`
\* (`mount.epoch = localEpoch[a]`), and `~mount.fenced` are ALL removed as unfaithful --
\* they modeled a per-write body check the product never performs. `owner = a` is KEPT as
\* the identity anchor (not a body read: `owner` is the sticky, clock-free, constant
\* identity object, a faithful proxy for the writer's own cached (uuid) -- without it a
\* non-owner could write on the shared scalar `fenceUntil`; making `fenceUntil` per-actor
\* instead is a bigger, deliberately out-of-scope refactor). `clock < fenceUntil` remains
\* the SOLE mechanical liveness check -- the drift-aware TRUE local-fence-deadline, never
\* the stamp (`mount.deadline`). `~crashed[a]` remains -- the process is actually alive (a
\* MECHANICAL fact set by `Die`).
\*
\* `~localLost[a]` is DELIBERATELY ABSENT (unchanged since round 3): `localLost` is PURE
\* KNOWLEDGE (what `a` has learned), and appears in NO safety guard anywhere in this model --
\* a dangerous wall-clock-reclaim is precisely a holder that does NOT know it was superseded
\* (drifted clock still says the lease is fine) while GLOBAL truth already moved on; baking
\* politeness into a guard would mask exactly that bug. `lostThenWrote`/
\* `SupersededWriterMakesNoMutation` are KEPT (round 8: still have live readers/checkers --
\* see the task report's kept-vs-removed table) but are no longer a universally-green
\* regression guard: with `~mount.fenced` gone, a `Write` CAN now fire with `localLost[a] =
\* TRUE` if the local fence is still valid at that moment. In every honest cfg this cannot
\* happen (`localLost` is only set on a fenced/different-epoch body or a moved adopt token,
\* all of which coincide with the fence having already expired); it becomes reachable only in
\* `_sab_wallclockreclaim.cfg` (the drift window keeps the fence valid past a reclaim), where
\* `GlobalSupersededWriterMakesNoMutation` fires first at a shallower depth (the direct drift
\* Write, no `Renew` needed) -- so `SupersededWriterMakesNoMutation` is dropped from THAT
\* cfg's invariant list (round 8) rather than left to invite "why is this masked-green"
\* confusion.
\*
\* `trulySuperseded`/`supersededThenWrote` are the GLOBAL-truth witness: `epoch` (the
\* durable counter, a SEPARATE object from `mount`) has been advanced by a completed
\* reclaim (`ObservedReclaim`/`WallClockReclaim`) past what `a` itself last allocated --
\* independent of whether `a` ever learned about it. `Write` is guarded on NO epoch/token/
\* global-truth quantity mirroring this invariant -- honest-path safety is a REACHABILITY
\* argument (the fence provably expired before a reclaim, and every continuation write's
\* OWN body-check independently refuses to re-arm it), never a guard copying the invariant.
Write(a) ==
    LET trulySuperseded == epoch > localEpoch[a]
    IN
    /\ ~rejected[a] /\ ~wedged[a]
    /\ ~crashed[a]
    /\ owner = a
    /\ clock < fenceUntil
    /\ wrote' = wrote \union {<< a, localEpoch[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ supersededThenWrote' = (supersededThenWrote \/ trulySuperseded)
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, firstOwner, reclaimed,
                    remountedAfterFence, fenceUntil, obsToken, obsSince, observedReclaimEver >>

\* SABOTAGE action (only enabled under SabEpochReset): zero the durable epoch
\* counter when the mount is cleared. Breaks epoch monotonicity.
SabResetEpoch ==
    /\ SabEpochReset
    /\ mount = None
    /\ epoch > 0
    /\ epoch' = 0
    /\ UNCHANGED << epochCeiling, epochWiped, owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* Clear an expired UNFENCED mount (lets SabResetEpoch fire; also a benign honest step).
\* A fenced slot is never cleared: in the implementation mount objects persist, and the
\* fenced body is exactly what lets a restart classify "my old incarnation was fenced".
\*
\* rev.6 round 10 (Drift-aware, per decision #2's PRINCIPLE, not a specific named product
\* function): this is an OBSERVER-side action -- some other party (GC, or a fresh incarnation
\* discovering a stale record) declaring the CURRENT mount dead by time, exactly like
\* `GcFence`. Decision #2's principle is not specific to the fence-out flavor: "GC fence-out
\* becomes observation-based" is one instance of the general rule that ANY observer-side
\* death verdict -- fence it, clear it, reclaim it -- must clear the full rate-bound
\* observation threshold (`TTL + Drift`) before concluding the record is truly dead; clearing
\* an expired mount record is GC bookkeeping on a mount already presumed dead, and follows the
\* SAME rule `GcFence` does. Guard changed from `mount.deadline <= clock` to `mount.deadline +
\* Drift <= clock` -- IDENTICAL mutual-exclusion-with-`Write` proof as `GcFence`'s (see that
\* action's comment), byte-identical to the old guard at `Drift = 0`. Still lets
\* `SabResetEpoch` fire once the clear lands (unchanged).
ClearExpiredMount ==
    /\ mount # None
    /\ ~mount.fenced
    /\ mount.deadline + Drift <= clock
    /\ mount' = None
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* --- rev.6: observation-based reclaim vs wall-clock sabotage -------------------------
\* StartObservation: a reclaimer (GC leader, or a same-uuid successor probing an
\* apparently-dead mount) begins watching the write-token. Restarts implicitly: ANY
\* mount write bumps mtoken, so a stale obsToken simply stops matching mtoken and
\* ObservedReclaim below cannot fire until a fresh StartObservation re-arms it.
StartObservation ==
    /\ mount # None
    /\ obsToken = None
    /\ obsToken' = mtoken
    /\ obsSince' = clock
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    observedReclaimEver, supersededThenWrote >>

\* ObservedReclaim: the FIXED protocol -- the SUCCESSOR reclaim (epoch-advancing), NOT the
\* GC fence-out (`GcFence`, non-epoch-advancing, `gc_fenced=TRUE`, uuid/epoch preserved).
\* These are two DISTINCT product mechanisms (spec S:unclean-takeover vs S:gc-fence-out):
\* only the successor form ever advances the durable epoch.
\*
\* Reclaim only once the token has held stable for the FULL rate-bound wait (TTL + Drift)
\* on the reclaimer's OWN clock -- this provably guarantees clock >= fenceUntil (the
\* holder's true fence already expired), because the holder's last renewal/claim producing
\* this token happened at or before obsSince, and that renewal's fenceUntil is bounded by
\* (that renewal's clock) + TTL + Drift <= obsSince + TTL + Drift <= clock.
\*
\* rev.6 round 8 (P1, consult/review-reconciled -- FIXES review C1, replaces round-7's
\* `heldToken` compensating field): INSTALLS THE SUCCESSOR'S BODY, exactly like the real
\* token-guarded `putOverwrite` reclaim (`CasServerRoot.cpp:331-332`: same uuid, the
\* successor's freshly-allocated `writer_epoch`, `gc_fenced=false`, fresh `expires_at_ms`,
\* fresh token). `mount.uuid` stays `owner` (only the sticky owner ever legitimately holds
\* this slot); `mount.epoch` becomes the JUST-bumped `epoch'` (GLOBAL truth, a separate
\* object from `mount` per the model header, but now ALSO reflected in the body, faithfully);
\* `deadline` and `fenced` are fresh/unfenced, matching a genuine new incarnation. Does NOT
\* touch `localLost` -- setting `localLost` here would be modeling "the reclaimer's belief
\* becomes the holder's knowledge" for free, exactly the politeness-implies-safety
\* conflation round 2 removed. Does NOT touch `fenceUntil` -- that is the PREDECESSOR's own
\* local fence, which a reclaimer cannot reach or reset.
\*
\* With the body now faithful, EVERY continuation-arm action's PRE-EXISTING body
\* classification (present since round 0/P3.1, untouched by this round) independently
\* refuses to re-arm the predecessor: `Renew`'s different-epoch branch (`mount.epoch #
\* localEpoch[a]`); `AdoptRead`'s guard (same comparison, so no token is ever observed to
\* adopt with); `ClaimMount`'s `refreshOK` (`sameEpoch` false). No compensating field
\* needed -- this is why `heldToken` (round 7) is deleted in this round.
ObservedReclaim ==
    /\ ~SabWallClockReclaim
    /\ mount # None
    /\ obsToken # None /\ obsToken = mtoken            \* token stable since obsSince
    /\ clock - obsSince >= TTL + Drift                 \* full rate-bound wait on OUR clock
    /\ epoch < MaxEpoch
    /\ (epoch > 0 \/ epochCeiling = 0)   \* see AllocEpoch's identical guard/comment
    /\ mtoken < MaxToken
    /\ epoch' = epoch + 1
    /\ epochCeiling' = IF epoch' > epochCeiling THEN epoch' ELSE epochCeiling
    /\ mtoken' = mtoken + 1
    /\ mount' = [uuid |-> mount.uuid, epoch |-> epoch', deadline |-> clock + TTL, fenced |-> FALSE]
    /\ obsToken' = None
    /\ observedReclaimEver' = TRUE
    /\ UNCHANGED << owner, epochWiped, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner, lostThenWrote,
                    reclaimed, remountedAfterFence, fenceUntil, obsSince, supersededThenWrote >>

\* WallClockReclaim: the SABOTAGE. Trusts the stamp directly (clock > mount.deadline)
\* with NO observation wait at all -- exactly the old cross-node wall-clock comparison
\* the rev.6 design replaces. When Drift > 0 the true holder may still be safely inside
\* its own fenceUntil at this clock value, so this can fire strictly earlier than
\* ObservedReclaim ever could -- and, just like ObservedReclaim (round 8: same body-install,
\* P1), it installs the successor body (epoch-advancing, fresh unfenced deadline) and never
\* touches `localLost` (the old holder's KNOWLEDGE): the whole point of the sabotage is a
\* reclaimer that is wrong to believe the holder is dead, so the old holder correctly does
\* NOT learn anything here.
\*
\* THE LOAD-BEARING ASYMMETRY: installing the successor body does NOT block the sabotage's
\* own violation. `Write` (round 8, P2) no longer reads `mount` AT ALL -- its only
\* mechanical liveness check is `clock < fenceUntil`, the PREDECESSOR's own local fence,
\* which this reclaim cannot touch or reset. The old holder's next `Write` fires on its
\* STILL-VALID existing fence (`clock < fenceUntil` true because `Drift` bought a tick) --
\* completely independent of what body the reclaim just installed. The asymmetry between
\* the honest and sabotage paths therefore lives ENTIRELY in the reclaim's PRECONDITION
\* (full `TTL + Drift` observation wait vs. trusting the stamp), never in what the reclaim
\* writes to the body -- both write the identical kind of body.
WallClockReclaim ==
    /\ SabWallClockReclaim
    /\ mount # None /\ clock > mount.deadline
    /\ epoch < MaxEpoch
    /\ (epoch > 0 \/ epochCeiling = 0)   \* see AllocEpoch's identical guard/comment
    /\ mtoken < MaxToken
    /\ epoch' = epoch + 1
    /\ epochCeiling' = IF epoch' > epochCeiling THEN epoch' ELSE epochCeiling
    /\ mtoken' = mtoken + 1
    /\ mount' = [uuid |-> mount.uuid, epoch |-> epoch', deadline |-> clock + TTL, fenced |-> FALSE]
    /\ UNCHANGED << owner, epochWiped, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner, lostThenWrote,
                    reclaimed, remountedAfterFence, fenceUntil, obsToken, obsSince,
                    observedReclaimEver, supersededThenWrote >>

\* --- 2026-07-24 fence-not-rescue gate (spec rev.4): the epoch-wipe twin + re-mint guard ---
\* WipeEpoch: environmental sabotage-adjacent action (always enabled, subject to the
\* ONE-SHOT-per-behavior `~epochWiped` guard below -- the environment can lose the durable
\* epoch OBJECT at any time, independent of any Sab flag: this models a genuine data-loss
\* event, e.g. the keeper znode/S3 object backing `epoch` vanishing or being recreated empty).
\* The QUESTION this task's guard answers is not "can the epoch object be lost" (yes, always)
\* but "is re-minting OVER that loss, while a mount may still be live, guarded" -- see
\* `RemintEpoch` below.
\*
\* `~epochWiped` (state-space bound, not a safety claim): without this, `WipeEpoch` composes
\* with subsequent re-mints (`AllocEpoch`/`RemintEpoch`/`RemintEpochDecom`) to wipe again,
\* repeatedly, multiplying the explored state space with no new KIND of scenario -- confirmed
\* empirically (`rev6_observe.cfg` reached 179M states generated / 28M distinct with an
\* unbounded, still-growing queue before this guard was added). Both negative-control cfgs
\* (`_sab_epochwipelive`, `_sab_decomblindbypass`) stay reachable with a SINGLE wipe (that is
\* what births the same-pair twin); repeated epoch-object loss within one behavior is out of
\* this gate's modeled scope by deliberate, documented choice (see `epochWiped`'s own comment).
WipeEpoch ==
    /\ ~epochWiped
    /\ epoch > 0
    /\ epoch' = 0
    /\ epochWiped' = TRUE
    /\ UNCHANGED << epochCeiling, owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* RemintEpoch: the code's absent-epoch branch of `allocateWriterEpoch` -- distinct from
\* `AllocEpoch` above, which models the CAS bump over a PRESENT epoch object. Fires only when
\* the durable counter reads 0 (freshly lost, e.g. by `WipeEpoch`). `SabEpochGuardOff = FALSE`
\* (honest): the Phase C guard requires `mount = None` -- an authoritative record that no
\* mount can possibly be live for this server_root, so nothing gets orphaned/duplicated by
\* the re-mint. `SabEpochGuardOff = TRUE` (sabotage) drops that guard: the pre-fix
\* `allocateWriterEpoch` re-mints epoch 1 unconditionally whenever it reads 0, including
\* while `mount # None` still holds a live (unfenced) incarnation.
RemintEpoch(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch = 0
    /\ epochCeiling > 0   \* a genuine WIPE of a previously-allocated epoch, not bare genesis
                          \* (epoch = 0 alone is ambiguous with "never yet allocated" -- the
                          \* pre-existing `ClaimMount(mount=None /\ strictOK)` bootstrap path
                          \* already covers genuine first-ever allocation via `AllocEpoch`)
    /\ (SabEpochGuardOff \/ mount = None)   \* the Phase C guard: authoritative mount absence
    /\ epoch' = 1
    /\ epochCeiling' = IF epoch' > epochCeiling THEN epoch' ELSE epochCeiling
    /\ localEpoch' = [localEpoch EXCEPT ![a] = 1]
    /\ UNCHANGED << owner, epochWiped, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

\* RemintEpochDecom: Phase C's `DecommissionRecovery` branch -- also re-mints over a lost
\* (epoch = 0) durable counter, but ONLY when a mount record still exists (`mount # None`,
\* the decommission-recovery precondition: this actor is reconciling a mount object left
\* behind by a prior incarnation). Honest (`SabDecomBlindBypass = FALSE`): the re-mint is
\* permitted ONLY over a TERMINAL mount (fenced, or wall-clock expired) and mints strictly
\* PAST the terminal mount's own epoch (`mount.epoch + 1`, distinct by construction -- never
\* reuses the terminal incarnation's epoch value). Sabotage
\* (`SabDecomBlindBypass = TRUE`, the round-3 finding-1 bug): mints epoch 1 unconditionally,
\* regardless of whether the mount is still LIVE.
RemintEpochDecom(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch = 0
    /\ epochCeiling > 0   \* a genuine WIPE of a previously-allocated epoch, not bare genesis
                          \* (see RemintEpoch's identical guard for the rationale)
    /\ crashed[a]         \* decommission is ALWAYS performed on behalf of a presumed-dead
                          \* identity (`openForDecommission` derives the victim uuid FROM a
                          \* surviving mount left behind by a DIFFERENT process/run -- never
                          \* the same still-live process examining its own in-flight mount).
                          \* This model has no separate "decommission operator" identity
                          \* distinct from `Actors` (round-3 finding-5's actor-uuid caveat),
                          \* so `crashed[a]` (the model's existing "this process genuinely
                          \* died, a fresh incarnation is now running" marker -- see
                          \* `ClaimMount`'s own comment on `crashed`-reset-on-reclaim) is the
                          \* faithful proxy: without it, TLC finds a spurious self-decommission
                          \* on a's OWN still-in-sync, actively-serviced mount purely because
                          \* the wall clock passed its deadline (a beat-blocked-but-alive
                          \* incarnation, exactly the case `Renew`'s late-renewal already
                          \* handles safely elsewhere) -- not the genuine abandoned-slot
                          \* scenario this action exists to model.
    /\ mount # None
    /\ IF SabDecomBlindBypass
       THEN epoch' = 1                                  \* the round-3 finding-1 bug
       ELSE /\ mount.fenced \/ mount.deadline + Drift <= clock  \* TERMINAL, Drift-aware:
                                                                 \* an OBSERVER-side death
                                                                 \* verdict (see `GcFence`/
                                                                 \* `ClearExpiredMount`'s
                                                                 \* identical convention)
            /\ mount.epoch < MaxEpoch                    \* TLC finiteness (mirrors AllocEpoch)
            /\ epoch' = mount.epoch + 1                 \* distinct by construction
    /\ epochCeiling' = IF epoch' > epochCeiling THEN epoch' ELSE epochCeiling
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch']
    /\ UNCHANGED << owner, epochWiped, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>

Next ==
    \/ Tick
    \/ ClearExpiredMount
    \/ SabResetEpoch
    \/ GcFence
    \/ \E a \in Actors : ClaimOwnerEmpty(a)
    \/ \E a \in Actors : RejectForeignOwner(a)
    \/ \E a \in Actors : AllocEpoch(a)
    \/ \E a \in Actors : ClaimMount(a)
    \/ \E a \in Actors : Renew(a)
    \/ \E a \in Actors : AdoptRead(a)
    \/ \E a \in Actors : AdoptWrite(a)
    \/ \E a \in Actors : Die(a)
    \/ \E a \in Actors : Write(a)
    \/ StartObservation
    \/ ObservedReclaim
    \/ WallClockReclaim
    \/ WipeEpoch
    \/ \E a \in Actors : RemintEpoch(a)
    \/ \E a \in Actors : RemintEpochDecom(a)

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
TypeOK ==
    /\ owner \in Actors \union {None}
    /\ epoch \in 0..MaxEpoch
    /\ epochCeiling \in 0..MaxEpoch
    /\ epochWiped \in BOOLEAN
    /\ mtoken \in 0..MaxToken
    /\ clock \in 0..MaxClock
    /\ \/ mount = None
       \/ /\ mount.uuid \in Actors
          /\ mount.epoch \in 0..MaxEpoch
          /\ mount.deadline \in 0..(MaxClock + TTL)
          /\ mount.fenced \in BOOLEAN
    /\ localEpoch \in [Actors -> 0..MaxEpoch]
    /\ localLost \in [Actors -> BOOLEAN]
    /\ crashed \in [Actors -> BOOLEAN]
    /\ rejected \in [Actors -> BOOLEAN]
    /\ adoptObs \in [Actors -> (0..MaxToken) \union {None}]
    /\ fencedEpochs \subseteq (Actors \X (0..MaxEpoch))
    /\ wedged \in [Actors -> BOOLEAN]
    /\ wrote \subseteq (Actors \X (0..MaxEpoch))
    /\ rootEmpty \in BOOLEAN
    /\ firstOwner \in Actors \union {None}
    /\ lostThenWrote \in BOOLEAN
    /\ reclaimed \in [Actors -> BOOLEAN]
    /\ remountedAfterFence \in [Actors -> BOOLEAN]
    /\ fenceUntil \in 0..(MaxClock + TTL + Drift)
    /\ obsToken \in (0..MaxToken) \union {None}
    /\ obsSince \in 0..MaxClock
    /\ observedReclaimEver \in BOOLEAN
    /\ supersededThenWrote \in BOOLEAN

\* Owner is sticky: once set it never transitions to a different non-None value.
NoTwoServerUuidsOwnSameServerRoot ==
    (owner # None) => (owner = firstOwner)

\* A mount is never held by a non-owner, for ANY clock/deadline.
ForeignUuidNeverAutoTakesOver ==
    (mount # None) => (mount.uuid = owner)

\* Epochs are writer-unique AND monotone-bounded:
\*  (1) no two distinct actors share a written epoch;
\*  (2) the durable `epoch` counter is a monotone ceiling over every written
\*      epoch -- it only ever grows, so a written epoch can never exceed it.
\* 2026-07-24 fence-not-rescue gate: NOTE -- an earlier draft of this task's change read
\* `epochCeiling` (the monotone high-water mark) here instead of the live `epoch`, to tolerate
\* an honest `WipeEpoch` with no consequential re-mint. That draft was WRONG: it also made the
\* PRE-EXISTING `SabResetEpoch` sabotage (`CaCasMountCore_sab_epochreset.cfg`) undetectable by
\* this conjunct, since `SabResetEpoch` zeroes `epoch` the identical way `WipeEpoch` does and
\* `epochCeiling` is deliberately immune to both -- a real regression (old RED silently turned
\* GREEN), caught by re-running the full battery per Step 5. Reverted to the ORIGINAL raw
\* `epoch` reading, unchanged from every pre-existing round. `WipeEpoch`'s honest false-alarm
\* on this exact conjunct is instead accepted and documented at the cfg level (see
\* `CaCasMountCore_stage1.cfg`'s own comment, alongside `FenceCostsEpoch` for the identical
\* underlying reason: an ungated epoch-zeroing action, honest or not, makes this conjunct
\* momentarily unable to distinguish "temporarily inconsistent, self-healing" from "genuinely
\* corrupted" -- exactly the design spec's own "Residual hole, honestly stated").
WriterEpochMonotoneUnique ==
    /\ \A x \in wrote : \A y \in wrote : (x[2] = y[2]) => (x[1] = y[1])
    /\ \A x \in wrote : x[2] <= epoch

\* A writer that has locally LEARNED it was superseded/fenced (KNOWLEDGE, via
\* Renew/AdoptRead/Die noticing a mismatch or a crash) never enters a NEW mutation.
\* See `GlobalSupersededWriterMakesNoMutation` for the rev.6 GLOBAL-truth counterpart.
\*
\* rev.6 round 8 (P2-full, consult/review-reconciled): `SabSupersededWrites` (the sabotage
\* flag this invariant used to be paired against) is RETIRED -- with `epochOK` removed from
\* `Write` by construction (not conditionally), the flag has nothing left to toggle; its bug
\* class ("a superseded holder mutates because the epoch-match check was dropped") models a
\* per-write check the product's write path never performs (`CasStore.cpp:201-205`). This
\* invariant and its `localLost`/`lostThenWrote` machinery are KEPT -- they still have live
\* readers (`Write` still computes `lostThenWrote'`, and this invariant is still checked in
\* most cfgs) -- see the task report's round-8 kept-vs-removed table for the full audit. It
\* is NOT a universally-green regression guard any more, though: `Write` no longer checks
\* `~mount.fenced`, so a `Write` CAN fire with `localLost[a] = TRUE` if the local fence is
\* still valid at that moment. This is unreachable in every honest cfg (localLost only gets
\* set once the fence has already expired) and reachable-but-masked in
\* `_sab_wallclockreclaim.cfg` (dropped from that cfg's invariant list, round 8, to avoid
\* "why is this green" confusion -- `GlobalSupersededWriterMakesNoMutation` fires first
\* there at a shallower depth regardless).
SupersededWriterMakesNoMutation ==
    ~lostThenWrote

\* rev.6: a writer never mutates once GLOBAL truth (the durable `epoch` counter, a
\* completed `ObservedReclaim`/`WallClockReclaim`) has already moved past its own
\* `localEpoch[a]` -- regardless of whether the writer ever LEARNED this (`localLost`
\* is knowledge, not a safety fact; see `Write`'s header comment). `SabWallClockReclaim`
\* must violate this: a wall-clock-trusting reclaim can complete strictly before the
\* true holder's `fenceUntil` expires, so the holder's next mechanically-valid `Write`
\* lands after global truth already superseded it. Round 8: this is now THE regression
\* guard for reclaim-body faithfulness -- `_rev6_observe.cfg`'s GREEN under this invariant
\* (with `ObservedReclaim` enabled) supersedes the retired `_sab_supersededwrites` canary.
GlobalSupersededWriterMakesNoMutation ==
    ~supersededThenWrote

\* P3.1: a fence costs an epoch -- no LIVE (unfenced) mount body ever exists under a
\* (uuid, epoch) that a GcFence stamped. Same-epoch resurrection violates this.
FenceCostsEpoch ==
    (mount # None /\ ~mount.fenced) => (<< mount.uuid, mount.epoch >> \notin fencedEpochs)

\* P3.1: no actor ever reaches the permanent fail-closed wedge state (the pre-fix
\* Store::open abort). Under the FIXED protocol `wedged` is unreachable; the
\* SabAdoptWedgeOnTouch cfg must violate this (reproducing the S13 wedge).
NoPermanentWedge ==
    \A a \in Actors : ~wedged[a]

----------------------------------------------------------------------------
\* LIVENESS WITNESSES (asserted as invariants in dedicated cfgs so TLC reports them
\* VIOLATED when the good state is REACHABLE).

\* Some actor reclaims a mount it had to expiry.
W_SameUuidReclaimsExpired ==
    ~(\E a \in Actors : reclaimed[a])

\* Some actor whose (uuid, epoch) was FENCED later holds a live mount again (with a new
\* epoch, by FenceCostsEpoch) -- the no-permanent-wedge recovery loop actually completes.
W_RemountAfterFence ==
    ~(\E a \in Actors : remountedAfterFence[a])

\* rev.6: the FIXED (observation-based) reclaim rule actually fires at least once --
\* the "wait for full token stability" recovery path is not vacuous.
W_ObservedReclaim ==
    ~observedReclaimEver

\* rev.6 round 7 (consult point 5, anti-wedge/anti-dead-end check): the re-arm block after
\* an `ObservedReclaim` (round 8: the faithful successor-body install + the PRE-EXISTING body
\* classification in `Renew`/`ClaimMount`/`AdoptRead` -- no `heldToken` any more) must NOT
\* wedge the superseded incarnation's uuid forever -- the legitimate recovery loop (`GcFence`
\* on the now-stale, still-unfenced successor body -> `AllocEpoch` fresh epoch, now allowed
\* since `mount.fenced` -> `ClaimMount`'s `fencedReclaim` branch, `strictOK` satisfied since
\* `localEpoch[a]` now equals the fresh `epoch` -> `Write`, not superseded) must still
\* complete for the SAME actor `ObservedReclaim` fired against. Combining the two existing
\* witnesses is exactly this claim: `observedReclaimEver` (the reclaim fired) together with
\* some actor's `remountedAfterFence` (a full fence -> realloc -> reclaim cycle completed)
\* proves honest-path safety is not "safe by permanent dead-end" -- the recovered
\* incarnation writes again.
W_RecoveryAfterObservedReclaim ==
    ~(observedReclaimEver /\ (\E a \in Actors : remountedAfterFence[a]))
=============================================================================

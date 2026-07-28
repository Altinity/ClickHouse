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
(*                                                                         *)
(* 2026-07-28 v9 RECOVERY-GENERATION layer (spec                           *)
(* 2026-07-27-cas-ref-chain-complete-cut-design.md, §3 "Recovery           *)
(* ownership" and §9's r9-5 sabotage; plan task 5).                        *)
(* This module ALREADY owned the mount-fence GENERATION                    *)
(* itself -- the durable writer epoch (`epoch`, its per-actor view         *)
(* `localEpoch`, and `fencedEpochs`). v9 adds the CONSUMER side: an        *)
(* operation captures that generation AT ADMISSION and must re-present it  *)
(* on every later write, with a recheck POST-I/O immediately before every  *)
(* publication. Three hand-offs from plan tasks 1 and 3 land here, each    *)
(* named at the construct that discharges it:                              *)
(*                                                                         *)
(*   - `recGen` -- the generation captured when a RECOVERY is admitted;    *)
(*      `Install` re-presents it under the install lock.                   *)
(*      `SabStaleInstall` drops that recheck, so an old recovery's result  *)
(*      returns after a self-remount and publishes anyway. This is task    *)
(*      3's zombie-install residual (`CaRefCatalogCore_RESULTS.md`         *)
(*      Scoping, task-3 report concern 7): that module treats "the         *)
(*      generation still validates" as an ORACLE (`creatorAlive`) and      *)
(*      hands the generation's own issuance / observation / recheck        *)
(*      lifecycle here.                                                    *)
(*   - `wedgeGen` -- the generation STORED BY A WEDGED LANE (INV-1's       *)
(*      every-attempt rule). Its ONE bounded conditional create            *)
(*      (`WedgeRetryCreate`) is generation-guarded; `SabWedgeRetryOldGen`  *)
(*      drops that guard, so a dead lane injects its bytes into the        *)
(*      successor incarnation's live stream.                               *)
(*   - `slot` -- the frontier key, i.e. this model's `slot-occupy` result  *)
(*      (`None` = `Created`; `seal = FALSE` = `Occupied(bytes)`; `seal =   *)
(*      TRUE` = `Occupied(EpochSeal)`). `WedgeRetryOccupied` resolves      *)
(*      `Occupied` BY BYTES; `SabSlotNoByteCompare` drops the comparison   *)
(*      and acks the lane's own operation while someone ELSE's bytes are   *)
(*      at the key -- the ACKED-THEN-LOST branch                           *)
(*      `CaRefTableSnapshotLogCore` cannot express (it grants the writer   *)
(*      perfect knowledge of `writtenEver`; task-1 report concern 1 hands  *)
(*      it here). Caught by the ONE new invariant this round adds,         *)
(*      `AckedOpsAreDurable`.                                              *)
(*                                                                         *)
(* The seal also discharges task 1's SECOND hand-off. With a single        *)
(* `rPhase` that module has at most one recovery in flight, so INV-2's     *)
(* "`Occupied` with an `EpochSeal` terminates the walk" branch is never    *)
(* exercised there. Here it is: `SealSlot` is planted by the SUCCESSOR     *)
(* incarnation's recovery, and `WedgeRetryOccupied`'s first branch is the  *)
(* OLD generation's lane meeting it -- INV-1's "a successor's `EpochSeal`  *)
(* found at the key is a conclusive rejection". `W_SealRejectedRetry` pins *)
(* that the branch is reachable rather than decorative.                    *)
(*                                                                         *)
(* Two structural decisions, stated because either could have produced a   *)
(* falsely-green gate:                                                     *)
(*                                                                         *)
(*   1. §3 says "self-remount cancels OR WAITS OUT recovery before         *)
(*      rearming". `ClaimMount` deliberately does NOT clear                *)
(*      `recGen`/`wedgeGen`. Modeling the CANCEL arm would make the        *)
(*      recheck unfalsifiable -- the stale result could never return --    *)
(*      and r9-5 explicitly targets the POST-I/O return, an attempt whose  *)
(*      I/O was already outstanding when the remount happened, which no    *)
(*      cancel can recall. So the modeled arm is refuse-on-return          *)
(*      (`RecoveryRefused` / `WedgeAbandonStale`), the load-bearing one.   *)
(*      Same reasoning as `CaDiskLifecycle`'s [M1] step-0 intent-bail: it  *)
(*      stops NEW attempts, not one mid-flight.                            *)
(*   2. The generation guard sits on the MUTATING branches only. Reading   *)
(*      the key is not a mutation, so a stale lane's read happens          *)
(*      regardless -- which is exactly what lets the seal do its own,      *)
(*      generation-INDEPENDENT rejecting. v9 has BOTH defenses, and the    *)
(*      model must not let one hide the other.                             *)
(*                                                                         *)
(* A third decision, forced by measurement rather than by the spec: this   *)
(* layer's variables are ORTHOGONAL to the mount machinery, so they        *)
(* MULTIPLY its state space instead of extending it. At `_stage1`'s bounds *)
(* the honest gate had no verdict at all (46 M distinct at depth 19,       *)
(* queue still growing), and smaller numeric bounds barely helped. Hence   *)
(* `MaxAdmissions` -- a bound on how many operations/recoveries a          *)
(* behavior admits, in the declared spirit of `epochWiped`'s one-shot      *)
(* `WipeEpoch` guard: a state-space bound, not a safety claim. See its own *)
(* comment, and the `_v9_recoverygen.cfg` header for the chosen bounds.    *)
(*                                                                        *)
(* `RecoveryGenOn = FALSE` in every PRE-EXISTING cfg keeps this layer      *)
(* wholly inert: every new variable holds its `Init` value forever, so no  *)
(* legacy cfg's state space changes (verified against a same-machine,      *)
(* same-`-workers` baseline of the committed model -- see                  *)
(* `CaCasMountCore_RESULTS.md`). The layer is exercised only by the new    *)
(* cfgs.                                                                   *)
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
    SabDecomBlindBypass,  \* FALSE = honest (decommission re-mint requires a TERMINAL mount); TRUE =
                          \* the rejected blind bypass -- mint epoch 1 regardless of mount liveness
                          \* (the round-3 finding-1 bug)
    RecoveryGenOn,        \* 2026-07-28 v9: FEATURE GATE (not a sabotage) for the recovery-generation
                          \* layer below. FALSE in every PRE-EXISTING cfg: every new action is
                          \* disabled, every new variable is frozen at its `Init` value, and the
                          \* legacy state spaces are therefore unchanged (the module's biggest cfgs
                          \* already do not complete -- `rev6_observe` -- so making the new layer
                          \* free for them is a hard requirement, not a nicety). TRUE only in the
                          \* four new cfgs.
    SabStaleInstall,      \* 2026-07-28 v9 (§9 r9-5): FALSE = honest -- `Install` rechecks, under the
                          \* install lock, that the generation captured at admission is still the
                          \* current one. TRUE drops the recheck: an old recovery's result returns
                          \* after a self-remount and publishes anyway.
    SabWedgeRetryOldGen,  \* 2026-07-28 v9 (INV-1 + §9 r9-5): FALSE = honest -- a wedged lane's one
                          \* bounded conditional create only fires while its STORED generation is
                          \* still current. TRUE lets the retry fire under the NEW generation
                          \* unchecked, so a dead lane's bytes land in the successor's live stream.
    SabSlotNoByteCompare, \* 2026-07-28 v9 (INV-2 slot-occupy resolution, task-1 hand-off): FALSE =
                          \* honest -- an `Occupied` result is resolved BY COMPARING BYTES, and only
                          \* the lane's OWN bytes let it ack. TRUE skips the comparison and acks
                          \* regardless: acked-then-lost.
    MaxAdmissions         \* 2026-07-28 v9: STATE-SPACE BOUND -- how many operations/recoveries may be
                          \* admitted per behavior in total (`RecoveryStart` + `WedgeAdmit`). NOT a
                          \* safety claim, in the identical spirit to `epochWiped`'s one-shot
                          \* `WipeEpoch` guard (see its own comment): without a bound, `recGen` and
                          \* `wedgeGen` re-admit and resolve without limit at every clock value and
                          \* every mount state, multiplying the pre-existing space by a large
                          \* constant per lane with no new KIND of scenario for these invariants to
                          \* distinguish -- measured, not assumed: at MaxAdmissions unbounded the
                          \* v9 green gate passed 46 M distinct states at depth 19 with a still-
                          \* growing queue and no verdict (numbers in `CaCasMountCore_RESULTS.md`).
                          \* It is also the faithful direction: a wedged lane holds ONE operation and
                          \* a fresh incarnation runs ONE recovery, so a handful of admissions is
                          \* what the product actually produces, not an artificial cut. Every cfg
                          \* records its value and the runner's `ADMISSIONS=<n>` override re-runs the
                          \* whole suite at another value to show the bound is not doing the work.
                          \* 0 in every pre-existing cfg (where `RecoveryGenOn = FALSE` already
                          \* disables both admission actions).

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
    supersededThenWrote, \* rev.6 history: TRUE if Write ever fired while `epoch` (GLOBAL truth,
                        \* the durable counter) had already advanced past the writer's OWN
                        \* `localEpoch[a]` -- i.e. a completed reclaim the writer did not know about
    \* --- 2026-07-28 v9 recovery-generation layer (all frozen when RecoveryGenOn = FALSE) --------
    recGen,       \* [Actors -> (0..MaxEpoch) \cup {None}]; the mount-fence generation captured when
                  \* a RECOVERY was admitted (spec §3 "captured at admission"). None = no recovery
                  \* result outstanding for this actor. This IS the model's whole representation of
                  \* recovery: `RecoveryStart` issues it, `Install` re-presents it under the install
                  \* lock, `RecoveryRefused` drops it when the recheck refuses.
    wedgeGen,     \* [Actors -> (0..MaxEpoch) \cup {None}]; the generation a WEDGED lane stored at
                  \* its own admission (INV-1: "the wedge stores its admission fence generation").
                  \* None = no wedged lane. Kept DISJOINT from `recGen`: they are two independent
                  \* consumers of the same generation, and collapsing them into one variable would
                  \* let one sabotage's counterexample stand in for the other's.
    slot,         \* None, or [by |-> Actor, gen |-> Nat, seal |-> BOOLEAN]; ONE modeled frontier key
                  \* -- the `slot-occupy` primitive's subject. `None` is `Created` (absent, so a
                  \* conditional create lands); `seal = FALSE` is `Occupied(bytes)` where the bytes'
                  \* identity is the operation `<< by, gen >>` that wrote them, BOUND AT ADMISSION
                  \* and never re-tagged; `seal = TRUE` is `Occupied(EpochSeal)` planted by `by`'s
                  \* recovery at generation `gen`. Nothing ever deletes or overwrites the key: a seal
                  \* only ever occupies an ABSENT one (INV-2), which is why `durable` below is
                  \* monotone. ONE key, not a walk -- see the `slot` bound note in _RESULTS.md.
    acked,        \* SUBSET (Actors \X (0..MaxEpoch)); operations whose SUCCESS was reported to their
                  \* caller. Monotone.
    durable,      \* SUBSET (Actors \X (0..MaxEpoch)); operations whose bytes ever became durable at
                  \* the frontier key. Monotone (see `slot`). `acked \subseteq durable` is
                  \* `AckedOpsAreDurable`, the acked-then-lost property.
    staleRefusedEver, \* history: TRUE once a post-I/O generation recheck actually REFUSED a
                      \* returning result (at either site). Non-vacuity for the honest recheck: a
                      \* green gate whose recheck never fired would prove nothing.
    admissions,   \* 0..MaxAdmissions; how many operations/recoveries have been admitted so far.
                  \* Purely a state-space bound (see `MaxAdmissions`): it appears in no safety guard
                  \* and no invariant, only in the two admission actions' enabling conditions.
    sealRejectedEver  \* history: TRUE once a lane's retry met a SUCCESSOR generation's `EpochSeal`
                      \* at the key and was conclusively rejected -- INV-2's walk-terminating
                      \* branch, task 1's second hand-off. Note SUCCESSOR: a seal planted under the
                      \* lane's OWN generation does not set this. See `WedgeRetryOccupied`.

vars == << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
           adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
           lostThenWrote, reclaimed, remountedAfterFence,
           fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote,
           recGen, wedgeGen, slot, acked, durable, admissions, staleRefusedEver,
           sealRejectedEver >>

\* 2026-07-28: the recovery-generation layer's own variables as one tuple, so each of the 19
\* PRE-EXISTING actions declares them unchanged with a SINGLE extra conjunct instead of having its
\* (already long) UNCHANGED tuple retyped. An edit that touches 19 multi-line tuples is exactly
\* where a silent omission lives, and TLC would report the omission as a spurious nondeterminism
\* rather than an error.
rgVars == << recGen, wedgeGen, slot, acked, durable, admissions, staleRefusedEver,
              sealRejectedEver >>

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
    /\ recGen = [a \in Actors |-> None]
    /\ wedgeGen = [a \in Actors |-> None]
    /\ slot = None
    /\ acked = {}
    /\ durable = {}
    /\ admissions = 0
    /\ staleRefusedEver = FALSE
    /\ sealRejectedEver = FALSE

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
\*
\* 2026-07-28 v9: this action IS the self-remount spec §3 names, and it deliberately does NOT
\* clear `recGen[a]`/`wedgeGen[a]` (`UNCHANGED rgVars` below). §3 offers two arms -- "self-remount
\* cancels OR WAITS OUT recovery before rearming" -- and the cancel arm cannot be the safety
\* mechanism: r9-5's scenario is a result whose I/O was ALREADY OUTSTANDING at remount time, which
\* no cancellation can recall. Modeling the cancel here would make the post-I/O generation recheck
\* unfalsifiable (the stale result could never return), so the modeled arm is refuse-on-return --
\* see `Install`/`RecoveryRefused`/`WedgeAbandonStale` and the module header's decision 1.
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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

\* Abstract time advances (so a deadline can pass -> fence + reclaim reachable).
Tick ==
    /\ clock < MaxClock
    /\ clock' = clock + 1
    /\ UNCHANGED << epochCeiling, epochWiped, owner, epoch, mount, mtoken, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

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
    /\ UNCHANGED rgVars

\* --- 2026-07-28 v9 recovery-generation layer (spec §3 "Recovery ownership", §9 r9-5) ---------
\* Every action below is gated on `RecoveryGenOn`, so the whole layer is inert in every
\* pre-existing cfg (see the module header). Read the section top-to-bottom: it is a lifecycle --
\* the generation is ISSUED at admission, OBSERVED across an I/O, and RECHECKED immediately before
\* every publication.

\* `GenerationCurrent(a, g)`: is the generation `a` captured still THE current one, from `a`'s own
\* LOCAL point of view? `localEpoch[a]` is the module's per-actor view of the mount-fence
\* generation and is bumped by exactly the actions that re-arm a mount (`AllocEpoch`,
\* `RemintEpoch`, `RemintEpochDecom`), so `g = localEpoch[a]` is precisely "no self-remount has
\* happened since `g` was captured" -- the product's cached-generation-vs-current-generation
\* comparison under the install lock, a LOCAL check.
\*
\* Deliberately NOT a read of `epoch`, `mount` or `mtoken`: the round-8 P2 discipline (see
\* `Write`'s header) is that a publication-time check reads NO shared state -- `Store::mayMutate`
\* is two local atomics. Reading global truth here would be a guard that copies
\* `GlobalSupersededWriterMakesNoMutation`'s own predicate, which would make that invariant
\* unfalsifiable-by-construction instead of proved. The mechanical liveness half is carried, as
\* everywhere else in this module, by `clock < fenceUntil` at each call site.
GenerationCurrent(a, g) == g = localEpoch[a]

\* RecoveryStart(a): ADMISSION. A recovery is admitted only by a mounted writer holding a LIVE,
\* unfenced mount at its own current generation, and it captures that generation (spec §3, "the
\* mount-fence generation is captured at admission"). One outstanding recovery per actor: a second
\* admission would overwrite the first's captured generation, which is exactly the bookkeeping
\* r9-5 says must never be lost.
RecoveryStart(a) ==
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ recGen[a] = None
    /\ owner = a
    /\ mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced
    /\ admissions < MaxAdmissions        \* state-space bound only -- see `MaxAdmissions`
    /\ admissions' = admissions + 1
    /\ recGen' = [recGen EXCEPT ![a] = localEpoch[a]]
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    wedgeGen, slot, acked, durable, staleRefusedEver, sealRejectedEver >>

\* SealSlot(a): the recovery walk finds the key `Created` (absent) and the seal OCCUPIES it --
\* INV-2's "`Created` -> the seal occupies `(E, T+1)` and the `Late Predecessor PUT` ghost can
\* never land: the store's conditional create is the fence". `slot = None` IS that conditional
\* create. Carries the captured generation per §3's "required on every `slot-occupy`".
\* Once this lands, `StragglerLands` below can never fire again -- that is the structural half of
\* v9's defense, independent of any generation recheck.
SealSlot(a) ==
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ recGen[a] # None
    /\ GenerationCurrent(a, recGen[a])
    /\ owner = a
    /\ clock < fenceUntil
    /\ slot = None
    /\ slot' = [by |-> a, gen |-> recGen[a], seal |-> TRUE]
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    admissions, recGen, wedgeGen, acked, durable, staleRefusedEver, sealRejectedEver >>

\* Install(a): the recovery result RETURNS FROM I/O and is published. §9 r9-5 puts the generation
\* recheck exactly here -- "post-I/O, immediately before every install/unwedge/`_ckpt`
\* publication". `SabStaleInstall` drops it.
\*
\* The publication is a MUTATION, so it is booked like every other mutation in this module: into
\* `wrote`, and into the two superseded-writer witnesses. It is tagged by the CAPTURED generation
\* `recGen[a]`, not by whatever is current -- the recovery result was computed under `recGen[a]`
\* and belongs to that incarnation, which is the whole point.
\*
\* Why the honest guard is load-bearing rather than decorative, and why `clock < fenceUntil` does
\* not already do its work: a bare GC fence-out cannot get here (`GcFence` fires only once
\* `mount.deadline + Drift <= clock`, which implies `clock >= fenceUntil`, so `Install` is
\* mutually exclusive with it by the same construction proof `Write` relies on). What DOES get
\* here is a self-remount: `ClaimMount` installs a FRESH `fenceUntil`, so the mechanical liveness
\* check passes again while `epoch` has meanwhile advanced past `recGen[a]`. That is precisely
\* §3's "self-remount ... before rearming" case, and the recheck is the only thing standing in it.
Install(a) ==
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ recGen[a] # None
    /\ owner = a
    /\ clock < fenceUntil
    /\ (SabStaleInstall \/ GenerationCurrent(a, recGen[a]))
    /\ recGen' = [recGen EXCEPT ![a] = None]
    /\ wrote' = wrote \union {<< a, recGen[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ supersededThenWrote' = (supersededThenWrote \/ (epoch > recGen[a]))
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, firstOwner,
                    reclaimed, remountedAfterFence, fenceUntil, obsToken, obsSince,
                    observedReclaimEver,
                    admissions, wedgeGen, slot, acked, durable, staleRefusedEver, sealRejectedEver >>

\* RecoveryRefused(a): the honest r9-5 outcome -- the post-I/O recheck finds the captured
\* generation stale and the result is DISCARDED (nothing published). Also the layer's anti-dead-end
\* escape: without it a refused recovery would pin `recGen[a]` forever and `a` could never admit
\* another one, turning a correct refusal into a modeling artifact that looks like a wedge.
\* Not gated on `~crashed[a]`: dropping a result is not a mutation, and a dead process's
\* outstanding result is exactly what the successor's install lock refuses.
RecoveryRefused(a) ==
    /\ RecoveryGenOn
    /\ recGen[a] # None
    /\ ~GenerationCurrent(a, recGen[a])
    /\ recGen' = [recGen EXCEPT ![a] = None]
    /\ staleRefusedEver' = TRUE
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    admissions, wedgeGen, slot, acked, durable, sealRejectedEver >>

\* --- the wedged lane (INV-1's every-attempt rule) --------------------------------------------
\* WedgeAdmit(a): an operation is admitted into `a`'s lane and its send comes back AMBIGUOUS, so
\* the lane wedges (INV-1: "an id is freed only when nothing was sent or every sent attempt has
\* its own conclusive rejection; a definite rejection AFTER an ambiguous attempt keeps the lane
\* wedged"). Admission captures the generation, same rule and same preconditions as
\* `RecoveryStart`. Nothing is acked: the caller got no answer, which is why the lane is wedged
\* and why dropping the operation later is safe.
WedgeAdmit(a) ==
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ wedgeGen[a] = None
    /\ owner = a
    /\ mount # None /\ mount.uuid = a /\ mount.epoch = localEpoch[a] /\ ~mount.fenced
    /\ admissions < MaxAdmissions        \* state-space bound only -- see `MaxAdmissions`
    /\ admissions' = admissions + 1
    /\ wedgeGen' = [wedgeGen EXCEPT ![a] = localEpoch[a]]
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    recGen, slot, acked, durable, staleRefusedEver, sealRejectedEver >>

\* StragglerLands(a): the ambiguous attempt's IN-FLIGHT PUT actually lands. This is the model's
\* `Late Predecessor PUT` (`CaRefTableSnapshotLogCore`'s `LatePredecessorPut`, flipped in v9 from
\* counterexample to ordinary adoption path) and the reason INV-2's `Occupied` branch exists at
\* all. Three properties are deliberate:
\*   - `slot = None`: the store's conditional create IS the fence. After a `SealSlot` this action
\*     is dead -- the ghost "can never land", INV-2's exact claim.
\*   - no `clock < fenceUntil`, no `~crashed[a]`: this is the STORE's outstanding I/O, not a
\*     decision by a live process. A ghost that only lands while its writer is healthy would be no
\*     ghost at all.
\*   - bytes are tagged `wedgeGen[a]` -- bound at ADMISSION, never re-tagged -- and nothing is
\*     acked, and neither superseded-writer witness is touched. The protocol's answer to a
\*     straggler is ADOPTION (INV-2), not exclusion; `supersededThenWrote` tracks a writer
\*     DECIDING to mutate, which is what `Install`/`WedgeRetryCreate` model. Booking the straggler
\*     there would turn v9's honest adoption path into a false alarm.
StragglerLands(a) ==
    /\ RecoveryGenOn
    /\ wedgeGen[a] # None
    /\ slot = None
    /\ slot' = [by |-> a, gen |-> wedgeGen[a], seal |-> FALSE]
    /\ durable' = durable \union {<< a, wedgeGen[a] >>}
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    admissions, recGen, wedgeGen, acked, staleRefusedEver, sealRejectedEver >>

\* WedgeRetryCreate(a): the lane's ONE bounded same-(key, bytes) conditional create (INV-1: "each
\* later caller's flush performs at most one bounded same-(key, bytes) conditional create under
\* that generation (no background deadline-resetting loop)"). `slot = None` is the create
\* succeeding; it lands the lane's bytes and RESOLVES the lane, so the caller is acked.
\*
\* This is a MUTATION, so §3's "required on every `slot-occupy`" applies and the retry must present
\* a generation that is still current. `SabWedgeRetryOldGen` drops that: a lane belonging to a dead
\* incarnation then creates in the SUCCESSOR's live id space, which is the injection r9-5 names.
\* Note what the sabotage does NOT get: once the successor has SEALED the key, `slot = None` is
\* false and the create is refused by the store no matter what generation the retry presents. The
\* sabotage's whole reachable damage is therefore the pre-seal window -- and that is exactly why
\* the recheck has to come FIRST rather than lean on the seal.
\*
\* Bytes stay tagged `wedgeGen[a]` in both cases: the operation belongs to the incarnation that
\* admitted it, and re-tagging it would hide the violation inside the encoding.
WedgeRetryCreate(a) ==
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ wedgeGen[a] # None
    /\ owner = a
    /\ clock < fenceUntil
    /\ (SabWedgeRetryOldGen \/ GenerationCurrent(a, wedgeGen[a]))
    /\ slot = None
    /\ slot' = [by |-> a, gen |-> wedgeGen[a], seal |-> FALSE]
    /\ durable' = durable \union {<< a, wedgeGen[a] >>}
    /\ acked' = acked \union {<< a, wedgeGen[a] >>}
    /\ wedgeGen' = [wedgeGen EXCEPT ![a] = None]
    /\ wrote' = wrote \union {<< a, wedgeGen[a] >>}
    /\ rootEmpty' = FALSE
    /\ lostThenWrote' = (lostThenWrote \/ localLost[a])
    /\ supersededThenWrote' = (supersededThenWrote \/ (epoch > wedgeGen[a]))
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, firstOwner,
                    reclaimed, remountedAfterFence, fenceUntil, obsToken, obsSince,
                    observedReclaimEver,
                    admissions, recGen, staleRefusedEver, sealRejectedEver >>

\* WedgeRetryOccupied(a): the retry's `slot-occupy` returns `Occupied`. NO generation guard on this
\* action: reading the key is not a mutation, and r9-5 puts the recheck POST-I/O -- a stale lane
\* does perform its read, and what it finds is what resolves it. Three branches, in the order the
\* resolution must take them:
\*
\*   1. `Occupied(EpochSeal)` -- a SUCCESSOR closed this frontier. INV-1: "a successor's
\*      `EpochSeal` found at the key is a conclusive rejection"; INV-2: the seal "terminates the
\*      walk". The lane resolves DEFINITIVELY FAILED with no ack, and this is
\*      GENERATION-INDEPENDENT -- the structural half of the defense, which no sabotage in this
\*      round touches, so the two halves cannot mask each other. This is task 1's second hand-off:
\*      that module's single `rPhase` admits at most one recovery, so it can never reach a seal
\*      planted by a DIFFERENT recoverer; here the successor incarnation's `SealSlot` plants it and
\*      the old generation's lane meets it (`W_SealRejectedRetry`).
\*   2. `Occupied(bytes)` and THE BYTES ARE OURS -- our own ambiguous attempt did land
\*      (`StragglerLands`). Adopt and replay it, and only now ack the caller: the operation IS
\*      durable, so the ack is honest.
\*   3. `Occupied(bytes)` and the bytes are SOMEONE ELSE'S -- our operation is NOT at this key.
\*      No ack. Whatever else happens, the caller must not be told an operation succeeded that
\*      nothing ever wrote.
\*
\* `SabSlotNoByteCompare` merges branch 3 into branch 2: it acks without comparing. That is the
\* acked-then-lost branch task 1 could not express (its writer has perfect knowledge of
\* `writtenEver`), and `AckedOpsAreDurable` is what catches it. "Someone else" here is a DIFFERENT
\* INCARNATION of the same uuid -- this module's established idiom for a second writer (see
\* `CaCasMountCore_sab_epochwipelive.cfg`'s note on the actor-equals-uuid structure); the
\* operation identity is `<< actor, generation >>`, so a different generation IS different bytes.
WedgeRetryOccupied(a) ==
    \* `slot # None` is repeated inside the LET (not merely conjoined below) for the same reason
    \* `AdoptWrite`'s `selfCaused` does it: a record access guarded only by evaluation order is a
    \* trap for the next editor who reorders the conjuncts.
    LET mine == slot # None /\ slot.by = a /\ slot.gen = wedgeGen[a]
    IN
    /\ RecoveryGenOn
    /\ ~rejected[a] /\ ~wedged[a] /\ ~crashed[a]
    /\ wedgeGen[a] # None
    /\ owner = a
    /\ slot # None
    /\ wedgeGen' = [wedgeGen EXCEPT ![a] = None]
    /\ IF slot.seal
       THEN \* The REJECTION is generation-independent (any seal at the key is conclusive). The
            \* WITNESS flag is not: it counts only a seal planted under a STRICTLY LATER
            \* generation, i.e. r9-5's "returning after the SUCCESSOR sealed the slot" and task 1's
            \* concurrent-recoverer hand-off. Without the `>` the first draft of
            \* `W_SealRejectedRetry` was satisfied by a degenerate route BFS finds sooner -- one
            \* generation sealing the key and its OWN lane then meeting it (depth 8) -- which is the
            \* ordinary self-walk, not the cross-generation race that module could not express.
            /\ sealRejectedEver' = (sealRejectedEver \/ slot.gen > wedgeGen[a])
            /\ UNCHANGED acked
       ELSE /\ UNCHANGED sealRejectedEver
            /\ IF mine \/ SabSlotNoByteCompare
               THEN acked' = acked \union {<< a, wedgeGen[a] >>}
               ELSE UNCHANGED acked
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    admissions, recGen, slot, durable, staleRefusedEver >>

\* WedgeAbandonStale(a): the post-I/O recheck refuses the lane's publication -- its captured
\* generation is no longer current, so it can neither create nor unwedge, and the successor
\* incarnation owns the key from here. INV-1's own disposition: "A permanently quiet wedged
\* namespace retries on its next caller or an independently occurring remount -- acceptable: the
\* operation was never acknowledged." Dropped WITHOUT an ack, for exactly that reason. The
\* `RecoveryRefused` twin, and the same anti-dead-end role.
WedgeAbandonStale(a) ==
    /\ RecoveryGenOn
    /\ wedgeGen[a] # None
    /\ ~GenerationCurrent(a, wedgeGen[a])
    /\ wedgeGen' = [wedgeGen EXCEPT ![a] = None]
    /\ staleRefusedEver' = TRUE
    /\ UNCHANGED << owner, epoch, epochCeiling, epochWiped, mount, mtoken, clock, localEpoch,
                    localLost, crashed, rejected, adoptObs, fencedEpochs, wedged, wrote, rootEmpty,
                    firstOwner, lostThenWrote, reclaimed, remountedAfterFence, fenceUntil,
                    obsToken, obsSince, observedReclaimEver, supersededThenWrote,
                    admissions, recGen, slot, acked, durable, sealRejectedEver >>

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
    \* 2026-07-28 v9 recovery-generation layer (all inert when RecoveryGenOn = FALSE)
    \/ \E a \in Actors : RecoveryStart(a)
    \/ \E a \in Actors : SealSlot(a)
    \/ \E a \in Actors : Install(a)
    \/ \E a \in Actors : RecoveryRefused(a)
    \/ \E a \in Actors : WedgeAdmit(a)
    \/ \E a \in Actors : StragglerLands(a)
    \/ \E a \in Actors : WedgeRetryCreate(a)
    \/ \E a \in Actors : WedgeRetryOccupied(a)
    \/ \E a \in Actors : WedgeAbandonStale(a)

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
    /\ recGen \in [Actors -> (0..MaxEpoch) \union {None}]
    /\ wedgeGen \in [Actors -> (0..MaxEpoch) \union {None}]
    /\ \/ slot = None
       \/ /\ slot.by \in Actors
          /\ slot.gen \in 0..MaxEpoch
          /\ slot.seal \in BOOLEAN
    /\ acked \subseteq (Actors \X (0..MaxEpoch))
    /\ durable \subseteq (Actors \X (0..MaxEpoch))
    /\ admissions \in 0..MaxAdmissions
    /\ staleRefusedEver \in BOOLEAN
    /\ sealRejectedEver \in BOOLEAN

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

\* 2026-07-28 v9, the ONE invariant this round adds (spec §2 INV-1's every-attempt rule + INV-2's
\* `slot-occupy` resolution): every operation whose SUCCESS was reported to its caller is durably
\* present at the key it claimed. `acked` and `durable` are both monotone, so this is a genuine
\* safety property over the whole behavior, not a snapshot coincidence -- and it is a STRUCTURAL
\* inclusion, not a ghost flag: the counterexample is a real ack of an operation nothing ever
\* wrote.
\*
\* Why this invariant lives HERE and not in `CaRefTableSnapshotLogCore`: that module (plan task 1,
\* see its `_RESULTS.md` Scoping and the task-1 report's concern 1) gives the writer perfect
\* knowledge of `writtenEver`, so its `INV_NO_PHANTOM` can only express the OPPOSITE damage -- a
\* write the caller was told had failed becoming durable. The dangerous direction, a writer that
\* sees `Occupied` at its key, SKIPS the byte comparison and acks its own operation while someone
\* else's bytes are there, needs a WRITER-LOCAL view of the frontier; that is what the
\* `wedgeGen` + `slot` pair provides. `SabSlotNoByteCompare` must violate this, and the honest
\* configs must not.
AckedOpsAreDurable ==
    acked \subseteq durable

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

\* 2026-07-28 v9: the honest post-I/O generation recheck actually REFUSES a returning result at
\* least once. Without this, `_v9_recoverygen`'s green would be consistent with a recheck that
\* never had anything to reject -- the recheck would be green by unreachability, which is the
\* failure mode the house rule about unfalsifiable invariants exists to prevent. The refusal is
\* reachable at either site (`RecoveryRefused` or `WedgeAbandonStale`); COVERAGE=1 in the runner
\* reports the per-action counts that show BOTH fire.
W_GenerationRefused ==
    ~staleRefusedEver

\* 2026-07-28 v9: a lane's retry actually MEETS A SUCCESSOR GENERATION'S `EpochSeal` at the key and
\* is conclusively rejected by it -- INV-1's "a successor's `EpochSeal` found at the key is a
\* conclusive rejection", INV-2's walk-terminating branch. This is plan task 1's second hand-off
\* made reachable: `CaRefTableSnapshotLogCore` has a single `rPhase`, so a seal planted by a
\* DIFFERENT recoverer than the one reading it is unrepresentable there
\* (`CaRefTableSnapshotLogCore_RESULTS.md` Scoping, "Concurrent recoverers"). Here the successor
\* incarnation's `SealSlot` plants it and the OLD generation's wedged lane meets it.
\*
\* The word SUCCESSOR is load-bearing and is enforced in `WedgeRetryOccupied` (`slot.gen >
\* wedgeGen[a]`), not merely intended: the first draft of this witness counted ANY seal, and BFS
\* satisfied it at depth 8 with one generation sealing the key and its OWN lane then meeting it --
\* the ordinary self-walk, which the sibling module already covers and which proves nothing about
\* two recoverers racing. Tightened, and the trace is now the cross-generation one.
W_SealRejectedRetry ==
    ~sealRejectedEver
=============================================================================

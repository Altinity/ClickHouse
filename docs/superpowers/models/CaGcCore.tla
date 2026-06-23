-------------------------------- MODULE CaGcCore --------------------------------
\* =====================================================================================
\* HISTORICAL / SUPERSEDED. Models the abandoned EBR / epoch / generation GC design
\* (2026-06-07 EBR-era spec). The CURRENT model is CaIncarnationCore.tla (incarnation-token
\* core). Kept only as the record of EBR-era checking. The "STABILIZED CORE" wording below
\* refers to that superseded design. See CaIncarnationCore_README.md / README.md.
\* =====================================================================================
(***************************************************************************)
(* TLA+ specification of the STABILIZED CORE of the content-addressed (CA) *)
(* MergeTree garbage collector (the "Keeper-only" Epoch-Based Reclamation  *)
(* profile, hardened with the corrected hinges from the three adversarial  *)
(* reviews).                                                               *)
(*                                                                         *)
(* Source design:                                                         *)
(*   docs/superpowers/reports/2026-06-07-ca-gc-ebr-design-plan.md          *)
(*   docs/superpowers/reports/2026-06-07-ca-gc-keeper-only-profile.md      *)
(* Reviews whose races we target:                                         *)
(*   ...-simplified-correctness-review.md  (V1/V3/V4)                      *)
(*   ...-ebr-design-plan-review.md         (V-EBR-1/2/3)                   *)
(*   ...-keeper-only-review.md             (V-K1..K21)                     *)
(*                                                                         *)
(* WHAT IS MODELLED (the safety-critical mechanics):                      *)
(*  - Durable store S3, strongly consistent. ONE content hash H (fixed),  *)
(*    so an object key (H,e) collapses to its epoch e. Each epoch's object *)
(*    is in a state Absent | Present | Condemned | Deleted.                *)
(*  - epoch_current: monotone, lives in S3.                               *)
(*  - refs: set of epochs named by a live ref (the commit point).         *)
(*  - log: per-epoch + pin with a DURABILITY flag (flush-+-then-advance).  *)
(*    The closed-epoch fold reads only DURABLE + in CLOSED epochs.         *)
(*  - Keeper: leader fence (monotone); per-writer O_W + session state.     *)
(*  - The session-expiry-vs-client-awareness gap: a writer may be          *)
(*    ServerExpired (O_W dropped from the live set) while still believing  *)
(*    it is alive until it observes Disconnected. Writers AND leader       *)
(*    fail-stop on Disconnected (NOT on confirmed Expired).                *)
(*                                                                         *)
(* THIS IS BOUNDED MODEL CHECKING (finite epochs/writers), NOT A PROOF.    *)
(***************************************************************************)
EXTENDS Integers, FiniteSets, TLC

CONSTANTS
    Writers,        \* set of writer ids, e.g. {w1, w2}
    Leaders,        \* set of leader ids, e.g. {L1, L2} (two so split-brain is genuinely modelled)
    MaxEpoch,       \* epochs range 0..MaxEpoch (bounds the state space)
    Retention,      \* reconcile retention backstop: never reclaim e > epoch_current - Retention
    EnableExpiry,   \* TRUE -> allow independent server-side session expiry (the gap)
    EnableSplit,    \* TRUE -> allow a second leader to steal the fence (split-brain)
    EnableWipe      \* TRUE -> allow total Keeper wipe + recovery

\* Object lifecycle states for blobs/<H>/<e>.
\* Absent: never created. Present: bytes exist. Condemned: tombstone present (bytes still
\* readable). Deleted: bytes physically reclaimed (terminal).
Absent    == "Absent"
Present   == "Present"
Condemned == "Condemned"
Deleted   == "Deleted"

\* Writer session (Keeper server-side truth) and client-side belief.
SessAlive       == "Alive"          \* Keeper session live (writers/<W> present, O_W counted)
SessExpired     == "ServerExpired"  \* server expired session; writers/<W> gone; O_W NOT counted
ConnConnected   == "Connected"
ConnDisconnected== "Disconnected"

Epochs == 0..MaxEpoch

VARIABLES
    epoch_current,  \* S3: monotone current epoch
    objState,       \* S3: [Epochs -> {Absent,Present,Condemned,Deleted}]
    condemnEpoch,   \* S3: for a condemned/deleted object, the epoch it was condemned in (its e_a == the key e here)
    refs,           \* S3: subset of Epochs currently named by a live ref
    pin,            \* S3: [Writers -> SUBSET Epochs] durable + that writer W holds (per-writer, so a drop nets only that writer's +)
    epochClosed,    \* S3: subset of Epochs that the leader has CLOSED (fenced close barrier)
    \* --- writers (Keeper-side O_W + session + client belief + local in-progress decision) ---
    O_W,            \* [Writers -> Epochs] observed epoch (published in Keeper writers/<W>)
    sess,           \* [Writers -> {Alive, ServerExpired}] Keeper server-side session truth
    conn,           \* [Writers -> {Connected, Disconnected}] client-side belief
    decided,        \* [Writers -> Epochs \cup {-1}] the epoch this writer has decided to reuse but not yet committed (-1 = none)
    plusIssued,     \* [Writers -> SUBSET Epochs] + this writer ISSUED but maybe not yet durable
    \* --- leader / fence (Keeper). TWO leader identities so split-brain is genuinely modelled. ---
    fence,          \* current highest fence token granted by Keeper (monotone)
    heldFence,      \* [Leaders -> Nat] the fence each leader believes it holds (0 = not a leader)
    leaderConn,     \* [Leaders -> {Connected, Disconnected}] each leader's client-side belief
    safe_epoch,     \* [Leaders -> ...] each leader's last-computed safe_epoch
    \* --- bookkeeping ---
    pc,             \* per-writer program counter for the multi-step commit
    everDeleted     \* HISTORY: set of epochs that were EVER Deleted (for the ABA invariant)

vars == << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
           O_W, sess, conn, decided, plusIssued,
           fence, heldFence, leaderConn, safe_epoch, pc, everDeleted >>

\* Writer commit phases (program counter).
PCidle    == "idle"      \* not committing
PCdecided == "decided"   \* chose epoch to (re)use, may have issued +
PCpinned  == "pinned"    \* +(e) is DURABLE in S3
PCcommit  == "committed"  \* ref published (terminal for this commit attempt)

----------------------------------------------------------------------------
(* Helpers *)

\* A writer is "live-leased" from the leader's POV iff its Keeper session is Alive.
\* (A ServerExpired writer's writers/<W> ephemeral is gone, so it is NOT counted.)
LiveWriters == { w \in Writers : sess[w] = SessAlive }

\* The set of epochs that currently have at least one DURABLE + (across all writers' pins).
\* (Each + records (H,e); the fold sums by key. A per-writer drop nets only that writer's +.)
PinnedDurable == UNION { pin[w] : w \in Writers }

\* safe_epoch as the design computes it: min O_W over live-leased writers; epoch_current if none.
ComputeSafe ==
    IF LiveWriters = {} THEN epoch_current
    ELSE CHOOSE m \in { O_W[w] : w \in LiveWriters } :
            \A w \in LiveWriters : O_W[w] >= m

\* The closed-epoch fold's view of "is epoch e referenced/pinned?" — ONLY durable + in CLOSED
\* epochs count, plus a live ref. (This is the load-bearing closed-epoch S3 barrier.)
\* For our single hash H, "the object e is referenced" iff a ref names e, OR a durable + in a
\* closed epoch pins e. (A + in an OPEN epoch is, by construction, NOT folded yet.)
FoldUnreferenced(e) ==
    /\ e \notin refs
    /\ ~(e \in PinnedDurable /\ e \in epochClosed)

\* Whether leader l may currently act: it must hold the CURRENT (highest) fence AND be Connected
\* (fail-close on Disconnected, and a stale-fence leader is blocked = fencing). This single guard
\* encodes both the K5 split-brain fail-close and the Kleppmann fence re-check before each mutation.
LeaderActive(l) == heldFence[l] = fence /\ heldFence[l] > 0 /\ leaderConn[l] = ConnConnected

----------------------------------------------------------------------------
(* Init *)

Init ==
    /\ epoch_current = 0
    /\ objState = [e \in Epochs |-> Absent]
    /\ condemnEpoch = [e \in Epochs |-> -1]
    /\ refs = {}
    /\ pin = [w \in Writers |-> {}]
    /\ epochClosed = {}
    /\ O_W = [w \in Writers |-> 0]
    /\ sess = [w \in Writers |-> SessAlive]
    /\ conn = [w \in Writers |-> ConnConnected]
    /\ decided = [w \in Writers |-> -1]
    /\ plusIssued = [w \in Writers |-> {}]
    /\ fence = 1
    \* Exactly one leader starts holding the current fence (1); the others hold 0 (not a leader).
    /\ heldFence = [l \in Leaders |-> IF l = CHOOSE x \in Leaders : TRUE THEN 1 ELSE 0]
    /\ leaderConn = [l \in Leaders |-> ConnConnected]
    /\ safe_epoch = [l \in Leaders |-> 0]
    /\ pc = [w \in Writers |-> PCidle]
    /\ everDeleted = {}

----------------------------------------------------------------------------
(* WRITER ACTIONS *)
(* THE CORRECTED HINGE (keeper-only review must-fix #2): a writer may perform a       *)
(* CONSEQUENTIAL op (decide-reuse, advance O_W, pin +, publish ref) only while it is  *)
(* both Connected AND its Keeper session is still server-side Alive. The latter is     *)
(* NOT something the client reads directly; it is the GUARANTEE provided by the writer *)
(* self-fencing on a LOCAL-ELAPSED-TIME deadline strictly inside T_session, so it has  *)
(* gone read-only BEFORE the server could expire its session. Modelling it as          *)
(* sess[w]=SessAlive is the faithful encoding of "self-fence inside T_session": it     *)
(* forbids exactly the [t_expire, t_aware] gap where a Connected-believing but          *)
(* server-expired writer would otherwise commit into a quiescent window (K1/K2).        *)
(* (Plain "fail-stop on Disconnected" — conn alone — is INSUFFICIENT; TLC proved that.) *)
WriterMayAct(w) == conn[w] = ConnConnected /\ sess[w] = SessAlive

CanWrite(w) == WriterMayAct(w) /\ pc[w] = PCidle

\* W obtains H by CREATING a fresh object at epoch_current (new content / first create).
\* Models step 1b. The object becomes Present.
WriterCreate(w) ==
    /\ CanWrite(w)
    /\ LET e == epoch_current IN
       /\ objState[e] = Absent
       /\ objState' = [objState EXCEPT ![e] = Present]
       /\ decided' = [decided EXCEPT ![w] = e]
       /\ pc' = [pc EXCEPT ![w] = PCdecided]
    /\ UNCHANGED << epoch_current, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W dedup-reuses an existing NON-condemned object e (any Present epoch). Models step 1c-none.
\* The reuse decision (W.head) must be under a fresh non-lagged O_W; we require e <= O_W[w]
\* (the writer observed the epoch) OR e = epoch_current.
WriterReuse(w) ==
    /\ CanWrite(w)
    /\ \E e \in Epochs :
        /\ objState[e] = Present
        \* The reuse decision must be under a fresh, non-lagged O_W: a writer may only reuse an
        \* epoch e it currently observes (e >= O_W[w]). Reusing an epoch BELOW O_W is the
        \* unprotected case the reviews kill (its lease no longer pins safe_epoch <= e). To keep
        \* the dependency protected, the writer's lease must cover e, so we also require it to
        \* (logically) hold O_W <= e while the + is not durable -- enforced by WriterAdvanceOW.
        /\ e >= O_W[w]
        /\ decided' = [decided EXCEPT ![w] = e]
        /\ pc' = [pc EXCEPT ![w] = PCdecided]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W finds its target condemned and RESURRECTS to epoch_current (a fresh key). ABA-safe:
\* the new generation is a different epoch from the condemned one.
WriterResurrect(w) ==
    /\ CanWrite(w)
    /\ \E e \in Epochs :
        /\ objState[e] = Condemned
        /\ LET ne == epoch_current IN
           /\ objState[ne] \in {Absent, Present}
           /\ objState' = [objState EXCEPT ![ne] = Present]
           /\ decided' = [decided EXCEPT ![w] = ne]
           /\ pc' = [pc EXCEPT ![w] = PCdecided]
    /\ UNCHANGED << epoch_current, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W issues a + for its decided epoch. May NOT be durable yet (in-flight). This is the early-+.
WriterIssuePlus(w) ==
    /\ WriterMayAct(w)
    /\ pc[w] = PCdecided
    /\ decided[w] # -1
    /\ plusIssued' = [plusIssued EXCEPT ![w] = @ \cup {decided[w]}]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, fence, heldFence, leaderConn, safe_epoch, pc >>

\* W's issued + becomes DURABLE in S3, into log/<O_W[w]>. IMPORTANT (closed-epoch barrier):
\* if the writer's observed epoch O_W[w] has already been CLOSED by the leader, the writer must
\* REAPPEND into the open epoch epoch_current (reappend-to-open-epoch). We model the + landing
\* in the writer's CURRENT open epoch.
WriterPlusDurable(w) ==
    /\ pc[w] = PCdecided
    /\ decided[w] # -1
    /\ decided[w] \in plusIssued[w]
    /\ pin' = [pin EXCEPT ![w] = @ \cup {decided[w]}]
    /\ pc' = [pc EXCEPT ![w] = PCpinned]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* flush-+-then-advance: W advances O_W ONLY after its + for the decided epoch is durable
\* AND that epoch's + is folded-eligible (durable). We require the decided + durable.
\* O_W may advance up to epoch_current; the <=1-lag rule: do not advance past epoch_current.
WriterAdvanceOW(w) ==
    /\ conn[w] = ConnConnected
    /\ sess[w] = SessAlive
    \* flush-+-then-advance (the V-EBR-1/V-EBR-2 fix): a writer may not advance O_W past e while
    \* it holds any decided-but-not-yet-durably-pinned dependency at <= e. Equivalently: the
    \* writer must have NO in-progress commit whose + is not yet durable, AND every issued + is
    \* durable. This closes the decide->+-durable window the reviews target.
    /\ \A e \in plusIssued[w] : e \in pin[w]    \* every issued + is durable
    /\ ( pc[w] = PCidle \/ pc[w] = PCpinned \/ pc[w] = PCcommit )  \* not mid-decide before its + is durable
    /\ ( decided[w] = -1 \/ decided[w] \in pin[w] )         \* the decided dep is durably pinned
    /\ O_W[w] < epoch_current
    /\ O_W' = [O_W EXCEPT ![w] = O_W[w] + 1]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

\* W re-checks the tombstone before committing; if its decided epoch got condemned, it must
\* route away (resurrect). This models the W.recheck/route-to-g+1 step. If condemned, abort
\* this commit (go back to idle, drop decided).
WriterRecheckRoute(w) ==
    /\ WriterMayAct(w)
    /\ pc[w] \in {PCdecided, PCpinned}
    /\ decided[w] # -1
    /\ objState[decided[w]] \in {Condemned, Deleted}
    /\ decided' = [decided EXCEPT ![w] = -1]
    /\ pc' = [pc EXCEPT ![w] = PCidle]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W PUBLISHES the ref (commit point, LAST). Self-fence (the corrected hinge): the writer commits
\* only while WriterMayAct -- Connected AND session still server-side Alive (guaranteed by the
\* local-deadline self-fence inside T_session). Its + must already be durable (PCpinned). The
\* decided epoch must still be Present; if it was condemned/deleted the writer routed away
\* (WriterRecheckRoute) before reaching here.
WriterPublishRef(w) ==
    /\ WriterMayAct(w)
    /\ pc[w] = PCpinned
    /\ decided[w] # -1
    /\ refs' = refs \cup {decided[w]}
    /\ pc' = [pc EXCEPT ![w] = PCcommit]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W finishes a commit: clears decided, returns to idle so it can commit again.
WriterFinish(w) ==
    /\ pc[w] = PCcommit
    /\ decided' = [decided EXCEPT ![w] = -1]
    /\ pc' = [pc EXCEPT ![w] = PCidle]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* W DROPS a ref it published: remove ref FIRST, then the - is implicit (we just remove from refs
\* and from pin to model the - netting the + to 0). Only an epoch this writer pinned.
\* A drop removes the ref FIRST, then logs a - that nets THIS writer's + for e to zero. We only
\* drop an epoch this writer both pinned AND that is in refs, and that the writer is not currently
\* re-using (decided[w] # e), so a drop never removes the pin an in-flight reuse depends on.
\* If after removing this writer's pin no other writer pins e, e leaves PinnedDurable; if it is
\* also unreferenced, it becomes a GC candidate. refs is shared (the ref is a single S3 object),
\* so removing e from refs is sound only if e is not named by another live commit -- with one
\* hash and per-writer pins we model refs as the set of epochs with >=1 live ref; a writer drops
\* its own contribution, which we approximate by requiring it holds the pin for e.
WriterDrop(w) ==
    /\ WriterMayAct(w)
    /\ \E e \in refs :
        /\ e \in pin[w]
        /\ decided[w] # e
        /\ pin' = [pin EXCEPT ![w] = @ \ {e}]
        \* the ref is removed iff no other writer still pins e (no other live reference to it)
        /\ refs' = IF \E v \in Writers \ {w} : e \in pin[v] THEN refs ELSE refs \ {e}
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

----------------------------------------------------------------------------
(* LEADER ACTIONS — a single fenced round: CLOSE, FOLD/CONDEMN, QUIESCE, RECLAIM. *)
(* Every mutation is gated on LeaderActive (fail-close on Disconnected + fence held). *)

\* R0 CLOSE: advance epoch_current; mark the just-closed epoch as CLOSED (the barrier).
LeaderClose(l) ==
    /\ LeaderActive(l)
    /\ epoch_current < MaxEpoch
    /\ epochClosed' = epochClosed \cup {epoch_current}
    /\ epoch_current' = epoch_current + 1
    /\ UNCHANGED << objState, condemnEpoch, refs, pin,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

\* R1+R2 FOLD+CONDEMN: for a Present object in a CLOSED epoch that the fold finds unreferenced,
\* create the tombstone (condemn). FoldUnreferenced uses ONLY durable + in closed epochs + refs.
LeaderCondemn(l) ==
    /\ LeaderActive(l)
    /\ \E e \in Epochs :
        /\ e \in epochClosed
        /\ objState[e] = Present
        /\ FoldUnreferenced(e)
        /\ objState' = [objState EXCEPT ![e] = Condemned]
        /\ condemnEpoch' = [condemnEpoch EXCEPT ![e] = e]
    /\ UNCHANGED << epoch_current, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

\* R3 QUIESCE: recompute THIS leader's safe_epoch (a sync-ed, linearizable read of live writers).
\* A successor MUST run its own R3 before any R4 (it cannot trust an inherited safe_epoch).
LeaderQuiesce(l) ==
    /\ LeaderActive(l)
    /\ safe_epoch' = [safe_epoch EXCEPT ![l] = ComputeSafe]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, pc >>

\* R4 RECLAIM: delete a condemned object e iff
\*   epoch_current >= e+2  (Crossbeam limbo)
\*   AND safe_epoch[l] > e (QSBR grace, this leader's own quiesce)
\*   AND still unreferenced in the fold
\*   AND retention backstop: e <= epoch_current - Retention
\*   AND still leader (current fence + connected) -- the fence re-check is LeaderActive(l).
LeaderReclaim(l) ==
    /\ LeaderActive(l)
    /\ \E e \in Epochs :
        /\ objState[e] = Condemned
        /\ epoch_current >= e + 2
        /\ safe_epoch[l] > e
        /\ FoldUnreferenced(e)
        /\ e <= epoch_current - Retention
        /\ objState' = [objState EXCEPT ![e] = Deleted]
    /\ UNCHANGED << epoch_current, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

----------------------------------------------------------------------------
(* FAILURE ACTIONS — adversarially interleaved. *)

\* Writer's Keeper session is server-expired INDEPENDENTLY of client awareness (the gap).
\* O_W is dropped from the live set (sess -> ServerExpired); conn stays whatever it was.
ServerExpire(w) ==
    /\ EnableExpiry
    /\ sess[w] = SessAlive
    /\ sess' = [sess EXCEPT ![w] = SessExpired]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, conn, decided, plusIssued, fence, heldFence, leaderConn, safe_epoch, pc >>

\* Writer observes Disconnected (client-side). The CORRECTED HINGE: fail-stop. Once Disconnected,
\* the writer may not take consequential steps (enforced by CanWrite / conn guards). It must abort
\* any in-progress commit (drop decided, go idle) — a fail-stop. We model the abort here.
WriterDisconnect(w) ==
    /\ conn[w] = ConnConnected
    /\ conn' = [conn EXCEPT ![w] = ConnDisconnected]
    \* fail-stop: abort any in-flight commit (cannot have published if not at PCcommit).
    /\ decided' = [decided EXCEPT ![w] = IF pc[w] = PCcommit THEN decided[w] ELSE -1]
    /\ pc' = [pc EXCEPT ![w] = IF pc[w] = PCcommit THEN PCcommit ELSE PCidle]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, plusIssued, fence, heldFence, leaderConn, safe_epoch >>

\* Writer reconnects. If the server had expired it, it learns Expired -> must re-register:
\* re-read epoch_current as fresh O_W (never reuse stale O_W). Session becomes Alive again.
WriterReconnect(w) ==
    /\ conn[w] = ConnDisconnected
    /\ conn' = [conn EXCEPT ![w] = ConnConnected]
    /\ sess' = [sess EXCEPT ![w] = SessAlive]
    \* re-registering writer re-reads epoch_current as its O_W (K17 / E19).
    /\ O_W' = [O_W EXCEPT ![w] = IF sess[w] = SessExpired THEN epoch_current ELSE O_W[w]]
    /\ plusIssued' = [plusIssued EXCEPT ![w] = IF sess[w] = SessExpired THEN {} ELSE plusIssued[w]]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    decided, fence, heldFence, leaderConn, safe_epoch, pc >>

\* Leader l observes Disconnected -> fail-close: it stops acting (LeaderActive(l) becomes FALSE).
LeaderDisconnect(l) ==
    /\ leaderConn[l] = ConnConnected
    /\ leaderConn' = [leaderConn EXCEPT ![l] = ConnDisconnected]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, safe_epoch, pc >>

\* Leader l reconnects.
LeaderReconnect(l) ==
    /\ leaderConn[l] = ConnDisconnected
    /\ leaderConn' = [leaderConn EXCEPT ![l] = ConnConnected]
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, fence, heldFence, safe_epoch, pc >>

\* Split-brain: a successor leader l steals leadership by acquiring a FRESH higher fence. The OLD
\* leader (whoever held the prior fence) is NOT touched here -- it KEEPS its now-stale heldFence and
\* may still BELIEVE it leads (its leaderConn may still be Connected). This is the dangerous K5
\* split-brain setup: two leaders, one with a stale fence. Safety relies on LeaderActive requiring
\* heldFence[l] = fence (current), so the stale leader is blocked. A successor must run its OWN
\* R3 quiesce (its safe_epoch is reset to a safe low value here) before any R4.
LeaderSteal(l) ==
    /\ EnableSplit
    /\ heldFence[l] # fence            \* l is not already the current leader
    /\ fence' = fence + 1
    /\ heldFence' = [heldFence EXCEPT ![l] = fence + 1]
    /\ leaderConn' = [leaderConn EXCEPT ![l] = ConnConnected]
    /\ safe_epoch' = [safe_epoch EXCEPT ![l] = 0]   \* successor must re-quiesce before deleting
    /\ UNCHANGED << epoch_current, objState, condemnEpoch, refs, pin, epochClosed,
                    O_W, sess, conn, decided, plusIssued, pc >>

\* Total Keeper wipe + recovery: all sessions gone, leader re-elects with a fresh higher fence,
\* reads epoch_current from S3 and BUMPS it once (fence off pre-outage in-flight epoch), closing
\* the just-current epoch. Writers must re-register (O_W reset, plusIssued cleared, conn reset to
\* Disconnected so they fail-stop and re-register). refs/objState/pin (S3) survive.
KeeperWipeRecover ==
    /\ EnableWipe
    /\ epoch_current < MaxEpoch
    /\ LET rl == CHOOSE x \in Leaders : TRUE IN   \* one leader re-elects
       /\ fence' = fence + 1
       /\ heldFence' = [l \in Leaders |-> IF l = rl THEN fence + 1 ELSE 0]  \* all election znodes purged; one re-elects
       /\ leaderConn' = [l \in Leaders |-> ConnConnected]
       /\ safe_epoch' = [l \in Leaders |-> 0]            \* recovery leader must re-quiesce (R3) before any R4
    /\ epochClosed' = epochClosed \cup {epoch_current}   \* close the pre-outage epoch
    /\ epoch_current' = epoch_current + 1                 \* bump once on recovery (fence off in-flight epoch)
    /\ sess' = [w \in Writers |-> SessExpired]            \* all sessions gone (server-side)
    /\ conn' = [w \in Writers |-> ConnDisconnected]       \* writers see Disconnected -> fail-stop
    \* fail-stop aborts in-flight commits that have NOT published a ref:
    /\ decided' = [w \in Writers |-> IF pc[w] = PCcommit THEN decided[w] ELSE -1]
    /\ pc' = [w \in Writers |-> IF pc[w] = PCcommit THEN PCcommit ELSE PCidle]
    /\ UNCHANGED << objState, condemnEpoch, refs, pin, O_W, plusIssued >>

----------------------------------------------------------------------------
(* Next-state relation *)

\* The action disjunction. Each sub-action leaves everDeleted UNCHANGED (it is updated by the
\* top-level Next conjunct below from the resulting objState').
Actions ==
    \/ \E w \in Writers :
        \/ WriterCreate(w)
        \/ WriterReuse(w)
        \/ WriterResurrect(w)
        \/ WriterIssuePlus(w)
        \/ WriterPlusDurable(w)
        \/ WriterAdvanceOW(w)
        \/ WriterRecheckRoute(w)
        \/ WriterPublishRef(w)
        \/ WriterFinish(w)
        \/ WriterDrop(w)
        \/ ServerExpire(w)
        \/ WriterDisconnect(w)
        \/ WriterReconnect(w)
    \/ \E l \in Leaders :
        \/ LeaderClose(l)
        \/ LeaderCondemn(l)
        \/ LeaderQuiesce(l)
        \/ LeaderReclaim(l)
        \/ LeaderDisconnect(l)
        \/ LeaderReconnect(l)
        \/ LeaderSteal(l)
    \/ KeeperWipeRecover

\* Next runs an action, then accumulates any newly-Deleted epoch into the everDeleted history.
Next ==
    /\ Actions
    /\ everDeleted' = everDeleted \cup { e \in Epochs : objState'[e] = Deleted }

Spec == Init /\ [][Next]_vars

----------------------------------------------------------------------------
(* INVARIANTS *)

\* Type correctness.
TypeOK ==
    /\ epoch_current \in Epochs
    /\ objState \in [Epochs -> {Absent, Present, Condemned, Deleted}]
    /\ refs \subseteq Epochs
    /\ pin \in [Writers -> SUBSET Epochs]
    /\ epochClosed \subseteq Epochs
    /\ O_W \in [Writers -> Epochs]
    /\ safe_epoch \in [Leaders -> 0..(MaxEpoch+1)]
    /\ heldFence \in [Leaders -> 0..(MaxEpoch+5)]
    /\ leaderConn \in [Leaders -> {ConnConnected, ConnDisconnected}]
    /\ fence \in 1..(MaxEpoch+5)
    /\ everDeleted \subseteq Epochs

\* INV_NO_LOSS: every epoch named by a live ref resolves to present bytes (not Deleted).
\* (A live ref always resolves to Present or Condemned bytes — never Deleted/Absent.)
INV_NO_LOSS ==
    \A e \in refs : objState[e] \in {Present, Condemned}

\* INV_NO_DANGLE: no live ref names an Absent or Deleted object.
INV_NO_DANGLE ==
    \A e \in refs : objState[e] # Absent /\ objState[e] # Deleted

\* INV_NO_ABA: an epoch key that was EVER Deleted is never observed Present again (recreate must
\* use a fresh epoch key — resurrection to epoch_current, a different key). everDeleted is the
\* history of all epochs that have ever been Deleted; none may be Present in any reachable state.
INV_NO_ABA ==
    \A e \in everDeleted : objState[e] # Present

----------------------------------------------------------------------------
(* STATE CONSTRAINT — bound the explored state space for tractable, exhaustive  *)
(* checking. We are doing BOUNDED model checking: this caps depth without losing *)
(* the safety-critical interleavings (every race fits well within these bounds). *)
\* The total number of currently-live refs and pins is small; epoch is bounded by MaxEpoch
\* already. The main blowup is unbounded commit/drop churn, which adds no new ordering once
\* every (decide, +-durable, condemn, advance, quiesce, reclaim, publish) interleaving has
\* been seen. We bound the number of DISTINCT epochs that have ever been condemned-or-deleted
\* plus the live-ref/pin cardinality to keep the graph finite-and-small.
StateConstraint ==
    /\ Cardinality(refs) <= 2
    /\ \A w \in Writers : Cardinality(pin[w]) <= 1
    /\ \A w \in Writers : Cardinality(plusIssued[w]) <= 1
    /\ fence <= MaxEpoch + 2   \* bound the (otherwise unbounded) fence from steals/wipes

\* A stronger, history-aware ABA check (a Deleted key never becomes Present in a later state)
\* is expressed as an action property below.
NoResurrectDeleted ==
    [][ \A e \in Epochs : (objState[e] = Deleted) => (objState'[e] = Deleted) ]_vars

----------------------------------------------------------------------------
(* LIVENESS (optional, cheap temporal property): a genuinely-unreferenced, condemned object  *)
(* that is fully quiesced is eventually Deleted. We keep this WEAK and bounded.               *)
\* (Checked only in dedicated configs to keep state space small.)
Liveness ==
    \A e \in Epochs :
        ( objState[e] = Condemned /\ e \notin refs ) ~> ( objState[e] \in {Deleted, Present} )

=============================================================================

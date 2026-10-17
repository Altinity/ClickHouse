---------------------------- MODULE CaRetiredInRun ----------------------------
(* Retired-list-inside-the-run settlement gate (spec 2026-07-10-cas-retired-in-snapshot §6).

   One gc-shard, one implicit writer, blobs Blobs. Writers add/remove source edges through an
   append-only journal; the GC leader folds a journal prefix (the cut) into an attempt-scoped
   artifact {edges, condemned rows} (one atom = the merged run + its seal coverage), and adopts it
   via one gc/state CAS (adopted' = the artifact). Settlement follows spec §3 exactly:
     1. delete_pending at d = 0 -> redelete (exact-token delete executes pre-CAS, row dropped);
        delete_pending at d > 0 -> structurally-impossible spare (row dropped, no delete).
     2. d > 0 -> spared (row dropped).
     3. d = 0 and condemn_round < current_round -> graduated (republished delete_pending).
     4. d = 0 otherwise -> carried unchanged.
   Fresh zero-transition at d = 0: present blob -> kCondemned row minted at the current round with
   the head-observed token (write-only this pass: nothing destructive reads a fresh observation);
   absent blob -> nothing (the absent-at-condemn / plain zero-marker case).

   Deviations from the task-brief draft (the gate iteration; each was required to make the honest
   config green AND every sabotage red):
   - Fold-read coverage floor `lenAtAdopt`: each fold's cut must cover every journal entry landed
     before the PREVIOUS adoption (round N+1 reads the journals after round N's CAS completed).
     Without it TLC finds the unreal "GC ignores a landed edge forever" hole and the honest config
     is red. Entries landing between the previous adoption and this fold's read may still be missed
     (cut < Len(journal)) — that is the real racing window kept nondeterministic.
   - Incremental fold: newEdges = adopted.edges + delta over (adopted.cut, cut], exactly like the
     real 2-cursor merge (prior run x deltas). The draft recomputed edges from the whole journal
     prefix, which silently HEALS a stale-bytes adoption and turns the attempt_reuse sabotage green.
   - Monotone per-blob token mint `nextTok` (etags are unique): the draft re-minted token 1 after a
     delete, letting an old pending row exact-token-match a fresh re-upload.
   - WriterStaleReuse: the racing dedup-hit — EDGE-BEFORE-OBSERVE journals the edge first, then the
     freshness-meta point-read raced with the CURRENT round's meta write and returned the pre-fold
     "clean", so the writer reuses the condemned incarnation without a token bump. The staleness is
     bounded by ONE round (a read that starts after the next adoption sees the condemned meta) —
     the single-writer abstraction of the ack floor. This is the window the one-round graduation
     gap covers, and what makes the no_pacing sabotage bite.
   - WriterAdd (dedup-hit) does not write the meta (only upload/resurrect do), and reuse requires a
     clean meta point-read; the draft flipped meta -> "clean" on every add.
   - no_pacing sabotage = graduate-at-birth: the fresh condemn is minted directly delete_pending
     (the literal `condemn_round < current_round` gate is also bypassed, but a carried row always
     has condemn_round < current_round, so bypassing only the carried-row gate never bites).
   - attempt_reuse sabotage = the next fold reuses the previous attempt's keys: putIfAbsent
     collides, and instead of the byte-equal-or-CORRUPTED_DATA refusal (the honest conjunct, which
     disables the transition = the round aborts fail-closed) the stale run bytes are silently
     adopted under the retry's newer claimed coverage (spec §4's incoherence class): adopted takes
     the stale edges/cond with the new cut/round.
   - Ghost badDeleteEver + INV_NO_RETURN = the write-only-fresh-observations discipline: a delete
     that executes with a token different from the durably adopted condemn-time token trips it
     (the inmem_token sabotage deletes a resurrected incarnation it just re-observed).
   - INV_COVERAGE (spec §6 coverage-coherence): the adopted edge counts always equal the journal
     truth at the claimed cut — this is what the attempt_reuse mixed adoption breaks first.
   - MaxJournal bound (finite state space) and the NoOp self-loop (house pattern: exhausted bounds
     are not a TLC deadlock).

   Same-buffer resend and competing leaders need no dedicated action: attempt keys are minted from
   the monotone lease.seq (nextAttempt), so a resend is a byte-identical putIfAbsent collision
   (state-identical, allowed by the byte-equal conjunct) and two leaders can never share a key.
   Clamp suppression and pure ref-carry are out of this gate's scope (brief draft scope): both only
   REMOVE destructive behaviors from a round, so their absence over-approximates the destructive
   schedule this model checks.

   Sabotage \in {"none","inmem_token","attempt_reuse","no_pacing"} — honest config must be green,
   each sabotage config must be red. *)
EXTENDS Integers, Sequences, FiniteSets, TLC

CONSTANTS Blobs,          \* e.g. {b1, b2} (model values)
          MaxRound,       \* fold-round bound, e.g. 4
          MaxToken,       \* incarnation tokens per blob, e.g. 3
          MaxJournal,     \* journal length bound, e.g. 5
          Sabotage        \* "none" | "inmem_token" | "attempt_reuse" | "no_pacing"

ASSUME Sabotage \in {"none", "inmem_token", "attempt_reuse", "no_pacing"}

VARIABLES
  journal,      \* Seq of <<blob, op>>, op \in {"add","rm"} — writer edge events (abstract sources)
  phys,         \* [Blobs -> 0..MaxToken] physical incarnation token; 0 = absent
  nextTok,      \* [Blobs -> 1..MaxToken+1] next fresh token to mint (etags never repeat)
  liveRef,      \* [Blobs -> BOOLEAN] writer holds a live reference (edge added, not removed)
  adopted,      \* durable adopted artifact:
                \*   [gen, cut, edges: [Blobs -> Int],
                \*    cond: [Blobs -> [st: {"none","cond","pend"}, tok: Int, round: Int]], round]
  artifacts,    \* attempt-keyed store: attempt |-> artifact — the putIfAbsent byte-equal domain
  nextAttempt,  \* monotone attempt counter (lease.seq abstraction; bumped every round)
  lenAtAdopt,   \* Len(journal) at the last adoption — the next fold's coverage floor
  meta,         \* [Blobs -> {"clean","cond"}] advisory freshness meta; no destructive read
  badDeleteEver \* ghost: a delete executed with a token /= the durably adopted condemn-time token

vars == <<journal, phys, nextTok, liveRef, adopted, artifacts, nextAttempt, lenAtAdopt, meta,
          badDeleteEver>>

NoRow == [st |-> "none", tok |-> 0, round |-> 0]

\* Journal truth at a prefix (the oracle the invariants compare against).
EdgeCount(b, cut) ==
  Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "add"})
    - Cardinality({i \in 1..cut : journal[i][1] = b /\ journal[i][2] = "rm"})

\* Incremental delta over the window (lo, hi] — what the real 2-cursor merge consumes.
Delta(b, lo, hi) ==
  Cardinality({i \in (lo + 1)..hi : journal[i][1] = b /\ journal[i][2] = "add"})
    - Cardinality({i \in (lo + 1)..hi : journal[i][1] = b /\ journal[i][2] = "rm"})

Init ==
  /\ journal = <<>>
  /\ phys = [b \in Blobs |-> 0]
  /\ nextTok = [b \in Blobs |-> 1]
  /\ liveRef = [b \in Blobs |-> FALSE]
  /\ adopted = [gen |-> 0, cut |-> 0, edges |-> [b \in Blobs |-> 0],
                cond |-> [b \in Blobs |-> NoRow], round |-> 0]
  /\ artifacts = <<>>
  /\ nextAttempt = 1
  /\ lenAtAdopt = 0
  /\ meta = [b \in Blobs |-> "clean"]
  /\ badDeleteEver = FALSE

(* Writer add: EDGE-BEFORE-OBSERVE — the edge is journalled first, then the observation decides
   fresh upload (absent blob: mint a fresh incarnation, write clean meta) vs dedup-hit reuse
   (present blob whose meta point-read is clean; the writer writes nothing). A clean-read reuse of
   a condemned incarnation is only possible through WriterStaleReuse below. *)
WriterAdd(b) ==
  /\ Len(journal) < MaxJournal
  /\ ~liveRef[b]
  /\ IF phys[b] = 0
     THEN /\ nextTok[b] <= MaxToken
          /\ phys' = [phys EXCEPT ![b] = nextTok[b]]
          /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
          /\ meta' = [meta EXCEPT ![b] = "clean"]
     ELSE /\ meta[b] = "clean"
          /\ UNCHANGED <<phys, nextTok, meta>>
  /\ journal' = Append(journal, <<b, "add">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ UNCHANGED <<adopted, artifacts, nextAttempt, lenAtAdopt, badDeleteEver>>

(* The racing dedup-hit: the meta point-read overlapped the LATEST fold's meta write and returned
   the pre-fold "clean" — the writer reuses the just-condemned incarnation (no token bump, no meta
   write). Staleness is bounded by one round: the window is open only while the row's condemn round
   IS the adopted round (once the next round adopts, any read starts after this condemn's meta
   write and sees "cond"). This is the racing writer the one-round graduation gap spares. *)
WriterStaleReuse(b) ==
  /\ Len(journal) < MaxJournal
  /\ ~liveRef[b]
  /\ phys[b] > 0
  /\ adopted.cond[b].st \in {"cond", "pend"}
  /\ adopted.cond[b].round = adopted.round
  /\ journal' = Append(journal, <<b, "add">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ UNCHANGED <<phys, nextTok, adopted, artifacts, nextAttempt, lenAtAdopt, meta, badDeleteEver>>

(* Writer resurrect: the meta point-read said "cond" — fresh re-upload from source ONLY (never a
   read of the condemned object): a fresh incarnation token, the edge journalled, clean meta. *)
WriterResurrect(b) ==
  /\ Len(journal) < MaxJournal
  /\ ~liveRef[b]
  /\ phys[b] > 0
  /\ meta[b] = "cond"
  /\ nextTok[b] <= MaxToken
  /\ phys' = [phys EXCEPT ![b] = nextTok[b]]
  /\ nextTok' = [nextTok EXCEPT ![b] = @ + 1]
  /\ journal' = Append(journal, <<b, "add">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = TRUE]
  /\ meta' = [meta EXCEPT ![b] = "clean"]
  /\ UNCHANGED <<adopted, artifacts, nextAttempt, lenAtAdopt, badDeleteEver>>

WriterRemove(b) ==
  /\ Len(journal) < MaxJournal
  /\ liveRef[b]
  /\ journal' = Append(journal, <<b, "rm">>)
  /\ liveRef' = [liveRef EXCEPT ![b] = FALSE]
  /\ UNCHANGED <<phys, nextTok, adopted, artifacts, nextAttempt, lenAtAdopt, meta, badDeleteEver>>

(* Settlement per spec §3 (pc = prior condemned row, d = merged net in-degree at the new cut,
   tokNow = head-observed token for a fresh condemn, newRound = the folding round). The redelete's
   physical delete is executed by FoldRound; here the pending row is dropped either way (delete
   landed, or exact-token miss = outcome Replaced). *)
Settle(pc, d, tokNow, newRound) ==
  IF pc.st = "pend"
    THEN NoRow                                     \* d=0: redelete; d>0: structurally-impossible spare
  ELSE IF d > 0
    THEN NoRow                                     \* spared (recovery wins even past the pacing gate)
  ELSE IF pc.st = "cond"
    THEN IF pc.round < newRound \/ Sabotage = "no_pacing"
           THEN [st |-> "pend", tok |-> pc.tok, round |-> pc.round]   \* graduated
           ELSE pc                                                    \* carried
  ELSE IF tokNow > 0                               \* fresh condemn (head observation, write-only)
    THEN [st |-> IF Sabotage = "no_pacing" THEN "pend" ELSE "cond",   \* graduate-at-birth = no pacing
          tok |-> tokNow, round |-> newRound]
  ELSE NoRow                                       \* absent-at-condemn: plain zero-marker, no row

(* One fold round = one gc/state CAS (one-pass). The cut is any point covering the previous
   adoption's journal (coverage floor) up to the current head. The artifact is PUT at the attempt
   key (putIfAbsent); an honest collision must be byte-equal or the round aborts fail-closed
   (CORRUPTED_DATA — the conjunct disables the transition). Redelete executes pre-CAS against
   prior-adopted pending rows at d = 0, with the token read from the durable prior artifact. *)
FoldRound ==
  /\ adopted.round < MaxRound
  /\ \E cut \in lenAtAdopt..Len(journal) :
     LET newRound == adopted.round + 1
         attempt  == IF Sabotage = "attempt_reuse" /\ nextAttempt > 1
                       THEN nextAttempt - 1 ELSE nextAttempt
         newEdges == [b \in Blobs |-> adopted.edges[b] + Delta(b, adopted.cut, cut)]
         newCond  == [b \in Blobs |-> Settle(adopted.cond[b], newEdges[b], phys[b], newRound)]
         art      == [gen |-> adopted.gen + 1, cut |-> cut, edges |-> newEdges,
                      cond |-> newCond, round |-> newRound]
         collide  == attempt \in DOMAIN artifacts
         (* Under attempt_reuse the stale run bytes at the reused key are silently adopted under
            the retry's newer claimed coverage — the exact incoherence class of spec §4. *)
         durable  == IF collide
                       THEN [art EXCEPT !.edges = artifacts[attempt].edges,
                                        !.cond = artifacts[attempt].cond]
                       ELSE art
         delTok(b)  == IF Sabotage = "inmem_token" THEN phys[b] ELSE adopted.cond[b].tok
         delExec(b) == adopted.cond[b].st = "pend" /\ newEdges[b] = 0 /\ phys[b] = delTok(b)
     IN /\ (collide /\ Sabotage /= "attempt_reuse") => (artifacts[attempt] = art)
        /\ artifacts' = IF collide THEN artifacts ELSE artifacts @@ (attempt :> art)
        /\ phys' = [b \in Blobs |-> IF delExec(b) THEN 0 ELSE phys[b]]
        /\ badDeleteEver' = (badDeleteEver
                             \/ \E b \in Blobs : delExec(b) /\ phys[b] /= adopted.cond[b].tok)
        /\ adopted' = durable
        (* Advisory meta written pre-CAS from THIS attempt's in-memory merge result. *)
        /\ meta' = [b \in Blobs |-> IF newCond[b].st \in {"cond", "pend"} THEN "cond" ELSE "clean"]
        /\ nextAttempt' = nextAttempt + 1
        /\ lenAtAdopt' = Len(journal)
        /\ UNCHANGED <<journal, liveRef, nextTok>>

(* Self-loop so bounded counters exhausting is not a TLC deadlock (house pattern). *)
NoOp == UNCHANGED vars

Next ==
  \/ \E b \in Blobs : WriterAdd(b) \/ WriterRemove(b) \/ WriterResurrect(b) \/ WriterStaleReuse(b)
  \/ FoldRound
  \/ NoOp

Spec == Init /\ [][Next]_vars

TypeOK ==
  /\ Len(journal) <= MaxJournal
  /\ \A i \in 1..Len(journal) : journal[i][1] \in Blobs /\ journal[i][2] \in {"add", "rm"}
  /\ \A b \in Blobs : phys[b] \in 0..MaxToken /\ nextTok[b] \in 1..(MaxToken + 1)
                      /\ phys[b] < nextTok[b]
  /\ \A b \in Blobs : liveRef[b] \in BOOLEAN /\ meta[b] \in {"clean", "cond"}
  /\ adopted.round \in 0..MaxRound
  /\ adopted.cut \in 0..MaxJournal /\ adopted.cut <= Len(journal)
  /\ \A b \in Blobs : adopted.edges[b] \in 0..MaxJournal
  /\ \A b \in Blobs : adopted.cond[b].st \in {"none", "cond", "pend"}
  /\ lenAtAdopt <= Len(journal)
  /\ badDeleteEver \in BOOLEAN

(* INV_NO_LOSS: a blob the writer holds a live journalled reference to is never physically absent
   once its edge is inside the adopted coverage (folded live edge => present). *)
INV_NO_LOSS ==
  \A b \in Blobs : (liveRef[b] /\ EdgeCount(b, adopted.cut) > 0) => phys[b] > 0

(* INV_NO_RETURN: the write-only-fresh-observations discipline — a delete's token always equals
   the durably adopted condemn-time token; a delete executed via a re-observed in-memory token
   (which can name a resurrected live incarnation) trips the ghost. *)
INV_NO_RETURN == ~badDeleteEver

(* INV_COVERAGE: coverage-coherence — the adopted edge state always equals the journal truth at
   the claimed cut (the sealed coverage describes the adopted run bytes). *)
INV_COVERAGE ==
  \A b \in Blobs : adopted.edges[b] = EdgeCount(b, adopted.cut)

(* INV_ONE_PASS: one-pass adoption — the adopted artifact is always one stored attempt artifact. *)
INV_ONE_PASS ==
  adopted.round = 0 \/ \E a \in DOMAIN artifacts : artifacts[a] = adopted

THEOREM Spec => [](TypeOK /\ INV_NO_LOSS /\ INV_NO_RETURN /\ INV_COVERAGE /\ INV_ONE_PASS)
===============================================================================

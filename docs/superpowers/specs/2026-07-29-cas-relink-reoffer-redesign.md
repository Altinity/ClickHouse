---
description: 'Design: the CAS fetch-by-relink confirm is re-expressed as a second, identity-only OFFER against the same endpoint. The separate confirm action, its six-field source token and its percent-encoding codec, the `CasConfirmAnswer` enum crossing the replication seam, the namespace-routing predicate and the lane-quiescence refusal all disappear; the mount fence moves first so a `No` becomes authoritative knowledge.'
sidebar_label: 'CAS relink re-offer redesign'
sidebar_position: 20260729
slug: /superpowers/specs/cas-relink-reoffer-redesign
title: 'CAS relink re-offer: retiring the confirm endpoint'
doc_type: 'reference'
---

# CAS relink re-offer: retiring the confirm endpoint {#cas-relink-reoffer-redesign}

**Date:** 2026-07-29. **Status:** DESIGN, awaiting approval. **Branch:** `cas-gc-rebuild`.
Spec only; no code landed.

**Binding input, not relitigated here:**
`docs/superpowers/cas/2026-07-29-relink-confirm-per-ref-draft.md` §0 DECISION — variant (ii)
re-offer is chosen, variant (i) (the per-ref `MutationScope` index) is retired pre-ship, and
axis (iii) (receiver-pays) is recorded as rejected. Everything that document fixes as a
constraint is carried into this design as a requirement, not re-argued.

**Supersedes:** `docs/superpowers/specs/2026-07-23-cas-fetch-handoff-publish-confirm-design.md`
(rev.5, shipped as protocol v11, `260a6f81169`, 2026-07-25) — its `§confirm-primitive`,
`§wire-protocol` and `§failure-taxonomy` sections. `§core-idea` and `§correctness` survive
verbatim: the publish-then-confirm ORDER is untouched, and only the shape of the question changes.
`§A2` (the apply-pending state machine) is not superseded but is **amended** — see §5.1.2, which
promotes its marker from defense-in-depth to primary guard and states what that promotion costs.

**Companion backlog item:** `docs/superpowers/cas/BACKLOG.md` `{#relink-confirm-busy-lane}`.

## 1. What changes, in one paragraph {#summary}

Today the receiver proves the source's binding by calling a SECOND, purpose-built interserver
action that carries a six-field opaque token; the sender decodes the token, re-routes it from
untrusted strings to one of its disks, filters it through the parts set, and answers
`yes`/`unproven` out of a bespoke six-rule snapshot taken across both ref-lane mutexes with zero
object-store I/O. Under this design the receiver instead **asks the same question the offer
already answered, a second time**: it re-issues the ORIGINAL offer request — same endpoint, same
`part` parameter, same advertised pool identity — with one added flag meaning *"answer with the
binding's identity only; send no manifest body"*. The sender answers from its ordinary committed
view. The receiver compares two opaque strings. Everything that existed only to carry a question
without its context — the token, its codec, the routing predicate, the parts-set proof gate, the
answer enum crossing the replication seam — is deleted, and because the mount fence is evaluated
FIRST rather than last, a `No` becomes authoritative knowledge the receiver can act on instead of
a refusal it must retry.

**Two gates, both of which can stop this design, stated up front rather than discovered late:**

1. **The apply-pending marker must be made synchronized before the lane-quiescence refusal can be
   removed** (§5.1.2). The refusal this design deletes is currently the *properly locked* guard on
   the durable-but-unapplied window; the marker meant to replace it is a relaxed atomic written
   outside any lock. This is a real change to the ref-lane append path, not to the relink path.
2. **The TLA model must be refined and must then pass** (§12.3). As it stands the model refutes
   this design — correctly, given what it represents. The refinement is what distinguishes "the
   deleted terms were redundant" from "the deleted terms were load-bearing", and if the refined
   `_sab_stalecache` does not pass, the design is wrong and must not be implemented.

## 2. The measured problem {#measured-problem}

Rule 3 of `CasRefLedger::confirmExactRef` refuses whenever the namespace's append lane is not
quiescent (`CasRefLedger.cpp:444`):

```cpp
if (rt.wedge.has_value() || !rt.pending.empty() || rt.leader_active)
    return ConfirmAnswer::Unknown;
```

A busy writer's lane is essentially never quiescent, so the flagship cheap-replication path is
off almost exactly when it is most valuable:

| Measurement | Value | Source |
| --- | --- | --- |
| Relink availability under sustained writes | **~17%** (~92k refusals vs 19,531 proven) | T14 soak run 1 |
| Refusals, cluster-wide | **248,400 in ~32 min**, peak **9,219/min** | gc-audit, same run |
| Cost of one refusal | an ERROR-severity exception, ~23 symbolized frames | throw site, `DataPartsExchange.cpp:1549` |
| Share of the `NETWORK_ERROR` population | ≈ all of it | gc-audit |
| Stage-A regression? | No — the stage's whole diff over `confirmExactRef` is comment-only | verified 2026-07-29 |

The cost shape matters for how urgently this must be fixed: the abandon throws `NETWORK_ERROR`,
the retry-later class, so the replication queue stores the fetch, backs off and re-selects. A
byte re-request to the same source is explicitly forbidden as unsound on that path. Refusals
therefore cost **convergence latency and log/CPU noise, never duplicate bytes and never a double
publish** — replication converged in both soaks. This is an availability defect of the mechanism,
not a correctness defect.

## 3. The enabling fact {#enabling-fact}

The confirm asks: *"does `ref_name` in `root_namespace` still name exactly this manifest?"* The
offer already answered a strictly stronger question one round trip earlier: *"what does the ref
for part `P` name, and what is in it?"* (`ContentAddressedMetadataStorage::getRelinkOffer`,
`ContentAddressedMetadataStorage.cpp:2000`, which routes `part_path` → `(ns, ref)` and resolves).

The token exists for exactly one reason: **the confirm request has no part context.** It is
dispatched at `DataPartsExchange.cpp:308` before `part` is even read, so every field the sender
needs to find the answer has to travel inside it — `pool_uuid` and `server_root_id` to select the
mount, `root_namespace` and `ref_name` to name the binding, `part_name` for the parts-set filter,
`manifest_ref_text` for the comparison. Each of those fields then arrives as untrusted peer input
and needs its own defence: a strict percent-encoding bijection so a namespace's `/` and `@` and an
operator-chosen `server_root_id` survive a cookie and a query parameter
(`ContentAddressedExchange.cpp:38-100`), an `ownsNamespace` predicate with an *exactly one match*
rule because a pool UUID cannot select a mount on its own (`DataPartsExchange.cpp:227-244`), and a
canonical-form parser hardened against forged input (`CasTypes.h:172-179`).

**Give the question its part context back and all six fields evaporate.** A request that carries
`part` and `endpoint` — which is to say, an ordinary fetch request — lets the sender resolve the
part exactly as the offer did, through the same router every other read and write of that part
uses. Nothing is routed from a peer-supplied string, so nothing needs to be encoded, decoded,
range-checked or disambiguated.

The one field that is not routing — `manifest_ref_text`, the thing being compared — moves in the
other direction: the sender puts its binding's identity on the wire with the offer, and the
receiver keeps it and compares it itself.

## 4. The mechanism {#mechanism}

### 4.1 Wire protocol {#wire-protocol}

One request parameter and one response cookie replace the five confirm names.

| Name | Direction | Meaning |
| --- | --- | --- |
| `content_addressed_relink_identity_only` | request | *"Answer with the binding identity for `part`; send no manifest body and no bytes."* Present only on the second request. |
| `content_addressed_manifest_id` | response | The sender's binding identity for `part`: the canonical `writer_epoch:build_sequence:manifest_ordinal` text, or the literal `none`. Absent means the sender did not answer. |

Set on **both** responses — the first offer and the re-offer — because that is what makes the
comparison a comparison of like with like. `content_addressed_source_token` is deleted.

The identity is **opaque to the receiver**: it is stored and compared byte-exactly and never
parsed. This is not a style preference, it removes an obligation — today the canonical form is
parsed from untrusted peer input on the sender (`tryParseManifestRef`, whose comment documents
exactly that hazard); under this design no peer-supplied manifest reference is ever parsed by
anyone. `none` cannot collide with a real identity: the canonical form is three colon-separated
decimal fields, and the ordinal `0` is a reserved invalid sentinel that is never emitted.

The receiver's pool identity (`content_addressed_pool_uuid`) rides on the re-offer exactly as it
rides on the first offer, so the same-pool gate at `DataPartsExchange.cpp:410` applies unchanged
and needs no restatement.

### 4.2 Answer vocabulary and the fence-first reordering {#answer-vocabulary}

Three outcomes, and — unlike today — two of them are knowledge.

| Cookie | Outcome | What the sender is asserting |
| --- | --- | --- |
| identity equal to the held one | **Yes** | *"I hold the mount fence, and this ref still names exactly that manifest."* |
| identity present but different, or `none` | **No** — authoritative | *"I hold the mount fence, and this ref does not name that manifest (any more)."* |
| absent | **Unknown** | *"I cannot speak for this namespace right now."* |

The fence check moves from LAST to FIRST. `CasConfirmAnswer`'s own comment already prescribes the
reorder for anyone who wants to distinguish `No` (`ContentAddressedExchange.h:19-26`): "Any code
that ever treats `No` as authoritative knowledge ... makes that ordering wrong and must hoist rule
6 above the row comparison first." That is precisely what this design does, and it is what buys
the payoff in §4.4.

The order of the sender's evaluation, therefore:

1. **Mount fence** (`fence_ok_fn` and `superseded_by_remount`). Not held ⇒ `Unknown`, and nothing
   below is even evaluated. A fenced-out mount is not this namespace's writer and its answer is
   not an answer.
2. **Residency and recovery.** The runtime exists and is recovered (see §4.3 for the mandatory
   recovery and its budget); `recovery_in_progress` or `superseded_by_remount` ⇒ `Unknown`.
3. **Wedge** (`rt.wedge`). An object that may be durable and is not applied — it may BE the removal
   being asked about ⇒ `Unknown`.
4. **Apply state.** Anything other than `RefApplyState::Clean` ⇒ `Unknown`. `ApplyPending` means a
   transaction is in flight between its `PUT` and its install; `Poisoned` means this cache is
   known to be missing a durable transaction.
5. **The row.** The committed row for the ref: present ⇒ its identity; absent ⇒ `none`. Both are
   authoritative, because 1–4 established that this view is neither stale nor unentitled.

Rules 1, 3, 4 and 5 are today's rules 6, 3a, 4 and 5 with the fence hoisted. **Rules 2's
"cold/evicted ⇒ Unknown" arm and rule 3's `!pending.empty() || leader_active` terms are the two
things that go**, and §5 is the argument for why.

**What a `Yes` still does not prove**, carried forward verbatim in intent from
`CasRefLedger::confirmExactRef`'s own comment so nobody has to rediscover it: that this runtime's
recovered view is a COMPLETE replay of the durable log. Completeness is recovery's contract, not
this answer's. Rules 1–4 exclude every way this MOUNT can have fallen behind its own durable
writes; a recovery that silently observed less than it should have is a different defect, in a
different component — and it is the defect the v9 chain models exist to exclude (§12).

### 4.3 The sender's answer path {#sender-answer-path}

The answer comes from the ORDINARY read path, not from a bespoke snapshot. Concretely, the ledger
grows one entry point that shares `resolveRef`'s recovery preamble and differs in three ways.

**Mandatory recovery.** `ensureRefTableRecovered` is called. This is the constraint the decision
fixes first: *"no-maintenance" cannot mean "no-recovery"* — for a cold table, recovery IS the
answer path, and refusing to recover would simply reintroduce the availability hole one level
down.

**Discretionary maintenance suppressed.** `sweepStalePrecommitsForRead` and
`maybeScheduleSnapshotPublish` — which `resolveRef` calls at `CasRefLedger.cpp:287-288` — are NOT
called. The governing sentence, to be reproduced in the code comment:

> A peer-initiated read performs exactly what is needed to answer, and schedules nothing beyond.

Mechanically this is a `ReadMaintenance` parameter on `resolveRef` (`Full` for local callers,
`AnswerOnly` for this one) rather than a parallel resolve implementation — one resolve, one
recovery preamble, one place where the committed row is read.

**A budget on peer-initiated recovery.** Recovery is a full `LIST` plus log replay, orders of
magnitude beyond the `HEAD`-free query it replaces, and the LRU eviction budget is an
**amplifier**: with `ref_table_cache_bytes` at 256 MiB (`CasPool.h:221`), a peer cycling re-offers
across many namespaces makes every answer cold, and each cold answer evicts a table the writer
was using — a self-sustaining recovery storm driven entirely by a remote peer. Two mitigations,
in the order they should be implemented:

1. **Do not mark a peer-initiated recovery most-recently-used.** `ensureRefTableRecovered`
   bumps `last_touch_tick` so `enforceRefTableCacheBudget` evicts idle tables first
   (`CasRefLedger.cpp:925`). For a peer-initiated recovery the opposite is wanted: leave the tick
   untouched so the table sorts FIRST among eviction candidates and can never displace a table
   this writer is using. One line; it defuses the amplifier at its mechanism. It costs nothing in
   the steady state, where confirms are warm by construction (§10 measurement 1), and costs a
   re-recovery only in exactly the abusive pattern it is defending against.
2. **A pool-global cap on peer-initiated recoveries.** Checked BEFORE any I/O and only when the
   table is not already warm — a warm answer never touches the budget. Over budget ⇒ `Unknown`,
   which is soft by construction, since `Unknown` already means retry-later.

The DoS contract survives, reformulated: it was *"a peer can make this writer do no I/O at all"*
and becomes ***"everything a peer can make this writer do is bounded above."*** That weakening is
real and is the one structural property this design trades away; it is stated here rather than
buried, and measurement 1 of §10 is what decides whether the budget is theoretical insurance or
load-bearing structure.

**Observability requirement, from the decision:** budget refusals get their OWN counter, distinct
from every other `Unknown`. The rule-3 storm took a live cluster to diagnose precisely because
refusal reasons were indistinguishable on the wire and unlogged on the sender.

### 4.4 The receiver's comparison, and the byte-fetch fast path {#receiver-comparison}

The receiver holds the identity from the first offer. It re-issues the offer with
`content_addressed_relink_identity_only`, reads the cookie, and compares:

- **equal** ⇒ promote (T3), exactly as today;
- **different or `none`** ⇒ `abort` the prepared relink, then **fetch the bytes from the same
  sender**;
- **absent** (refusal, older peer, transport failure, timeout — one outcome, as today) ⇒ `abort`,
  then throw the retry-later `NETWORK_ERROR`.

The middle line is new and is the payoff of the fence-first reordering. Today it is forbidden:
row 3's ban on a same-sender byte re-request exists because *"a byte re-request goes back to the
very source whose state is in doubt."* Under fence-first there is no doubt left to protect
against — the sender held its fence when it answered and told us, as the namespace's single
writer, that it does not name that manifest. The receiver's precommit is released before the byte
fetch, so nothing is published twice; and the byte fetch is an ordinary fetch, whose behaviour
when the sender no longer has the part at all (`NO_SUCH_DATA_PART`, queue re-selects) is the
behaviour every other fetch already has.

The common real cause of a `No` is a repoint under the same part name — the committed-part repoint
that mutable per-part files produce — where the sender demonstrably still has a part under that
name and the byte fetch succeeds immediately. Today that case costs a full retry-later cycle
through the replication queue.

## 5. Why fail-close is not weakened {#fail-close}

Two refusals are removed. Each needs its own argument, because they fail for different reasons.

### 5.1 The lane-quiescence terms {#lane-quiescence}

What a `Yes` must establish is not *"no mutation is in flight"* but ***"my committed view is not
behind my own durable log."*** Those are different statements, and the current rule 3 conflates
them. Separating them is what makes the removal possible — but the separation comes with a
prerequisite, and §5.1.2 is that prerequisite. **This subsection is the one place where the
design changes code outside the relink path, and it must not be skipped.**

#### 5.1.1 Why the terms are redundant in principle {#lane-quiescence-principle}

Trace the append lane. `armApplyPending` is called at `CasRefLedger.cpp:2806`, and its comment
fixes the placement: *"IMMEDIATELY before the `PUT` — the last statement that still runs while
nothing of this transaction can possibly be durable."* `clearApplyPending` is the last statement
of the install region at `CasRefLedger.cpp:2990`, in the same allocation-free scope as
`rt->state.swap(*candidate)`: *"'recorded' and 'no apply owed' become true together or not at
all."* Every other exit either proves nothing was sent (marker back to `Clean`,
`CasRefLedger.cpp:2825` and `:3110`) or leaves the table wedged or `Poisoned`.

The marker is armed and cleared **per chunk**, inside `commitRefChunk`, not per tenure. So the
window *"durable but not visible in `committed`"* is, logically, **exactly** `apply_state !=
Clean` or a wedge — both of which rules 3a and 4 already refuse and this design keeps. Meanwhile:

- **A pending item is admitted, not durable.** Its ops do not even exist yet — `build_ops` is
  deferred and materializes only at flush. Nothing of it has been `PUT`, so no GC fold can have
  observed it, so it cannot have condemned anything.
- **A leader tenure spans chunks, and each chunk closes its own window.** Between chunk N's
  install and chunk N+1's arm, `apply_state` is `Clean` and the view is complete with respect to
  everything durable. `leader_active`'s claim to cover "the partially-durable window" is
  discharged by the marker, one layer down and more precisely.

That leaves the hazard sentence rule 3 was written for: *"a pending item may be the removal being
asked about."* Suppose it is. The removal commits at some T3. The receiver's `+1` became durable
at T1, before the question was asked at T2 < T3. The custody chain is exactly the ordering the
protocol was built on: the receiver's reference is already in the ref log when that removal is
appended, so the removal is a LATER removal and the blobs stay protected. The refusal was not
protecting anything.

The genuinely dangerous case is the mirror image — a removal appended BEFORE T1, with the sender's
view too stale to know — and that is what rules 1–4 exclude, one by one.

#### 5.1.2 PREREQUISITE: the apply marker must be synchronized {#apply-marker-synchronization}

The paragraph above is a statement about program ORDER. It is not yet a statement about what a
concurrent reader can OBSERVE, and today it is not one, because the marker is not synchronized
against a reader:

- `armApplyPending` performs a `compare_exchange_strong` with `std::memory_order_relaxed`
  (`CasRefLedger.cpp:1681`) and is called at `:2806` **while no lock is held** — the preceding
  `state_mutex` scope ends at `:2715`, and the install does not re-take it until `:2928`.
- The answer reads the committed row under `state_mutex`.

So a reader can acquire `state_mutex` after the arm, in real time, and still be entitled by the
memory model to observe `Clean` — there is no happens-before edge between the relaxed store and
the reader's acquire. Today this is harmless because rule 3 carries the weight: `leader_active` is
set under `ref_queue_mutex` at `CasRefLedger.cpp:1588` and cleared under it at `:1668`, it is TRUE
for the whole tenure, and the confirm reads it under that same mutex. **Rule 3's terms are not
merely redundant with rule 4 — they are the properly synchronized guard, and rule 4 is a relaxed
atomic riding along.** Deleting rule 3 without fixing that would leave the durable-but-unapplied
window guarded by an unsynchronized read: precisely the "stale cache" hazard the design is
supposed to keep excluding.

The superseded spec says both halves of this in its own voice, which is the strongest available
evidence that the reading above is the intended one and not a misinterpretation. Its rule 3
carries a *"Chunked-lane note"*: *"one tenure commits MULTIPLE durable transactions, so mid-tenure
a table is partially durable; `leader_active` covers the whole tenure, so this is already
unknown — a wider unknown window under load, not a hole."* And its §A2 introduces the marker as
*"a cheap defense-in-depth marker (it is also the confirm's rule 4)"*, with an explicit ordering
caveat warning: *"Do not ship the marker as the only protection."* This design does exactly that —
makes the marker the only protection for this window — so it must first make the marker
sufficient.

**Required change, and it is small.** Arm the marker inside a `state_mutex` scope immediately
before the `PUT`, and read it under `state_mutex` (which the answer path holds anyway, to read the
committed row). Mutual exclusion then supplies the whole argument, in exactly the shape today's
`ref_queue_mutex` argument has:

- a reader that acquires `state_mutex` **after** the arm necessarily observes `ApplyPending` and
  refuses;
- a reader that acquires **before** the arm observes `Clean` — and at that instant the `PUT` has
  not been issued, so nothing of that transaction can be durable, and the view it reads is
  complete.

Cost: one uncontended mutex acquire per committed chunk, against a network `PUT`. `armApplyPending`
stays `noexcept` and allocation-free; no OTHER lock is held at that call site, so introducing
`state_mutex` there cannot invert the established order (`state_mutex` nests under
`ref_queue_mutex`, never the reverse).

**Implementation obligation:** every site that can produce a durable ref-log effect must arm under
`state_mutex` before the effect. The plan must enumerate them exhaustively — `commitRefChunk` and
the wedge-resolution install are the two known ones — because the argument above is only as strong
as that enumeration.

### 5.2 The cold/evicted refusal {#cold-refusal}

`confirmExactRef` answers `Unknown` for a cold or evicted table on purpose: it uses `find`, never
`getRefTableRuntime`, so a peer cannot grow this writer's table cache or make it pay a recovery
(`CasRefLedger.cpp:410-416`). Removing that refusal is what §4.3's budget pays for, and it is a
deliberate trade rather than an oversight: keeping it would mean a cold table can never answer,
which for a peer whose entire question is about a cold table is the same availability hole in a
different place. The mitigations are the LRU non-promotion and the global cap; the property that
survives is boundedness, not zero.

## 6. The receiver's state machine {#state-machine}

Four states replace the seven taxonomy rows. Each state answers the same two questions once, for
every path that reaches it, instead of once per row.

### 6.1 S0 — NOT STAGED {#s0-not-staged}

Nothing of this relink is durable at the receiver. Reached by: no identity cookie on the offer (a
peer that predates this design), an unrecognized relink cookie value, a reservation that landed
outside the advertised pool, an undecodable manifest, or the retryable staging class
(`CaRelinkPrepare::MechanismFallbackAllowed` — body-absent precommit, precommit no longer the live
owner, ref conflict).

**Action:** return `nullptr`; the caller byte-fetches from the same sender, with the recursion
brake (`allow_ca_relink=false`) set.

- *No part loss?* No — the sender still has the part and streams it.
- *No double publish?* No — nothing was staged, so there is nothing to promote. For the staging
  class specifically: a precommit is not a committed ref, and a later byte fetch publishes the
  same ref name over it without conflict.

### 6.2 S1 — STAGED {#s1-staged}

The receiver's `+1` is durable and nothing is published. Exactly one terminal operation is owed.
This is the state the re-offer interrogates, and it has three exits, one per answer of §4.2:

| Answer | Action |
| --- | --- |
| Yes | `promote` ⇒ S2 or S3 |
| No (authoritative) | `abort`, then byte-fetch from the same sender |
| Unknown | `abort`, then throw the retry-later `NETWORK_ERROR` |

- *No part loss?* No. On `No` the bytes are fetched; on `Unknown` the queue stores the exception,
  backs off and re-executes, recomputing source and covering-part discovery. For the three
  detached callers with no queue entry (`FETCH PARTITION`, `FETCH PART ... FROM`), the retry-later
  error surfaces to the user, who re-issues — and nothing replicated was expecting the part.
- *No double publish?* No. `abort` appends the exact precommit removal and no committed ref
  exists on either exit. `abort` never throws, by contract: it runs while the receiver's own
  retry-later error is already in flight.

### 6.3 S2 — PUBLISH UNCERTAIN {#s2-publish-uncertain}

`promote` returned `CaRelinkPromote::Unresolved`: the ref-log append was attempted and came back
without a verdict, so the receiver's ref MAY be committed.

**Action:** throw the retry-later `NETWORK_ERROR`. Never `nullptr`.

- *No part loss?* No — retry-later; by re-execution the ref lane has resolved the ambiguity.
- *No double publish?* No. Nothing is published on this exit, and the handle's abandon is
  REJECTED by the state machine if the promote in fact landed — a promoted binding is no longer a
  precommit — so no committed ref is ever undone here. **This is the one state where a byte fetch
  would be a defect**, because it would publish the part a second time over a relink that may
  already be committed.

### 6.4 S3 — COMMITTED {#s3-committed}

The receiver's ref is committed.

**Action:** return the relinked part; the usual `tmp-fetch_<part>` re-key follows —
`renameTempPartAndReplace` for the active path, `renameTo(detached/<part>)` for the detached one.
Both are ref repoints within one namespace, so a relinked part re-keys exactly as a byte-fetched
one does.

- *No part loss?* No.
- *No double publish?* No — `promote` is the handle's single terminal operation, the handle is
  released immediately after it, and a second call is rejected rather than re-driving a finished
  transaction.

### 6.5 What falls out of the taxonomy {#taxonomy-deltas}

Two whole classes of row disappear rather than move:

- **Token classes.** "The source sent no token", "the token did not decode", "the token routed to
  no mount / to several mounts" — there is no token.
- **Quiescence classes.** "The lane was busy" is no longer an outcome (§5.1).

And one row splits usefully: today's row 3 lumps `unproven`, an absent cookie, a transport failure
and a timeout into one action. Under §4.2 the authoritative `No` separates from the rest and gets
the byte fetch; everything else stays exactly one outcome.

## 7. The invariant {#the-invariant}

Recorded verbatim, because it is the sentence every future change to this path must be checked
against:

> Адопция переносит свидетельство живости: ресивер принимает n блобов силой одного сертификата
> committed-привязки; кто убирает сертификат — обязан вернуть по-блобные свидетельства.

*Adoption transfers liveness evidence: the receiver accepts n blobs on the strength of ONE
certificate of a committed binding; whoever removes that certificate is obliged to return
per-blob evidence.*

This is what the design is FOR, and it explains each of its otherwise-arbitrary rules. The
receiver never reads a blob body and never re-establishes any blob's liveness for itself; it
adopts n references on the strength of a single assertion about one ref row. The certificate is
therefore load-bearing in a way an ordinary read is not, which is why:

- it must be issued only while the sender is entitled to speak (the fence, hoisted to first);
- it must be issued only from a view that is not behind its own durable log (wedge, apply state);
- and it must be issued strictly AFTER the receiver's own `+1` is durable, so that the obligation
  it transfers is already discharged by an edge in the log at the moment the certificate is
  withdrawn.

**Layering, recorded so the next reviewer does not read a gap as a defect:** the confirm certifies
the sender's committed binding. The GC contract — v9-conditional — is what turns a committed
binding into blob liveness. A `Yes` means "the source still holds exactly this manifest right
now", which closes the handoff window and nothing more. These are layered, not defective.

## 8. What gets deleted {#deletions}

Line counts are measured against the current tree.

### 8.1 Shared surface {#deletions-shared}

| Deleted | Location | Lines |
| --- | --- | --- |
| `Service::resolveContentAddressedConfirm` (routing, "exactly one match", gate 0) | `DataPartsExchange.cpp:212-269` | 58 |
| `Service::answerContentAddressedConfirm` | `DataPartsExchange.cpp:271-300` | 30 |
| confirm dispatch in `processQuery` | `DataPartsExchange.cpp:304-312` | 9 |
| confirm wire names (5 constants + the `No`/`Unknown` conflation paragraph) | `DataPartsExchange.cpp:130-153` | 24 |
| the T2 confirm request block | `DataPartsExchange.cpp:1494-1553` | 60 |
| the seven-row failure taxonomy comment | `DataPartsExchange.cpp:1291-1387` | 97 |
| two protocol-version constants collapsed to one | `DataPartsExchange.cpp:94-108` | 15 |
| `Service` confirm declarations | `DataPartsExchange.h:55-74` | 20 |
| `enum class CasConfirmAnswer` opaque forward declaration | `DataPartsExchange.h:33-36` | 4 |

Replaced by: an identity-only answer branch (≈35), two wire names (≈12), one protocol-version
constant (≈5), the re-offer request block (≈50), the four-state machine comment (≈55), one
`Service` declaration (≈8). **Net: 703 → ≈556 CA-attributable lines in the two shared files, −147
(−21%).**

### 8.2 Content-addressed surface {#deletions-ca}

| Deleted | Location | Lines |
| --- | --- | --- |
| the whole relink-token codec (percent-encoding, strict decode, version tag, field caps) | `ContentAddressedExchange.cpp` — entire file | 170 |
| `CasConfirmAnswer`, `CasRelinkSourceToken`, both codec declarations, `ownsNamespace`, `confirmExactRef` | `ContentAddressedExchange.h` | 68 |
| `ContentAddressedMetadataStorage::ownsNamespace` + `::confirmExactRef` | `ContentAddressedMetadataStorage.cpp:1934-1998` | 65 |
| `CasRefLedger::confirmExactRef` — the six rules, the two-mutex snapshot, the `try_to_lock` rationale | `CasRefLedger.cpp:375-481` | 107 |
| `Cas::ConfirmAnswer` + its declaration | `CasRefLedger.h` | ≈40 |

Replaced by an identity-only mode on `getRelinkOffer` (≈18), a peer-facing resolve on the ledger
(≈45) and their declarations (≈31). **Net ≈ −369.** Combined with §8.1: **≈ −516 lines**, plus
roughly 250 lines of tests that become moot (§11).

### 8.3 Concepts that stop crossing the seam {#deletions-concepts}

This is the measure that matters for the house rule, and it is the strongest result of the
design.

| Content-addressed name appearing in `src/Storages/MergeTree/` | Before | After |
| --- | --- | --- |
| `IContentAddressedExchange` | ✓ | ✓ |
| `CasConfirmAnswer` (enum, opaque-declared in the shared header) | ✓ | — |
| `CasRelinkSourceToken` (struct) | ✓ | — |
| `encodeCasRelinkSourceToken` / `decodeCasRelinkSourceToken` | ✓ | — |
| `ICaPreparedRelink`, `CaRelinkPrepare`, `CaRelinkPromote` | ✓ | ✓ |

**5 → 2.** The shared replication path stops naming any content-addressed *type* except the two
narrow interfaces it necessarily owns a window on. It never again decodes, routes, range-checks
or disambiguates a content-addressed identifier.

Interface methods on the seam that exist only for the confirm also go: `confirmExactRef` and
`ownsNamespace`. The latter has exactly one non-test caller in the tree — the confirm's routing at
`DataPartsExchange.cpp:236` — so it dies with the endpoint.

## 9. Observability {#observability}

Three gaps, all of them causes of the diagnostic cost recorded in §2.

**The transient-`Unknown` message.** USER DIRECTIVE: an `Unknown` that is our own uncertainty and
part of the protocol must not present as an error. The receiver's abandon drops from ERROR to
INFO, and the message names a transient state rather than a failure:

> the source did not answer the relink re-offer for part `<P>`; this is an expected, transient
> outcome while the source is unable to speak for the namespace, and the fetch is re-queued and
> will retry

**Counters.** `ProfileEvents` pairs so the storm becomes a metric instead of ~5–9k log lines per
minute at ~23 frames of unwinding each. None of these exist today — the tree has no relink or
confirm counter at all.

| Counter | Meaning |
| --- | --- |
| `CasRelinkProven` | re-offer returned an equal identity; the promote is authorized |
| `CasRelinkRefused` | authoritative `No` — the binding changed or is gone; the receiver byte-fetches |
| `CasRelinkUnknown` | the sender could not speak; retry-later |
| `CasRelinkRecoveryBudgetRefused` | **its own counter**, per the decision: an `Unknown` caused by the peer-initiated recovery budget, distinguishable from every other `Unknown` |

**Which rule refused.** Today an `Unknown` maps silently through
`ContentAddressedMetadataStorage.cpp:1996`, and diagnosing rule 3 took a live cluster. The
sender-side `LOG_DEBUG` must name the rule that produced the refusal (fence / unrecovered /
wedge / poison / budget) — the answer stays binary on the wire, the diagnosis stays where the
rule that produced it can be named.

## 10. Measurements {#measurements}

In priority order; the first is decisive for whether §4.3's budget is insurance or structure.

1. **Share of re-offers hitting a COLD table.** Prior expectation: steady-state confirms are warm
   almost by definition — the sender of a part being fetched is writing that namespace. Cold
   bursts should cluster around node recovery, which is exactly when the sender is busiest, which
   is why the budget exists at all. If the cold share is non-trivial in steady state, the budget
   is load-bearing and both mitigations of §4.3 must ship together.
2. **How often the identity actually changes between offer and re-offer** — i.e. how often the
   answer can even be `No`. Calibrates how much machinery the authoritative-`No` path deserves.
3. Relink width distribution (blobs adopted per relink).
4. Proven-path cost share.
5. Post-fix re-offer rate, against the ~17% availability baseline.

## 11. Test matrix {#test-matrix}

USER DIRECTIVE: **the unhappy path gets its own precise test.** Each refusal class is driven
deliberately, and each asserts four things — the severity, that the message names a transient
state, that no byte re-request goes to the same source on the `Unknown` exits, and that the retry
cycle converges.

Rebuilt from the state machine of §6, not transplanted from the old row list:

| # | Class | Driven by | Asserts |
| --- | --- | --- | --- |
| 1 | S1 → Yes | ordinary busy writer, sustained inserts | promote; **availability is not degraded by a busy lane** — the direct regression test for §2 |
| 2 | S1 → No, repointed | repoint the source ref inside the pause window | authoritative `No`; byte fetch to the SAME sender; part converges |
| 3 | S1 → No, absent | drop/merge the source part inside the pause window | authoritative `No`; no promote; no double publish |
| 4 | S1 → Unknown, fence lost | revoke the sender's mount fence | absent cookie; INFO severity; transient-state message; retry-later; **no byte re-request** |
| 5 | S1 → Unknown, wedge | wedge the sender's append lane | as 4 |
| 6 | S1 → Unknown, poisoned | poison the sender's apply state | as 4 |
| 7 | S1 → Unknown, budget | exhaust the peer-initiated recovery budget | as 4, plus `CasRelinkRecoveryBudgetRefused` increments and no other `Unknown` counter does |
| 8 | S0 | `cas_relink_receiver_force_mechanism_failure` | byte fetch; recursion brake bounds it to one attempt |
| 9 | S2 | promote forced to `Unresolved` | retry-later; **never** a byte fetch |
| 10 | Cross-pool | receiver in a different pool | no offer; bytes; unchanged |
| 11 | Version mix | peer advertising the pre-redesign version | no identity cookie ⇒ S0 ⇒ bytes |
| 12 | Sender-side answer contract | direct ledger test | discretionary maintenance is NOT scheduled by a peer-initiated read; recovery IS |

The existing failpoint `cas_relink_receiver_pause_before_confirm` is the seam that opens the T1→T2
window and stays (renamed for accuracy); it is what makes 2–7 reachable at all, since none of them
is reachable from configuration.

Existing tests in `gtest_cas_confirm_exact_ref.cpp`, and what happens to each group:

| Group | Lines | Fate |
| --- | --- | --- |
| token round-trip + malformed/overlong token battery | 747-877 | **delete** — the codec no longer exists |
| `OwnsNamespaceSelectsTheMountByServerRootInEveryLifecycleState` | 878-919 | **delete** — the predicate no longer exists |
| lane-quiescence refusals (`InFlightAppendIsUnknown`, `MidTenureChunkBoundaryIsUnknown`) | 443-559 | **delete** — they assert the behaviour this design removes; row 1 of the matrix above is their inverse |
| `ColdTableIsUnknownWithZeroBackendRequests`, `EvictedTableIsUnknownWithZeroBackendRequests` | 330-380 | **rewrite** — the answer becomes "recovers and answers", and the zero-request assertion moves to the budget-refused path (row 7) |
| exact-match / repoint / drop-and-recreate, wedge, poison, lost fence, snapshot ordering | 263-329, 560-686 | **keep**, re-pointed at the new entry point — these are rules 1, 3, 4 and 5, which survive |

≈250 lines are deleted outright; the surviving rule tests are the ones worth keeping, and they
are the ones this design does not change.

## 12. TLA obligations {#tla}

The model is `docs/superpowers/models/CaRelinkConfirmCore.tla` (436 lines), with twelve
configurations: `_main`, `_main2r`, five `_sab_*` sabotages, four `_witness_*`, and
`_sab_holeylist`. `_main` checks `TypeOK`, `GraduationIsPhased`, `ConfirmedRelinkNeverDangles` and
`PromotedNeverDangles`. This design changes what the model is responsible for, what it must be
refined to represent, and what it must be re-derived against.

### 12.1 Dangle-freedom is reassigned {#tla-dangle-reassign}

`_sab_holeylist` already demonstrates that with **every confirm rule intact** and exactly one
incomplete listing page permitted (`MaxHoles = 1`), `ConfirmedRelinkNeverDangles` still breaks —
BACKLOG `{#list-as-journal-dataloss-2026-07-25}`. Its own header says it: *"The confirm protocol
cannot repair this; it is an independent release-blocker."* That is the model telling us the
property was never the confirm's to prove. It is reassigned to the v9 chain models, where listing
completeness is the subject, and the confirm model keeps only the custody chain.

### 12.2 The named assumption {#tla-named-assumption}

The assumption exists today, but as a model PARAMETER with a prose warning rather than a name:
`_main.cfg` sets `MaxHoles = 0` and its comment concedes *"That assumption is NOT established by
the shipped code."* An unnamed assumption is one nobody notices going vacuous. Give it a name:

> ASSUME `CommittedEdgesAreGcVisible`: a committed ref edge that is durable before a removal is
> appended is observed by every GC fold that observes the removal. (Today: `MaxHoles = 0`.)

Discharged by the v9 chain models. Naming it here is what makes weakening those models break this
proof **visibly** instead of silently.

### 12.3 REQUIRED REFINEMENT: the apply-pending window {#tla-apply-pending}

**The model as written refutes §5.1's first-pass argument, and the refutation is correct.** In the
model, `SenderDurable` (`:180-189`) makes a transaction durable while leaving `sCacheRef`
un-updated, and the ONLY predicate that refuses in that state is rule 3:

```tla
quiescent == SabotageStaleCache \/ (~sPending /\ ~sLeader)   \* rule 3
```

`sPoison` is set only by `SenderApply`'s sibling `SenderPoison` — the apply-THREW case — so the
model has no representation of the code's `ApplyPending` marker at all. Consequently
`_sab_stalecache` (drop rule 3) MUST produce a counterexample, and does. Read naively, that says
this design is unsound.

It is not, but only because of the code fact the model does not represent, and only once
§5.1.2's synchronization fix lands. The model must therefore be refined **before** the design is
implementable:

1. Add an `sApplyPending` variable, armed by a step ordered strictly before `SenderDurable` and
   cleared atomically with `SenderApply`'s row update — mirroring arm-before-`PUT` and
   clear-inside-the-install.
2. Split the sabotage in two, which is the whole point of the refinement:
   - `_sab_noapplypending` (drop the marker) **must** still produce the stale-cache
     counterexample — that is the guard doing real work;
   - `_sab_stalecache` (drop `~sPending /\ ~sLeader`, marker intact) **must now pass** — that is
     the proof that the terms this design deletes were redundant.
3. If (2)'s second half does not pass, the design is wrong and must not be implemented. **This is
   the gate.**

### 12.4 Re-derivation against the new rule set {#tla-rederivation}

Beyond the refinement: the fence moves first (`Gate1Answer` currently folds rules 3, 4 and 6 into
one `unknown` arm, so hoisting the fence changes which answer a fenced-out instance gives); `No`
becomes an authoritative outcome with its own successor action, and the model must show that the
byte fetch following a `No` cannot publish twice — today `RAbort` is the only non-`yes` successor
and it simply releases. The witnesses `_witness_confirmno` and `_witness_confirmunknown` must be
re-derived accordingly, since the states they prove reachable are no longer the same states.

Gate 0 and rule 2 (warm/recovered) are recorded as out of scope in the current model. Rule 2's
exclusion needs revisiting: this design makes recovery MANDATORY on the answer path (§4.3), so
"no half-recovered view is observable" stops being an assumption about a path nobody takes and
becomes an assumption about the path every cold answer takes.

## 13. Migration {#migration}

**Zero compatibility scaffolding**, per the standing pre-release rule: the content-addressed
subsystem has no released build and no persisted data to be compatible with, so no dual-path code
is written and no token is kept alive for old peers.

The replication protocol version does all the work. `..._WITH_CA_CONFIRM = 11` is replaced by a
single `..._WITH_CA_RELINK = 12`, and the two pre-release constants 10 and 11 are collapsed into
one line of historical note, because no released build ever advertised them:

- a v12 sender offers a relink only to a receiver advertising ≥ 12;
- a v12 receiver talking to an older sender receives an offer with no identity cookie, which is
  state S0 — the capability gate — and byte-fetches;
- an older receiver never advertises ≥ 12, so it is never offered a relink.

Mixed versions degrade to bytes in both directions, never to an unconfirmed relink. This is
exactly the property the current gate has, achieved with one constant instead of three.

## 14. Rejected alternatives {#rejected}

**(i) The per-ref `MutationScope` index.** `RefMutationItem.build_ops` is deferred, but
`MutationScope` (`CasRefProtocol.h:39`) already names each pending mutation's target at admission
(`Kind::Ref` with a `ref_name`, or `Kind::WholeShard`), so rule 3's table-scoped terms could have
been narrowed to "an item scoped to THIS ref, or any `WholeShard` item". Retired pre-ship, not
shipped as a stopgap: it adds an index to the very protocol surface both reviews marked worst on
complexity-per-guarantee — a concept the PR series would carry only to delete two steps later.
That is patch accretion where the answer was invariant rediscovery. Affordable to skip because
the storm is an availability and waste cost, not a correctness one (unproven ⇒ retry-later).
Mechanically (i) could live inside (ii) as a warm fast path; also rejected, because it resurrects
the index and eats (ii)'s surface-deletion win — which is the whole point of (ii).

**(iii) Receiver-pays verification.** The receiver could verify by reading the sender's ref state
directly from the shared pool: `recoverRefTableDetailed` is a free function and the offer names
the namespace and ref. Recorded as rejected so it is not re-proposed, but the axis it clarifies is
worth remembering — **who pays**. Under (iii) the DoS concern of §4.3 vanishes as a class: the
beneficiary pays, and an abusive receiver harms only itself. It was rejected on cost — a `LIST`
plus replay per verification — and because it inherits the listing-trust questions the v9 chain
exists to close, which is precisely the property §12 has just reassigned to those models.

## 15. Open questions {#open-questions}

1. **The apply-marker synchronization (§5.1.2) touches the append hot path — is that acceptable,
   and is the mutex the right instrument?** The alternative is to keep a cheap synchronized
   in-flight indicator instead of promoting the marker: retain `leader_active` in the answer but
   NOT `pending`, which costs a `ref_queue_mutex` acquire on the answer path and leaves a wider
   refusal window than the design targets (a tenure is longer than a chunk). Recommendation is the
   marker fix, because it removes a conflation rather than trimming one; but it is a change to
   code this design otherwise does not touch, and that deserves an explicit decision. Related
   question: should `armApplyPending` keep its `noexcept` allocation-free contract by taking the
   lock at the call site rather than inside itself?
2. **Split the authoritative `No`?** §4.4 sends both `No` shapes to a byte fetch. The wire already
   distinguishes them, so routing `none` (the sender has no such ref) to retry-later instead —
   letting the queue re-select a source rather than issuing a byte request that will fail with
   `NO_SUCH_DATA_PART` — is free in wire terms and costs one branch on the receiver. It saves one
   doomed request in a case whose frequency measurement 2 will tell us. Ship the simple version
   and revisit, or take the branch now?
3. **Budget shape and default.** §4.3 recommends a pool-global concurrency cap plus the LRU
   non-promotion. Is a rate ceiling (a leaky bucket) needed in addition, or does concurrency plus
   non-promotion bound it adequately? Default value?
4. **Config knob, or none at all?** `ref_table_cache_bytes` is currently a struct default
   (`CasPool.h:221`) with no XML plumbing. Recommendation is global-first — one pool-wide setting
   — but if the budget turns out to be theoretical insurance (measurement 1), a hard-coded
   constant with no operator surface is one fewer knob to document and support. Which?
5. **Does the re-offer keep the `assertEOF` strictness?** The identity-only response has an empty
   body by construction, exactly as the confirm's did, so requiring EOF before reading the cookie
   keeps a misrouted or desynchronized reply "unproven" rather than half-parsed. Confirm this is
   wanted on the re-offer path too.
6. **Naming.** `content_addressed_relink_identity_only` / `content_addressed_manifest_id` are
   descriptive but long. Preference?
7. **Does the identity-only branch dispatch before or after `findPart`?** This spec dispatches it
   early (as the confirm is today at `DataPartsExchange.cpp:308`) so a vanished part can be
   ANSWERED with `none` rather than thrown as `NO_SUCH_DATA_PART` — preserving "a question is
   answered, never thrown". Worth confirming that is the preferred shape, since it is the one
   place the re-offer is not literally an ordinary fetch request.

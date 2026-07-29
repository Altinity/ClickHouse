---
description: 'Design: the CAS fetch-by-relink confirm is re-expressed as a second, identity-only OFFER against the same endpoint, carrying a strong digest over the mount- and namespace-qualified binding in private headers rather than HTTP conditional ones. The separate confirm action, its six-field source token and its percent-encoding codec, the `CasConfirmAnswer` enum crossing the replication seam, the namespace-routing predicate and the lane-quiescence refusal all disappear; the mount fence moves first so a `No` becomes authoritative knowledge.'
sidebar_label: 'CAS relink re-offer redesign'
sidebar_position: 20260729
slug: /superpowers/specs/cas-relink-reoffer-redesign
title: 'CAS relink re-offer: retiring the confirm endpoint'
doc_type: 'reference'
---

# CAS relink re-offer: retiring the confirm endpoint {#cas-relink-reoffer-redesign}

**Date:** 2026-07-29. **Status:** DESIGN (**v9 — final pre-plan**), approved except the TLA gate. **Branch:** `cas-gc-rebuild`.
Spec only; no code landed.

**v2:** the confirm is expressed as an HTTP conditional request (`ETag` / `If-None-Match`), with
the two candidate forms reconciled in §4.1.6; the simplification claim is quantified in §1.1;
every mechanism names what it deletes or which obligation it discharges; the §5.1.2 marker fix is
scoped and sized.

**v3 (Codex round 1 — 2 blocker, 6 major, 1 minor):** the validator becomes a
**mount-and-namespace-qualified digest**, because a bare `ManifestRef` is not an identity
(`CasTypes.h:131-134`) and a same-pool disk move could otherwise yield a false `Yes` (§3, §4.1);
a **per-confirm nonce** closes response replay structurally where `no-store` was only policy
(§4.1.4); the **certified answer becomes an explicit header** so a stripped mode flag cannot make
an ungated offer look affirmative, with `assertEOF` promoted from an open question to a normative
second defence (§4.1.3); the **budget is redesigned** after its one-line mitigation was refuted by
the eviction code (§4.3); `CaRelinkPromote::MechanismFallbackAllowed` gets its transition (§6.2);
the TLA gate gains three expressiveness requirements (§12.5); and the §1.1 arithmetic is corrected
against a grep rather than against the design.

**v9 — THE WRITE-RELEASE SEAM IS EXTRACTED (user direction).** Everything §6.5 carried about
`PartWriteTxn` / `PreparedPartWrite` / caller-guard ownership now lives in its own document,
`2026-07-29-cas-part-write-release-seam.md`, together with the apply-marker prerequisite that was §5.1.2. The reason is the reason those
sections kept failing review: they were repairing a general part-write seam inside a protocol spec,
so each fix arrived as contract mass in a document about something else. Their hazards — unproven
releases, premature ERROR lines, nine scattered no-send exits — belong to EVERY part write, which
this spec's own counter generalization had already conceded. **Relink now CONSUMES a contract
instead of defining one:** §6.5 is a reference, §5.1.2 is a reference, and the seam's test rows move
out, leaving §11 with only rows that exercise relink behaviour.

**v9 content, unchanged by the move (Codex round 6 — 0 blocker, 2 major; the v8 sweep verified closed).** Both majors were depth
on v8's routes, and both changed the mechanism rather than the wording. **The proof channel is
inverted:** v8 asked each refusal site to prove it sent nothing, which is not implementable — the
proof is erased into a generic `NETWORK_ERROR` before `precommitAdd` can read it
(`CasRefLedger.cpp:3105-3126`), `RefMutationItem` carries no such field
(`CasRefLedger.h:454-463`), and there are NINE no-send exits, not two. v9 marks the single
TRANSMISSION point instead: `RefMutationItem` gains `attempted`, set beside `armApplyPending`
(`:2806`) immediately before the PUT, so all nine exits prove themselves by never reaching it, and
the one exit that is marked-but-unsent is cleared by the existing hardened
`unresolvedProvesNothingWasSent`. One set-site, one clear-site, one read-site, with a nine-row
mapping table in §6.5. **The log fix widened:** moving the destructor's ERROR line is necessary but
not sufficient — three more sites log before the final attempt. They are DEMOTED to INFO with
transient wording rather than deferred, because "this attempt failed" is true when written and only
the severity is wrong. Per-site, and they do not all get the same answer: two demote, and
`logCasWriteRetryLater` stays untouched because it is the generic CAS-write retry line, not a
release claim — which in turn NARROWS the settled-late assertion to the authoritative message class
rather than to severity globally.

**v8 (Codex round 5 — 1 blocker, 2 major; the v7 inversion HELD).** The blocker was textual: v6's
plumbing was still normatively REQUIRED in prose §6.5 had already deleted, so an implementer faced
two contradictory contracts. **Swept, by grep not memory** — six passages corrected: §6.1's "TWO
entries need a release guard" block and its two obligation bullets, §6.2's promote-table cell, its
"an earlier draft read this edge as proof" paragraph, its "Required: consult `isTerminal`"
paragraph, and §15's implementation-detail line. The only surviving mentions of the carrier, the
observation point and `isTerminal`-as-release-bit are in this changelog and in §6.5's own record of
what died. **Two refinements:** `Uncertain` is set BEFORE the append
(`CasPartWriteTxn.cpp:965-971`), so it is entered even when a refusal transmitted nothing — fixed
at the source by downgrading to `NotAttempted` on PROVEN non-transmission, reusing the existing
hardened `unresolvedProvesNothingWasSent` predicate rather than documenting a false-positive class;
and the destructor's ERROR line currently precedes its final release attempt, so it must move after
it, both halves of the emission firing only on a failed retry. Test rows 22 and 23 pin both.

**v7 (Codex round 4, §6-targeted — 2 blocker, 3 major, 1 minor, ALL in the §6.5 that v6
introduced).** Re-derived rather than patched, on the house rule that review rounds which keep
adding object kinds mean the invariant is wrong. **What DIED:** the `RELEASE-INCOMPLETE` exit
attribute as a concept; the carrier struct and out-parameter; the observation-point rule against
the retry chain; the `isTerminal` release guard (it was never a release bit — it is also set right
after a durable commit); every release-result parameter on the exchange seam; the receiver's
involvement in release reporting entirely; the `LeakedLivePrecommit` fsck class and its windowed
discriminator (whose premise was false besides — `retireBuildSeq` also runs on the successful
promote and abandon paths); and §6.5.1/§6.5.2 with them. **What replaced it:** one predicate in one
place — at destruction, emit iff `PartWriteTxn::precommitState` is `Uncertain` or `Durable`
(`CasPartWriteTxn.h:280-299`), a state the transaction already maintains for exactly this question.
Also: S0 renamed to PUBLICATION PROVEN ABSENT (v6's "NOT STAGED" was wrong in precisely the way
§6.5 is about), the counter generalized to `CasPrecommitReleaseUnproven` and re-scoped as an upper
bound rather than a leak count, and the retention bound corrected from "the mount's epoch" to
"eligible at epoch rollover, reclaimed by a successor's sweep".

**v6 (Codex round 3, targeted at §6 — 2 blocker, 2 major, 1 minor):** release-incomplete is
demoted from a state to an **orthogonal exit attribute** (§6.5) — as a state its single byte-fetch
action would have permitted the double publish §6.3 forbids and would have discarded S1's
`Unknown`; the staging path gets the same release guard as the promote path, and it is the one with
**no destructor retry at all** (§6.1); the release-result contract is pinned down so it is
implementable — carrier, observation point, which outcomes carry it, emission points (§6.5.1); and
the fsck backstop becomes an actionable `LeakedLivePrecommit` class with an exact recognition rule
(§6.5.2). *Recording the process lesson too: this round exists because v5's "it resolved into the
existing structure" was offered as a reason to SKIP review. That is precisely when review is worth
most.*

**v5 (Codex round 2 — 0 blocker, 5 major, 3 minor):** the `MechanismFallbackAllowed` → S0 edge is
now **guarded by `isTerminal`**, because a failed release leaves a live-epoch precommit that
nothing reclaims — and that find exposed a **third proof obligation**, *does it leak retention?*,
now answered by every state; the TLA section stops claiming the encoding is
state-space-neutral and names a `FreshCertifiedResponse` assumption instead (§12.2); §12.5(i) now
requires equal-namespace/different-disk mounts and a `_sab_nodiskqualification` sabotage; the nonce
paragraph, test row 15, the stale LRU sentence and the endpoint/grammar counts are corrected.

**v4 (final user rulings):** the apply-marker fix is **approved**; the recovery budget is cut to
**minimal protection** — a per-recovery work cap plus a hard-coded concurrency limit — after the
three-bound design was rejected as overengineering against the warm-by-construction argument
(§4.3, §14); **no configuration knob** (§4.3.1); wire names settled on the endpoint's existing
`content_addressed_*` family (§4.1); and §9 now requires the sender to log the hashed COMPONENTS
beside the digests, since a digest says *mismatch* but not *which side moved*. §15 has no blocking
questions left.

**v3.1 (user directions):** the 304/503 status mapping is **decided against** — the value encoding
stands, so the answer always rides in an explicit header value on a 200 (§4.1.6). And the standard
conditional vocabulary goes with it: `ETag`/`If-None-Match` are replaced by **private
private `content_addressed_*` request parameters and response cookies**, because a standard conditional header does not merely describe our
intent, it *invites* an RFC-conforming intermediary to evaluate the condition and answer 304 from
its own copy — the forged-certificate shape arriving through a conforming component (§4.1.1). That
reverses this design's own earlier verdict on axis 1 of §4.1.6, and the reversal is recorded there
with its reasoning intact.

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
`part` parameter, same advertised pool identity — **made conditional on the binding it adopted**.
The offer carries a strong validator — a digest over the binding QUALIFIED by its mount and
namespace, because a bare `ManifestRef` is not an identity; the confirm is that same request back
carrying that validator as the receiver's expectation, plus a fresh replay nonce. The headers are
private rather than HTTP's conditional ones, so no intermediary can join in (§4.1.1). The sender answers from its ordinary committed view. Everything that existed only to carry
a question
without its context — the token, its codec, the routing predicate, the parts-set proof gate, the
answer enum crossing the replication seam — is deleted, and because the mount fence is evaluated
FIRST rather than last, a `No` becomes authoritative knowledge the receiver can act on instead of
a refusal it must retry.

**Two gates, stated up front rather than discovered late. The first is approved; the second is
still open and can still stop this design:**

1. **The apply-pending marker must be made synchronized before the lane-quiescence refusal can be
   removed** (§5.1.2, specified in `2026-07-29-cas-part-write-release-seam.md` §6) — **APPROVED (user, 2026-07-29); ships as specced.** The refusal this design
   deletes is currently the *properly locked* guard on the durable-but-unapplied window; the marker
   meant to replace it is a relaxed atomic written outside any lock. This is a real change to the
   ref-lane append path, not to the relink path, which is why it was raised as a gate — scoped at
   under 20 lines in one file with no control-flow change.
2. **The TLA model must be refined and must then pass** (§12.3, §12.5). As it stands the model
   refutes this design — correctly, given what it represents. The refinement is what distinguishes
   "the deleted terms were redundant" from "the deleted terms were load-bearing", and if the
   refined `_sab_stalecache` does not pass, the design is wrong and must not be implemented. The
   refinement is not only that split: the model must also be able to EXPRESS a cross-mount
   validator collision and a multi-transaction tenure, or it can go green while the wire is unsafe
   (§12.5).

### 1.1 The arithmetic {#arithmetic}

This is a simplification effort, so the claim is stated as a number before anything else is
argued. Deletions are measured against the tree; additions are estimated.

| | Before | After |
| --- | --- | --- |
| Receiver outcomes to reason about | 7 taxonomy rows | **4 states** — release observation is not a state at all (§6.5) |
| Registered interserver endpoints | 1 | **1** — unchanged; the confirm was always a MODE of the parts endpoint, never a second one |
| What the second mode costs to serve | its own token routing, parts-set gate and answer path | the offer's routing with the body suppressed |
| Fields the receiver must round-trip | 6, parsed | **16 opaque bytes**, parsed by nobody |
| Bespoke wire names | 5 | **5** — unchanged, and §4.1 says so plainly |
| Proof obligations answered per outcome | 2 | **3** — codex r2 exposed a missing one, *does it leak retention?* |
| Where release cleanup is observed | implicit, nowhere | **1 place** — the handle's destructor, one predicate (§6.5) |
| Content-addressed identifiers in `src/Storages/MergeTree/` | **6** | **4** |
| Seam methods serving the confirm | `confirmExactRef` + `ownsNamespace` | **0** |
| Lane mutexes held to answer | 2, in a bespoke snapshot | **1**, the ordinary read path |
| Reversible codecs on the path | 1 (~170 lines) | **0** |
| Wire grammars to parse and defend | **1** — the token codec's | **0** — every value compared byte-wise, none parsed |

| Lines | Deleted | Added |
| --- | --- | --- |
| Shared (`DataPartsExchange.cpp`/`.h`) | 317 | ≈181 |
| Content-addressed (incl. the **~170-line token codec, whole file**) | 450 | ≈124 |
| Apply-marker synchronization (§5.1.2) | — | **≈20, no control-flow change** |
| **Net production** | | **≈−440** |
| Tests made moot / added (§11) | ≈250 | ≈80 |
| **Net overall** | | **≈−610** |

Two honesty notes, because both numbers moved after review and a spec whose arithmetic drifts is
worse than one with no arithmetic. **(1)** The identifier row is 6→4, not the 5→2 an earlier draft
claimed: `CasRelinkSourceToken` and `encodeCasRelinkSourceToken` never appear in the shared tree
at all (only the decoder does), so the earlier table counted a concept that was not there. The
four that remain after are `IContentAddressedExchange`, `ICaPreparedRelink`, `CaRelinkPrepare`,
`CaRelinkPromote`; the two that go are `CasConfirmAnswer` and `decodeCasRelinkSourceToken`.
**(2)** The added column moved twice. Codex round 1 grew it — the recovery budget became a
~100-line three-bound design — and the user then rejected that as overengineering (§14), leaving
~25 lines of minimal protection. The net deletion is better for it, and the episode is the reason
§4.3 now leads with why a confirm is warm rather than with machinery.

Everything else either removes something or discharges a stated obligation, named in one sentence
where it is introduced: §4.1 (the digest — deletes the codec), §4.1.3 (the certified answer —
offer/confirm confusability), §4.1.4 (the nonce — replay), §4.1.5 (`no-store` — cache forgery),
§4.2 (fence-first — deletes the `No`-is-not-knowledge doctrine), §4.3 (the budget — the DoS
obligation), §5.1.2 (marker synchronization — the precondition for the availability fix).

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

**Give the question its part context back and the fields stop being ROUTING.** A request that
carries `part` and `endpoint` — which is to say, an ordinary fetch request — lets the sender
resolve the part exactly as the offer did, through the same router every other read and write of
that part uses. Nothing is routed from a peer-supplied string, so nothing needs to be
range-checked, disambiguated, or defended as untrusted input, and the "exactly one match or
`Unknown`" rule disappears with the routing it existed to police.

**They do NOT evaporate as QUALIFICATION, and an earlier draft of this design was wrong to say so.**
`CasTypes.h:131-134` states the rule in the codebase's own words:

> Two namespaces may legally carry the same `ManifestRef` tuple without addressing the same
> object; therefore those structures must use `ManifestId`, never `ManifestRef` alone.

A bare `ManifestRef` is not an identity. Worse, a namespace does not disambiguate a MOUNT either:
a namespace is `<server_root_id>/store/<u3>/<uuid>@cas@`, so two content-addressed disks
configured with the same `server_root_id`, in the same pool, serving the same table, produce the
IDENTICAL namespace string — which is precisely why `resolveContentAddressedConfirm` demands
*exactly one* matching mount and answers `Unknown` when several match. So a part that moves between
same-pool disks (`MOVE ... TO DISK`) between offer and confirm can be answered for by a mount that
never made the offer, and a bare tuple that happens to collide would be a false `Yes` over blobs
nobody is protecting.

So the qualification survives — **it just stops being parsed.** The comparison value becomes a
one-way digest over the qualified tuple (§4.1): the sender recomputes it from its own state and
never reads a field back out of it, so the strict reversible codec, the charset problem and the
untrusted-parse obligation all go, while the identity the token's fields were carrying stays
exactly as strong. The honest summary is not "six fields evaporate" but **"six parsed fields
become sixteen opaque bytes nobody parses."**

## 4. The mechanism {#mechanism}

### 4.1 Wire protocol: a conditional request in FORM, in a private vocabulary {#wire-protocol}

**The confirm keeps the conditional-request FORM and drops the HTTP conditional VOCABULARY.** The
offer carries a strong validator for the binding it is offering; the confirm is the same request
back, carrying the receiver's expectation, and the sender answers against it. That shape — same
request, expectation on the wire, sender evaluates, receiver compares — is what replaces the
six-field token and its codec, and it is what makes precise mismatch logging possible. What the
shape does NOT need is `ETag` and `If-None-Match`, and §4.1.1 is why using them would be actively
harmful rather than merely ornamental.

Every name is ours, and they follow the channel and prefix this endpoint already uses rather than
introducing a second convention beside it: **request values are query parameters, response values
are cookies, everything is `content_addressed_`-prefixed** — exactly as
`content_addressed_pool_uuid` and `content_addressed_relink` are today. That also means no new
client plumbing: `getResponseCookie` is the accessor the receiver already calls.

| Name | Channel | Meaning |
| --- | --- | --- |
| `content_addressed_confirm` | request param | **Presence selects confirm mode** — answer about the binding, send no manifest body and no bytes — and its VALUE is the per-confirm nonce (§4.1.4). |
| `content_addressed_expected` | request param | The validator the receiver adopted at T1. |
| `content_addressed_identity` | response cookie | The sender's current validator for `part`. Set on **both** the offer and the confirm; absent on the confirm when there is no binding. |
| `content_addressed_answer` | response cookie | The CERTIFIED answer: `proven`, `changed`, or `unknown:<reason>`. Emitted **only** by the confirm path (§4.1.3). |
| `content_addressed_nonce` | response cookie | The nonce echoed back (§4.1.4). |
| `Cache-Control: no-store` | response | Pinned on both responses — see §4.1.5. |

**Five names, against five today — so the honest headline is that the name count does not move at
all.** It is worth saying plainly, because two earlier drafts of this section claimed a reduction
that does not survive counting. What changes is what the names CARRY. Today's five are an action
parameter, a six-field token needing a 170-line reversible codec, an answer cookie and two magic
values. The five above are each single-purpose and single-valued, and **not one of them is parsed
beyond a byte comparison**: the receiver compares `content_addressed_identity` to what it holds,
compares `content_addressed_answer` to the literal `proven`, compares the nonce to what it sent,
and logs anything else verbatim without interpreting it. Even `unknown:<reason>` is never split —
it is one opaque token that fails both equality tests and gets logged.

Choosing private names over `If-None-Match`/`ETag` also declines an inherited grammar: wildcards,
comma-separated lists and weak comparison are each a shape a sender would have to recognize and
refuse, and one of them (`*`) is a forged-certificate hole (§4.1.1). **Grammars on the path: one
today — the token codec's — down to zero.** Stated against the shipped tree, which has no
`If-None-Match` grammar in it; the standard-vocabulary draft would have ADDED one, and declining it
avoids an addition rather than removing something that exists. That, not the name count, is what
the codec deletion was always about.

**The validator is a digest over the QUALIFIED binding**, not over the `ManifestRef` alone — see
§3 for why the bare tuple is not an identity:

```text
validator = hex( digest( pool_uuid ‖ disk_name ‖ root_namespace ‖ ref_name ‖ manifest_ref_text ) )
content_addressed_identity=3f2a…c1
```

Each field is **length-prefixed** before hashing. That is not pedantry: without unambiguous
framing, `("ab","c")` and `("a","bc")` hash equal, and a namespace/ref pair that collides across
mounts is exactly the hazard the qualification exists to close. `disk_name` is in the tuple
because `root_namespace` does not identify a mount (§3): two same-`server_root_id` disks in one
pool serving one table share a namespace string, and `disk_name` is what
`resolveContentAddressedConfirm` already uses to tell them apart.

*What this deletes:* the strict percent-encoding bijection, its version tag, field caps,
control-character screening and strict inverse — ~170 lines whose entire job was to let a peer's
bytes be parsed back into fields. **Nothing parses the digest**, so none of it is needed. The cost
is ~5 lines: concatenate with length prefixes, hash, hex. A digest is not an encoding layer
returning by the back door; an encoding layer is reversible by definition and this is not.

*Digest choice:* a fixed 128-bit `XXH3_128` via the tree's existing system-header wrapper
(`Primitives/CasXxh3Streamer.h`, which exists precisely because a plain `#include <xxhash.h>`
resolves to lz4's bundled copy in the `dbms` target). **Fixed, not the pool's pluggable
`blob_hash`** — this is a protocol validator, not blob identity, and coupling it to a per-disk
configuration choice would make two disks in one pool unable to compare validators.
*Adversary model, stated because hash equality always needs one:* both endpoints are inside the
interserver authentication boundary and already extend each other ordinary `ReplicatedMergeTree`
fetch trust, so the threat is ACCIDENTAL collision, not forgery; at 128 bits over a handful of
in-flight relinks that is not a risk anyone can express. If the trust boundary ever changes, this
sentence is what must be revisited.

It is a **strong** validator in the sense that matters, and the design depends on that.
`precommitAdd` mint-tightening (superseded spec §A3) guarantees that every repoint and every
recreate mints a FRESH `ManifestRef`, so validator equality is exact identity with no ABA to
defend against. There is no weak form to forbid, because there is no `W/` prefix in a vocabulary
we define: the value is one hex token, compared byte-for-byte, and anything else fails to match.

The validator is **opaque to the receiver**: stored and compared byte-exactly, never parsed. That
removes an obligation rather than adding one — today the canonical form is parsed from untrusted
peer input on the sender (`tryParseManifestRef`, whose comment documents exactly that hazard).
Under this design no peer-supplied manifest reference is parsed by anyone.

The receiver's pool identity (`content_addressed_pool_uuid`) rides on the confirm exactly as it
rides on the offer, so the same-pool gate at `DataPartsExchange.cpp:410` applies unchanged.

#### 4.1.1 No standard conditional vocabulary — the decisive argument {#no-standard-conditional}

The confirm emits no `ETag` and no `Last-Modified`, and honours no `If-None-Match`,
`If-Modified-Since` or `If-Match`. This is the design's position, not an omission.

**A standard conditional request header does not merely describe our intent — it INVITES third
parties to act on it.** `If-None-Match` is defined so that any recipient holding a stored response
may evaluate the condition, and an RFC-conforming intermediary cache is entitled to answer **304
Not Modified out of its own copy without ever consulting the origin**. That is precisely the
forged-liveness-certificate shape of §4.1.5, arriving through a *conforming* intermediary rather
than a broken one — and it would land on the receiver as an instant exception besides, since
`assertResponseIsOk` (`src/IO/HTTPCommon.cpp:75-79`) does not accept 304. A header that no
intermediary recognizes engages no conditional machinery anywhere on the path. **The
active-participant cache risk therefore dies structurally**, and `Cache-Control: no-store` demotes
from load-bearing policy to a second line of defence (§4.1.5).

It costs nothing to give up, because the standard vocabulary was never going to do any work for us:
both legs are already `POST` (`DataPartsExchange.cpp:780` and `:1519`), and a POST response is not
cacheable without explicit freshness metadata we do not send. We would have been paying the risk of
standard semantics while receiving none of their benefit.

The private vocabulary also deletes three hazards outright rather than defending against them.
Under `If-None-Match` each of these needed a rule; under a single opaque token none of them exists:

- **`*`** — the wildcard means "if any representation exists", which would let a peer obtain an
  affirmative answer *without naming a binding at all*. There is no wildcard in our grammar.
- **A comma-separated list** — "one of these matched" is not this protocol's semantics. Our header
  carries one token; a value containing anything else simply fails to match.
- **`W/` weak comparison** — admits "semantically equivalent" representations, and this protocol
  has no notion of equivalence, only identity. There is no weak form to send.

**And time is rejected on its own merits**, so that nobody reintroduces it later under a private
name: **bindings change IDENTITY, not time.** The temptation is close at hand —
`RefCommittedRow` already carries a `published_at_ms` that `resolveRef` returns to every caller, so
a timestamp is one field access away. It must not be used. It would put clock trust into a decision
that depends on no clock today, and it cannot express the property the confirm establishes: two
manifests can be published within the same millisecond, and a binding repointed away and back would
look unmodified to any time comparison. Mint-tightening makes both cases impossible for an
identity validator, by construction.

#### 4.1.2 What the sender does with the condition {#sender-condition}

The sender evaluates `content_addressed_expected` itself, under the fence-first ordering of §4.2, and this
placement is deliberate: it keeps the comparison in the shape `confirmExactRef` already has (rule
5's exact equality plus rule 6's fence become the conditional evaluation), and it puts the
receiver's EXPECTED validator on the sender, where a mismatch can be logged as
expected-versus-actual by the node that knows which rule produced it. The receiver still compares
the returned `content_addressed_identity` too — that comparison is what it acts on — but the sender is no longer
answering a question it cannot see.

#### 4.1.3 The certified answer must not be confusable with an offer {#certified-answer}

The `content_addressed_identity` alone is not enough, and the reason is sharp: **the offer response carries the same
validator, and the offer path is not gated.** `getRelinkOffer` resolves through the ordinary read
path; it applies none of §4.2's fence, wedge or apply-state checks, because an offer is a
proposal, not a certificate. So if the identity-only parameter is stripped, ignored by an older
peer, or misdispatched, the sender answers with an ordinary OFFER — whose `content_addressed_identity` may well equal
the receiver's held validator — and a receiver comparing validators alone would read that as
`Yes` and promote on an ungated resolve.

Two independent defences, because this one is worth over-covering:

1. **STRUCTURAL — the certified answer is a different header.** `content_addressed_answer`
   is emitted only by the confirm path, after §4.2's ordering has run. An offer never carries it,
   so a stripped flag cannot produce one. The receiver requires `proven` and treats its absence as
   `Unknown`, never as an inference.
2. **NORMATIVE `assertEOF`.** An offer response carries a manifest body; a confirm response is
   empty by construction. Requiring EOF before acting rejects a full offer outright. **This is a
   requirement with a test (§11 row 15), not a preference** — an earlier draft carried it as an
   open question, which understated it.

This is also why the answer is EXPLICIT rather than inferred from a missing `content_addressed_identity`. Absence is a
weak signal: a stripped header, a misroute and a genuinely absent binding are three different
facts, and only the last of them is the sender speaking. The `unknown:<reason>` form carries the
refusing rule (`fence`, `unrecovered`, `wedge`, `poison`, `budget`, `no-such-part`) — **diagnostic
only, authorizing nothing**; only `proven` plus a matching validator authorizes, so a peer that
lies in the reason field changes nothing but its own logs.

#### 4.1.4 Replay defence: the per-confirm nonce {#replay-nonce}

`no-store` is policy, and policy does not bind a reverse proxy the receiver never configured. The
structural guard is a **nonce**: the receiver generates 16 random bytes per confirm and sends them
as the VALUE of `content_addressed_confirm` — the mode-selecting parameter, per the wire table in
§4.1, which is the single authoritative statement of what each name carries — and requires them
echoed in `content_addressed_nonce`. A mismatch or an absence is `Unknown`. The validator travels
separately, in `content_addressed_expected`; the two are never carried by the same name.

This closes the replay class that `no-store` only discourages: an intermediary replaying an
earlier identity response for the same URL — a response that may PREDATE T1, and therefore
certifies nothing about the window the protocol exists to cover — cannot carry this confirm's
nonce. Note why echoing the validator instead would not do: a replayed response for the same
binding would echo the same validator and pass, since the validator says *which* binding, never
*when*.

*What it costs:* one request-parameter VALUE — the parameter already had to exist to select the
mode, so this adds no name, the same reasoning the deleted design used for its own action parameter
("an action without its token is not a question anyone can answer") — and one response cookie.
*What it discharges:* the replay obligation, structurally. A side benefit falls out of putting it
in the query string: every confirm has a UNIQUE URL, so no cache keyed on a previous URL can serve
it at all.

#### 4.1.5 NEW RISK: a forged or replayed affirmative answer {#cache-risk}

An affirmative confirm produced by anything other than the live sender would be a **forged
liveness certificate**: the receiver would promote n blobs on the strength of an assertion no live
writer made, a direct violation of §7's invariant. Interserver traffic should not traverse shared
caches — but the spec must not rest on "should not".

The risk has two shapes needing different answers, which is why the defences below are not
interchangeable and none is redundant.

**ACTIVE participation — an intermediary that ANSWERS on the origin's behalf.** Dead structurally
(§4.1.1): with no standard conditional header there is no condition for a cache to evaluate, and
with no `ETag` there is nothing to validate against. A cache cannot forge an answer in a vocabulary
it does not know it is looking at. Three further reasons back this up, of which only the second is
policy: both legs are `POST` (`DataPartsExchange.cpp:780`, `:1519`) and a POST response is not
cacheable absent freshness metadata we never send; `Cache-Control: no-store` is pinned on both
responses — `no-store`, not `no-cache`, because `no-cache` permits storage with revalidation while
`no-store` forbids storage at all; and the interserver authentication path runs before the shared
handler dispatches to any endpoint.

**PASSIVE repetition — something REPLAYS a response the origin really did send, but earlier.** No
cache directive prevents this, because nothing needs to be cached: a stale-but-genuine response is
bit-for-bit valid, and one that predates T1 certifies nothing about the window the protocol exists
to cover. The nonce of §4.1.4 is the only defence that closes this shape, which is why it stays
even though the active shape is already structurally dead.

The test matrix asserts both: `no-store` on both responses (row 13) and replay rejection by nonce
mismatch (row 16).

#### 4.1.6 Reconciliation: the two candidate forms, and why this one {#status-variant}

Three forms were on the table, and they are cousins rather than rivals: **all carry an opaque
strong validator, and none trusts a timestamp.** An identity-cookie form (the sender states its
current binding in a response cookie; the receiver compares), a standard-conditional form (`ETag`
+ `If-None-Match`), and the private-header form that this design adopts. They differ on three
axes.

**Axis 1 — how the validator travels. The verdict here REVERSED during review, and the reason is
worth keeping.** The first argument was: a cookie is a name this codebase invents, documents,
versions and defends, whereas `ETag` is a name every HTTP stack, proxy, packet capture and
engineer already knows — so "one more custom mechanism versus one standard header" seemed to
decide it outright. That reasoning was right about recognizability and wrong about what
recognizability *does*. **On this path, being recognized means being ACTED ON**: an RFC-conforming
intermediary that understands `If-None-Match` is entitled to evaluate the condition itself and
answer 304 from its own stored copy, which is the forged-certificate shape of §4.1.5 arriving
through a conforming component rather than a broken one (§4.1.1 has the full argument, plus the
`assertResponseIsOk` consequence). A standard header is an invitation, and this is a protocol that
must invite nobody. The private-header form wins axis 1 — and it also deletes the wildcard,
list and weak-comparison hazards that `If-None-Match`'s grammar would have obliged us to refuse
one by one.

The honest cost, recorded because §1.1's arithmetic depends on it: going private costs **one more
bespoke name** than the standard-vocabulary draft (four instead of three). What it buys is that
the number of grammars we must parse and defend goes to zero. That is the better trade, but it is
a trade, not a free win.

**Axis 2 — who compares. Both, and that is not redundancy.** The receiver's comparison of the
returned identity is what it ACTS on, and it is load-bearing. The sender's evaluation of the
receiver's expectation is diagnostic, and it earns its place under a named obligation rather than as
polish: §9's requirement — a directive, because the rule-3 storm took a live cluster to diagnose —
is that a refusal be explainable, and expected-versus-actual can only be logged by a node that
knows both values. The header has to be on the wire anyway for the conditional form to exist, so
the sender either looks at it or wastes it. It costs one comparison.

**Axis 3 — the response mapping.** The natural full-HTTP spelling puts the three answers on status
codes: **304 Not Modified** = Yes, **200 with a different ETag** = authoritative No, **503 +
`Retry-After` + a reason** = Unknown, including budget-refused. It is a better-looking protocol,
it lands exactly on our three-answer semantics, and it expresses the transient-state directive in
standard vocabulary instead of a custom message class. It is nevertheless recorded as a variant
rather than the design, for a verified blocker and a risk.

One hypothesis to retire first: the status mapping does **not** delete more custom code from the
shared handler. On the sender both spellings are one line — set a header, or set a status. On the
receiver the validator spelling is a header read plus a string comparison, while the status
spelling needs exception handling for its success path plus a shared-IO change. It adds; it does
not subtract.

**The blocker is in the shared HTTP client.** `assertResponseIsOk` (`src/IO/HTTPCommon.cpp:71-90`)
accepts **only** 200, 201, 202, 206 and redirects; every other status is turned into a thrown
`HTTPException`. So under the status mapping, **304 and 503 both arrive as C++ exceptions** — the
receiver would have to catch and inspect `getHTTPStatus`, which is the pattern
`http_skip_not_found_url` already uses for 404 (`ReadWriteBufferFromHTTP.cpp:482`). Two
consequences:

- **The protocol's SUCCESS path becomes an exception.** The refusal storm this whole effort exists
  to remove costs ~23 symbolized frames per event (§2); making the *affirmative* answer pay that
  is the wrong direction, and it is the same directive — an expected protocol outcome must not
  present as an error — applied to the happy path.
- **Avoiding that means changing shared IO code.** 304 cannot simply be added to
  `assertResponseIsOk`'s global accept list: for an ordinary `url` read, which sends no
  conditional request, a 304 is a protocol violation and must stay an error. It would have to be
  an opt-in flag threaded through `ReadWriteBufferFromHTTP`'s constructor, its builder and
  `assertResponseIsOk` — three or four files of shared upstream IO, for an encoding whose benefit
  over §4.1 is aesthetic. That is a direct trade against goal 2.

**The risk** is §4.1.5's: a 304 is precisely what an intermediary synthesizes from its own stored
copy, so under the status mapping the affirmative answer is the one thing a cache can forge, and
`no-store` becomes load-bearing policy rather than a second line of defence.

**The risk compounds §4.1.1's.** A 304 is precisely what an intermediary synthesizes from its own
stored copy — and it can only be ASKED to do so if we also send the standard request header that
triggers the machinery. The status mapping and the standard vocabulary are therefore not
independent choices: taking either re-opens the active-participation shape §4.1.1 closes.

**DECIDED (user, 2026-07-29): the value encoding stands** — *"мне без разницы, сделать так как
будет проще всего"*, and this is the simplest: no shared-IO change, and no success path delivered
as an exception. The answer rides in an explicit tri-state header value on a 200, never in the
absence of anything, with `assertEOF` normative. Both the status mapping and the standard
conditional vocabulary are recorded here as analysed and rejected, each with its own evidence —
the former on `assertResponseIsOk` and the cache-forgery asymmetry, the latter on §4.1.1's
invitation argument. Both are decided; §15 carries the full ledger.

The one piece of real functionality the 503 spelling would add is `Retry-After` — sender-paced
backoff instead of the queue's fixed one. It is deliberately NOT adopted here as a consolation
header: it deletes nothing and discharges no stated obligation, and the queue's backoff is not
currently a problem anyone has measured. If sender-paced backoff is wanted it should arrive as its
own change with its own evidence, not as a rider on this one.

### 4.2 Answer vocabulary and the fence-first reordering {#answer-vocabulary}

Three outcomes, and — unlike today — two of them are knowledge.

| `content_addressed_answer` | `content_addressed_identity` | Outcome | What the sender is asserting |
| --- | --- | --- | --- |
| `proven` | equal to `content_addressed_expected` | **Yes** | *"I hold the mount fence, and this ref still names exactly that binding."* |
| `changed` | present, different | **No** — authoritative | *"I hold the mount fence, and this ref names a different binding now."* |
| `unknown:<reason>` | absent | **Unknown** | *"I cannot speak for this binding right now, and here is which rule stopped me."* |
| absent / anything else | — | **Unknown** | Not an answer at all: a stripped header, a misroute, an older peer, an offer that reached the wrong branch. |

The answer and the validator must BOTH agree before a promote: `proven` with a non-matching `content_addressed_identity`
is a contradiction, not a `Yes`, and it is treated as `Unknown`. Each is separately sufficient to
fail closed.

Note what the third row absorbs, because it closes a question the earlier draft of this design left
open. A part that has been merged or dropped away entirely has no binding and therefore no
validator, so it lands in **Unknown**, not in `No` — and that is the correct action rather than a
concession: there is nothing to byte-fetch from a sender that no longer has the part, so
retry-later, which re-executes the queue entry and recomputes the covering-part discovery, is
exactly what should happen. The authoritative-`No` byte-fetch payoff (§4.4) applies to the case
where it actually pays — a repoint, where the sender demonstrably still has a part under that
name. No sentinel value is needed on the wire, so the `none` literal an identity-cookie encoding
would have required does not exist.

The fence check moves from LAST to FIRST. `CasConfirmAnswer`'s own comment already prescribes the
reorder for anyone who wants to distinguish `No` (`ContentAddressedExchange.h:19-26`): "Any code
that ever treats `No` as authoritative knowledge ... makes that ordering wrong and must hoist rule
6 above the row comparison first." That is precisely what this design does, and it is what buys
the payoff in §4.4.

*What the reorder deletes:* the entire `No`-is-not-knowledge apparatus — the 24-line wire-constant
comment arguing why `no` must never cross the wire, the deliberate binary-vocabulary collapse, and
the taxonomy row that lumps four distinguishable outcomes into one action. Moving one check
upward removes a doctrine, not just a line.

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
5. **The row.** The committed row for the ref: present ⇒ emit its validator as the `content_addressed_identity` and
   compare it with `content_addressed_expected`; absent ⇒ emit no `content_addressed_identity`. The emitted validator is
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

*This is a parameter, not a mechanism, and that is the point: it is what keeps the answer path
from becoming a second resolve implementation to maintain alongside `resolveRef`.* Mechanically
it is a `ReadMaintenance` parameter on `resolveRef` (`Full` for local callers,
`AnswerOnly` for this one) rather than a parallel resolve implementation — one resolve, one
recovery preamble, one place where the committed row is read.

**Minimal protection on peer-initiated recovery.** *This is the only mechanism in the design that
deletes nothing; it exists to discharge a named safety obligation — that a remote peer cannot drive
UNBOUNDED work on this writer — which removing the zero-I/O contract would otherwise leave
undischarged.* An earlier draft of this section specified three bounds and ran to ~100 lines. It
was **rejected as overengineering** (user, 2026-07-29), and the reason it was wrong is worth
stating because it is structural rather than a matter of taste.

**The confirm is warm by construction.** It follows the offer by seconds, and the offer was
answered by this same sender for this same namespace: `getRelinkOffer` routes the part and resolves
its ref, which runs the ordinary read preamble, which calls `ensureRefTableRecovered` — and that
call **marks the table most-recently-used** (`CasRefLedger.cpp:925`). So at confirm time the table
is not merely warm, it is the *last* candidate the LRU pass would evict. For a confirm to be cold,
the table must have been evicted within those seconds despite being the most recently touched one
in the cache. That is a corner, not a load; the confirm is not a hot path in the sense that would
justify machinery; and the corner's outcome is `Unknown` ⇒ retry, which is the softest failure this
protocol has.

What survives is the smallest thing that still bounds the unbounded:

1. **A per-recovery work cap.** A single namespace's `LIST` plus replay has no natural bound, so
   this is the one bound whose absence is a real hazard rather than a theoretical one. A
   peer-initiated recovery carries a work budget (pages listed / transactions replayed / elapsed
   deadline); exceeding it ABANDONS the recovery and answers `Unknown`. Safe because recovery
   installs its result atomically — abandoning before the install leaves nothing partial — and it
   applies **only** to the peer-initiated class, since a local recovery must always be allowed to
   complete or the writer could starve itself.
   Two boundaries the implementation must respect. **The cap PRESERVES the existing failure
   boundaries rather than inventing new ones:** recovery performs durable side-writes before it
   installs — epoch seals and `_ckpt` updates (`CasRefLedger.cpp:807-819`, `:898-915`) — and those
   are conditional and adoptable by construction, so abandoning a capped recovery leaves exactly
   the states an interrupted recovery already leaves today, and no new abandon hazard is created.
   **And the budget spans a whole recovery including all of its internal retries**, not one
   attempt: a cap that reset per retry would bound nothing, since the retry envelope is where an
   unbounded recovery actually spends its time.
2. **A hard-coded global concurrency limit** of one or two peer-initiated recoveries at a time.
   Over the limit ⇒ `Unknown`, with its own counter. Checked before any I/O and only when the table
   is not already warm, so the ordinary path never touches it. **"Global" means process-wide** —
   one counter across every endpoint, every table and every ledger in the server, not per-ledger or
   per-table. A per-ledger limit would multiply by the number of mounted content-addressed disks
   and bound nothing in aggregate.

Both are constants in the source. There is **no configuration knob** (see §4.3.1), no token bucket,
no cache segmentation and no fairness machinery; §14 records why those were dropped.

The DoS contract survives, reformulated: it was *"a peer can make this writer do no I/O at all"*
and becomes ***"everything a peer can make this writer do is bounded above"*** — bounded in
per-request work and in concurrency. That weakening is real and is the one structural property this
design trades away; it is stated here rather than buried.

**Observability requirement, from the decision:** refusals from either bound get their OWN
counter, distinct from every other `Unknown`. The rule-3 storm took a live cluster to diagnose
precisely because refusal reasons were indistinguishable on the wire and unlogged on the sender.

#### 4.3.1 No configuration knob {#no-knob}

YAGNI, decided (user, 2026-07-29): the work cap and the concurrency limit are hard-coded constants
with **no operator surface at all**. `ref_table_cache_bytes` is already a struct default with no XML
plumbing (`CasPool.h:221`), so this introduces no inconsistency. A knob is a documentation,
support and compatibility obligation forever; it arrives only if §10 measurement 1 ever contradicts
the warm-by-construction premise, and that measurement exists precisely as the tripwire for
revisiting this.

### 4.4 The receiver's comparison, and the byte-fetch fast path {#receiver-comparison}

The receiver holds the validator from the first offer. It re-issues the offer with
`content_addressed_expected=<that validator>` and `content_addressed_confirm=<fresh nonce>`, then
promotes **only if all four of these hold**:

1. the response body is empty (`assertEOF`, §4.1.3) — it is a confirm, not an offer;
2. `content_addressed_nonce` echoes the nonce (§4.1.4) — it is THIS confirm, not a replay;
3. `content_addressed_answer` is `proven` (§4.1.3) — the sender certified, under §4.2's
   ordering, rather than merely resolved;
4. the `content_addressed_identity` equals the held validator (§4.1) — the certified binding is the adopted one.

Any single failure is `Unknown`. The four are deliberately independent: each covers a different
way the answer could fail to be an answer, and none of them is inferred from the absence of
another.

The other two outcomes:

- `changed` with a present, different `content_addressed_identity` ⇒ `abort` the prepared relink, then **fetch the bytes
  from the same sender**;
- everything else (refusal, older peer, no such part, transport failure, timeout — one outcome, as
  today) ⇒ `abort`, then throw the retry-later `NETWORK_ERROR`.

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

§5.1.1 is a statement about program ORDER. It is not yet one about what a concurrent reader can
OBSERVE, and today it is not: `armApplyPending` performs a relaxed `compare_exchange_strong`
(`CasRefLedger.cpp:1681`) called at `:2806` with no lock held, while the answer reads the committed
row under `state_mutex`. A reader acquiring after the arm may still observe `Clean`. What carries
the weight today is precisely the predicate this design deletes — `leader_active`, set and cleared
under `ref_queue_mutex` (`:1588`, `:1668`). **Rule 3's terms are not merely redundant with rule 4;
they are the properly synchronized guard, and rule 4 is a relaxed atomic riding along.**

The superseded spec says both halves in its own voice: its rule 3 carries a *"Chunked-lane note"*
assigning the partially-durable window to `leader_active`, and its §A2 introduces the marker as *"a
cheap defense-in-depth marker"* warning *"Do not ship the marker as the only protection."* This
design does exactly that, so it must first make the marker sufficient.

**The fix is specified in the seam document** (`2026-07-29-cas-part-write-release-seam.md` §6), because it is a property of the ref lane
that every reader depends on rather than something relink can arrange for itself: arm under
`state_mutex`, read under `state_mutex`, lock at the call site. **Approved by the user; under 20
lines, one file, no control-flow change.** It is a gate on this design, and the seam owns it.

### 5.2 The cold/evicted refusal {#cold-refusal}

`confirmExactRef` answers `Unknown` for a cold or evicted table on purpose: it uses `find`, never
`getRefTableRuntime`, so a peer cannot grow this writer's table cache or make it pay a recovery
(`CasRefLedger.cpp:410-416`). Removing that refusal is what §4.3's budget pays for, and it is a
deliberate trade rather than an oversight: keeping it would mean a cold table can never answer,
which for a peer whose entire question is about a cold table is the same availability hole in a
different place. The mitigations are §4.3's per-recovery work cap and its hard-coded global
concurrency limit; the property that
survives is boundedness, not zero.

## 6. The receiver's state machine {#state-machine}

Four states replace the seven taxonomy rows. Each answers **three** questions once, for every path
that reaches it, instead of once per row: *does it lose a part?*, *does it publish twice?*, and —
added after codex round 2 found it missing — *does it leak retention?*

Release cleanup — whether a staged `+1` was provably released — is deliberately **not part of this
machine**. Two drafts tried to make it one (a fifth state in v5, an exit attribute in v6) and both
were wrong for the same reason: the receiver cannot observe it, because its classification happens
before the last release attempt. §6.5 moves the observation to the handle that owns it. **No state
below carries release plumbing, and no state's action depends on it.**

### 6.1 S0 — PUBLICATION PROVEN ABSENT {#s0-not-staged}

**No ref of this relink is published, and that is PROVEN rather than assumed** — the state is
about publication, not about staging. It was called "NOT STAGED" through v6, which was wrong in
exactly the way §6.5 is about: a staged precommit may still exist here if its release could not be
proven. Nothing published; possibly something staged. Reached by: no `content_addressed_identity` on the offer (a peer that predates this design), an unrecognized
relink cookie value, a reservation that landed outside the advertised pool, an undecodable
manifest, the retryable staging class (`CaRelinkPrepare::MechanismFallbackAllowed` — body-absent
precommit, precommit no longer the live owner, ref conflict), and a `promote` that returned
`CaRelinkPromote::MechanismFallbackAllowed`.

**Both of those last two entries can leave a staged `+1` behind**, and neither this state nor the
receiver does anything about it: whether the release completed is observed by the handle that owns
it, once, at destruction (§6.5). No entry condition here consults it, and no action here depends
on it.

**Action:** return `nullptr`; the caller byte-fetches from the same sender, with the recursion
brake (`allow_ca_relink=false`) set.

- *No part loss?* No — the sender still has the part and streams it.
- *No double publish?* No — nothing of this relink is published, so there is nothing to promote,
  and a staged precommit is not a committed ref: a later byte fetch publishes the same ref name
  over it without conflict.
- *No retention leak?* Not this state's question. A staged `+1` may survive here; whether it was
  released is observed and counted by the handle at destruction (§6.5), and the answer changes
  nothing about this state's action.

### 6.2 S1 — STAGED {#s1-staged}

The receiver's `+1` is durable and nothing is published. Exactly one terminal operation is owed.
This is the state the re-offer interrogates, and it has three exits, one per answer of §4.2:

| Answer | Action |
| --- | --- |
| Yes | `promote` ⇒ **S3**, **S0** or **S2** — see below |
| No (authoritative) | `abort`, then byte-fetch from the same sender |
| Unknown | `abort`, then throw the retry-later `NETWORK_ERROR` |

`promote` has **three** outcomes, not two:

| `CaRelinkPromote` | Lands in | Why |
| --- | --- | --- |
| `Committed` | **S3** | the ref is committed |
| `MechanismFallbackAllowed` | **S0** | rejected BEFORE its ref-log append, so "nothing was committed" is proven, not assumed. Whether the `+1` was released is a separate fact that this classification does not carry and does not need — see §6.5 |
| `Unresolved` | **S2** | the append was attempted without a verdict |

**An earlier draft read this edge as proof that the `+1` was released.** It is not:
`PreparedPartWrite::promote`'s catch sets `terminal` only when the abandon actually landed
(`PartFolderAccess.cpp:452`), so a failed removal leaves a live precommit that the
prior-epoch-scoped stale-precommit sweep will not touch (`PartFolderAccess.cpp:365-370`). The
classification above is still correct — nothing was committed — it simply says nothing about
cleanup, and §6.5 is where cleanup is observed.

**`promote` reports the rejection and nothing else.** It needs no release result and must not grow
one: the rejection says which state the receiver is in, and that is the whole of its job. The
release question is answered elsewhere, by the owner that can actually answer it (§6.5).

**The find behind this paragraph still stands, though its fix moved.** The exposure is not special
to the promote path — `abort` is `noexcept` and swallows a failed removal append too
(`ContentAddressedMetadataStorage.cpp:2116-2123`), and the staging path discards its release result
entirely — which is exactly why the answer could not live in any single exit classification.

- *No part loss?* No. On `No` the bytes are fetched; on `Unknown` the queue stores the exception,
  backs off and re-executes, recomputing source and covering-part discovery. For the three
  detached callers with no queue entry (`FETCH PARTITION`, `FETCH PART ... FROM`), the retry-later
  error surfaces to the user, who re-issues — and nothing replicated was expecting the part.
- *No double publish?* No. `abort` appends the exact precommit removal and no committed ref
  exists on either exit. `abort` never throws, by contract: it runs while the receiver's own
  retry-later error is already in flight.
- *No retention leak?* **Not guaranteed on either non-`Yes` exit** — `abort` is `noexcept` and
  swallows a failed removal append (`ContentAddressedMetadataStorage.cpp:2116-2123`). Such an exit
  keeps its own action — byte-fetch after `No`, **retry-later throw after `Unknown`** — and carries
  §6.5's observation. The action must NOT change: converting an `Unknown` into a byte fetch because
  its cleanup failed would discard the very uncertainty that made it `Unknown`.

### 6.3 S2 — PUBLISH UNCERTAIN {#s2-publish-uncertain}

`promote` returned `CaRelinkPromote::Unresolved`: the ref-log append was attempted and came back
without a verdict, so the receiver's ref MAY be committed.

**Action:** throw the retry-later `NETWORK_ERROR`. Never `nullptr`.

- *No part loss?* No — retry-later; by re-execution the ref lane has resolved the ambiguity.
- *No double publish?* No. This exit publishes NOTHING ITSELF — whether the earlier append landed
  is precisely what is unknown, and the receiver adds nothing to it. The handle's abandon is
  REJECTED by the state machine if the promote in fact landed — a promoted binding is no longer a
  precommit — so no committed ref is ever undone here. **This is the one state where a byte fetch
  would be a defect**, because it would publish the part a second time over a relink that may
  already be committed.
- *No retention leak?* Bounded. The abandon is attempted and, if the promote in fact landed, is
  REJECTED by the state machine rather than undoing a committed ref; if it did not land, the
  precommit is released or §6.5's observation fires. **The action is unchanged either way** — S2 still throws retry-later and still never byte-fetches. This is the state where
  treating a failed release as a reason to byte-fetch would permit exactly the double publish this
  section forbids.

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
- *No retention leak?* None — the precommit became a committed ref; nothing is left staged.

### 6.5 Release cleanup is the seam's, not this machine's {#release-incomplete}

**The receiver carries no release plumbing.** Whether a staged `+1` was provably released is
observed once, by the transaction handle that owns the last attempt, and is specified in its own
document: `2026-07-29-cas-part-write-release-seam.md`. Nothing in §6.1-§6.4 consults it, and no state's action depends on it.

That separation is the outcome of four review rounds, and it is worth one sentence of why. The
receiver cannot observe the release — its classification necessarily happens BEFORE the last
attempt, since its scope guard runs on the way out (`DataPartsExchange.cpp:1481`) and `abort`
returns `void`. Three drafts tried to route the fact to the receiver anyway (a fifth state, then
an exit attribute, then a result carrier) and each added contract mass without fixing the ordering.
The seam contract puts the observation where it can actually be made.

What relink relies on, and nothing more, is the consumer guarantee of that document: an unproven
release is counted and logged exactly once, reflecting the FINAL attempt; a refusal that provably
transmitted nothing stays silent; and the accounting never changes what this state machine does on
any exit. The seam's own test rows cover the release half; §11 keeps only the rows that exercise
relink behaviour.

### 6.6 What falls out of the taxonomy {#taxonomy-deltas}

Two whole classes of row disappear rather than move:

- **Token classes.** "The source sent no token", "the token did not decode", "the token routed to
  no mount / to several mounts" — there is no token.
- **Quiescence classes.** "The lane was busy" is no longer an outcome (§5.1).

And one row splits usefully: today's row 3 lumps `unproven`, an absent cookie, a transport failure
and a timeout into one action. Under §4.2 the authoritative `No` separates from the rest and gets
the byte fetch; everything else stays exactly one outcome.

**The reduction, counted honestly.** Seven rows become four states, but not because three rows
were deleted — rows 4, 5, 5b and 6 all survive as TRANSITIONS into S3, S0, S2 and S0 respectively
(§6.2). What actually shrinks is the number of places a reviewer must re-answer "does this lose a
part / publish it twice": seven answers, once per row, become four, once per state — now three questions each rather than
two, since codex round 2 showed the retention question was never being asked — and every new exit
added later inherits its state's answers instead of needing a new row. Release cleanup, which
v6 tried to model as a fifth outcome and then as an exit attribute, turns out not to belong to this
machine at all: it is observed once by the handle that owns it (§6.5). **Legacy row 6** ("any other
exception") maps to whichever state the receiver is in when the exception propagates — S0 before
staging, S1 after — and needs no row of its own for the same reason. Rows 1 and 2 collapse into
S0 because they differ only in provenance, and the token and quiescence classes disappear
outright.

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

Replaced by: an identity-only answer branch (≈35), five wire names (≈20), one protocol-version
constant (≈5), the confirm request block including the nonce and the four-condition check (≈60),
the four-state machine comment (≈55), one `Service` declaration (≈8). **Net: 703 → ≈567
CA-attributable lines in the two shared files, ≈−136 (−19%).**

The wire-name row is a straight substitution — five names for five — and NOT the near-total
deletion two earlier drafts claimed. What actually goes to zero is the number of GRAMMARS on the
path: the 170-line reversible codec is deleted, and no standard header's grammar (wildcards,
lists, weak comparison) is inherited in its place (§4.1.1). Every value on the wire is compared
byte-wise and none is parsed; that is the entire parsing surface.

### 8.2 Content-addressed surface {#deletions-ca}

| Deleted | Location | Lines |
| --- | --- | --- |
| the whole relink-token codec (percent-encoding, strict decode, version tag, field caps) | `ContentAddressedExchange.cpp` — entire file | 170 |
| `CasConfirmAnswer`, `CasRelinkSourceToken`, both codec declarations, `ownsNamespace`, `confirmExactRef` | `ContentAddressedExchange.h` | 68 |
| `ContentAddressedMetadataStorage::ownsNamespace` + `::confirmExactRef` | `ContentAddressedMetadataStorage.cpp:1934-1998` | 65 |
| `CasRefLedger::confirmExactRef` — the six rules, the two-mutex snapshot, the `try_to_lock` rationale | `CasRefLedger.cpp:375-481` | 107 |
| `Cas::ConfirmAnswer` + its declaration | `CasRefLedger.h` | ≈40 |

Replaced by an identity-only mode on `getRelinkOffer` (≈18), a peer-facing resolve on the ledger
(≈45), their declarations (≈31), the validator digest (≈5, §4.1) and §4.3's minimal protection —
a per-recovery work cap plus a hard-coded concurrency limit (≈25). **Net ≈ −326.** Combined with
§8.1 and the ≈20-line marker fix: **≈ −440 production lines**, plus the test delta of §11.

### 8.3 Concepts that stop crossing the seam {#deletions-concepts}

This is the measure that matters for the house rule, and it is the strongest result of the
design.

Counted as LITERAL IDENTIFIERS occurring in the tree, verified by grep rather than by reading the
design — an earlier draft grouped concepts instead and got this wrong in both directions.

| Identifier | Occurrences before | After |
| --- | --- | --- |
| `IContentAddressedExchange` | 5 | ✓ |
| `CasConfirmAnswer` | 18 | **0** |
| `decodeCasRelinkSourceToken` | 1 | **0** |
| `ICaPreparedRelink` | 1 | ✓ |
| `CaRelinkPrepare` | 1 | ✓ |
| `CaRelinkPromote` | 3 | ✓ |
| `CasRelinkSourceToken`, `encodeCasRelinkSourceToken` | **0 — never appeared here** | 0 |

**6 → 4 identifiers, 29 → 10 occurrences.** The shared replication path stops naming any
content-addressed *answer type* and stops calling any codec; what remains is the narrow interface
plus the prepared-relink handle types, which are the window it necessarily owns. It never again
decodes, routes, range-checks or disambiguates a content-addressed identifier.

The occurrence count is the more honest of the two: `CasConfirmAnswer` alone accounts for 18 of
the 29, because the shared file threads that enum through a routing function, a handler, a
forward declaration and a two-value wire vocabulary — all of which go.

Interface methods on the seam that exist only for the confirm also go: `confirmExactRef` and
`ownsNamespace`. The latter has exactly one non-test caller in the tree — the confirm's routing at
`DataPartsExchange.cpp:236` — so it dies with the endpoint.

## 9. Observability {#observability}

Three gaps, all of them causes of the diagnostic cost recorded in §2.

**The transient-`Unknown` message.** USER DIRECTIVE: an `Unknown` that is our own uncertainty and
part of the protocol must not present as an error. The receiver's abandon drops from ERROR to
INFO, and the message names a transient state rather than a failure:

> the source did not certify the relink binding for part `<P>` (`<reason>`); this is an expected,
> transient outcome while the source is unable to speak for the namespace, and the fetch is
> re-queued and will retry

The `<reason>` comes straight off the wire (§4.1.3) — which is a change from the earlier drafts,
where the refusing rule stayed sender-side only. It is safe to transmit because it authorizes
nothing: the receiver logs it and acts solely on the answer/validator pair, so a peer that lies in
that field corrupts its counterparty's log line and nothing else. It is worth transmitting because
the alternative is what §2 measured — a refusal class that took a live cluster to identify.

**Counters.** `ProfileEvents` pairs so the storm becomes a metric instead of ~5–9k log lines per
minute at ~23 frames of unwinding each. None of these exist today — the tree has no relink or
confirm counter at all.

| Counter | Meaning |
| --- | --- |
| `CasRelinkProven` | `proven` + matching validator; the promote is authorized |
| `CasRelinkChanged` | authoritative `No` — the binding changed; the receiver byte-fetches |
| `CasRelinkUnknown` | the sender could not speak; retry-later |
| `CasRelinkRecoveryBudgetRefused` | **its own counter**, per the decision: an `Unknown` caused by the peer-initiated recovery budget (§4.3), distinguishable from every other `Unknown` |
| `CasPrecommitReleaseUnproven` | **Owned by `2026-07-29-cas-part-write-release-seam.md`**, not by relink — it is general to part writes and deliberately not relink-attributed. Listed here only so a reader of relink's counters knows it exists and what it is an upper bound on; the seam spec defines its meaning and emission |
| `CasRelinkAnswerRejected` | the response failed one of §4.4's four conditions — empty body, nonce echo, explicit answer, validator match. **Must be ~0 in a healthy cluster**: a non-zero value means a stripped header, an intermediary, or a misroute, and that is exactly the class §4.1.3 and §4.1.4 exist to catch |

**Which rule refused, and what was expected.** Today an `Unknown` maps silently through
`ContentAddressedMetadataStorage.cpp:1996`, and diagnosing rule 3 took a live cluster. The
sender-side `LOG_DEBUG` must name the rule that produced the refusal (fence / unrecovered /
wedge / poison / budget / no-such-part) — the same value it now also puts on the wire.

The conditional form improves this materially and for free: because `content_addressed_expected`
puts the receiver's EXPECTED validator on the sender, a mismatch can be logged as
**expected-versus-actual on the node that knows why**, rather than as a bare "unproven" that tells
the reader nothing about which of the two sides moved. That was impossible with the old token,
where the sender learned the expectation only as one of six fields it had to decode first.

**The digest costs readability, so the log must pay it back — and it takes BOTH sides to do it.**
The old token was plain text: a human reading a refusal saw the namespace, the ref and the manifest
reference directly. Two sixteen-byte hex strings say only *match* or *mismatch*. The sender can
only expand its own half — a digest is one-way, so nothing on the sender can say which component of
the RECEIVER's expectation differs. Hence two log lines, one per side, correlated by part name:

- **Sender, on mismatch:** the components it just hashed — `root_namespace`, `ref_name`,
  `ManifestRef`, `disk_name` — beside both digests.
- **Receiver, on abandon:** the sender-identity fields it already holds from T1, read out of the
  transferred manifest body (`ManifestRef` and `root_namespace_id`) plus the part name.

Together they name which component moved; separately neither can. **This costs no state retention
on either side**, which matters because the sender remembering anything between the two requests is
exactly the property the token was designed to avoid — a sender that had to correlate offers with
confirms would need a table of them, and that is a worse trade than a second log line. Using the
manifest's sender-identity fields for DIAGNOSTICS is not a trust violation: they are non-
authoritative for adoption, and they remain non-authoritative here — nothing is decided from them,
they are only printed.

## 10. Measurements {#measurements}

In priority order; the first is decisive for whether §4.3's budget is insurance or structure.

1. **Share of confirms hitting a COLD table — now the TRIPWIRE for a decided design, not an input
   to an open one.** §4.3's minimal protection rests on the claim that a confirm is warm by
   construction; this measurement is what would falsify it. A materially non-zero cold share in
   steady state is the signal to revisit §14's rejected bounds.
   *(Original framing:)* **Share of re-offers hitting a COLD table.** Prior expectation: steady-state confirms are warm
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
| 3 | S1 → Unknown, part gone | drop/merge the source part inside the pause window | no `content_addressed_identity`; retry-later; no promote; no double publish; **no byte re-request** |
| 4 | S1 → Unknown, fence lost | revoke the sender's mount fence | no `content_addressed_identity`; INFO severity; transient-state message; retry-later; **no byte re-request** |
| 5 | S1 → Unknown, wedge | wedge the sender's append lane | as 4 |
| 6 | S1 → Unknown, poisoned | poison the sender's apply state | as 4 |
| 7 | S1 → Unknown, budget | exhaust the peer-initiated recovery budget | as 4, plus `CasRelinkRecoveryBudgetRefused` increments and no other `Unknown` counter does |
| 8 | S0 | `cas_relink_receiver_force_mechanism_failure` | byte fetch; recursion brake bounds it to one attempt |
| 9 | S2 | promote forced to `Unresolved` | retry-later; **never** a byte fetch |
| 10 | Cross-pool | receiver in a different pool | no offer; bytes; unchanged |
| 11 | Version mix | peer advertising the pre-redesign version | no `content_addressed_identity` ⇒ S0 ⇒ bytes |
| 12 | Sender-side answer contract | direct ledger test | discretionary maintenance is NOT scheduled by a peer-initiated read; recovery IS |
| 13 | **Cache safety** | inspect both responses | offer AND confirm carry `Cache-Control: no-store` (§4.1.5) |
| 14 | **No standard conditional machinery** | send `If-None-Match` (both matching and `*`), `If-Modified-Since`, and a two-valued list | ALL are ignored — they select nothing and certify nothing; neither response ever carries `ETag` or `Last-Modified`; no promote is reachable through any of them (§4.1.1) |
| 15 | **Stripped mode parameter ⇒ offer, not answer** | issue the confirm with **`content_addressed_confirm`** removed (the MODE selector — stripping `content_addressed_expected` instead leaves the sender in confirm mode and exercises neither defence) | the sender replies with a full OFFER; the receiver promotes NOTHING even though `content_addressed_identity` matches. Two subcases, each disabling the other defence, so neither can carry the test alone: **(a)** with the body check disabled, the missing `content_addressed_answer` cookie alone must prevent promotion; **(b)** with the answer check disabled, the non-empty offer body alone must prevent promotion via `assertEOF` |
| 16 | **Replay** | replay a captured earlier confirm response for the same part and validator | nonce mismatch ⇒ `Unknown`; no promote (§4.1.4) |
| 17 | **Cross-mount collision** | two same-pool CA disks, same `server_root_id`, same table; offer from disk A, `MOVE ... TO DISK` to B, force B to hold the same `ManifestRef` tuple for that ref | validators DIFFER (the digest is mount-qualified), so the answer is `changed`, never `proven`. This is B1's shape and the direct regression test for §3 |
| 18 | **S1 → S0 via promote** | force `promote` to `MechanismFallbackAllowed` **with the release succeeding** | byte fetch to the same sender; no double publish; distinct from row 9's `Unresolved`, which must NOT byte-fetch |
| 19 | **A failed release never changes the action** | force every removal attempt to fail on S1's `Unknown` exit and on S2's `Unresolved` exit; for S2 run BOTH promote-landed and promote-not-landed variants so the outcome is pinned rather than nondeterministic | all variants still throw retry-later and **none byte-fetches** — S2 is where a byte fetch would double-publish. This is the RELINK half; the emission itself is asserted by the seam spec's rows S1-S5 |

Rows covering the release accounting itself — unproven-release emission, the settled-late silence,
proven non-transmission, the fail-closed half, and marker synchronization — live in
`2026-07-29-cas-part-write-release-seam.md` §8 and are not duplicated here.

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

**Disposition:** `CaRelinkConfirmCore` is **kept as the historical witness** of the v11 protocol
and is not edited in place — its `_sab_*` results are the evidence that v11's rules were each
load-bearing, and rewriting it would destroy that record. The redesign gets its **own** model,
covering the custody chain under the named assumption of §12.2.

**The wire encoding does not change the ANSWER space — but it does add two transport hazards the
model cannot express, and an earlier draft overstated this.** The three answers are the same
(`"yes"` / `"no"` / `"unknown"` in `Gate1Answer`), so §12.3's refinement and §12.4's re-derivation
are independent of the spelling. What is NOT independent: `RConfirm`
(`CaRelinkConfirmCore.tla:265-287`) computes a FRESH `Gate1Answer` at the moment it fires, so the
model literally cannot choose to deliver a pre-T1 response, and it has no notion of an offer
response being mistaken for a confirm response. Replay (§4.1.4) and offer/confirm confusion
(§4.1.3) are therefore outside what any refinement of this model would check.

Rather than teach the model request/response generations — which would grow its state space to
cover a transport property, for no gain over a direct test — this is recorded as a **named
assumption**, in the same style as §12.5(iii):

> ASSUME `FreshCertifiedResponse`: the response the receiver acts on was produced by the sender,
> in reply to THIS request, after T1 — not replayed from an earlier exchange, and not an offer
> response mistaken for a confirm.

It is discharged by mechanism and by evidence, not by assertion: the nonce makes a replayed
response detectable (§4.1.4, test row 16), and the confirm-only answer cookie plus normative
`assertEOF` make an offer response unusable as a certificate (§4.1.3, test row 15 with both
subcases). **If either of those test rows is weakened, this assumption goes with it** — which is
the point of naming it.

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

### 12.5 Expressiveness the model must GAIN {#tla-expressiveness}

The apply-pending split of §12.3 is necessary but **not a sufficient gate**, and the reason is a
property of the current model rather than of the design: it fixes one sender namespace (`NsS`) and
treats `Token` as globally identifying its blobs, so a model satisfying §12.3's gate could stay
green while the wire is unsafe. Three things it cannot currently express, with an explicit verdict
on each:

| # | Shape | Verdict |
| --- | --- | --- |
| i | **Cross-mount collision** — a second mount whose bare `ManifestRef` collides with the first's (B1's shape, §3) | **MODEL IT**, and specifically: two mounts with **EQUAL `root_namespace` and DIFFERENT `disk_name`**, which is the configuration §3 shows is legal and which namespace-qualification alone does NOT separate. Without that shape a model can pass while qualifying by namespace only. TWO sabotages must go RED: `_sab_barevalidator` (validator qualified by ref alone) and **`_sab_nodiskqualification`** (validator keeps the namespace, drops `disk_name`). The second is the one that pins B1's actual fix. |
| ii | **Chunk-boundary tenure** — one `leader_active` tenure committing several durable transactions | **MODEL IT.** The design's central claim is that the marker, not the tenure, guards the durable-but-unapplied window; a single `SenderDurable`→`SenderApply` step cannot distinguish the two, so §12.3's green would be an artefact of the model having only one transaction per tenure. |
| iii | **Unresolved promote (S2)** | **ASSUME, NAMED.** The receiver-side promote ambiguity is a local state-machine property with no GC interaction: `ASSUME UnresolvedPromoteNeverBytes` — an unresolved promote never leads to a byte fetch. Discharged by test row 9 rather than by the model, and named so that anyone weakening row 9 sees what it was holding up. |

The wedge-resolution tenure is a fourth candidate; it is already covered by the wedge refusal
being retained unchanged, so it stays out of scope and is recorded here so the omission is a
decision rather than an oversight.

## 13. Migration {#migration}

**Zero compatibility scaffolding**, per the standing pre-release rule: the content-addressed
subsystem has no released build and no persisted data to be compatible with, so no dual-path code
is written and no token is kept alive for old peers.

The replication protocol version does all the work. `..._WITH_CA_CONFIRM = 11` is replaced by a
single `..._WITH_CA_RELINK = 12`, and the two pre-release constants 10 and 11 are collapsed into
one line of historical note, because no released build ever advertised them:

- a v12 sender offers a relink only to a receiver advertising ≥ 12;
- a v12 receiver talking to an older sender receives an offer with no `content_addressed_identity`, which is
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

**The token bucket and the cache-class segmentation** (rejected 2026-07-29 by the user as
overengineering). An earlier §4.3 specified three bounds: a per-recovery work cap, a pool-global
rate token bucket, and a peer/local cache-class segmentation of the ref-table budget, with per-peer
fairness as a follow-on question. Only the work cap and a hard-coded concurrency limit survive. The
rationale is §4.3's warm-by-construction argument, in the user's words: *"confirm отстоит от
начального fetch на секунды — вряд ли он будет холодным, поэтому это не hot path; можно какую-то
минимальную защиту добавить, но точно не стоит того чтобы из-за этого сильно усложнять код."* The
rate bucket and the segmentation both defend a cold-confirm storm, and a confirm arriving seconds
after an offer that just marked its table most-recently-used is not cold except in a corner —
whose outcome is `Unknown` ⇒ retry. Roughly 60 lines and an entire cache-classification concept
were bought for a corner. Per-peer fairness and reservations die with them, since there is no
longer a shared allowance to be unfair about. **Tripwire:** §10 measurement 1 measures the cold
share directly; if it ever contradicts the premise, this is the entry to revisit, and the design is
written down here rather than discarded so revisiting is cheap.

**(iii) Receiver-pays verification.** The receiver could verify by reading the sender's ref state
directly from the shared pool: `recoverRefTableDetailed` is a free function and the offer names
the namespace and ref. Recorded as rejected so it is not re-proposed, but the axis it clarifies is
worth remembering — **who pays**. Under (iii) the DoS concern of §4.3 vanishes as a class: the
beneficiary pays, and an abusive receiver harms only itself. It was rejected on cost — a `LIST`
plus replay per verification — and because it inherits the listing-trust questions the v9 chain
exists to close, which is precisely the property §12 has just reassigned to those models.

## 15. Open questions {#open-questions}

**None blocking. Every question this spec raised has been answered**, and the answers live in the
sections they belong to rather than in a list at the end. Recorded here so a reader can see that
the list is empty by decision and not by omission:

| Was asked | Answer | Where |
| --- | --- | --- |
| Apply-marker synchronization — acceptable on the append path? | **APPROVED**, ships as specced (~20 lines, one file) | §5.1.2 |
| 304/503 status mapping? | **Rejected** — value encoding stands | §4.1.6 |
| Standard `ETag`/`If-None-Match` vocabulary? | **Rejected** — a standard header invites intermediaries to answer | §4.1.1, §4.1.6 |
| `assertEOF` strictness — wanted? | **Normative**, with a test; it is one of two defences against a stripped mode parameter | §4.1.3, §11 row 15 |
| Split the authoritative `No`? | **Dissolved** — a vanished part has no validator, so it lands in `Unknown` and retry-later, which is the correct action | §4.2 |
| How much of the recovery budget ships? | **Minimal only** — work cap + hard-coded concurrency limit; token bucket and cache segmentation rejected as overengineering | §4.3, §14 |
| Budget fairness — global, per-peer, reserved? | **Dissolved** with the shared allowance it would have divided | §14 |
| Configuration knob? | **No knob** — hard-coded constants, no operator surface | §4.3.1 |
| Wire naming? | **Decided** — `content_addressed_*` request parameters and response cookies, matching the endpoint's existing family | §4.1 |
| Dispatch before or after `findPart`? | **Decided** — early, so a vanished part is answered rather than thrown; the routing half was settled by the qualified validator | §4.2, §3 |

Two items remain, and neither needs a decision now because neither is answerable from a document:

- **The TLA gate (§12.3, §12.5)** — the model must be refined and must then pass. This is the one
  thing that can still stop the design, and it is settled by running TLC, not by choosing.
- **§10 measurement 1 (cold-confirm share)** — the tripwire for §4.3's warm-by-construction
  premise. It validates a decision already taken; it does not gate the work.

Everything else is implementation-time detail: the exact work-cap and concurrency constants, the
enumeration of `apply_state` writers §5.1.2 requires the plan to audit, and the single destructor
emission §6.5 specifies (one predicate over `precommitState`, `noexcept`, logging only after the
final release attempt).
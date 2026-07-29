---
description: 'Contract for the CAS part-write release seam: how a part-write transaction reports whether its staged precommit was released, how non-transmission is proven rather than guessed, which log site owns the last word, and the synchronization the apply marker needs before any reader may trust it. Relink-independent; prerequisite plumbing for fetch-by-relink and valuable on its own.'
sidebar_label: 'CAS part-write release seam'
sidebar_position: 20260729
slug: /superpowers/specs/cas-part-write-release-seam
title: 'CAS part-write release seam: unproven-release accounting and marker synchronization'
doc_type: 'reference'
---

# CAS part-write release seam {#cas-part-write-release-seam}

**Date:** 2026-07-29. **Status:** DESIGN, awaiting approval. **Branch:** `cas-gc-rebuild`.
Spec only; no code landed.

**This contract is RELINK-INDEPENDENT.** Every hazard it addresses belongs to part writes in
general: any `PartWriteTxn` can end with a staged precommit whose release could not be proven, any
of them can log an alarming line before the last attempt has run, and any reader of a committed
ref can observe a stale row through an unsynchronized apply marker. Fetch-by-relink is the
consumer that made these visible — it is the first path where a remote peer acts on the answer —
but nothing here is relink-specific, and the accounting deliberately is not relink-attributed
(§3.4). It is prerequisite plumbing for
`2026-07-29-cas-relink-reoffer-redesign.md`, and it stands on its own.

**Why it is a separate document.** The relink spec carried this material through four review
rounds, and every round found blockers in it. The reason was structural rather than analytical:
one section was simultaneously repairing an ownership seam and specifying a protocol, so each fix
to the seam arrived as new contract mass inside a document about something else. Splitting it lets
relink CONSUME a contract instead of defining one.

## 1. What the seam is {#the-seam}

Three objects share responsibility for one staged `+1`, and until now no single one of them owned
the question *"was it released?"*:

- **`PartWriteTxn`** — the transaction. Stages the manifest, appends the precommit, and owes the
  removal if it never promotes.
- **`PreparedPartWrite`** — the handle. Owes exactly one terminal operation, `promote` or `abort`,
  and its destructor is the backstop for both.
- **The caller** — for relink, the receiver's scope guard; for a local write, the publishing path.

The failure the seam has to account for is narrow and real: **a staged precommit binding whose
removal append did not land.** Nothing reclaims it while the writer lives — the stale-precommit
sweep is prior-epoch-scoped and GC never touches a live precommit
(`PartFolderAccess.cpp:365-370`) — so it pins its manifest and the blobs beneath it.

## 2. `PrecommitState` is the predicate, and it already exists {#precommit-state}

No new state is introduced. `PartWriteTxn` maintains `PrecommitState`
(`CasPartWriteTxn.h:280-299`) for exactly this question, and its own comment explains why it is a
state rather than a bool: an `Unresolved` append MAY have landed, so the intent is recorded BEFORE
the ambiguous append and settled either way afterwards.

| Value | Meaning | Release proven? |
| --- | --- | --- |
| `NotAttempted` | `precommitAdd` never reached its append; nothing can be owned, and the staged bodies are ordinary writer debris | nothing to release |
| `Uncertain` | the append was **entered**, outcome unknown; the build OWES the removal *as if it were durable* | no |
| `Durable` | the append returned; the binding is this build's | no |
| `Settled` | the owed terminal operation discharged the duty | yes |

The pre-set to `Uncertain` before the append is deliberate and **must not be weakened**: its
comment records the bug it fixed, an object that "believed it had never precommitted" and so
writer-deleted a body that a later wedge resolved as committed. §4 works with that ordering rather
than against it.

## 3. The emission contract {#emission-contract}

### 3.1 The owner of the LAST attempt reports {#last-attempt-owns}

The natural-seeming alternative — the caller classifies its exit, so the caller reports the
release — cannot work, and it is worth stating why so it is not re-proposed. **The caller's
classification necessarily happens BEFORE the last release attempt.** Its scope guard runs on the
way out, `abort` returns `void`, and the destructor's own retry reports to nobody
(`PartFolderAccess.cpp:403-416`). No amount of plumbing repairs an ordering that runs backwards.

So the handle reports, at destruction, and the contract is one line:

> **Emit iff `precommitState` is `Uncertain` or `Durable` at destruction**, after the final release
> attempt has run.

`noexcept`, exactly once, reusing the ERROR line `~PreparedPartWrite` already logs rather than
adding a second beside it. A destructor retry that SUCCEEDS moves the state to `Settled`, so it
emits nothing — the asymmetry is carried by the state machine rather than by a rule someone has to
remember.

### 3.2 It covers every exit by construction {#covers-every-exit}

The destructor runs on all of them: enum outcomes; exceptions that propagate before a caller
installs its guard (`prepareAdoptFromManifest` maps only two error codes and lets the rest
through); the `prepareEntries` path that returns **no handle at all**
(`PartFolderAccess.cpp:491-493`), whose own comment says the cleanup "cannot be deferred to one";
and destructor-only exits. A caller-side scheme has to enumerate those and can miss one. This
cannot.

### 3.3 What the counter means {#counter-meaning}

`CasPrecommitReleaseUnproven` counts **not provably released**, which is strictly weaker than
*leaked*. An `Uncertain` removal may in fact have landed and be settled later by the ref lane's
wedge machinery; an `Unresolved` promote may have committed, leaving no precommit at all. **It is
an upper bound on real residue and must be read as one.** A non-zero value is a prompt to look,
not a proof of loss.

### 3.4 What it costs: no per-consumer attribution {#no-attribution}

The handle does not know WHY the write happened, so the counter is general rather than
relink-specific. That is the honest trade, and on reflection the better metric: the hazard is a
property of part writes, and a consumer that wants attribution has its own log line naming the
part. Restoring per-consumer attribution is the one thing that would require caller-side plumbing
back, and this contract declines it.

## 4. Proving non-transmission: mark the transmission point {#proof-channel}

### 4.1 The problem with `Uncertain` {#uncertain-overreports}

Because `Uncertain` is set before the append (`CasPartWriteTxn.cpp:965-971`), it is entered even
when the append is refused before anything is transmitted. Ownership is then PROVEN absent while
the state still says `Uncertain`, so a naive reading of §3.1 floods the counter during fencing and
shutdown — precisely when an operator is reading it.

The fix belongs at the source: **a site that can prove nothing was transmitted downgrades the
state to `NotAttempted`.** That is not a new value; it is what `NotAttempted` already means.

### 4.2 Do not enumerate refusals — mark the send {#mark-the-send}

Proving it per refusal site does not work. There are **nine** such sites, and the proof is erased
before the caller can read it: `RefMutationItem` carries only `done`/`error`/`committed_id`
(`CasRefLedger.h:454-463`), and the one arm that already HAS the proof discards it a line later
into a generic `NETWORK_ERROR` (`CasRefLedger.cpp:3105-3126`).

So mark the single transmission point instead. **`RefMutationItem` gains one field, `attempted`**,
set for every survivor of a chunk immediately before `putIfAbsentControlled` — adjacent to
`armApplyPending` (`CasRefLedger.cpp:2806`), already documented as "the last statement that still
runs while nothing of this transaction can possibly be durable". It is the same
record-intent-before-the-ambiguous-act discipline `PrecommitState` itself uses.

| # | No-send exit | Site | How the channel proves no-send |
| --- | --- | --- | --- |
| 1 | shutdown drain refuses admission | `CasRefLedger.cpp:1546-1549` | never enqueued ⇒ never marked |
| 2 | lost fence | `:2179-2184` | pre-PUT ⇒ never marked |
| 3 | supersession | `:2193-2200` | pre-PUT ⇒ never marked |
| 4 | supersession | `:2267-2274` | pre-PUT ⇒ never marked |
| 5 | supersession | `:2639-2646` | pre-PUT ⇒ never marked |
| 6 | prior wedge | `:2206-2233` | pre-PUT ⇒ never marked |
| 7 | deposition | `:2296-2319` | pre-PUT ⇒ never marked |
| 8 | item validation | `:2425-2460`, `:2513-2577` | pre-PUT ⇒ never marked |
| 9 | unattempted chunk remainder | `:2482-2486` | its chunk never reached the PUT |
| 10 | pre-PUT construction | `:2688-2797` | pre-PUT ⇒ never marked |
| 11 | deadline exhausted, nothing sent | `:3105` | marked, then **cleared** by `unresolvedProvesNothingWasSent` |

Rows 1-10 are free: they happen before the mark, so they prove themselves with no per-site work.
Row 11 is the only exit that is marked and still sent nothing, and it is exactly the case the
existing hardened predicate decides — `unresolvedProvesNothingWasSent`
(`Backend/CasRequestControl.h:76`), which that arm already computes and already counts as
`CasRefAppendPreAttemptRefused`. It additionally clears `attempted`.

**One set-site, one proof-gated clear-site, one read-site**, against nine special cases.
`appendRefOps` surfaces the item's `attempted` to its caller on both the normal and the throwing
path; `precommitAdd` downgrades to `NotAttempted` iff the append failed and `attempted` is false.
Consumers inherit the channel without doing anything: the receiver-side staging path reaches
`precommitAdd` through `prepareEntries`.

### 4.3 Three properties the shape depends on {#channel-properties}

- **Chunking must not split an item.** The mark is per-item, so an item whose ops landed in a
  transmitted chunk must read `attempted` even if a later chunk of the same flush was refused. This
  holds today — the batch builder admits at most one mutation per ref name per flush — and the plan
  must assert it rather than assume it.
- **Retries must not reset it.** It is set before the FIRST attempt and cleared only by the proof
  above, so the controller's internal retries and a later wedge resolution both keep it true.
- **A process death between the mark and the send over-reports.** Accepted, and the fail-closed
  direction: the rule is "emit unless non-transmission is PROVEN", so a lost proof is a louder
  counter, never a silent leak.

## 5. The severity ladder {#severity-ladder}

Moving the destructor's line after its final attempt is necessary but not sufficient: three other
sites log before the last word is in. Those lines are not FALSE — "this attempt failed" is true
when written — they are at the wrong SEVERITY, claiming a leak prematurely. They are therefore
**demoted with transient-state wording, not deleted**, per the standing rule that a protocol's own
expected uncertainty must not present as an error.

Each site was checked separately, and they do not all get the same answer:

| Site | Verdict |
| --- | --- |
| `abandonBuildBestEffort` (`PartFolderAccess.cpp:378-381`) | **INFO.** It reports ONE attempt and is called from three places, one being the destructor — so it must not carry the authoritative line, or the final word becomes indistinguishable from an intermediate one. The destructor emits its own line after its attempt fails. |
| caller scope-guard `abort` (`ContentAddressedMetadataStorage.cpp:2116-2123`) | **INFO, unconditionally.** A caution that this site also fires on real aborts does NOT hold: `abort` early-returns when `write.isTerminal()`, so the catch is reachable only when `write.abort()` threw — exactly a failed removal append. A successful abort logs nothing. No conditional, no message split. |
| `logCasWriteRetryLater` (`Backend/CasRequestControl.cpp:183-186`) | **UNCHANGED.** Not a release claim and not release-specific: the generic "CAS write could not be committed; retrying later" line every CAS write path shares, already rate-limited by a `LogSeriesLimiter`. Its statement is true however the release resolves. |

**Required:** attempt the final release first; then emit BOTH halves of the authoritative emission
— counter and ERROR line — only if it failed. A settled-late transaction produces neither.
Because the third site stays as it is, **assertions about a silent settled-late path are scoped to
the authoritative message class**, not to "no WARNING anywhere": a generic retry-later warning may
still appear and is correct.

## 6. Prerequisite: the apply marker must be synchronized {#apply-marker-synchronization}

This belongs to the seam for the same reason as the rest: it is a property of the ref lane that
every reader of a committed row depends on, not something a consumer can arrange for itself.
**Already approved by the user.**

`armApplyPending` performs a `compare_exchange_strong` with `std::memory_order_relaxed`
(`CasRefLedger.cpp:1681`) and is called at `:2806` **while no lock is held** — the preceding
`state_mutex` scope ends at `:2715` and the install does not re-take it until `:2928`. A reader
that acquires `state_mutex` after the arm is therefore still entitled by the memory model to
observe `Clean`. Today that is harmless only because a separate, properly locked predicate carries
the weight (`leader_active`, set and cleared under `ref_queue_mutex` at `:1588` and `:1668`). Any
consumer that stops relying on that predicate inherits an unsynchronized read.

**Required:** arm the marker inside a `state_mutex` scope immediately before the `PUT`, and read
it under `state_mutex`. Mutual exclusion then supplies the whole argument: a reader acquiring
AFTER the arm necessarily observes `ApplyPending` and refuses; a reader acquiring BEFORE it
observes `Clean`, and at that instant the `PUT` has not been issued, so nothing of that transaction
can be durable.

**The lock goes at the CALL SITE, not inside `armApplyPending`** — decided, not preferred.
`forceWedgeForTest` (`CasRefLedger.cpp:1381-1397`) already calls it at `:1396` while holding
`state_mutex` acquired at `:1390`, so a self-locking version would deadlock at an existing call
site. `armApplyPending` keeps its `noexcept`, allocation-free, lock-free body and gains a
documented precondition that this call site already satisfies. No memory-order change is needed
either: with both sides under the mutex, the mutex supplies the happens-before.

**Size: an ordering upgrade, not a lane refactor.** Two lines at `CasRefLedger.cpp:2806`, plus an
audit bringing the file's eight `apply_state` writes (`armApplyPending`, `clearApplyPending`,
`poisonApplyState`) under `state_mutex`; most are already inside the install region. **Under 20
lines, one file, no control-flow or signature change.** Runtime cost: one uncontended mutex
acquire per committed chunk, against a network `PUT`.

## 7. Retention, and why no fsck class {#retention}

An unreleased binding is **not** bounded by "the mount's epoch". Epoch rollover makes it
*eligible*: a successor incarnation's `sweepStalePrecommitsNow` removes bindings whose
`writer_epoch` predates the live one (`CasRefLedger.cpp:3738-3745`). The honest statement is
**eligible at epoch rollover, reclaimed by a successor's sweep — unbounded only if no successor
ever mounts that namespace, or if every sweep attempt fails.**

An earlier draft specified a dedicated fsck finding for the residue. It is not needed, and its
proposed discriminator was wrong besides — `retireBuildSeq` runs on the successful promote and
abandon paths too (`CasPartWriteTxn.cpp:1250`, `:1313`, `:1413`), not only in the destructor, and
it is an in-memory operation. The cases partition cleanly without it:

- **The process is alive** ⇒ the destructor ran ⇒ the residue is already counted and logged.
  Nothing for fsck to discover.
- **The process died first** ⇒ the binding's `writer_epoch` now predates the next incarnation ⇒ it
  is exactly what `sweepStalePrecommitsNow` exists to reclaim, on the designed path.

The only accepted blind spot is the second case while no successor has yet mounted, which is a
statement about an unmounted namespace rather than about this seam.

## 8. Test matrix {#test-matrix}

| # | Class | Driven by | Asserts |
| --- | --- | --- | --- |
| S1 | Unproven release, promote path | every removal attempt fails — the promote-internal abandon, the caller's scope guard, and the destructor retry — so `precommitState` is `Durable`/`Uncertain` at destruction | `CasPrecommitReleaseUnproven` increments **exactly once**, from the destructor |
| S2 | Unproven release, STAGING path | fail `precommitAdd` uncertainly and fail the `prepareEntries` abandon — the path that returns NO handle (`PartFolderAccess.cpp:491-493`) | the same single emission, proving the observation does not depend on a handle reaching a caller |
| S3 | A successful late release is silent | fail the first removal attempts, let the DESTRUCTOR retry succeed | `precommitState` reaches `Settled`, so **neither half** fires: no counter, no ERROR line of the authoritative class. Intermediate INFO lines are EXPECTED and must not fail the assertion (§5) |
| S4 | Proven non-transmission is silent | drive the marked no-send exits — at minimum the shutdown drain (row 1), a lost fence (row 2), and the deadline-exhausted arm (row 11) | `attempted` stays false (or is cleared), the state downgrades to `NotAttempted`, nothing is emitted |
| S5 | A transmitted attempt is NOT downgraded | let the PUT be sent, then fail ambiguously | `attempted` is true, the state stays `Uncertain`, the emission STANDS. The fail-closed half of S4 — without it a channel bug that never marks anything would silence the counter and look like success |
| S6 | Chunking does not split the mark | a flush whose first chunk transmits and whose second is refused | the first chunk's items read `attempted`; the second chunk's do not (§4.3) |
| S7 | Marker synchronization | a reader taking `state_mutex` between the arm and the install | observes `ApplyPending` and refuses; never a stale `Clean` (§6) |

## 9. What this contract guarantees to a consumer {#consumer-contract}

A consumer — fetch-by-relink is the first — may rely on exactly this, and needs no release
plumbing of its own:

1. **Loud.** A staged precommit whose release was not proven is counted and logged, once.
2. **Last word.** The emission reflects the FINAL attempt, so a late success is silent.
3. **No false positives from refusals.** A refusal that provably transmitted nothing is silent.
4. **Fail-closed.** Where proof is unavailable, the emission stands; the counter over-reports
   rather than under-reports.
5. **Actions are the consumer's own.** Nothing in this contract changes what a consumer does on
   any exit; it only records what happened to the staged `+1`.

A consumer's own tests should assert its ACTIONS and cite §8 for the release half rather than
duplicating it.

---
description: Checkpoint re-review of Task 4-C's Critical fixes (C1, C2, C3+I3, I1), recording six new findings including the third route to a vacuously-complete destructive frontier
sidebar_label: 'Stage B critical checkpoint review'
sidebar_position: 1
slug: /development/cas/stage-b-critical-checkpoint-review
title: 'Stage B critical checkpoint review (2026-07-30)'
doc_type: reference
---

# Stage B critical checkpoint review {#stage-b-critical-checkpoint-review}

Scoped re-review of commit `81ace46e089`, which fixed the Task 4-C review's C1, C2, C3+I3 and I1. Verdict:
**1 critical / 2 important / 3 minor.** All four fixes were confirmed ADDRESSED by reading the code; the
findings below are new.

**Recorded here after the fact, and the reason is a process defect of mine worth naming.** I had told
reviewers to return verdicts in their final message *rather than* writing a file, because several verdicts
had been lost to file-only returns. That made this one message-only, so it existed nowhere durable and the
implementer could not read the findings it was asked to fix. The correct instruction is **both** — a file
for durability and a message for delivery.

## NEW-1 — critical — CODE. The third route to a vacuous frontier: incarnation MISMATCH {#new-1}

The C1 fix keys on the catalog not naming a namespace **at all**; it does nothing when the catalog names it
at an incarnation whose key space is empty.

Scenario: the catalog says `N` is live at incarnation `Y`, while `N`'s objects and its shard-0 fold cursor
belong to `X` — a recreated namespace under today's name-keyed cursors, a writer holding a cached life, or a
rolled-back entry. The walk GETs `refLogKey(Y, cursor+1)` → absent. `witnessAbove` finds nothing, because
the R10 filter dropped every `X` key from `ref_tables` and `readCheckpointWitnesses` reads `_ckpt` at `Y`
too. No carried hold ⇒ `frontier_proven = true`. Meanwhile R10's different-incarnation branch drops all of
`X`'s keys **silently by design** — that is I1's ordinary expected case, not damage — so nothing objects.
Frontier complete, `X`'s live blobs read in-degree zero, destruction.

Not reachable today: drop-and-recreate under the catalog belongs to Tasks 5/6, and `Gc/CasGc.cpp` documents
the name-keyed cursor as the Stage-A residual whose only protection is `UniversePolicy` suppression. **Which
is why it must gate Task 7b rather than sit in a ledger** — that suppression is exactly what 7b removes.

**A detector that needs nothing from Task 5:** a namespace whose CURRENT life contributed nothing (no entry
in `checkpoints`, empty listing) while a NON-CURRENT life of the same name HAS listed objects is a
contradiction, and the R10 loop already holds both facts.

## NEW-2 — important — CODE + PROSE. An un-cataloged item is skipped forever, and the suppression is permanent {#new-2}

Nothing prunes a shard-0 cursor from the seal in the steady state — `per_ns_shard` is only ever assigned, and
the sole erase-like path is the admin rebuild, which keeps holds. Any cursor-carrying namespace is re-added
to `walk_targets` every round by the `parent_cursors` loop, or carried when the probe budget runs out.

So once a namespace's catalog entry is gone: its C1 anomaly recurs **every round**, and since
`suppress_destructive = !anomalies.empty() || …`, reclamation stops **pool-wide and stays stopped**; its
cleanup item can never retire, because retirement requires `ref_tables.find(item.ns)` and R10 dropped all its
keys; `runNamespaceCleanupPasses` skips it forever, so the `_cleanup` marker is never published, the name
becomes **permanently unrecreatable**, and the physical prefixes are never reclaimed. Only
`SYSTEM CONTENT ADDRESSED GC REBUILD` clears it.

Unreachable today (nothing deletes an entry yet). The accepted cost as ruled was *one anomaly per removal
round*; the actual cost is a permanent GC stall plus a permanently unrecreatable name. Task 5 must own both
halves: delete the entry only after the marker and the item's retirement are durable, **and prune the
cursor**.

Stale prose that made it easy to miss: the claim that `per_ns_shard` "is written ONLY for namespaces in THIS
round's listing … so a fully-deleted namespace leaves no cursor behind and a later recreation folds from
`{0, 0}`". False — `walk_targets` includes every shard-0 parent cursor regardless of the listing.

## NEW-3 — important — CODE + PROSE. The optional `life` defaulted to self-resolution at the one site its doc excluded {#new-3}

`CasFsck.cpp` resolves `life` for `walkRefStream`, then calls `crossEpochFromSeal` **without** passing it —
C3's divergence relocated into fsck. A namespace dropped and recreated mid-walk makes the crossing probe read
`refLogKey(new_life, {E,1})` while the walk reads the old life, so a provable crossing reports unproven and
fsck emits a false "closing seal never consumed" verdict against healthy data. Read-only, so a wrong audit
rather than a deletion.

`CasRefProtocol.h` said the nullopt path was "for callers with no round-wide resolution to reuse (fsck's own
independent walk)", which was wrong about the only caller it named. Fix: pass `life` and make the parameter
non-optional — with both sites resolved, nothing needs the default.

## NEW-4 — minor — PROSE. A dead-variable justification {#new-4}

`Gc/CasGc.cpp` says the sentinel `life` "is still needed for logging/key-construction parity with the
catalog-named case further down this loop". It is not: every use of `life` is inside `while (expected)`, which
the un-cataloged path never enters. On that path `life` is dead.

## NEW-5 — minor — PROSE. Comments describe a removal path that does not exist {#new-5}

`gtest_cas_part_folder_access.cpp` ("the catalog entry survives until Task 5's last step") and both GC anomaly
comments ("expected on an ordinary removal too") describe a removal path absent from the tree: **`CasRefCatalog`
has no entry-deletion API at all** yet. Today the entry survives unconditionally, so neither anomaly can fire
on a removal. Write them as the future obligation they are, not as a present cost.

## NEW-6 — minor — TEST. Unclosed parenthesis {#new-6}

`gtest_cas_ref_gc.cpp`, in the new comment: "(also correct, but for the WRONG reason -- … that guard."

## What the checkpoint confirmed as done, and how {#confirmed}

Recorded because each was verified against the code rather than the report, and the reasoning is reusable.

- **C1(a)** — the sentinel fallback is gone from the frontier walk at exactly the re-entry point, and both
  claims hold: **no probe** (every `refLogKey` GET and every `witnessAbove` call is inside `while (expected)`,
  which never runs, and `readCheckpointWitnesses` also `continue`s for an absent namespace) and **no hold**
  (the only reachable `hold(...)` is inside the loop or in the undecodable-`_ckpt` arm whose `if (expected)` is
  already false).
- **C1(b)** — **not addressed as stated.** The predicate is byte-identical; the commit changed the numerator's
  SOURCE. The equivalent guarantee holds, verified through two preconditions — an un-cataloged walk target
  still increments the denominator, and the sole `frontier_proven` assignment is inside the loop — but it is an
  implicit invariant across three sites where one explicit predicate was asked for, and NEW-1 is what the
  union-shaped predicate still permits.
- **C2** — the widening past the three named sites is justified: `marker_key` is the guard the physical
  manifest and verbatim passes consult, so they cannot run without a resolved life. The still-sentinel
  `namespaceFilesPrefix` is correct, because every writer of namespace files is sentinel-keyed too — that layer
  is Task 6's, not a C2 leftover.
- **C3 + I3** — no consumer inside the round re-resolves; the two remaining GC-side `resolveLifeOrSentinel`
  calls are legitimately out of scope (probe A's diagnostic HEAD, and the admin rebuild's own round).
- **I1** — the short-circuit order is right: the different-incarnation case never touches the
  already-recorded set, so it stays silent and cannot pre-poison a later genuine gap for the same name.
- **Test sweep** — no inverted premise. Removing the pin from `DropNamespaceErasesAllViews` was the
  correction: the earlier uniform pin had made the one end-to-end real-incarnation test run entirely at the
  sentinel.

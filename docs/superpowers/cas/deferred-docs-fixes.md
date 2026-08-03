# Deferred documentation and comment fixes {#deferred-docs-fixes}

**Policy, adopted 2026-07-30 by user directive.** Findings whose entire content is
non-executing — comments, doc text, report and commit-message claims — are collected HERE and
executed in ONE pass, not in per-task fix rounds.

The reason is measured, not stylistic. Stage B Task 1c spent **three consecutive fix rounds** on
comment and claim accuracy with **zero defects in the code or the tests**, and each round introduced
the next round's finding, because fixing false prose is itself a prose-writing act with the same
failure rate. Rounds are for defects that can break something. Prose is batched.

## THE POLICY CHANGED (2026-07-31) — re-read every entry before executing it {#policy-change}

A comment policy landed after most of the 22 entries below were written, and it changes what several of them
should DO. Read it in the plan's Global Constraints before running the pass; the summary that matters here:

- **Code must read without comments.** For each entry, the first question is no longer "how should this
  sentence read" but **"should the code change so no sentence is needed"**. An entry saying "rewrite the
  comment to state X" may now be answered by a name, a type, or an assertion instead.
- **No comment may cite a plan, spec, BACKLOG entry, review round, finding ID or task number** — those are
  branch-local and get deleted, so the citation becomes a pointer nobody can resolve. **Several entries below
  are themselves phrased as "state the reason from finding N", which is now forbidden in the code.** Keep the
  reason, drop the citation.
- **Short beats complete.** An entry offering "state the necessary-not-sufficient scope, OR delete the clause"
  now resolves toward the deletion unless a reader is misled without it.
- **A rule that became an executing check deletes the rule's prose.** Where a check now enforces what a
  comment described, the comment goes rather than being corrected.

So the pass is no longer "apply 22 instructions". It is: **for each entry, decide between structure, a short
self-contained reason, and deletion** — with deletion the default when the surrounding code already says it.
Expect several entries to resolve as "no comment at all", which is a better outcome than a corrected one, and
record that outcome rather than silently dropping the entry.

## What belongs here, and what does not

**Here:** a comment that is false or over-claims; a doc statement that contradicts the code; a report
or commit-message claim that attributes content to another location it does not carry; a citation of
the wrong symbol; a figure nobody can reconstruct.

**NOT here — these remain ordinary code work and keep their fix rounds:**

- A test whose assertion does not establish what it tests. The comment is wrong AND the assertion is
  ineffective; the second is a code defect.
- A vestigial guard, a loose prefix, a wrong default — anything where the executing behaviour differs
  from the intent, however small.
- Any finding whose body reveals a behaviour question, even when it is filed as a wording problem.
  Split it: the wording comes here, the behaviour goes to `BACKLOG.md` or a task.

**How the one pass runs.** Each entry below must be self-sufficient — the false text, the verified
truth, and the file plus SYMBOL (never a line number, which shifts). The pass is deletion-first: a
deletion is the only edit that cannot introduce a new false claim. Where a sentence must be replaced
rather than removed, the replacement gets the same verification as new code, because the record shows
that rewriting is where the next defect comes from.

---

## Entries

All of D1-D15 are closed as of the 2026-07-30 batched pass (D11 was already resolved before the pass
and was left untouched per its own note). See
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/prose-pass-report.md` for the full pass
report.

### D1 — RESOLVED: `parseNamespaceFileKey`'s refusal contract over-claims {#d1-parse-namespace-file-key-contract}

**RESOLVED 2026-07-30 (batched pass).** Deleted the "IS one of our namespace files" clause in both
`CasLayout.h`'s declaration doc and the mirrored comment in `CasLayout.cpp`, leaving the trigger
condition ("carries the reserved segment, but the incarnation segment is missing/non-canonical/zero")
stated without the necessary-vs-sufficient overclaim.

From the Task 1c review (MINOR-1), prose half. The behaviour half is a separate item — see
`BACKLOG.md` `{#loose-mountpoint-object-as-corrupt-namespace-file}`.

**Where:** `Layout::parseNamespaceFileKey` — the declaration doc in `CasLayout.h` and the mirrored
comment in `CasLayout.cpp`.

**The false claim:** *"the key IS one of our namespace files — it carries the reserved segment"*.

**The verified truth:** carrying `/_files/` is NECESSARY, not SUFFICIENT. `Layout::mountpointObjectKey`
does not enforce the `_files` reservation — its own doc says the reservation "still appl[ies] to its
segments via the path itself (these never appear in a real ClickHouse loose-file path)", i.e. asserted
and never checked. The Task 1c report grounded a first-occurrence premise on `checkNamespace`, which
governs *namespaces* and says nothing about mountpoint paths, so that premise does not cover the case.

**Fix:** state the necessary-not-sufficient scope, or delete the "IS one of our namespace files"
clause and let the code speak.

### D2 — RESOLVED: a cited test covers one of the two key families the citing code handles {#d2-cited-test-one-family}

**RESOLVED 2026-07-30 (batched pass).** Confirmed against `task-1c-review.md` MINOR-3 and the cited
test (`gtest_cas_ref_gc.cpp`) that the citation covers only the ref-`_log` family. Rewrote
`ContentAddressedTransaction::removeRecursive`'s comment to scope the citation to that family and, for
the `_files` family, state the actually-verified reason it never reaches a GC round at all: `Cas::Gc`'s
fold only LISTs `casRefsPrefix()`, never `rootsPrefix()`.

From the Task 1c review (MINOR-3). Retrieve the exact citation and both families from
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-1c-review.md` MINOR-3 when the pass
runs; the finding is that the comment cites a test as covering the code's behaviour when it exercises
only one of the two key families that code handles.

**Fix:** either cite a test per family, or narrow the claim to the family actually covered.

### D3 — RESOLVED: the interconversion fence's rationale is a non-sequitur {#d3-interconversion-rationale}

**RESOLVED 2026-07-30 (batched pass).** Deleted the `explicit`-clause reasoning in
`gtest_cas_namespace_life_id.cpp`'s `NamespaceLifeIdAndRootNamespaceDoNotInterconvert` comment;
replaced with the true and shorter reason ("no `RootNamespace` constructor takes a
`NamespaceLifeId`"). Also fixed the identical restatement in the Task 1c brief
(`docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md`), the origin of the claim.

From the Task 1c review (MINOR-6). **The assertions are correct and do fence what they claim** —
adding `operator RootNamespace()` or `RootNamespace(const NamespaceLifeId &)` fails them. Only the
stated reason is wrong.

**Where:** `gtest_cas_namespace_life_id.cpp`,
`CasNamespaceLifeId.NamespaceLifeIdAndRootNamespaceDoNotInterconvert`'s comment.

**The false claim:** *"the type declares no conversion operator and `RootNamespace`'s own constructor
is `explicit`, so nothing interconverts in either direction today"*.

**The verified truth:** the explicitness of `RootNamespace(String)` has no bearing on
`std::constructible_from<RootNamespace, NamespaceLifeId>`. What makes that false is that NO
`RootNamespace` constructor takes a `NamespaceLifeId` at all. `RootNamespace` is also
default-constructible, which the sentence sits oddly beside. Inherited verbatim from the Task 1c
brief's own §Interfaces note, so not the implementer's invention.

**Fix:** delete the `explicit` clause; the "no constructor takes it" reason is the true one and is
shorter.

### D4 — RESOLVED: the fence's "AND IT REACHES NO PROSE" paragraph is stale by one commit of its own round {#d4-fence-prose-paragraph-stale}

**RESOLVED 2026-07-30 (batched pass).** Dropped the count entirely (mirroring the paragraph just above
it, which had already dropped its own) instead of trying to fix its unit, and replaced "docstrings"
with "comments and messages" — `run.py`'s two restatements are an inline comment and `WARNING`
f-strings, not docstrings.

From the Task 1c round-3 re-review (Minor 1). **The judgement it documents is sound** — the fence
deliberately does not enumerate prose sites, because a hand-maintained list at a fence that cannot read
it is a fifth thing to forget. Only the incidentals are wrong.

**Where:** `CasFsck.h`, the `AND IT REACHES NO PROSE` paragraph of the `kFsckHardFindings` tripwire.

**Two defects:** (1) it says "two of them were wrong about the exit set on the day this assert was
written", written before the same round found a third restatement in `utils/ca-soak/soak/run.py` — the
report now says three. Under the reading where "them" means the two named LOCATIONS the sentence is
defensible; under the reading where it means individual restatements it is false, and a later auditor
finds two counts of one event with no way to tell which is current. (2) it says the restatements live in
"the soak harness's docstrings", but two of the four were an inline comment and an operator-facing
WARNING string.

**Fix:** make the paragraph say which unit it counts, or drop the count as MINOR-4's was dropped; and
say "comments and messages", not "docstrings".

### D5 — RESOLVED: the report overstates MINOR-4's fix {#d5-report-overstates-minor4}

**RESOLVED 2026-07-30 (batched pass).** Narrowed `task-1c-report.md`'s sentence: it now attributes
the "and why" to `CasFsck.h` specifically and states plainly that `InterpreterSystemQuery.cpp` drops
the count without restating the reason, instead of claiming both comments do the same thing.

From the same re-review (Minor 2). `task-1c-report.md` says "both comments now state the claim
qualitatively … and say explicitly that no count is given **and why**". Only `CasFsck.h` says why;
`InterpreterSystemQuery.cpp` gives no count and no reason for giving none. Someone auditing MINOR-4 from
the report checks the SQL site, finds no such statement, and reopens a fixed item.

**Fix:** narrow the report sentence to the site that carries the reason.

### D6 — RESOLVED: the exit-code caveat is scoped to summary scans though the exclusion is unconditional {#d6-exit-caveat-scope}

**RESOLVED 2026-07-30 (batched pass).** Verified against `CommandFsck::executeImpl`
(`programs/disks/CommandFsck.cpp`) that `stale_edge` never throws in either mode. Rewrote
`08-testing-and-soak.md`'s caveat to cover both modes explicitly and name `stale_edge` as the reason a
`--detail` run's nonzero `stale_edge` still exits 0.

From the same re-review (Minor 3). **Not falsity — scope — but the consequence is operational, so the
one-pass writer should not treat this as cosmetic.**

**Where:** `docs/superpowers/cas/08-testing-and-soak.md`, the exit-code caveat.

**The problem:** "a zero exit code from a **summary scan** is therefore not by itself proof of a clean
pool" invites the contrast that a `--detail` scan's zero exit IS proof. It is not:
`CommandFsck::executeImpl` never throws on `stale_edge` in ANY mode, so a `--detail` run with
`stale_edge=5` prints a `note:` and exits 0. That is precisely why `utils/ca-soak/soak/run.py` needs both
the `exit_code != 0` gate AND `stale_edge_verdict`.

**Failure scenario:** automation gates on the exit code of a `--detail` fsck and passes over a class that
can never be reclaimed.

**Fix:** drop "summary" so the caveat covers both modes, and name `stale_edge` as the reason.

### D7 — RESOLVED: `worstCaseEntryFoldReservationBytes`'s doc claims a coverage the sum does not have {#d7-reservation-doc-coverage}

**RESOLVED 2026-07-30 (batched pass).** Verified `ns_cleanup_items`'s keying (`CasFoldSealFormat.cpp`)
supports more than one `nsc` row per namespace. Rewrote `CasRefCatalogFormat.h`'s doc to say the figure
is an over-estimate per entry and stopped claiming it is the exhaustive set of rows a namespace can
carry.

From the Task 2 review (Important 6, PROSE/FALSE). **The arithmetic is brief-prescribed and Task 2 is
conformant — only the sentence is wrong.** The substantive gap is a decision, placed as a step of plan
Task 5, so do NOT try to fix the formula here.

**The false claim:** the reservation covers "the two rows one namespace can simultaneously occupy".

**The verified truth:** a namespace can occupy more, and rows outlive the entry that reserved them.
`ns_cleanup_items` is keyed `{ns, remove_txn_id}` and retires only once a later round observes its
completion artifacts — retirement skipped entirely on a ref-folding abort — so a namespace removed,
recreated and removed again carries TWO `nsc` rows. And because removal deletes the catalog entry LAST,
an `nsc` row is routinely carried for a namespace with no entry at all, outside the sum's index set.
`btr` (per run segment) and `cnd` (per gc-shard) rows are charged neither in `fixed` nor per entry.

**Fix:** say the per-entry figure is an over-estimate PER ENTRY and stop quantifying over what the sum
covers. The recurring shape here is a sentence claiming what something "is all of".

### D8 — RESOLVED: the registry row's line-cap justification invokes a worst case that exceeds the cap {#d8-line-cap-justification}

**RESOLVED 2026-07-30 (batched pass).** Verified against `CasRefCatalogFormat.cpp`'s `checkLineBytes`
that what shipped is a byte-count refusal (`LIMIT_EXCEEDED`), not a change to the arithmetic — the
worst case genuinely still exceeds 4 KiB. Rewrote `CasFormat.cpp`'s comment to describe that: an
over-4-KiB entry is refused at encode time, not impossible.

From the same review (Minor 11, PROSE/IMPRECISE). `CasFormat.cpp`: "the line cap is tight (4 KiB)
because one entry's record is small and bounded by `kMaxNamespaceBytes`". The record is bounded by
`kMaxNamespaceBytes` times the 6-byte worst-case escape (3072 bytes) plus the unmentioned 255-byte
`server_root_id` (another 1530 escaped) — so the worst case the sentence invokes to justify the cap
EXCEEDS it. Fix once the code fix for that overflow has landed, so the sentence describes the shipped
behaviour.

### D9 — RESOLVED: `foldSealFixedBytes`'s doc omits the meta line and calls a floor a constant {#d9-fold-seal-fixed-bytes-doc}

**RESOLVED 2026-07-30 (batched pass).** Verified `encodeFoldSeal` writes a meta line (`g`/`pg`) between
header and trailer. Rewrote `CasRefCatalogFormat.h`'s doc for `foldSealFixedBytes` to name the meta
line and to call the value a floor (measured at `generation = 0`/`n = 0`), not a constant.

From the same review (Minor 12, PROSE/IMPRECISE). The doc says "header + trailer, zero entries", but
`encodeFoldSeal` writes a **meta** line between them (`g`/`pg`) which the measured value correctly
includes. Also: the value is measured at `generation = 0` / `n = 0`, so it is a FLOOR, not a constant —
a real seal's decimal widths add tens of bytes. Harmless (floor division leaves up to one whole
reservation of slack), but "fixed" hides that it is a floor. Fix both in one clause.

### D10 — PARTIALLY RESOLVED: "predicate (2) equality" overstates what the test asserts {#d10-predicate-2-equality-wording}

**PARTIALLY RESOLVED 2026-07-30 (batched pass).** Rewrote `task-2-report.md`'s Tests bullet to say
"exact-entry-count boundary" instead of the ambiguous "equality/`+1-entry` boundary", and to spell out
that this coincides with byte equality only when `cap - fixed` divides evenly — that part is report
text, in scope. The test NAME `Predicate2AcceptsEqualityRefusesOneEntryOver` is still imprecise but
renaming it is a code change, out of scope for this policy; left as-is and flagged in the pass report.

From the same review (Minor 13, PROSE/IMPRECISE). `Predicate2AcceptsEqualityRefusesOneEntryOver` and
the report's Tests bullet describe BYTE equality; the test asserts the last admissible ENTRY COUNT,
which coincides with byte equality only when `cap - fixed` divides by the reservation. The report's
Design-decisions paragraph states it correctly, so the imprecision is in the summary line and the test
NAME. No substantive gap — equality semantics live in the shared `fitsObjectCap`, which predicate (1)
does test at exact equality.

### D11 — RESOLVED, do not "fix" it {#d11-ckpt-cleanup-site-count}

From the Task 3 review (P1, PROSE/Important). There are FOUR call sites, not three. Both the code's own
comment and — before it was corrected — plan Task 4's obligation stated three. The plan side is already
fixed; the code comment is not.

**RESOLVED 2026-07-30 as a side effect, and the batched pass must NOT touch it.** Fix round 1 removed the
`!attempt_armed` call site, so there are three again and the comment saying "THREE branches" is TRUE.
Verified by re-deriving the count after the fix, not by assuming.

Kept as a closed entry for one reason: this figure went stale TWICE in one afternoon — the comment said
three, the review corrected it to four, the fix made it three again — and the second staleness was in a
plan step written to prevent exactly that. The lesson is not "count more carefully", it is **do not carry
a count that something else can change**; that step now states none and tells its reader to derive it.

### D12 — STALE, already fixed: "never reaches the object store" is false; the real reason is non-comparability {#d12-fence-generation-reason}

**STALE-NO-OP 2026-07-30 (batched pass).** Grepped the tree for the exact phrase "never reaches the
object store" — zero hits. All three sites this entry could refer to (`CasRefCatalog.h`,
`CasServerRoot.h`, `CasServerRoot.cpp`) already state the non-comparability reason accurately (the
persisted `fence_generation` is a process-local counter that restarts at zero, not incomparable
because it never reaches the store). Fixed by an earlier round without this entry being closed; no
further edit made.

From the same review (P2, PROSE/Important). A comment justifies not using `CreatorFence.fence_generation`
for cross-process terminality on the ground that it "never reaches the object store". It DOES — Task 2
serialises it into the catalog entry. The true reason, and the one that survives scrutiny, is that it is
a `std::atomic<uint64_t>` local to `CasMountRuntime`, so another process's counter starts at zero and
counts its own bumps: a persisted process-local counter is **not comparable** across processes. Say that
instead.

### D13 — RESOLVED: `probeNonTerminalMountSlots` is the precedent for one of the two conservative cases, not both {#d13-precedent-scope}

**RESOLVED 2026-07-30 (batched pass).** Verified against `probeNonTerminalMountSlots`'s own body
(`CasServerRoot.cpp`): an absent slot is treated as "nothing to hold" (not conservative), an
undecodable body is pushed as held (conservative). Moved the `probeNonTerminalMountSlots` citation in
`CasServerRoot.h`'s `isCreatorFenceTerminal` doc to sit only beside the undecodable-body case.

From the same review (P3, PROSE/Minor). The comment cites it for both; it supports one. Narrow the
citation to the case it actually covers.

### D14 — RESOLVED: "nothing is lost by removing it" reasons about the wrong thing {#d14-nothing-lost-scope}

**RESOLVED 2026-07-30 (batched pass).** Deleted the clause in `CasRefLedger.cpp`'s
`cleanupOrphanedBirthCkptBestEffort` comment — the surrounding paragraph's safety argument does not
depend on it, so nothing is lost by the deletion either.

From the same review (P4, PROSE/Minor). The sentence argues about the NEXT birth, while the obligation it
needs to discharge is about the recovery that must still ground the namespace. Rewrite it to address the
recovery, or delete it — the deletion loses nothing, since the surrounding argument does not depend on it.

### D15 — STALE, already fixed: the committed header never says the production path is ungated {#d15-production-gap-undisclosed}

**STALE-NO-OP 2026-07-30 (batched pass).** `CasRefCatalog.h`'s `checkPublicationAdmittedOrThrow` doc
already carries a "NOT YET ENFORCED ON THE PRODUCTION REF-WRITE PATH" paragraph naming exactly what is
not gated and where the gap closes (Task 4). Fixed by an earlier round without this entry being closed;
no further edit made.

From the same review (P5, PROSE/Minor). Task 3 enforces "`Creating` forbids publication" at the catalog
level ONLY, by explicit ruling — production-path wiring is Task 4's step. That is not a defect, but the
committed header does not say so, so a reader takes the invariant as global. Add the one sentence naming
what is NOT gated and where it is closed. This is the disclosure the ruling was conditional on.

### D16 — `CasListLiarEndToEnd`'s inverted test calls a raw fixture "the real writer path" {#d16-liar-test-real-writer-path-overclaim}

From the Task 4-C review (M1). `TheSameHiddenNamespacesBlobSurvivesEvenWhenTheUniverseIsDeclaredAuthoritative`'s
comment (`gtest_cas_list_liar_end_to_end.cpp`, right above the `TEST` line) says: *"`hidden` was born
through the real writer path (`publishAt`, `birth=true`)"*.

**The false claim:** that `publishAt` IS the real (production) writer path.

**The verified truth:** `publishAt` is a raw fixture that drains into `writeRefLogTxnRaw`
(`cas_test_helpers.h`) — Task 4-B's map established that none of the ten raw-write helpers can route
through real birth (`CasRefLedger::resolveNamespaceLife`'s fresh-random mint); they structurally cannot
(INV-1 holes, out-of-order ids, tables with no `_ckpt`). What actually makes `hidden` catalog-`Live` here
is `writeRefLogTxnRaw`'s own `casAdmitEntry` call — a test-only admission shim, not the production path.
A reader who trusts the comment believes this test covers production birth; it does not.

**Fix:** replace "born through the real writer path" with something like "admitted into the catalog by
`writeRefLogTxnRaw`'s own `casAdmitEntry` call (a raw fixture, not the production birth path)".

### D17 — `writeRefSnapshotRaw`'s docstring states its scope but not the fix {#d17-snapshot-raw-no-admit-pointer}

From the Task 4-C review (M2). `cas_test_helpers.h`, `writeRefSnapshotRaw`'s doc (right above the
function) already says honestly that it "does NOT itself admit an entry, so a namespace this helper is
the ONLY writer for stays exactly as invisible to the catalog as it was before Task 4-C". `writeRefLogTxnRaw`,
its sibling raw fixture two functions down, calls `casAdmitEntry` for exactly this reason and says so.
The asymmetry is a trap for the next test author who writes a snapshot-only fixture, expects
`discoverUniverse` to see it, and gets silence with no pointer to why or what to do about it.

**Fix:** add one sentence at `writeRefSnapshotRaw`'s doc: "if the caller needs this namespace
discoverable, call `casAdmitEntry` first (see `writeRefLogTxnRaw`, below)." Not a behaviour change —
`writeRefSnapshotRaw` keeps its pre-existing scope; only the doc gains the pointer.

### D18 — `CasGcShardIncarnation.ListNamespacesFromRefsNotRegistry`'s header misdates the semantics it describes {#d18-list-namespaces-stale-task-header}

From the Task 4-C review (M4). `gtest_cas_gc_shard_incarnation.cpp:108-109`: *"Task 4: listNamespaces is
LIST-based; no registry involved."* / *"Publishing into ns A makes it appear in `listNamespaces("")`; ns
B absent."*

**The problem, not quite falsity:** `Pool::listNamespaces` (`CasPool.cpp`) genuinely is still LIST-based
today — unlike `Gc::discoverUniverse`, Task 4-C's catalog-authority switch never touched it, so the
SECOND sentence remains true. But the header's bare "Task 4" (with no letter suffix, unlike every
Stage-B header elsewhere in this file, which say "Task 4-C") now reads, after the sibling test above it
was rewritten for Review I5 to talk explicitly about "the switch from LIST-based discovery" to
catalog-authoritative `discoverUniverse`, as if THIS test's `listNamespaces` had undergone the same
switch. It has not — it is a different API. Left as-is, the next reader conflates the two.

**Fix:** either qualify which Task 4 (the original numbered task that added `listNamespaces`, distinct
from Stage B's "Task 4-C"), or add a one-clause disclaimer that `listNamespaces` is unaffected by the
catalog-authority change this file's OTHER test now documents, so the two are not read as the same
mechanism.

### D19 — non-defects noted so they are not re-litigated {#d19-task-4c-non-defects}

From the Task 4-C review (M3, M5):

- **M3** asked whether the `CasEmptyProof` attribution report showed the actual revert-and-rebuild
  experiment (`git checkout <base> -- cas_test_helpers.h`, rebuild, re-run) rather than a mechanistic
  trace. It did not, at review time. **Resolved separately, not as a doc fix**: the experiment was run
  for real in this same round (see the I4/review-response status sent to team-lead) and reproduced the
  identical failure against the pre-session header, closing the question with evidence rather than
  argument. `task-4c-report.md` should carry that procedure and result when it is next synced to the
  review-response commits — tracked as an open item, not a prose defect in itself.
- **M5**: the R11 test's per-family assertions are subsumed by the `deleteTotal() == 0` it also asserts
  ("an aggregate hides a family" only applies to a nonzero aggregate). No action — noted so a future pass
  does not re-file it as a finding.

### D20 — `Gc/CasGc.cpp`'s C1-fix comment justifies keeping `life` with a use that never happens {#d20-life-dead-variable-justification}

From the checkpoint re-review of `81ace46e089` (NEW-4). The comment above the `life` computation in
`fold()`'s per-namespace walk loop says `life` "is still needed for logging/key-construction parity with
the catalog-named case further down this loop". It is not, on the un-cataloged path: every use of `life`
sits inside `while (expected)`, and the un-cataloged branch (`!catalog_names_this_namespace`) `reset()`s
`expected` immediately, so that branch never enters the loop body and never reads `life` at all.

**The false claim:** `life` is used for logging/key-construction parity on the un-cataloged path.

**The verified truth:** on that path `life` is computed (a cheap, harmless `NamespaceLifeId::stageATransition`
call) and then never read. Not a behavior bug — no request is issued, nothing is logged with the wrong
key — purely a comment overclaiming a use that doesn't happen.

**Fix:** narrow the sentence to the catalog-named case only, or state plainly that `life` is unused (but
still computed, for uniform control flow) on the un-cataloged path.

### D21 — anomaly/test comments describe removal as a mechanism the tree does not have yet {#d21-removal-mechanism-not-yet-present}

From the checkpoint re-review (NEW-5). Two of this round's own comments describe an "ordinary removal" as
a present, live code path, when `CasRefCatalog` has **no entry-deletion API at all** yet:

- `Gc/CasGc.cpp`'s two recorded-anomaly messages ("dropped rather than folded (expected on an ordinary
  removal too, not only on damage, until Task 5's removal-evidence check lands)" and "expected until
  Task 5's removal-evidence check lands") — both mine, from C1/I1's fix.
- `gtest_cas_part_folder_access.cpp:1141`: "the catalog entry survives until Task 5's last step, so it is
  still readable here."

**The problem:** today the catalog entry survives UNCONDITIONALLY — there is no removal step at all, not
merely one that hasn't reached its last step yet — so neither anomaly can currently fire on a removal;
both only anticipate a mechanism Task 5 has not built.

**Fix:** reword all three as the future obligation they are ("once Task 5's removal exists, an ordinary
removal will look like this and must not raise the anomaly") rather than describing a present cost or a
present multi-step process.

### D22 — unclosed parenthesis in a new test comment {#d22-unclosed-parenthesis}

From the checkpoint re-review (NEW-6). `gtest_cas_ref_gc.cpp`'s new comment: *"(also correct, but for the
WRONG reason -- … that guard."* — an opening `(` with no matching `)`.

**Fix:** close the parenthesis (or remove it if the parenthetical was meant to run to the end of the
sentence, matching the surrounding punctuation style).

### D23 — a public header generalizes the no-mint guarantee from the resolver to the operation {#d23-nomint-generalized-to-operation}

From the Task 4b fix-round review (finding 1). `CasPool.h` says the readable resolver "NEVER creates a
namespace ... so a **probe** or an `if_exists` unlink against a table that was never opened **cannot admit
an entry into the pool-wide catalog**." The first clause is true of the resolver. The second is a claim
about whole operations, and it is false: `listDirectory` of that same never-opened table dir admits an
entry one line before it consults the resolver, and so do a table-dir `removeRecursive` and DROP DETACHED.

Graded important rather than minor because it is **the same defect class the round existed to fix** — a
load-bearing sentence in a public header asserting a property the code does not have — one level up from
where it was fixed.

**Fix:** narrow the two generalized sentences to the namespace-file surface they actually cover. The
commit message's "unbounded growth driven by removals of tables that never existed" has the same overreach
and cannot be edited; the header is what a future reader will consult.

The code residual behind this sentence is NOT deferred here — it is placed in the plan, see
{#d23-code-half-is-placed} below.

#### The code half is placed, not filed {#d23-code-half-is-placed}

Three ref-layer entry points still recover-and-mint on a read or a removal: `CasRefLedger::dropNamespace`
(minting **before** its own "a never-touched namespace's drop is a harmless no-op" guard),
`CasRefLedger::listRefs`, and `CasRefLedger::resolveRef` via `dropRefIfPresent`. All three are
pre-existing. They now live in the Stage B plan as an executing step with a red-first detector, because a
residual that lives only in a ledger is one context loss from forgotten.

### D24 — the `gc_enabled = false` rationale names a hazard that was not live {#d24-gc-enabled-rationale}

From the same review (finding 2). The recording fixture constructs storage with a **null context**, and
the background scheduler starts only under `if (context && gc_enabled && !read_only)`. No scheduler ever
ran, so the four zeros were never "holding only because the first 60s tick outlived the test" — they were
fenced by the null context. The original review's minor 5 carried an unverified premise and the
implementer inherited it; the commit message then repeats it as fact.

**Fix:** keep the setting — it makes the fence explicit and survives a fixture that later passes a real
context, which is exactly the silent arming worth pre-empting — but say that, not the timer story.

### D25 — stale count in a test file header {#d25-stale-count-nsfile-profile}

`gtest_cas_namespace_file_request_profile.cpp`: "The **two** `CasNamespaceFileDiskProfile` cases at the
bottom of this file fence it there" — the same commit added a third.

**Fix:** drop the number rather than correct it. A count in prose goes stale on the next addition; this is
the third counted-in-prose defect of the stage.

### D26 — `RefTableRuntime::life` documents recovery as its only resolver {#d26-reftableruntime-life-second-writer}

`CasRefLedger.h` still says `life` is "resolved ONCE per table-open, on the FIRST recovery attempt of this
runtime's lifetime (`ensureRefTableRecovered`)". There is now a **second** writer —
`namespaceFilesLifeIfReadable`'s step 1 — which resolves it without recovering at all.

**Fix:** name both writers. This one is load-bearing beyond prose: the planned "invalidate the cached life
when the entry is removed" step must invalidate against both, including the one that runs on a pure read.

### D27 — the `what` parameter's parity claim is loose {#d27-what-parity-claim}

From the Task 4c review (M4). `CasRefCkpt.h` says `what` plays "the same role it plays in
`checkRefCkptInvariants`". There, `what` is a fixed descriptor (`"cas_ref_ckpt encode"`); here production
passes the object **key** and the tests pass `"cas_ref_ckpt"`. Both name the thing, but the two message
families now read differently and the parity claim invites a reader to expect one shape.

**Fix:** say what `what` is for here (identifying the object in the message) instead of claiming parity
with a site that uses it differently.

### D28 — "Both halves are fenced" overstates the runtime fences' reach {#d28-both-halves-fenced-overstated}

From the same review (M2). The two runtime size fences drive only the birth-time `_ckpt` writer. A
fixed-capacity, non-heap field populated solely by the sealer or the snapshot publisher escapes all three
fences.

**Fix:** state which writer the runtime fences drive, so the gap is visible rather than implied closed.

### D29 — ragged reflow after an insertion {#d29-ragged-reflow-ckpt-format}

`CasRefCkptFormat.h`: the reflow leaves a short ragged line ("...Writing the whole body is what makes a /
stale field dangerous, and") that does not match the file's wrapping.

## 2026-08-03 Stage-B midpoint audit — batched prose findings {#midpoint-audit-batch}

From `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md`. Findings against the OLD plan and
the handoff are closed wholesale by the supersession note (`f8df7d9a5e8`) and are not re-listed
here. Two findings were fixed directly at batch time (the R1 closure note's two test-name
citations); items with a named executor say so and are NOT to be fixed out of band.

### D30 — Task 5 Files list names a file that never existed {#d30-phantom-removal-lifecycle-file}

Old plan, Task 5 Files: `Create: src/Disks/tests/gtest_cas_ns_removal_lifecycle.cpp` — never
created on any branch; the removal-lifecycle tests live in `gtest_cas_ref_catalog.cpp`
(`CasRefCatalogRemoval`) and `gtest_cas_gc_frontier_gate.cpp`.

**Fix:** none needed in the superseded plan; recorded so an inventory reader does not hunt for it.

### D31 — `resolveLifeOrSentinel` doc comment describes callers it no longer has {#d31-resolvelife-doc-stale}

`Pool/CasRefCatalog.h`: the doc says it serves "non-production discovery-path readers —
`recoverRefTableDetailed`, fsck's exact stream walk, `CasOrphanManifestSweep`'s active-key set";
none of those call it any more (all 17 callers are tests).

**Executor: plan task T1c deletes the function entirely** — do not patch the comment separately.

### D32 — Task 9 closure note: `mountpointObjectKey` attribution {#d32-mountpoint-attribution}

`2026-08-02-r1-verbatim-file-aliasing-closure.md` says `Layout::mountpointObjectKey` maps the loose
branch to `roots/<server_root_id>/<path>`; the `<server_root_id>/` qualifier is prepended by the
caller (`ContentAddressedTransaction::writeFile`), not the Layout. The conclusion stands; the
safety property is attributed to the wrong layer.

**Fix:** one sentence naming the caller as the qualifier's source.

### D33 — Task 9 closure note: RED-evidence provenance {#d33-r1-red-provenance}

The note's RED-evidence sentence is sourced to the scratch report
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task6-ns-file-contract-report.md`
(now-tracked history records it; the underlying ASan logs under `build_asan/` are the primary
evidence). The record is genuine; the citation should point at the durable logs or the audit.

**Fix:** cite the audit's corroboration (`{#report-t6}`) or the log paths.

### D34 — stale TLA runner prose in three places {#d34-tla-runner-prose}

(1) `models/2026-07-28-v9-phase-RESULTS.md` `{#fix-runners}` closing paragraph still says four
models "have no runner at all"; (2) `models/README.md` summary table still shows `(inline TLC)`
for those four rows; (3) `cas/06-tla-models.md` `{#running-models}` still describes the jar as
"(v2.19)" and `run_tlc.sh`/`run_ackfloor.sh` as assert-nothing drivers.

**Executor: plan task T7 lane B owns (1) and (2) as 10c closure content.** (3) is batch material:
rewrite the section around the pinned-jar gate and the asserted suite runners.

### D35 — `CaGcDestructiveGateCore` RESULTS overstate term coverage {#d35-destructive-gate-results}

The correspondence section maps the production `frontier_complete` formula without disclosing that
`UniverseAuthoritative` is pinned `TRUE`, uncontrolled, and false in production pre-flip.

**Executor: plan task T7 (the 10f disclosure step)** — one sentence in RESULTS + one comment at the
constant.

### D36 — `Gc/CasGc.cpp` comments citing internal documents {#d36-gc-comment-citations}

`frontier_complete`'s comment cites `2026-07-28-ref-rework-adjacent-findings.md {#r11-…}` by path;
`ContentAddressedTransaction::writeFile` ends with "(directive §namespace-file-requirements)";
`CasNamespaceLifeId.h` says "`Task 6` DELETES it". Reasons are good, citations are forbidden.

**Executor: T1c deletes the `CasNamespaceLifeId.h` one with the constant; the rest belong to F2's
sweep (T8 residual row item 5 records them).**

### D37 — old-ledger foundation anchor mislabels a Task-5 commit {#d37-ledger-foundation-anchor}

The (now closed) old ledger says foundation Tasks 0–4d run "through `d278d130024`", whose subject
is a Task 5 commit; the Task-4d boundary is `6a3dd6a9245`.

**Fix:** none — ledger closed; recorded for historians.

### D38 — scratch report task6-ns-file-contract-report names a renamed test {#d38-scratch-test-name}

The scratch report cites `StaleReaderAfterSameNameRebirthNeverSeesSuccessorBytes`; the committed
test is `HeldLifeAfterSameNameRebirthNeverSeesSuccessorBytes` (renamed during landing). The R1
closure note inherited the stale name — fixed at batch time; the scratch report itself stays as-is
(scratch artifacts are not maintained).

### D39 — T1a slice-record prose (from the T1a review) {#d39-t1a-slice-record-prose}

Three findings from `t1a-review.md`, all in slice documents under
`.superpowers/sdd/2026-08-02-cas-stage-b-remaining/` (F3's source-comment half was fixed directly):

- F1 (FALSE): `t1a-classification.md` claims `readable_catalog_after_observation_hook_for_test`
  has zero test users; three `CasRefWriterRuntimeIdentity` tests in `gtest_cas_ref_writer.cpp` use it
  and pin exactly the between-observations race. Site-2 disposition unaffected.
- F2 (IMPRECISE): "a live table reader … issues zero catalog GETs today" is unqualified — an armed
  `needs_stale_precommit_sweep` piggybacks a mutation (`commitRefChunk`, class-2 read) onto a read
  entry point; and the test comment's "no backend request whatsoever" holds under the test's
  disarmed-maintenance setup, over `CountingBackend`'s counted ops.
- F4 (record): the mandatory mutation-demonstration sentence is absent from `t1a-report.md`
  (substance present); the commit body does not name the report.

**Fix:** slice records are historical once the lane closes — carry the F2 qualification into any
future prose that quotes the zero-GET claim; no retro-editing round.

### D40 — T1-lane and T6a-review prose batch {#d40-t1-lane-prose}

From the T6a review (`t6a-review.md`): (1) the verdict artifact's "only unproven exit" enumeration
was incomplete (walk-never-starts silent exit; also structurally closed by `357cf7b963f`) — the
artifact stands on the corrected argument recorded in the review; (2) `checkpoint_unusable` bucket
counts the no-cursor mechanism, not the cause (unreadable-`_ckpt`-with-cursor reports as `held`);
(3) the `FrontierDeficit` doc paragraph is attached to `enum FrontierUnproven` rather than the
struct/member; (4) the artifact's table lists `no_catalog_entry` and `append_above_frozen_tail`
as live causes though both buckets are unfireable dead arms today.

From the T1b review (`t1b-review.md`): (5) the Task 9 re-check reached the right verdict on a
wrong ground (dedup-log rotation does NOT reach `listNamespaceFiles`; the correct caller set is
recorded in the review); (6) the closure note's `{#read-and-delayed-write-aliasing}` sentence
carries a pre-existing imprecision noted there.

From the fence-race RCA (`rca-fence-race.md`): (7) `Gc/CasGc.cpp` carries a stale comment ("a
namespace has no `_ckpt` until its first snapshot publication commits") — false since
`completeCreation` publishes `_ckpt` at creation; executor: F2 sweep (keep the fail-safe reason,
fix the invariant claim).

Dead-arm REMOVAL (three unreachable arms in the frontier walk, confirmed by the T6a review) is
CODE, not prose: placed with plan task T6 Step 1 (same file/region, mechanical, under that
review).

### D41 — T4-review prose batch {#d41-t4-review-prose}

From `t4-review.md`: (1) IMPRECISE — the T4 report's delta line says "1985 … 4 more than 1980"
(1980+4=1984; the 1985 count itself is verified correct — the delta arithmetic is what fails);
(2) FALSE — report Step 4 claims the two real-round numbers are "the same two"
`SourceRetirementIsAccountingNeutral` proves (that test never observes `txns_unapplied`);
(3) FALSE — plan T4 Step 6's second mutation names `enqueueWriterCleanupDuty` (no `Uncertain`
branch exists there) and a `Durable`-shaped test as its detector; the implementor's disclosure is
the accurate record. Also PROV-1: `build/t4_asan_gate3.log` is not attributable to a binary
(relink inside its run window) — cite the review's §0b run instead.

### D42 — T3-review prose batch (fsck dead-life residue arc) {#d42-t3-review-prose}

From `t3-review.md` (F5 plus its own PROSE section, on `laneg/t3-finish`):

1. **F5 — report omission.** The T3 closure report's mutation-demonstration section covers
   mutations (i)/(ii)/(iii) but never performed the sensitivity check for the NEW arm-(b) test
   (`CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`) that plan T3 Step 2 calls for
   (arm (b)'s token/value comparison commented out; mutation applied, captured, reverted). The
   review verified the sensitivity analytically instead (with arm (b) removed, `backend->fired()`
   never becomes true and every assertion in that test fails) and confirmed the test IS
   load-bearing; only the recorded evidence is missing. Executor: if this arc gets another commit,
   run the demonstration for real and fold the output into `t3-report.md`; otherwise leave as a
   known gap in that report.
2. **FALSE (was true before this arc, now stale):** `docs/superpowers/cas/BACKLOG.md`'s entry
   ending "*Consequence: `ca-decommission` refuses fail-close and `ca-fsck` posts a hard
   `lifeless_keys`*" describes the exact behavior the fsck slice (`babea62289b`) removed —
   `ca-decommission` never actually refused on these keys; the retired `CommandFsck` message
   claimed it did, and that claim was itself stale.
3. **FALSE as a live instruction:** `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`'s
   T3 Step 1 tells the implementer to assert arm (a)'s warning string "*catalog still owns victim
   namespaces*", which the T3 closure commit retired for an affirmative, count-bearing message.
   The landed test correctly asserts the new string; the plan text now instructs the opposite of
   the landed code.
4. **IMPRECISE (defensible as dated history):** `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md`
   quotes the retired arm-(a) string verbatim as if it were the current warning.
5. **NO FIX OWED, recorded so nobody "fixes" it:** `docs/superpowers/cas/08-testing-and-soak.md`'s
   exit-code list (`dangling`, `chain_broken`, `corrupted_runs`, `lifeless_keys`) is still correct
   after the `janitor_pending` split — `namespace_janitor_pending` is deliberately soft and does not
   belong in that list.

### D43 — T5-review prose batch {#d43-t5-review-prose}

From `t5-review.md`: (P1, FALSE) `t5-report.md` misquotes the draft pick's commit subject;
(P2, FALSE) the kept detector tests' comment describes the pre-pick `probe_a_period` default that
no longer exists (and mislabels 16 as non-sampling); (P3, IMPRECISE) the anti-vacuity comment in
`gtest_cas_holey_list_detector.cpp` still speaks of a "sampled mechanism" post-retirement;
(P5, IMPRECISE) `pre_fold_ref_drain` and `namespace_cleanup` carry no `PHASE N/18` comment —
16 of 18 phases annotated. (P4 — the plan's gate-recipe `.*`-suffix trap — fixed directly in the
plan, it was an executor hazard.)

### D44 — T6-review + codex T5/T6 prose batch {#d44-t6-review-prose}

From `t6-review.md`: (P1, IMPRECISE) `t6-report.md` §9 cites a stateless log whose banner reads
`@ 30108b5ee642 laneg/t3-finish` — the claim is true (binary fingerprinted to the arc tip; the
version string is stale cmake configure metadata), but the report should name that trap;
(P2, IMPRECISE) `test_cas_replicated_relink`'s restored-guard comment says "GC reclaiming them"
while the assertion is reclaim-at-least-one; (P3, NOTE) `gtest_cas_list_liar_end_to_end.cpp`'s
header still frames the kill shot as "the one shape arithmetic intake cannot save" — reword
together with the TEST-1 fix.

From the codex T5+T6 review (`tmp/codex_t5t6_review_answer.md`): (T5-1, FALSE as live safety
commentary) `CasGc.cpp` B1 comment still says "probe A covers the listing" and the bounded-walk
discussion still calls the `tail < cursor` class a "sampled store-quality detector" — probe A was
deleted by T5; rewrite both to state that arithmetic exact-key intake closes LIST omissions;
`t5-report.md` also claims the retained holey-LIST tests leave `probe_a_period` at its production
default (the seam was deleted) and misquotes the draft pick's subject (already in D43).
(T6, IMPRECISE) the `planManifestCursorPage` call-site comment in `CasGc.cpp` says the planner
reuses the round's catalog/checkpoint cut; it takes its own later cut (reviewed conservative —
no over-delete); correct when the T6-1 budget rework touches this path.

### D45 — Fable T6-review prose batch {#d45-fable-t6-review-prose}

From `t6-review-fable.md`: (P-1, FALSE, pre-existing) a `CasGc.cpp` comment claims a `chassert`
that does not exist; (P-2, STALE) the `fold_ref_intake` metric comment still defines the universe
as "hint ∪ sealed cursors ∪ catalog", contradicting the arc's own catalog-authoritative rewrite;
(P-4, STALE) residual Stage-A framings in untouched files — `CasOrphanManifestSweep.h`,
`CasGc.cpp` R4-register citation, `CasGc.h` sentinel comment, `CasLayout.h`; (P-5, IMPRECISE)
`t6-report.md` §7 "No edit was needed" is true of the fix wave only. Observation T-1 (anomaly and
carried-hold gate terms are structurally redundant with frontier incompleteness on current shapes —
no test isolates them) is carried to the T8 residual/hygiene row, not a docs item.

### D46 — stale suite spellings after the Cas-prefix normalization {#d46-stale-suite-spellings}

The Cas-prefix sweep (`3d959928e06`, `c7a9b2c17bc`) renamed 22 suites and 3 `TEST_P` instantiation
prefixes. Two live `BACKLOG.md` items still name `CaWiringGc.*` — stale from an EARLIER rename, so
the correct spelling is now `CasWiringGc`; they are findings text in a history-bearing file, hence a
docs item rather than part of the sweep. Also: the `## Testing` bullet in
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md` should be re-read once the
invariant landed — it now claims `Cas*` runs the whole set, which became TRUE with this sweep;
confirm the surrounding text does not still describe the curated-list era.

Carried, not a docs fix: `CasRefInstallSafetyDeathTest` is executed by NEITHER gate lane, because
both release and sanitizer builds define `NDEBUG` while the suite is gated on
`MEMORY_TRACKER_DEBUG_CHECKS` (needs `!NDEBUG`). Covering it requires a plain debug build in the
gate. Pre-existing; belongs to the T8 residual row.

### D47 — pre-existing provenance-citing comments {#d47-provenance-comments}

The comment policy forbids citing reviews, plans, tasks, or finding IDs in code (those artifacts
leave the branch; the comment must stand alone with the REASON). Four pre-existing sites still cite
theirs and were deliberately left out of the T6b cleanup to keep that commit scoped:
`gtest_cas_mount.cpp:260` ("codex round-3 finding 1"), `gtest_cas_ref_log_format.cpp:188` and
`gtest_cas_ref_snapshot_format.cpp:129` ("codex round-2, finding 3"),
`gtest_cas_upload_fanout.cpp:975` ("Test 6c (codex stage-1 review, Critical)"). Keep each sentence's
constraint, drop the citation.

### D48 — T6b-review prose batch {#d48-t6b-review-prose}

From `t6b-review.md`: (P1, IMPRECISE) commit `5295d6c54ae`'s subject and the plan's Slice-2
checklist line say "page/byte/recovery budgets" — no byte budget exists; the correct spelling is
"page/namespace/recovery" (the report's heading already says so; the commit subject is immutable
history, the plan line is fixed separately); (P2, FALSE) the report's Slice-1 claim "each
`redelete`/`spared`/`graduated` entry maps to exactly one `OutcomeEntry` … bounded by construction"
— `graduated` maps to none and `spared` was uncapped (prose form of finding C1, closed by the C-fix
slice); (P3, IMPRECISE) `GcRoundWorkBudget::max_sweep_namespaces` comments describe a per-PAGE cap
while the counter is per-round cumulative — equivalent only while the sweep takes one page per
round; the comment states more than the field guarantees.

# Deferred documentation and comment fixes {#deferred-docs-fixes}

**Policy, adopted 2026-07-30 by user directive.** Findings whose entire content is
non-executing — comments, doc text, report and commit-message claims — are collected HERE and
executed in ONE pass, not in per-task fix rounds.

The reason is measured, not stylistic. Stage B Task 1c spent **three consecutive fix rounds** on
comment and claim accuracy with **zero defects in the code or the tests**, and each round introduced
the next round's finding, because fixing false prose is itself a prose-writing act with the same
failure rate. Rounds are for defects that can break something. Prose is batched.

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

## Open entries

### D1 — `parseNamespaceFileKey`'s refusal contract over-claims {#d1-parse-namespace-file-key-contract}

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

### D2 — a cited test covers one of the two key families the citing code handles {#d2-cited-test-one-family}

From the Task 1c review (MINOR-3). Retrieve the exact citation and both families from
`.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-1c-review.md` MINOR-3 when the pass
runs; the finding is that the comment cites a test as covering the code's behaviour when it exercises
only one of the two key families that code handles.

**Fix:** either cite a test per family, or narrow the claim to the family actually covered.

### D3 — the interconversion fence's rationale is a non-sequitur {#d3-interconversion-rationale}

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

### D4 — the fence's "AND IT REACHES NO PROSE" paragraph is stale by one commit of its own round {#d4-fence-prose-paragraph-stale}

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

### D5 — the report overstates MINOR-4's fix {#d5-report-overstates-minor4}

From the same re-review (Minor 2). `task-1c-report.md` says "both comments now state the claim
qualitatively … and say explicitly that no count is given **and why**". Only `CasFsck.h` says why;
`InterpreterSystemQuery.cpp` gives no count and no reason for giving none. Someone auditing MINOR-4 from
the report checks the SQL site, finds no such statement, and reopens a fixed item.

**Fix:** narrow the report sentence to the site that carries the reason.

### D6 — the exit-code caveat is scoped to summary scans though the exclusion is unconditional {#d6-exit-caveat-scope}

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

### D7 — `worstCaseEntryFoldReservationBytes`'s doc claims a coverage the sum does not have {#d7-reservation-doc-coverage}

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

### D8 — the registry row's line-cap justification invokes a worst case that exceeds the cap {#d8-line-cap-justification}

From the same review (Minor 11, PROSE/IMPRECISE). `CasFormat.cpp`: "the line cap is tight (4 KiB)
because one entry's record is small and bounded by `kMaxNamespaceBytes`". The record is bounded by
`kMaxNamespaceBytes` times the 6-byte worst-case escape (3072 bytes) plus the unmentioned 255-byte
`server_root_id` (another 1530 escaped) — so the worst case the sentence invokes to justify the cap
EXCEEDS it. Fix once the code fix for that overflow has landed, so the sentence describes the shipped
behaviour.

### D9 — `foldSealFixedBytes`'s doc omits the meta line and calls a floor a constant {#d9-fold-seal-fixed-bytes-doc}

From the same review (Minor 12, PROSE/IMPRECISE). The doc says "header + trailer, zero entries", but
`encodeFoldSeal` writes a **meta** line between them (`g`/`pg`) which the measured value correctly
includes. Also: the value is measured at `generation = 0` / `n = 0`, so it is a FLOOR, not a constant —
a real seal's decimal widths add tens of bytes. Harmless (floor division leaves up to one whole
reservation of slack), but "fixed" hides that it is a floor. Fix both in one clause.

### D10 — "predicate (2) equality" overstates what the test asserts {#d10-predicate-2-equality-wording}

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

### D12 — "never reaches the object store" is false; the real reason is non-comparability {#d12-fence-generation-reason}

From the same review (P2, PROSE/Important). A comment justifies not using `CreatorFence.fence_generation`
for cross-process terminality on the ground that it "never reaches the object store". It DOES — Task 2
serialises it into the catalog entry. The true reason, and the one that survives scrutiny, is that it is
a `std::atomic<uint64_t>` local to `CasMountRuntime`, so another process's counter starts at zero and
counts its own bumps: a persisted process-local counter is **not comparable** across processes. Say that
instead.

### D13 — `probeNonTerminalMountSlots` is the precedent for one of the two conservative cases, not both {#d13-precedent-scope}

From the same review (P3, PROSE/Minor). The comment cites it for both; it supports one. Narrow the
citation to the case it actually covers.

### D14 — "nothing is lost by removing it" reasons about the wrong thing {#d14-nothing-lost-scope}

From the same review (P4, PROSE/Minor). The sentence argues about the NEXT birth, while the obligation it
needs to discharge is about the recovery that must still ground the namespace. Rewrite it to address the
recovery, or delete it — the deletion loses nothing, since the surrounding argument does not depend on it.

### D15 — the committed header never says the production path is ungated {#d15-production-gap-undisclosed}

From the same review (P5, PROSE/Minor). Task 3 enforces "`Creating` forbids publication" at the catalog
level ONLY, by explicit ruling — production-path wiring is Task 4's step. That is not a defect, but the
committed header does not say so, so a reader takes the invariant as global. Add the one sentence naming
what is NOT gated and where it is closed. This is the disclosure the ruling was conditional on.

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

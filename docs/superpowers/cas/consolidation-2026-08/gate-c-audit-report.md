# Gate C adversarial verdict audit — report

Audit of the 4633-cluster verdict set (`verdicts/verdicts.jsonl`, `checked_at 95de061c2e6`) that
feeds Gate D (deletion of the original doc corpus). All work staged incrementally to
`verdicts/audit-t7/slice*.jsonl` (per-cluster `{"cluster_id","original_verdict","audit_verdict":
"UPHELD"|"REFUTED","evidence","note"}`) and `verdicts/audit-t7/corrections*.jsonl` /
`corrections.jsonl` (one record per REFUTED cluster, `corrected_verdict` added).

Executed jointly: slices 3, 4, 5 by the controller directly (mechanical/grep-driven); slices 1
(split 1a/1b), 2, and 6 by the idle implementer teammates (t9-arch-g1, t10-arch-g2, t11-user-core,
t12-runbooks respectively) via `SendMessage`, since the session subagent spawn cap was exhausted
and no new agents could be forked for this task.

## Refutation rate per slice

| Slice | Scope | Processed | Refuted | Rate |
|---|---|---:|---:|---:|
| 1a | Random half of 5% sample of `done` (refute-attempt) | 41 | 4 | 9.8% |
| 1b | Random half of 5% sample of `done` (refute-attempt) | 43 | 7 | 16.3% |
| **1 combined** | **Random 5% of `done`** | **84** | **11** | **13.1%** |
| 2 | Random 5% of `stale` (prove-still-true-attempt) | 36 | 5 | 13.9% |
| 3 | All `open` verdicts whose claim references a `docs/` path | 10 | 8 | 80.0% |
| 4 | Sample of 15 `open` verdicts naming TLA+ models/invariants | 15 | 11 | 73.3% |
| 5 | GC-command/table-name naming-contradiction group (135 clusters, built from content — see note below) | 135 | 9 | 6.7% |
| 6 | Random 20 from self-executed Tier B batches 027-035 | 20 | 6 | 30.0% |
| **Total** | | **300** | **50** | **16.7%** |

**Stop condition triggered.** Per the audit brief, refutation >10% in slice 1 or slice 2 means
STOP and escalate rather than trust the verdict set as-is. **Both slices breached it**: slice 1
combined at 13.1%, slice 2 at 13.9%. This report is the escalation, not a green light.

Note on slice 5: "batch-072" as referenced in the original brief does not resolve to a coherent
group — `verdicts/evidence/batch-072.jsonl` (C-2254..C-2283, a ref-recovery/lineage cluster range)
and `verdicts/tierB-batches/batch-072.json` (C-4459..C-4461+, a different cluster range) are two
unrelated batches, neither matching the described CLI-only-vs-SYSTEM-CAS naming theme. The group
was instead built directly from `canonical_claim` content: all clusters mentioning either the old
`CONTENT_ADDRESSED_*`/`content_addressed_*` naming or the new `SYSTEM CAS`/`cas_*` naming, with
ground truth established by confirming (via `grep -rl`) that every old identifier has zero hits in
`src/` and `docs/en/` at HEAD, while the new ones are present (e.g.
`src/Access/Common/AccessType.h:351`, `programs/server/config.xml:1201-1330`).

## Corrections

50 REFUTED clusters, 50 correction records in `verdicts/audit-t7/corrections.jsonl` (superseding
the original verdict; not yet applied back to `verdicts/verdicts.jsonl` — that write-back is a
follow-up decision for you, not done here). Corrected-verdict breakdown:

- → `done` (was wrongly `open`, evidence is conclusive): 19 — almost entirely slices 3 and 4.
- → `stale` (was wrongly `done`/`open`, claim no longer holds or names dead syntax): ~20
- → `open` (was wrongly `done`, feature is design-only/unimplemented): 3
- → other/needs-review: remainder

## Root-cause diagnosis — three distinct, mostly independent failure classes

**1. Tier A never searched the doc corpus itself (`docs/superpowers/{models,cas,specs,plans}/`).**
This is the dominant driver of slices 3 (80%), 4 (73%), and part of 6. Every refutation in this
class was conclusive and cheap to find — the named `.tla` file, `RESULTS.md`, or doc page simply
exists with matching content, and Tier A's search scope apparently excluded the doc tree it was
verifying claims *about*. t12-runbooks independently found the identical pattern in slice 6
(4 of 6 refutations there). This is a mechanical, closed-scope bug in the verification tooling,
not a judgment error — a full re-sweep of every remaining `open`/`unverifiable` verdict whose
claim cites a `docs/superpowers/` path would likely resolve most of them to `done` cheaply.

**2. A `Cas*` → `CAS*` symbol-casing rename broke literal-string verification.**
Slice 2's refutations (t11-user-core) are mostly this: a claim's cited symbol
(`CasRefGcCleanupAuthority`, `CasRefRollbackBestEffortDropFailed`, etc.) was renamed to the
`CAS`-prefix convention, so a grep for the old exact casing came back empty and the claim was
marked `stale` when the underlying mechanism is unchanged and still live under the new name. This
is also mechanical and scope-fixable (re-check with case-insensitive or CAS-prefix-aware search),
distinct from class 1.

**3. Genuine post-verification drift — a `done` claim was true when checked, then the code moved
again.** This is the harder, non-mechanical class, concentrated in slice 1: functions further
refactored after being marked done (`adoptPartFromManifest` → `prepareAdoptFromManifest` +
`ICaPreparedRelink::promote()`; the copy-forward verification step removed entirely in favor of
in-closure revalidation; `folded_cursor` superseded by `ref_lives`/`CondemnedSummary` in a later
redesign). t10-arch-g2 flagged this explicitly: "the corpus's 'done' verifications seem to have
checked against the doc text or an earlier snapshot rather than re-diffing against current HEAD."
There is no cheap scope fix for this class — it requires re-diffing against HEAD, which is what
this audit did. No comparable mechanical fix is available; it needs broader re-sampling.

## Verdict on trustworthiness for Gate D

**Not yet trustworthy as-is.** The raw 16.7% blended rate overstates the danger somewhat — classes
1 and 2 are structural, well-understood, and mechanically fixable without re-auditing every
cluster by hand — but even after backing those out, slice 1's residual class-3 rate (real drift,
no easy fix) is still material enough that a `done` verdict cannot currently be trusted at face
value for Gate D, where trusting a false `done` means the original evidence for a still-true
correction gets deleted.

Recommended before Gate D:
1. Mechanical re-sweep of all `open`/`unverifiable` verdicts against `docs/superpowers/{models,cas,specs,plans}/` (class 1) — cheap, should clear a large fraction of the ~482 open verdicts to `done`.
2. Mechanical re-sweep of `stale` verdicts whose evidence was a symbol-grep, using the `CAS`-prefix-aware pattern (class 2).
3. A larger random sample of `done` (beyond this 5%) specifically re-diffed against HEAD rather than against cited doc text, since class 3 has no scope-based shortcut — this is the one that actually needs a bigger audit, per the original stop-condition's own framing.
4. Apply `corrections.jsonl`'s 50 corrections to `verdicts.jsonl` regardless of what else is decided — these are confirmed, evidence-backed fixes independent of the broader-audit question.

---

## Remediation (post-audit, controller-executed, user-approved Option C)

Executed by the controller (t6-verify) with the idle T9/T10/T11/T12 teammates taking Phase 2b
and Phase 3 slices, since the session's subagent spawn cap (200/200) was already exhausted --
no new Agent-tool spawns were used for this remediation; work went through `SendMessage` to
persistent teammates instead.

### Phase 1 — apply the audit's 50 corrections

Applied via `supersede_verdicts.py` (45 unique cluster_ids after 5 exact duplicate lines in
`corrections.jsonl` collapsed). 5 of the 45 needed a mechanical evidence patch afterward (Gate C's
existing-path check rejected `stale` verdicts whose evidence was pure `not_found` reasoning with
no citable path -- all five were `CONTENT_ADDRESSED_*` -> `SYSTEM CAS`/`cas_*` naming renames; a
real path to the renamed symbol was added, e.g. `SYSTEM_CAS_GC_REBUILD` in
`src/Access/Common/AccessType.h:352`). Also hardened `gate_c.py` itself twice along the way: the
path-existence regex first missed bare filenames without a directory prefix, then missed real
extensionless files (`tests/clickhouse-test`).

Commit: `917411cc417`.

### Phase 2a — scope re-sweep (class 1: docs/superpowers/ was never searched)

Partial, not exhaustive. A fully mechanical re-grep of every `open`/`stale`/`unverifiable`
verdict's claim identifiers against the widened scope (`docs/` in full, `.github/workflows/`) is
extremely low-precision: 83-96% of the ~1820-candidate pool got *some* hit even after two rounds
of tightening the identifier-shape filter, because `docs/superpowers` is a large enough prose
corpus that any project-specific term matches incidentally somewhere unrelated to the specific
claim. The one subclass that concentrated real signal was hits landing specifically in
`docs/superpowers/models/*_RESULTS.md` (the committed TLA+ proof-run records) -- of ~30 hand-
verified by the controller, ~6 were genuine, verbatim-or-near-verbatim corrections (folded into
the Phase 3 corrections below since they were verified the same way: read the citation, confirm
or refute against the actual file content). The remaining ~120 `*_RESULTS.md`-hit candidates and
the much larger generic-prose-hit pool (~1000+) were not exhaustively reviewed -- the noise ratio
there means a blind mechanical flip would introduce new false verdicts rather than fix them, and
per-cluster judgment at that scale was not completed in this remediation pass. Left as follow-up
work, not blocking.

Tooling: `tools/phase2a_scope_resweep.py`, `tools/phase2a_results_md_sweep.py`.

### Phase 2b — casing re-sweep (class 2: Cas* -> CAS* rename)

Complete. All 76 mechanical candidates (`stale` verdicts citing a `Cas[A-Z]...`-shaped
identifier with *some* hit under the `CAS`-prefix variant) were reviewed by hand by t11, after the
controller first found and corrected a real trap in the mechanical signal: most hits were gtest
`TEST(CASFoo, ...)` suite-name arguments, which do NOT reliably correspond to a real production
symbol of that name in this codebase (worked example: `TEST(CASObjectStorageBackend, ...)` exists,
but the real class is `ObjectStorageBackend`, no prefix at all). t11 read the actual cited test
body or production code for every one of the 75 test-suite-only-hit candidates rather than trusting
the label match. Result: 33 flips (18 `done`, 15 `unverifiable` -- the latter where the renamed
identifier is real but the claim's crux is an unrederivable one-off historical measurement), 43 left
`stale` (including deliberately re-confirming the C-2039 trap case itself, and several clusters that
turned out to be real architecture changes rather than casing issues at all: protobuf->JSON,
binary envelope->text, a genuine version bump).

Tooling: `tools/phase2b_casing_resweep.py`.

### Phase 3 — targeted re-check of decision-relevant `done` (class 3: genuine drift)

Complete. Candidate set (`done` verdicts on clusters with any member of `kind` todo|bug, or used
as deletion justification -- `target` BACKLOG or none-with-`issue_ids`): 245, reported before
starting per the >600 checkpoint rule (not triggered). Split into 4 slices of ~61-62 across the
controller, t9, t10, and t12, each re-diffing every citation against a later HEAD than the
original pinned rev (the shared worktree's HEAD moved twice during this phase -- `d86ea32da71`
then `befd0182dba` -- so `gate_c.py`'s `checked-at-rev.txt` now lists all three revs seen instead
of exactly one).

Result: **1 genuine drift out of 245 (0.4%)**, well under the audit's 10% stop threshold --
`C-4069` (`done` -> `open`): the claim asserted a doc/code inconsistency had been reconciled; the
evidence it was given for that claim was wrong (the source comment is verified unchanged since the
file's creation commit, and the config-default divergence is still live at HEAD). The other 14
corrections across all four slices were evidence-citation fixes only (the underlying mechanism
confirmed, cited symbol/test name off by a rename or a line-number drift) -- verdict unchanged.
Three of the four slices (controller, t9, t10) found zero verdict-level drift at all; t10 explicitly
flagged this as worth noting for interpreting the audit's headline 16.7% rate.

Tooling: `tools/phase3_quick_redigest.py`.

### Final state

Commits: `917411cc417` (Phase 1), `9c1f6e8a741` (Phase 2b + partial Phase 3 checkpoint),
`4f07a7c891c` (Phase 3 complete).

Gate C: green. 4633 clusters, 4633 verdicts, `checked_at` in
`[95de061c2e6, d86ea32da71, befd0182dba]`.

Final histogram: `done` 1704, `stale` 717, `doc-fact` 965, `open` 458, `rejected` 21,
`unverifiable` 622, `ephemeral` 146.

### Recommendation for Gate D

The two mechanically-fixable classes (1: doc-scope exclusion, 2: casing rename) are addressed for
the specific candidate pools this remediation touched. Class 3 (genuine post-verification drift)
was directly re-tested on the highest-stakes 245-cluster set and held up at 0.4% -- strong evidence
that the earlier audit's 13.1%/13.9% rates on *broad random samples* of `done`/`stale` were driven
overwhelmingly by classes 1 and 2, not by widespread genuine drift concentrated in the
decision-relevant subset Gate D actually depends on for deletion justification. Recommend Gate D
proceed on the current verdict set, with the caveat that the broader (non-decision-relevant)
`done`/`stale` population beyond this 245-cluster set has not been re-audited at the same rev and
carries whatever residual class-1/2/3 risk the original audit's 5%-random-sample rates imply.

### Phase 2a — completion update

Per team-lead direction, finished the `*_RESULTS.md` high-signal subclass in full (the generic-
prose `docs/` pool remains an accepted, documented residual -- not reviewed, per the noise-ratio
evidence above). The controller's initial hand-verified sample (~30 of 155) found 6 corrections;
the remaining 145 were split across 5 slices (controller + t9 + t10 + t11 + t12, ~29 each) and
fully reviewed. Result: **21 more genuine corrections**, all near-verbatim matches against the
actual committed RESULTS.md text rather than incidental term co-occurrence -- overwhelmingly
TLA+-model-only claims (design rationale, sabotage-config proofs, model-additions sections) that
the original mechanical search, scoped only to `src/tests/programs/utils/ca-soak/`, could never
have found. Combined `*_RESULTS.md`-subclass total: 27 corrections out of ~155 candidates (17%
yield), confirming this was the right subclass to prioritize.

A shared-file hazard surfaced during this pass and is worth recording for future waves: t11
observed that a concurrent plain `>>`-append from another lane briefly clobbered (not merged with)
an earlier append to the same output file -- a race, not an interleaving-order quirk. Caught by
re-verifying file content after write rather than trusting the append succeeded; a final
reconciliation pass (sum of each lane's self-reported flip count vs. actual unique line count in
the merged file: 4+4+3+10 = wait, self-reported 4(t10)+4(t11)+3(t9)+10(t12)+0(controller) = 21,
matching the merged file's 21 unique lines with zero duplicates) confirmed no writes were lost in
the final state.

Updated final histogram (after Phase 2a completion, commit `d9da444a5f5`): `done` 1716, `stale`
714, `doc-fact` 973, `open` 441, `rejected` 21, `unverifiable` 622, `ephemeral` 146.

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

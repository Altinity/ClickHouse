# Gate M report — map-phase coverage and audit {#gate-m-report}

Date: 2026-08-04. Status: **PASSED** (mechanical green + two-wave sampled/supplementary audit complete).

## Mechanical gate {#mechanical}

`tools/gate_m.py`: `corpus=421 accounted=421 records=6094 errors=0` (exit 0).

- Every corpus file accounted exactly once (`records:N` or `ephemeral:<reason>`); PROBE.* excluded (duplicate probe coverage of one file).
- Accounting identities: 5550 codex record lines; 5667 distinct (record, cited-file) pairs == sum of manifest `records:N` (recomputed from jsonl ground truth by `tools/fix_manifest_counts.py` — manifests are derived bookkeeping, jsonl is truth).
- Ephemeral files: 4 (`RUN_HISTORY.md`, `Structurizr.md`, `non-doc-debris.md`, one sdd `progress.md`) — all with recorded reasons.
- One taxonomy ruling: 6 B016 records reclassified `roadmap`→`todo` (codecs-v3 plan phases; controller ruling, metadata-only).

## Audit wave 1 — 21 sampled files {#audit-wave-1}

`extracted/audit-m.jsonl` (94 records), verdicts in `gate-m-audit-verdicts.md`.
Result: 3 OK / 11 MINOR / 7 MATERIAL. Notable: `01-architecture.md` massively under-extracted (29 misses incl. the CURRENT ack-floor GC design — only the superseded design had been captured); `review1.md` missing its #1 blocker finding; `upstream-patch-inventory.md` (B051 mid-flight capture suspect) came back clean.

## Audit wave 2 — supplementary, 176 files {#audit-wave-2}

`extracted/audit-m2.jsonl` (450 records), verdicts in `gate-m-audit-verdicts-2.md`.

| Group | Files | OK | MINOR | MATERIAL | Records |
|---|---|---|---|---|---|
| A — cas core docs | 14 | 2 | 3 | 9 | 93 |
| B — worklogs | 20 | 1 | 12 | 7 | 72 |
| C — reports | 69 | 31 | 19 | 19 | 214 |
| D — spec/plan side-sections (73 of 152, grep-selected) | 73 | 30 | 40 | 3 | 73 |
| Total | 176 | 64 | 74 | 38 | 450 |

Top misses recovered: `reviews.md` SEC-4..SEC-9 security findings (31 records, exhaustive re-pass); archaeology `00-REPORT.md` risk register + duplication catalogue + open questions (30); archaeology `06-tests.md` test-map/coverage-gap tables (20); `11-walkthrough.md` crash-points/config/doc-drift tables (18); `03-writer-protocol.md` protocol contracts (16).

Pattern confirmed: codex map handled main bodies well; systematic misses were (a) dense reference tables in core docs, (b) side-sections (self-review, out-of-band notes, risks, open questions) in worklogs/reports. The D-group targeted sweep validated that spec/plan main bodies did not need re-auditing.

## Corpus totals entering Phase C (clustering) {#totals}

6094 records total = 5550 (codex map) + 94 (audit wave 1) + 450 (audit wave 2).
Known debris found during audit (recorded in `non-doc-debris.md`): 0-byte duplicate worklog, case-collision worklog artifact, embedded git-diff dump in `codex_r6_findings.md`.

## Process incidents (for the record) {#incidents}

- codex read-only-sandbox soft-fail (exit 0, no output): B015, B027 — caught by expected-vs-actual manifest diff; retried clean. Rule for future fan-outs: never trust the dispatcher's exit code alone; diff the expected batch-id set against on-disk outputs.
- Stray B051 retry raced the working tree post-commit; committed state restored; B051's largest file audited as mitigation (clean).
- Fork-result staging: results that lived only in a message (not a file) were nearly lost twice; every fork now stages a `.jsonl` and names the path.

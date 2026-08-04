# Task 4: Gate M — mechanical check (Phase 1 of 2)

Scope: mechanical accounting/integrity gate only. The sampled "what was missed" audit
(brief Step 3) is dispatched separately by the controller and is NOT covered here.

## Verdict

**FAIL** (exit 1) — 6 errors, all the same class: `bad kind roadmap` in `extracted/B016.jsonl`
(ids `B016-071` .. `B016-076`). Everything else is clean: coverage is complete, JSONL is
well-formed, source paths resolve, and record ids are unique.

```
corpus=421 accounted=421 records=5550 errors=6
ERR B016-071: bad kind roadmap
ERR B016-072: bad kind roadmap
ERR B016-073: bad kind roadmap
ERR B016-074: bad kind roadmap
ERR B016-075: bad kind roadmap
ERR B016-076: bad kind roadmap
```

### Content problem, not fixed here

The six `B016-0NN` records (all citing
`docs/superpowers/plans/2026-07-15-cas-codecs-v3-phase2-control-plane.md#dag-phase3` through
`#dag-phase8`) were tagged `kind:"roadmap"`, a value outside the closed taxonomy
(`contract`, `design-decision`, `rejected-alternative`, `bug`, `todo`, `runbook-fact`,
`user-fact`, `metric`, `setting`, `history`). This is an extraction-content defect from the
Task 3 map step, not an accounting/bookkeeping bug, so per instructions I did not edit the
jsonl or silently widen the gate's allowed-kind list — that would be a taxonomy decision, not
mine to make unilaterally. Options for whoever picks this up: re-run `run_map.sh` for `B016`
after deleting its outputs so the batch is re-extracted with a valid kind, or explicitly decide
these six claims belong to a kind already in the list (closest candidates are
`design-decision` or `history`, since they describe planned/ordered phases of an accepted
design, not open questions). **Gate M must be re-run to green before Task 5 starts.**

## PROBE exclusion

`extracted/PROBE.jsonl` + `PROBE.jsonl.manifest` are a single-file probe over
`docs/superpowers/cas/2026-08-03-list-trust-verdict.md`, which is also covered by a regular
batch. `tools/gate_m.py` skips both `PROBE.*` files entirely (both the manifest-accounting pass
and the jsonl-integrity pass) so the probe's 36 records and its manifest line don't collide with
the regular per-file exactly-once check.

## Accounting identities (verified)

- Non-`PROBE` manifest files: 51 (`B001`..`B051`).
- Sum of manifest `records:N` lines = **5667**.
- Distinct `(record id, cited-file)` pairs across all non-`PROBE` jsonl records (sources
  deduplicated per record, `#anchor` stripped) = **5667**. Matches.
- Total non-`PROBE` record lines (jsonl rows) = **5550**. Matches expected.
- The gap between 5667 pairs and 5550 records (117) is because some records cite more than one
  source file — expected for cross-referencing claims, not an error.

Both identities match the expected values exactly; no discrepancy to investigate.

## Ephemeral files (4)

Manifest lines whose status is `ephemeral:...` instead of `records:N` — files scanned by the map
step and deliberately yielding zero durable-claim records, with reason:

| file | batch | reason |
|---|---|---|
| `utils/ca-soak/scenarios/RUN_HISTORY.md` | B038 | entire file is an appended scenario run-status/chronology table, no standalone durable claim |
| `docs/superpowers/Structurizr.md` | B038 | artifact-preparation / local Structurizr usage instructions, no durable CAS claim |
| `docs/superpowers/cas/consolidation-2026-08/non-doc-debris.md` | B043 | repo-root debris inventory explicitly marked out of the document corpus |
| `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/progress.md` | B043 | execution ledger / task-status chronology, no standalone durable claim extracted |

These 4 count toward `accounted=421` (every corpus file accounted for exactly once) but
contribute 0 to `records=5550`.

## Per-group record totals

Grouped by the `group` column in `corpus-manifest.tsv`, counting each record once per distinct
cited source file (so this sums to 5667, matching the pairs identity, not 5550):

| group | records |
|---|---|
| `docs/superpowers/specs` (specs) | 1317 |
| `docs/superpowers/cas` (cas) | 1169 |
| `docs/superpowers/plans` (plans) | 1075 |
| `docs/superpowers/reports` (reports) | 875 |
| `.superpowers` (sdd — all under `.superpowers/sdd/`) | 503 |
| `docs/superpowers/worklogs` (worklogs) | 214 |
| `docs/superpowers/models` | 205 |
| `utils/ca-soak` | 118 |
| `other` | 101 |
| `docs/en` | 63 |
| `.claude` | 27 |
| **total** | **5667** |

The brief's requested bucket names (specs/plans/reports/worklogs/cas/sdd/other) map 1:1 onto the
corpus-manifest `group` values except that `models`, `utils/ca-soak`, `docs/en`, and `.claude`
don't fold into any of the seven named buckets — left as their own rows rather than force-fit
into "other", since `corpus-manifest.tsv` already treats `other` as its own distinct group (9
files, 101 records) separate from these.

## Concerns

1. **Gate is currently RED** — do not start Task 5 until the `B016` `kind:"roadmap"` issue above
   is resolved and a clean re-run is committed.
2. The per-group table counts (record, file) pairs, not distinct records — a record citing two
   files is counted once in each of that file's groups. This matches the pairs identity (5667)
   by construction but is not the same denominator as `records=5550`.

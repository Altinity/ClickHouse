# Task 4: Gate M — mechanical check (Phase 1 of 2)

Scope: mechanical accounting/integrity gate only. The sampled "what was missed" audit
(brief Step 3) is dispatched separately by the controller and is NOT covered here.

## Verdict

**PASS** (exit 0). Coverage is complete, JSONL is well-formed, source paths resolve, and record
ids are unique.

```
corpus=421 accounted=421 records=5550 errors=0
```

### Content problem found, then fixed under controller ruling

On the first run, 6 errors surfaced — all `bad kind roadmap` in `extracted/B016.jsonl` (ids
`B016-071` .. `B016-076`, citing
`docs/superpowers/plans/2026-07-15-cas-codecs-v3-phase2-control-plane.md#dag-phase3` through
`#dag-phase8`). `roadmap` is outside the closed taxonomy (`contract`, `design-decision`,
`rejected-alternative`, `bug`, `todo`, `runbook-fact`, `user-fact`, `metric`, `setting`,
`history`) — an extraction-content defect from the Task 3 map step, not an accounting/bookkeeping
bug, so I reported it rather than silently editing content or widening the gate's allowed-kind
list.

The controller ruled these six records are plan work items (codecs-v3 phases 3-8) and their
correct kind is `todo`; the verification phase will verdict them done/stale later. Under that
ruling, `tools/reclassify_kind.py` was added: it hardcodes the exact 6 ids and rewrites only
their `"kind"` field from `"roadmap"` to `"todo"`, byte-identical otherwise (verified by diff —
each of the 6 lines changes only the `kind` value; claim text, sources, and
`suggested_target:"roadmap"` are untouched). Gate M was re-run afterward and is green
(`errors=0`); the two accounting identities below were re-verified unchanged.

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

1. The per-group table counts (record, file) pairs, not distinct records — a record citing two
   files is counted once in each of that file's groups. This matches the pairs identity (5667)
   by construction but is not the same denominator as `records=5550`.
2. `suggested_target:"roadmap"` on the 6 reclassified records was intentionally left untouched
   (the ruling scoped the fix to the `kind` field only) — it's a separate field feeding a later
   clustering/placement step, not part of the kind taxonomy.

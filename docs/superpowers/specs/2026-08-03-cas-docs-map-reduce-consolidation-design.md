---
description: 'Design for the map-reduce consolidation of all CAS documentation: extract every durable claim from the ~390-file corpus, verify each against code at HEAD, synthesize a compact user-facing doc set under docs/en/antalya/cas/, regroom BACKLOG.md, and delete the old corpus behind a coverage gate.'
sidebar_label: 'CAS docs map-reduce consolidation'
sidebar_position: 1
slug: /superpowers/specs/cas-docs-map-reduce-consolidation
title: 'CAS Docs — Map-Reduce Consolidation Design'
doc_type: 'reference'
---

# CAS docs — map-reduce consolidation design {#cas-docs-map-reduce}

Date: 2026-08-03. Branch: `cas-gc-rebuild`. Status: approved design, pre-plan.

## 1. Problem and goal {#problem-and-goal}

The CAS documentation corpus has grown to ~390 markdown/text files (~150k lines): dated
specs, plans, reports, worklogs, sdd task reports, plus the numbered internal set
`docs/superpowers/cas/01`–`11`. User-facing documentation is nearly absent. Many documents
contain long-implemented or rejected ideas, yet also carry durable value — contracts, design
decisions, bugs, TODOs, runbook facts — so plain deletion would lose knowledge.

Goal: a map-reduce pipeline that (a) extracts every durable claim, (b) classifies each as
done / rejected / stale / still-open / doc-material with code-at-HEAD evidence, (c) synthesizes
one compact, current documentation set under `docs/en/antalya/cas/`, (d) regrooms the live
`BACKLOG.md`, and (e) deletes the old corpus behind a coverage gate.

## 2. Decisions taken during brainstorming {#decisions}

| Question | Decision |
|---|---|
| Relation of new set to `docs/superpowers/cas/` | One new set — everything moves to `docs/en/antalya/cas/`; the numbered `01`–`11` are reworked into its architecture section |
| Fate of old corpus | Still-valuable, still-actual specs stay where they lie (`KEEP-IN-PLACE`); everything else deleted after a coverage gate |
| Location and format | `docs/en/antalya/cas/`, full frontmatter + explicit `{#anchors}` per CLAUDE.md, Docusaurus-compatible markdown, mermaid diagrams |
| Input corpus | Everything CAS-related except `*.tla`/`*.cfg`: the md/txt files in `git diff $(git merge-base altinity/antalya-26.6 HEAD)..HEAD` (389 files), plus `.superpowers/sdd/`, `utils/ca-soak/scenarios/` (`BACKLOG.md`, `RUN_HISTORY.md`, `gc_wedge_forensics_20260710.txt`), plus untracked CAS notes in the repo root |
| Roadmap vs backlog | Public curated `roadmap.md` in the new set; `docs/superpowers/cas/BACKLOG.md` stays the internal working backlog (regroomed, issue IDs preserved, never renumbered) |
| Verification standard | Everything against code at HEAD — but scoped to what is published or what justifies removal (see §6) |
| Pipeline shape | Approach A (claim ledger, push) with a pull element in synthesis: page authors work from the verified ledger seeded by the freshest documents, and may read code directly |
| History | Public `architecture/design-history.md` is a condensed version of `how-we-got-here.md`; the full version stays in git history as future blog material |

## 3. Tone, style, and hard limits {#style}

- Positive open-source framing: what pain it removes, "all you need is a good S3 bucket".
- Reliability story: core algorithms and FSMs modelled in TLA+ and checked with TLC; code
  exercised on corner cases and multi-hour soak runs with chaos testing.
- Experimental status framed as freedom to improve, not as immaturity: works correctly today,
  code is being actively cleaned up, backward-incompatible format changes possible but unlikely.
- Every page skimmable in ~2 minutes of diagonal reading; no long text walls. Tables,
  mermaid diagrams, short examples, maybe simple animations — interleaved with prose.
  Do not treat the reader as an idiot; do not overload either. Standard technical-writing
  best practices apply (task-oriented headings, one idea per paragraph, reference tables).
- Mermaid diagrams are validated programmatically before commit (mermaid + jsdom parse).
- Platform claims stated exactly: AWS and GCP work; Azure probably works; other S3-compatible
  stores only if they support atomic/conditional operations (`If-None-Match` and friends).

## 4. Target documentation tree {#target-tree}

```
docs/en/antalya/cas/
├── index.md                  # What it is, what problems it solves, status, navigation
├── quick-start.md            # Empty bucket → first INSERT/SELECT; minimal working config
├── configuration.md          # Full reference: disk config block, all settings + defaults
├── bucket-requirements.md    # Object-storage capability table; conditional-write requirement;
│                             #   AWS / GCP / Azure-probably / other-S3 caveat; no versioning needed
├── operations/
│   ├── migration.md          # Add CAS disk, MOVE PARTITION, verify, roll back
│   ├── monitoring.md         # system.content_addressed_log, …_garbage_collection_log,
│   │                         #   …_mounts; key metrics + what to watch (links to existing
│   │                         #   docs/en/operations/system-tables/ pages, which stay)
│   ├── troubleshooting.md    # "What if?..." symptom → diagnosis → action table
│   └── debugging.md          # ca-fsck, ca-gc-dryrun, SYSTEM commands, reading GC logs,
│                             #   what to collect before filing a bug
├── architecture/             # One page ≈ one subsystem/algorithm; diagrams, states, settings
│   ├── index.md              # Git analogy, object model in one diagram, safety invariants,
│   │                         #   shared-nothing positioning            [walkthrough §1–3]
│   ├── storage-layout.md     # Bucket contents: key table, envelope, codecs, example tree,
│   │                         #   path conversion                       [§5–6 + formats]
│   ├── blob-protocol.md      # Conditional writes, dedup, writer-vs-GC race,
│   │                         #   deterministic artifacts               [§7]
│   ├── mounts-and-leases.md  # Server identity, owner claim, lease, counters, mount FSM [§8]
│   ├── manifests-and-refs.md # Part manifests + RefLedger, orphan sweep, publish protocol,
│   │                         #   recovery                              [§9–10]
│   ├── part-lifecycle.md     # build → precommit → upload → promote; crash points and their
│   │                         #   cleaners; repoint; MergeTree op mapping [§4, §11]
│   ├── replication.md        # Fetch-by-relink: gates, commit-before-release,
│   │                         #   detach/attach/drop                    [§12]
│   ├── garbage-collection.md # Leadership, the round, sharding, cost, observability [§13]
│   ├── read-path.md          # resolveRef → manifest → ranged reads; caches [§14 + 09]
│   ├── correctness.md        # What the TLA+/TLC models prove; soak/chaos methodology [§16 + 06]
│   └── design-history.md     # Condensed design journey: rejected paths (Merkle layer, EBR,
│                             #   refcount, zero-copy) and why; what the models caught
└── roadmap.md                # Curated public roadmap: done / in progress / planned /
                              # known limitations. No internal issue IDs
```

`[§N]` = seed sections of `11-walkthrough.md`. Walkthrough §15 (configuration surface) feeds
`configuration.md`; §17.2 (known gaps) feeds `roadmap.md`.

Source trust order on conflict: `how-we-got-here.md` (2026-08-03) > `11-walkthrough.md`
(2026-07-29) > `01`–`10` > older dated specs/plans. The ref/GC subsystem moved substantially
after the walkthrough was written (v5 invariant reset, contiguous streams, `_ckpt`, catalog,
Stage A/B — Stage B committed 2026-08-03), so `manifests-and-refs.md` and
`garbage-collection.md` are the highest-drift pages and get the strictest HEAD verification.
Every seed is only a seed: truth is established by verification against HEAD at synthesis time.

## 5. Ledger schema and classification {#ledger}

Working directory: `docs/superpowers/cas/consolidation-2026-08/` (committed, so every phase is
restartable and auditable; deleted at the end except the coverage matrix).

Extraction record (map phase), JSONL, one file per source batch:

```json
{
  "id": "X-0417",
  "kind": "contract | design-decision | rejected-alternative | bug | todo |
           runbook-fact | user-fact | metric | setting | history",
  "claim": "one self-contained statement, no reliance on document context",
  "sources": ["docs/superpowers/plans/2026-07-10-....md#section"],
  "issue_ids": ["B140"],
  "suggested_target": "architecture/garbage-collection | operations/monitoring |
                       roadmap | BACKLOG | keep-in-place | none"
}
```

Map rules: one record = one claim (not a document summary). Play-by-play narration (run
statuses, task checkboxes, night logs) produces no records — the file gets `ephemeral: <reason>`
in the file manifest. Every corpus file must appear in the manifest with either `records: N`
or `ephemeral: <reason>`.

Verdicts (classify+verify phase), appended after topic-level dedup:

| Verdict | Meaning | Required evidence |
|---|---|---|
| `done` | implemented at HEAD | `file:symbol` in `src/` or a test |
| `rejected` | consciously rejected | pointer to the decision + one-line reason |
| `stale` | obsolete (code/design moved) | what at HEAD contradicts it |
| `open` | still actual, to be done | confirmation it is absent at HEAD |
| `doc-fact` | fact for the new docs | confirmed against code at HEAD |
| `unverifiable` | cannot be checked (history, external) | why; never published without a caveat |

Routing: `done` + `rejected` → `roadmap.md` material and BACKLOG strike-through; `open` →
regroomed `BACKLOG.md` (IDs preserved; new items get new IDs); `doc-fact` → a card for its
target page; `stale`/`ephemeral` → coverage-matrix row only.

Anti-hallucination rule: the verify agent sees the claim and the pointer, never the whole
source document. It must find its evidence in the code at HEAD; "the document says so" is not
evidence.

## 6. Phases and gates {#phases}

Each phase is a separate orchestrated run; artifacts are committed between phases; the user
has a checkpoint at every gate.

**Phase 0 — corpus freeze** (script, no agents). Corpus = md/txt files in the diff against
the merge-base with `altinity/antalya-26.6` (389 files) + `utils/ca-soak/` docs + untracked
CAS-related notes in the repo root (the diff cannot see untracked files — known hole). Output:
`corpus-manifest.tsv` (path, size, last-commit date, category). Non-doc debris in the repo
root (stray `*.clickhouse` test files, `__cache__/`, `disks/`, …) is out of corpus but listed
as a separate cleanup line item so it is not forgotten.

**Phase M — map** (~35–40 agents). Batches of 8–15 files grouped by directory and topic
(a spec + its plan + its reports in one batch, so the agent sees the evolution). Output:
`extracted/batch-NN.jsonl` + manifest rows.
**Gate M** (mechanical script): every corpus file accounted for; no record points to a
non-existent source. Plus a sampled audit agent: 10 random files — "what did the map agent
miss?".

**Phase C — classify + verify.** Two steps:
1. *Clustering* (a few agents): group records by topic, collapse duplicates (one idea from
   five documents = one record with N sources). Expected shrink ×3–5.
2. *Verification* (the main cost, ~1 agent per topic cluster): verdict per the table above,
   `file:symbol` evidence at HEAD, blindness rule enforced.

Verification is scoped, not total: `ephemeral`/`history` records are not verified (nothing to
check against code); strict verification applies to records that (a) enter the new docs,
(b) enter BACKLOG as `open`, or (c) justify deleting something valuable (`done`/`stale`/
`rejected`).
**Gate C** (script): every non-ephemeral record has a verdict + evidence. An adversarial
audit agent re-checks a random 5% of `done` and `stale` verdicts (the dangerous classes:
a false `done` loses work, a false `stale` loses knowledge).

**Phase R — reduce** (~20 pages + surroundings):
- Architecture pages: seeded by `11-walkthrough.md` + `how-we-got-here.md`; ledger cards act
  as a completeness checklist; verified against HEAD.
- User/runbook pages: built from ledger cards + direct code reading; every config example is
  executed against a live server before it is published.
- Per-page style gate: diagonal readability, volume limit, programmatic mermaid validation,
  frontmatter + anchors per CLAUDE.md.
- `BACKLOG.md` regroomed from `open` verdicts; `roadmap.md` from `done`/`open`/known gaps.
- Final cross-page consistency review (terms, links, duplication).

**Gate D — deletion** (last, separate commits). Coverage matrix maps every corpus file →
destination(s) | `ephemeral` | `KEEP-IN-PLACE`. The user reviews the matrix; deletion happens
only after an explicit go-ahead. `*.tla`/`*.cfg` and `KEEP-IN-PLACE` files are untouched.

## 7. Resource model {#resources}

Corpus ≈ 1.5–2M tokens of source text. Unmitigated whole-pipeline estimate: 15–40M tokens.
Three standing mitigations, bringing the Claude share to roughly 5–10M spread across phases:

1. **Mechanical work goes to codex** (`codex exec -m gpt-5.6-luna`, prompt via file): the map
   phase ("read 12 files, emit records per schema") and clustering are prime candidates —
   they burn no Claude tokens at all.
2. **Cheap models on the conveyor, expensive on the conclusions.** Simple existence checks
   (does the setting/table/symbol exist — grep) → sonnet or codex. Fable only for: the 5%
   adversarial verdict audit, page synthesis, final review.
3. **Verification on demand, not total** (see §6): verify what gets published and what
   justifies removal; skip `ephemeral`/`history`.

Every gate reports tokens spent so far; the user can slow down or coarsen the next phase.

## 8. End state and deletion policy {#end-state}

| Location | What remains | Why |
|---|---|---|
| `docs/en/antalya/cas/` | the new set (~20 pages) | the only documentation of the feature |
| `docs/superpowers/cas/BACKLOG.md` | regroomed live backlog | working tool; IDs preserved |
| `docs/superpowers/specs/` (subset) | `KEEP-IN-PLACE` specs: still actual, likely to be implemented | user decision |
| `docs/superpowers/models/` | in full: `*.tla`, `*.cfg` **and** `*_RESULTS.md` | RESULTS are run protocols next to the model sources; `correctness.md` summarizes but does not replace them |
| `docs/superpowers/cas/consolidation-2026-08/` | coverage matrix only | the audit artifact of deletion |

Deleted after Gate D: dated specs/plans/reports/worklogs covered as `done`/`stale`/`rejected`/
`ephemeral`; all of `.superpowers/sdd/` (73 task reports — pure play-by-play); dated loose
files inside `cas/`; and — since the target is a single set — `01`–`11`, `README.md`,
`ROADMAP.md`, `INTENT.md`, `11-walkthrough.md`, and `how-we-got-here.md` themselves after
absorption (the full history stays in git for the future blog post; the public version is the
condensed `design-history.md`).

Deletion mechanics: separate commits after the user's explicit approval of the matrix, grouped
by category so a revert is surgical, each commit message pointing at the matrix. Git history
preserves everything.

## 9. Risks {#risks}

- **Docs describe a moving target.** The branch is actively developed (Stage B landed the day
  this spec was written). Mitigation: verification at HEAD at synthesis time; the roadmap
  states which subsystems are under active rework; the experimental banner reserves the right
  to change formats.
- **False `done`/`stale` verdicts lose work or knowledge.** Mitigation: evidence requirement +
  adversarial 5% audit on exactly those classes; Gate D user review.
- **Map agents summarize instead of extracting claims.** Mitigation: one-claim-per-record rule,
  sampled "what was missed" audit at Gate M.
- **New docs bloat back into walls of text.** Mitigation: per-page style gate with a volume
  limit and the diagonal-reading test as an explicit reviewer instruction.

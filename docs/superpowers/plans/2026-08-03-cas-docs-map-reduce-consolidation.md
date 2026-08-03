# CAS Docs Map-Reduce Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract every durable claim from the ~390-file CAS doc corpus, verify each against code at HEAD, synthesize a compact user-facing doc set under `docs/en/antalya/cas/`, regroom `BACKLOG.md`, and delete the old corpus behind a coverage gate.

**Architecture:** Four-phase pipeline (freeze → map → classify+verify → reduce) with mechanical gates between phases and user checkpoints at Gates M, C, and D. All intermediate artifacts live in `docs/superpowers/cas/consolidation-2026-08/` and are committed, so every phase is restartable. Spec: `docs/superpowers/specs/2026-08-03-cas-docs-map-reduce-consolidation-design.md` — read it first; it is the authority on schema, verdicts, routing, style, and deletion policy.

**Tech Stack:** python3 scripts (gates), `codex exec -m gpt-5.6-luna` for mechanical map/cluster work (prompt via file — NEVER inline), Claude subagents (sonnet) for verification, Fable-level agents only for the adversarial audit and page synthesis review, node + mermaid for diagram validation.

## Global Constraints

- Never rebase or amend; new commits only. Never commit to `master` (we are on `cas-gc-rebuild`). Never push.
- Every new page under `docs/en/` MUST have full frontmatter (`description`, `sidebar_label`, `sidebar_position`, `slug`, `title`, `doc_type`) and an explicit `{#kebab-case-anchor}` on EVERY heading.
- Slugs for new pages: `/antalya/cas/...` (e.g. `/antalya/cas/architecture/garbage-collection`).
- Wrap literal SQL/class/config names in backticks; write function names as `f` not `f()`; say "exception" not "crash" for logical errors.
- Style gate for every published page: skimmable in ~2 minutes; soft limit 300 lines / hard limit 500 lines per page; tables + mermaid diagrams interleaved with short prose; no long text walls.
- Platform claims verbatim from spec §3: AWS and GCP work; Azure probably works; other S3-compatible stores only with atomic/conditional operations (`If-None-Match` and friends).
- Codex prompts always go via file: `codex exec -m gpt-5.6-luna - < prompt.txt` (inline strings can strand stdin).
- `*.tla` / `*.cfg` files are never modified or deleted.
- Deletion of ANY old doc happens only in Task 16, only after the user approves the coverage matrix.
- Every gate task ends by reporting cumulative token/cost so the user can coarsen the next phase.

**Working directory for all pipeline artifacts:** `WORKDIR=docs/superpowers/cas/consolidation-2026-08`

---

### Task 1: Corpus freeze (Phase 0)

**Files:**
- Create: `docs/superpowers/cas/consolidation-2026-08/tools/build_corpus.py`
- Create: `docs/superpowers/cas/consolidation-2026-08/corpus-manifest.tsv` (generated)

**Interfaces:**
- Produces: `corpus-manifest.tsv` with TAB-separated columns `path`, `tracked` (Y/N), `lines`, `last_commit`, `group` — consumed by Tasks 2, 3, 14.

- [ ] **Step 1: Write the corpus builder**

```python
#!/usr/bin/env python3
"""Freeze the CAS doc corpus into corpus-manifest.tsv. Idempotent."""
import subprocess, sys, os, re

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, check=True).stdout

BASE = sh("git merge-base altinity/antalya-26.6 HEAD").strip()

# 1. All md/txt added/modified on the branch.
diff_files = sh(f"git diff {BASE}..HEAD --name-only --diff-filter=AM -- '*.md' '*.txt'").splitlines()

# 2. Untracked md/txt anywhere in the tree that mention CAS (the diff cannot see these).
untracked = sh("git ls-files --others --exclude-standard -- '*.md' '*.txt'").splitlines()
cas_re = re.compile(r'content[- _]address|\bCAS\b|\bca-(soak|fsck|gc)\b|RefLedger|part.manifest', re.I)
untracked_cas = []
for f in untracked:
    try:
        with open(f, errors='replace') as fh:
            if cas_re.search(fh.read(65536)):
                untracked_cas.append(f)
    except OSError:
        pass

# 3. Explicit adds from the user (tracked or not).
explicit = [
    "utils/ca-soak/scenarios/BACKLOG.md",
    "utils/ca-soak/scenarios/RUN_HISTORY.md",
    "utils/ca-soak/scenarios/gc_wedge_forensics_20260710.txt",
]

seen, rows = set(), []
def group_of(p):
    for prefix in ("docs/superpowers/specs", "docs/superpowers/plans", "docs/superpowers/reports",
                   "docs/superpowers/worklogs", "docs/superpowers/models", "docs/superpowers/cas",
                   ".superpowers", "utils/ca-soak", "docs/en", ".claude"):
        if p.startswith(prefix):
            return prefix
    return "other"

for p, tracked in [(f, "Y") for f in diff_files + explicit] + [(f, "N") for f in untracked_cas]:
    if p in seen or not os.path.exists(p):
        continue
    if p.endswith((".tla", ".cfg")):
        continue
    seen.add(p)
    lines = sum(1 for _ in open(p, errors='replace'))
    last = sh(f"git log -1 --format=%ad --date=short -- '{p}'").strip() if tracked == "Y" else "untracked"
    rows.append((p, tracked, lines, last, group_of(p)))

rows.sort()
out = os.path.join(os.path.dirname(__file__), "..", "corpus-manifest.tsv")
with open(out, "w") as f:
    f.write("path\ttracked\tlines\tlast_commit\tgroup\n")
    for r in rows:
        f.write("\t".join(map(str, r)) + "\n")
print(f"{len(rows)} files, {sum(r[2] for r in rows)} lines", file=sys.stderr)
```

- [ ] **Step 2: Run it and sanity-check**

Run: `python3 $WORKDIR/tools/build_corpus.py`
Expected: ≥ 389 files (diff base gave 389; untracked root notes such as `cas_disks_architecture_analysis.md`, `cas_disks_consistency_review.md`, `diff_25_8_26_3.md` must appear with `tracked=N`). Spot-check: `grep -c superpowers $WORKDIR/corpus-manifest.tsv` and confirm `utils/ca-soak/scenarios/RUN_HISTORY.md` is present.

- [ ] **Step 3: Record the non-doc debris line item (spec §6 Phase 0)**

Create `$WORKDIR/non-doc-debris.md` listing repo-root junk that is OUT of corpus but needs later cleanup: `03371_qbit_*.clickhouse` files, `__cache__/`, `b170_smoke_pool/`, `disks/`, `config_file_for_test.xml`, `compare_clickhouse_version*`. One line each, no action taken now.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation phase 0 — corpus freeze (manifest + debris list)"
```

---

### Task 2: Batch partition and map prompt template

**Files:**
- Create: `$WORKDIR/tools/make_batches.py`
- Create: `$WORKDIR/map-prompt-template.txt`
- Create: `$WORKDIR/batches.tsv` (generated)

**Interfaces:**
- Consumes: `corpus-manifest.tsv` (Task 1).
- Produces: `batches.tsv` (columns `batch_id`, `path`) and the prompt template — consumed by Task 3. Record schema fixed here is consumed by Tasks 5–7, 14.

- [ ] **Step 1: Write the batcher**

Group related documents together (a spec + its plan + its reports share a date+slug stem), then chunk to ≤ 12 files AND ≤ 4000 lines per batch:

```python
#!/usr/bin/env python3
import csv, re, os, sys

wd = os.path.join(os.path.dirname(__file__), "..")
rows = list(csv.DictReader(open(os.path.join(wd, "corpus-manifest.tsv")), delimiter="\t"))

def stem(p):
    b = os.path.basename(p)
    b = re.sub(r'\.(md|txt)$', '', b)
    b = re.sub(r'-(design|redesign|proposal|rfc|phase\d+|fix)$', '', b)
    return b

# Sort so cross-directory same-stem files (spec+plan+report) are adjacent.
rows.sort(key=lambda r: (stem(r["path"]), r["group"], r["path"]))

batches, cur, cur_lines = [], [], 0
for r in rows:
    n = int(r["lines"])
    if cur and (len(cur) >= 12 or cur_lines + n > 4000):
        batches.append(cur); cur, cur_lines = [], 0
    cur.append(r["path"]); cur_lines += n
if cur:
    batches.append(cur)

with open(os.path.join(wd, "batches.tsv"), "w") as f:
    f.write("batch_id\tpath\n")
    for i, b in enumerate(batches, 1):
        for p in b:
            f.write(f"B{i:03d}\t{p}\n")
print(f"{len(batches)} batches", file=sys.stderr)
```

- [ ] **Step 2: Write the map prompt template**

`$WORKDIR/map-prompt-template.txt` — placeholders `{BATCH_ID}`, `{FILE_LIST}`, `{OUT_PATH}` filled by Task 3's dispatcher:

```
You are extracting durable claims from ClickHouse CAS (content-addressed storage)
project documents. Read EVERY file listed below completely.

Files ({BATCH_ID}):
{FILE_LIST}

For each file, emit extraction records to {OUT_PATH} as JSON Lines. One record per
self-contained claim — NOT a summary of the document. Schema (all fields required,
issue_ids may be []):

{"id": "{BATCH_ID}-NNN",
 "kind": "contract|design-decision|rejected-alternative|bug|todo|runbook-fact|user-fact|metric|setting|history",
 "claim": "one self-contained statement understandable without the source document",
 "sources": ["<path>#<nearest heading anchor or line range>"],
 "issue_ids": ["B140"],
 "suggested_target": "architecture/<page>|operations/<page>|configuration|bucket-requirements|quick-start|index|roadmap|BACKLOG|keep-in-place|none"}

Rules:
- One claim per record. Split compound statements.
- Copy exact identifiers (setting names, table names, SQL, S3 key shapes) verbatim
  into the claim — these will be verified against code by someone who cannot see
  the source document.
- Play-by-play narration (run statuses, task checkboxes, night-log chronology,
  review sign-offs) produces NO records.
- Preserve issue IDs (B/T/S/D-numbers, rev.N) whenever mentioned.
- A file whose entire content is play-by-play gets zero records — that is fine.

After the JSONL, write {OUT_PATH}.manifest — one line per input file:
<path>\trecords:<N>        (N > 0)
<path>\tephemeral:<one-line reason>   (N == 0)
Every input file MUST appear exactly once.
```

- [ ] **Step 3: Run the batcher, eyeball 3 batches for sane grouping, commit**

```bash
python3 $WORKDIR/tools/make_batches.py
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — batching + map prompt template"
```

---

### Task 3: Map execution (codex fan-out)

**Files:**
- Create: `$WORKDIR/tools/run_map.sh`
- Create: `$WORKDIR/extracted/B*.jsonl` + `$WORKDIR/extracted/B*.jsonl.manifest` (generated)

**Interfaces:**
- Consumes: `batches.tsv`, `map-prompt-template.txt` (Task 2).
- Produces: `extracted/*.jsonl` records + per-file manifests — consumed by Gate M (Task 4) and clustering (Task 5).

- [ ] **Step 1: Write the dispatcher**

```bash
#!/usr/bin/env bash
# Usage: run_map.sh [batch_id ...]   (no args = all pending batches)
set -uo pipefail
WD="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$WD/extracted" "$WD/tmp"
batches=("$@")
[ ${#batches[@]} -eq 0 ] && batches=($(tail -n +2 "$WD/batches.tsv" | cut -f1 | sort -u))
for b in "${batches[@]}"; do
    out="$WD/extracted/$b.jsonl"
    [ -s "$out.manifest" ] && { echo "$b: done, skip"; continue; }
    files=$(awk -F'\t' -v b="$b" '$1==b{print $2}' "$WD/batches.tsv")
    sed -e "s|{BATCH_ID}|$b|g" -e "s|{OUT_PATH}|$out|g" "$WD/map-prompt-template.txt" \
        | awk -v fl="$files" '{gsub(/\{FILE_LIST\}/, fl)}1' > "$WD/tmp/$b.prompt"
    echo "=== $b ==="
    codex exec -m gpt-5.6-luna - < "$WD/tmp/$b.prompt" > "$WD/tmp/$b.log" 2>&1
    echo "$b exit=$?"
done
```

- [ ] **Step 2: Pilot on 2 batches, inspect quality**

Run: `bash $WORKDIR/tools/run_map.sh B001 B002`
Inspect `extracted/B001.jsonl`: records must be single claims with verbatim identifiers, not summaries. If quality is off, fix the template (Task 2) and re-run the pilot before the full run.

- [ ] **Step 3: Full run (background, resumable)**

Run: `bash $WORKDIR/tools/run_map.sh` in the background; the script skips completed batches, so it can be re-invoked after any interruption. Run 2–3 dispatcher instances on disjoint batch ranges if wall-clock matters.

- [ ] **Step 4: Commit the raw extraction**

```bash
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — map phase raw extraction (codex)"
```

---

### Task 4: Gate M — mechanical check + sampled audit

**Files:**
- Create: `$WORKDIR/tools/gate_m.py`
- Create: `$WORKDIR/gate-m-report.md`

**Interfaces:**
- Consumes: `corpus-manifest.tsv`, `extracted/*` (Tasks 1, 3).
- Produces: PASS/FAIL + `gate-m-report.md`. Tasks 5+ MUST NOT start until this passes.

- [ ] **Step 1: Write the mechanical gate**

```python
#!/usr/bin/env python3
"""Gate M: every corpus file accounted for; JSONL valid; sources exist; ids unique."""
import csv, glob, json, os, sys

wd = os.path.join(os.path.dirname(__file__), "..")
corpus = {r["path"] for r in csv.DictReader(open(os.path.join(wd, "corpus-manifest.tsv")), delimiter="\t")}
errors, accounted, ids = [], {}, set()

for mf in glob.glob(os.path.join(wd, "extracted", "*.manifest")):
    for line in open(mf):
        if not line.strip():
            continue
        path, status = line.rstrip("\n").split("\t", 1)
        accounted.setdefault(path, []).append(status)

for jf in glob.glob(os.path.join(wd, "extracted", "*.jsonl")):
    for i, line in enumerate(open(jf), 1):
        if not line.strip():
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError as e:
            errors.append(f"{jf}:{i}: bad json: {e}"); continue
        if r["id"] in ids:
            errors.append(f"{jf}:{i}: duplicate id {r['id']}")
        ids.add(r["id"])
        for s in r["sources"]:
            src = s.split("#")[0]
            if not os.path.exists(src):
                errors.append(f"{r['id']}: source does not exist: {src}")
        if r["kind"] not in ("contract design-decision rejected-alternative bug todo "
                             "runbook-fact user-fact metric setting history").split():
            errors.append(f"{r['id']}: bad kind {r['kind']}")

missing = corpus - set(accounted)
dupes = {p: v for p, v in accounted.items() if len(v) > 1}
for p in sorted(missing):
    errors.append(f"NOT ACCOUNTED: {p}")
for p in sorted(dupes):
    errors.append(f"ACCOUNTED TWICE: {p} -> {dupes[p]}")

print(f"corpus={len(corpus)} accounted={len(accounted)} records={len(ids)} errors={len(errors)}")
for e in errors:
    print("ERR", e)
sys.exit(1 if errors else 0)
```

- [ ] **Step 2: Run until clean**

Run: `python3 $WORKDIR/tools/gate_m.py`
Expected: exit 0. Fix by re-running `run_map.sh <failing batch>` after deleting its outputs.

- [ ] **Step 3: Sampled "what was missed" audit**

Pick 10 random corpus files: `tail -n +2 $WORKDIR/corpus-manifest.tsv | cut -f1 | shuf -n 10 --random-source=<(yes 42)`. Dispatch ONE Claude subagent (sonnet) with: the 10 file paths, their extracted records (grep by source path from `extracted/*.jsonl`), and the question "list concrete durable claims present in the file but absent from the records; ignore play-by-play". Append findings as new records with ids `AUDIT-M-NNN` to `$WORKDIR/extracted/audit-m.jsonl` (+ its `.manifest` re-listing those files as `records:<n>` — Gate M treats double-accounting from the audit file as OK only here: add `audit-m` exemption OR fold the new records and keep the original manifest rows; simplest is to name the manifest `audit-m.jsonl.manifest-notes` so the gate ignores it).

- [ ] **Step 4: Write `gate-m-report.md`, USER CHECKPOINT, commit**

Report: files/records/ephemeral counts by group, audit findings, tokens spent so far. Show it to the user before Phase C.

```bash
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — Gate M passed (coverage + sampled audit)"
```

---

### Task 5: Clustering (dedup by topic)

**Files:**
- Create: `$WORKDIR/tools/run_cluster.sh` (same dispatch pattern as `run_map.sh`)
- Create: `$WORKDIR/clusters/clusters.jsonl` (generated)
- Create: `$WORKDIR/tools/gate_cluster.py`

**Interfaces:**
- Consumes: `extracted/*.jsonl` (Tasks 3–4).
- Produces: `clusters/clusters.jsonl`, one line per cluster:
  `{"cluster_id": "C-NNN", "topic": "...", "target": "<one suggested_target>", "member_ids": ["B001-004", ...], "canonical_claim": "merged single claim", "issue_ids": [...], "sources": [union]}` — consumed by Tasks 6, 7, 14.

- [ ] **Step 1: Prepare clustering input and prompt**

Concatenate all records sorted by `suggested_target` then `kind` into chunks of ≤ 400 records: `jq -c . $WORKDIR/extracted/*.jsonl | sort -t'"' -k12 > $WORKDIR/clusters/all-records.jsonl; split -l 400 ...`. Prompt (via file, codex): "Group these records into topic clusters. Records making the same point = one cluster with a merged `canonical_claim` that loses NO specifics (union of identifiers, settings, IDs). Records with distinct claims stay in their own single-member clusters. Never merge across different `suggested_target`. Output schema: <as above>. Every input id must appear in exactly one cluster."
Cross-chunk pass: a second codex run over the resulting cluster topics list merges obviously-identical topics between chunks.

- [ ] **Step 2: Gate: partition check**

`gate_cluster.py`: every record id from `extracted/` appears in exactly one `member_ids` list; every cluster has non-empty `canonical_claim` and valid `target`. Same structure as `gate_m.py`; exit non-zero on any orphan/duplicate id.

- [ ] **Step 3: Run, fix, commit**

```bash
python3 $WORKDIR/tools/gate_cluster.py
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — clustering (dedup by topic)"
```

---

### Task 6: Verification fan-out

**Files:**
- Create: `$WORKDIR/verdicts/verdicts.jsonl` (generated, append-only)
- Create: `$WORKDIR/tools/gate_c.py`

**Interfaces:**
- Consumes: `clusters/clusters.jsonl` (Task 5).
- Produces: one verdict line per cluster:
  `{"cluster_id": "C-NNN", "verdict": "done|rejected|stale|open|doc-fact|unverifiable|ephemeral", "evidence": "src/path/File.cpp:SymbolName — one-line justification", "checked_at": "<git rev-parse --short HEAD>"}` — consumed by Tasks 7–14.

- [ ] **Step 1: Determine verification scope (spec §6)**

Clusters with `target: none` and `kind: history` get `verdict: ephemeral` / `unverifiable` mechanically (a 20-line python filter, no agents). Everything else — publishable, BACKLOG-bound, or deletion-justifying — goes to agents.

- [ ] **Step 2: Dispatch verify agents (sonnet, batches of ~15 clusters)**

Subagent prompt (blindness rule — include ONLY the JSON of the clusters, never source docs):

```
You verify claims about the ClickHouse content-addressed storage implementation
against the code at HEAD in this repository. For each cluster below, decide:
  done      — implemented at HEAD; evidence = file:symbol in src/ or a test name
  rejected  — a decision record explicitly rejects it; evidence = the decision + reason
  stale     — HEAD contradicts it; evidence = what at HEAD contradicts it
  open      — still actual and absent at HEAD; evidence = what you searched and did not find
  doc-fact  — a true statement about HEAD, usable in docs; evidence = file:symbol
  unverifiable — cannot be checked against code; say why
Use Grep/Read on src/, tests/, programs/, utils/ca-soak/. The claim text is your
only input — "a document says so" is NOT evidence. Copy identifiers exactly.
Append one JSON line per cluster to {OUT}. Clusters: {CLUSTERS_JSON}
```

Impossibility-style verdicts (`open`, `stale`) must name what was enumerated/searched (the discriminator), not just assert absence.

- [ ] **Step 3: Gate C mechanical check**

`gate_c.py` (same skeleton as `gate_cluster.py`): every cluster has exactly one verdict; every `done`/`stale`/`rejected`/`doc-fact` has non-empty `evidence` containing at least one existing path; `checked_at` equals current HEAD (stale verdicts from an older HEAD are re-queued, per the evidence-expires rule).

- [ ] **Step 4: Run until clean, commit**

```bash
python3 $WORKDIR/tools/gate_c.py
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — verification verdicts (Gate C mechanical)"
```

---

### Task 7: Adversarial audit of `done`/`stale` verdicts

**Files:**
- Modify: `$WORKDIR/verdicts/verdicts.jsonl` (corrections appended as new lines with `"supersedes": "C-NNN"`)
- Create: `$WORKDIR/gate-c-report.md`

**Interfaces:**
- Consumes: `verdicts.jsonl` (Task 6).
- Produces: corrected verdicts + Gate C report. USER CHECKPOINT before Phase R.

- [ ] **Step 1: Sample 5% of `done` and `stale` verdicts**

`jq -c 'select(.verdict=="done" or .verdict=="stale")' $WORKDIR/verdicts/verdicts.jsonl | shuf --random-source=<(yes 42) | head -n <5%>`.

- [ ] **Step 2: Dispatch adversarial re-checkers (Fable-level, ca-review agent type)**

Prompt per verdict: "Try to REFUTE this verdict. verdict=done: prove the feature is NOT fully implemented (find the gap). verdict=stale: prove the claim is still true at HEAD. Default to 'refuted' if the evidence pointer does not actually support the verdict." Any refuted verdict → correction line; if >10% of the sample is refuted, double the sample and re-run Task 6 for the affected verify batches.

- [ ] **Step 3: Write `gate-c-report.md`, USER CHECKPOINT, commit**

Verdict histogram, refutation rate, tokens spent. Show to user before synthesis.

```bash
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — Gate C passed (adversarial audit)"
```

---

### Task 8: Page tooling — style gate

**Files:**
- Create: `$WORKDIR/tools/check_page.py`
- Create: `$WORKDIR/tools/validate_mermaid.mjs`

**Interfaces:**
- Produces: `check_page.py <file.md>` exit 0/1 — run by every page task (9–13) before its commit.

- [ ] **Step 1: Write the style gate**

```python
#!/usr/bin/env python3
"""Style gate: frontmatter, anchors, volume, mermaid. Usage: check_page.py <file.md>..."""
import re, subprocess, sys, os

REQUIRED_FM = ["description", "sidebar_label", "sidebar_position", "slug", "title", "doc_type"]
rc = 0
for path in sys.argv[1:]:
    text = open(path).read()
    errs = []
    m = re.match(r'^---\n(.*?)\n---\n', text, re.S)
    if not m:
        errs.append("missing frontmatter")
    else:
        for k in REQUIRED_FM:
            if not re.search(rf'^{k}:', m.group(1), re.M):
                errs.append(f"frontmatter missing {k}")
        if "antalya/cas" in path and "slug: /antalya/cas" not in m.group(1).replace("'", ""):
            errs.append("slug must start with /antalya/cas")
    body = text[m.end():] if m else text
    in_code = False
    for i, line in enumerate(body.splitlines(), 1):
        if line.startswith("```"):
            in_code = not in_code
        if not in_code and re.match(r'^#{1,6} ', line) and not re.search(r'\{#[a-z0-9-]+\}$', line):
            errs.append(f"line {i}: heading without {{#anchor}}: {line[:60]}")
    n = len(text.splitlines())
    if n > 500:
        errs.append(f"HARD volume limit: {n} lines > 500")
    elif n > 300:
        print(f"WARN {path}: {n} lines > 300 soft limit")
    for block in re.findall(r'```mermaid\n(.*?)```', text, re.S):
        r = subprocess.run(["node", os.path.join(os.path.dirname(__file__), "validate_mermaid.mjs")],
                           input=block, capture_output=True, text=True)
        if r.returncode != 0:
            errs.append(f"mermaid parse failure: {r.stderr.strip()[:200]}")
    for e in errs:
        print(f"ERR {path}: {e}")
    rc |= bool(errs)
sys.exit(rc)
```

- [ ] **Step 2: Write the mermaid validator**

```javascript
// validate_mermaid.mjs — parse stdin as one mermaid diagram; exit 1 on parse error.
import { JSDOM } from "jsdom";
const dom = new JSDOM("<!DOCTYPE html><body></body>");
globalThis.window = dom.window; globalThis.document = dom.window.document;
globalThis.navigator = dom.window.navigator; globalThis.DOMPurify = { sanitize: s => s, addHook: () => {} };
const mermaid = (await import("mermaid")).default;
mermaid.initialize({ startOnLoad: false, securityLevel: "loose" });
let src = ""; for await (const c of process.stdin) src += c;
try { await mermaid.parse(src); process.exit(0); }
catch (e) { console.error(String(e).split("\n")[0]); process.exit(1); }
```

Install deps once in the tools dir: `cd $WORKDIR/tools && npm init -y >/dev/null && npm i mermaid jsdom >/dev/null`. Add `$WORKDIR/tools/node_modules` and `package*.json` to `.gitignore` inside `$WORKDIR` (do not commit node_modules). Remember: unquoted `;` inside mermaid sequence text breaks rendering — quote such labels.

- [ ] **Step 3: Self-test on a fixture (bad page must fail, good page must pass), commit**

Write `$WORKDIR/tools/fixtures/bad.md` (no frontmatter, heading without anchor, broken mermaid) and `good.md`; run the gate on both; expect exit 1 and 0 respectively.

```bash
git add docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation — page style gate (frontmatter/anchors/volume/mermaid)"
```

---

### Task 9: Architecture pages, group 1 — model and storage

**Files:**
- Create: `docs/en/antalya/cas/architecture/index.md`
- Create: `docs/en/antalya/cas/architecture/storage-layout.md`
- Create: `docs/en/antalya/cas/architecture/blob-protocol.md`
- Create: `docs/en/antalya/cas/architecture/mounts-and-leases.md`

**Interfaces:**
- Consumes: seeds `docs/superpowers/cas/11-walkthrough.md` §1–3 (index), §5–6 (storage-layout), §7 (blob-protocol), §8 (mounts-and-leases); `doc-fact` clusters routed to these pages: `jq -c 'select(.target=="architecture/<page>")' $WORKDIR/clusters/clusters.jsonl` joined with verdicts.
- Produces: four published pages; terminology set here (blob, part manifest, ref, `RefLedger`, mount lease, `server_root_id`, incarnation token) is the canonical vocabulary for ALL later pages.

Per page, one author subagent (sonnet; escalate a page to Fable only if the reviewer rejects twice), with this procedure — repeat for each of the four pages:

- [ ] **Step 1: Author `index.md`**

Subagent inputs: walkthrough seed sections (verbatim), the page's verified `doc-fact` cluster claims + evidence pointers, spec §3 style rules, frontmatter template:

```markdown
---
description: '<one sentence>'
sidebar_label: 'Architecture overview'
sidebar_position: 1
slug: /antalya/cas/architecture/
title: 'CAS Architecture — Overview'
doc_type: 'reference'
---
```

Content contract for `index.md`: the Git analogy table (blob/tree/commit ↔ blob/part-manifest/ref), ONE mermaid object-model diagram, safety-invariants summary table, shared-nothing positioning paragraph. The author MUST re-verify every identifier it copies from the seed against HEAD (Grep) — the seed is a seed, not truth; where seed and HEAD disagree, HEAD wins and the discrepancy is noted in the task report.

- [ ] **Step 2: Author `storage-layout.md`** — key table (every S3 key shape under `blobs/`, `cas/refs/`, `cas/manifests/`, `roots/`, `gc/` — copied from `Formats/` sources at HEAD, not from the seed), envelope format diagram, codec table, one worked example tree.

- [ ] **Step 3: Author `blob-protocol.md`** — conditional-write sequence diagram (mermaid `sequenceDiagram`), dedup behavior, writer-vs-GC race resolution, deterministic artifacts. Settings table for the knobs this subsystem exposes (names verified in `ContentAddressedSettings` at HEAD).

- [ ] **Step 4: Author `mounts-and-leases.md`** — `server_root_id` identity, owner claim, lease lifecycle, the two monotone counters, mount FSM as a mermaid `stateDiagram-v2`.

- [ ] **Step 5: Style-gate all four, review, commit**

Run: `python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/architecture/{index,storage-layout,blob-protocol,mounts-and-leases}.md` — expect exit 0.
One reviewer pass (ca-review) per group: verify 5 randomly chosen factual statements per page against HEAD; reject the page on any false statement.

```bash
git add docs/en/antalya/cas/architecture
git commit -m "cas-docs: antalya/cas architecture — model and storage pages"
```

---

### Task 10: Architecture pages, group 2 — protocols

**Files:**
- Create: `docs/en/antalya/cas/architecture/manifests-and-refs.md`
- Create: `docs/en/antalya/cas/architecture/part-lifecycle.md`
- Create: `docs/en/antalya/cas/architecture/replication.md`
- Create: `docs/en/antalya/cas/architecture/garbage-collection.md`
- Create: `docs/en/antalya/cas/architecture/read-path.md`

**Interfaces:**
- Consumes: walkthrough §9–14 + `01`/`03`/`04`/`09` as seeds; routed verified clusters; vocabulary from Task 9.
- Produces: five pages. `garbage-collection.md` section anchors are referenced by `operations/monitoring.md` (Task 12).

Same per-page procedure as Task 9 (author → verify identifiers at HEAD → style gate → review), with one addition:

- [ ] **Step 1: HIGH-DRIFT warning applies to `manifests-and-refs.md` and `garbage-collection.md`**

Spec §4: the ref/GC subsystem changed AFTER the walkthrough was written (v5 invariant reset, contiguous streams, `_ckpt`, catalog, Stage A/B — Stage B landed 2026-08-03). For these two pages the author must treat `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/{Pool,Gc}/` at HEAD as the primary source and the walkthrough only as an outline; `how-we-got-here.md` turns 17–20 list exactly what moved.

- [ ] **Step 2: Author the five pages** — each with: one FSM or sequence mermaid diagram, a crash-points/cleaners table (`part-lifecycle.md`), the gates-in-order table (`replication.md`), the round pipeline diagram fold → retire → fence → recheck → trim/reclaim (`garbage-collection.md`), the cache table (`read-path.md`).

- [ ] **Step 3: Style-gate, review (same 5-random-facts protocol), commit**

```bash
python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/architecture/*.md
git add docs/en/antalya/cas/architecture
git commit -m "cas-docs: antalya/cas architecture — protocol pages"
```

---

### Task 11: Architecture pages, group 3 — correctness and history; user-facing core

**Files:**
- Create: `docs/en/antalya/cas/architecture/correctness.md`
- Create: `docs/en/antalya/cas/architecture/design-history.md`
- Create: `docs/en/antalya/cas/index.md`
- Create: `docs/en/antalya/cas/quick-start.md`
- Create: `docs/en/antalya/cas/configuration.md`
- Create: `docs/en/antalya/cas/bucket-requirements.md`

**Interfaces:**
- Consumes: `06-tla-models.md`, walkthrough §16, `how-we-got-here.md` (correctness + design-history seeds); `user-fact`/`setting` clusters; walkthrough §15 for `configuration.md`.
- Produces: the entry pages. `quick-start.md`'s config example is the one validated live in Step 3.

- [ ] **Step 1: Author `correctness.md` and `design-history.md`**

`correctness.md`: table model → invariant proved → counterexample it caught (from `06-tla-models.md`, verified the `.tla` files still exist at the cited paths); one paragraph on soak/chaos methodology (positive tone per spec §3). `design-history.md`: CONDENSED (≤ 250 lines) — the rejected-paths table (Merkle layer, EBR core, integer refcount, namespace registry, zero-copy — each: what it was, the counterexample/finding that killed it, one line), and the turns-at-a-glance timeline. Explicitly NOT the full narrative — that stays in git history for a future blog post.

- [ ] **Step 2: Author `index.md`, `configuration.md`, `bucket-requirements.md`**

`index.md`: problem (byte duplication across replicas, zero-copy pain) → solution in one diagram → status box (experimental, Altinity builds, freedom-to-change framing per spec §3) → nav table. `bucket-requirements.md`: capability table (conditional PUT `If-None-Match`, exact-token delete, ranged GET, LIST), platform row per the verbatim spec claim (AWS ✓, GCP ✓, Azure probably, other S3 only with conditional ops), "versioning not required". `configuration.md`: the disk config block + full settings table generated from `ContentAddressedSettings` at HEAD (name, default, one-line description) — the settings list comes from code, never from old docs.

- [ ] **Step 3: Author `quick-start.md` and validate its example LIVE**

The page: minimal `<disk>` XML with `metadata_type = content_addressed`, storage policy, `CREATE TABLE`, `INSERT`, `SELECT`, one `system.content_addressed_mounts` query. Validation (spec §6: every config example is executed before publication): copy the exact XML/SQL from the page into `$WORKDIR/tmp/quickstart-validate/`, start a server on the branch binary (`ci/tmp/clickhouse` or `build*/programs/clickhouse`) with that config against a local/emulated backend (or rustfs if the example uses S3 endpoints — reuse `utils/ca-soak` compose), run the SQL, assert zero errors. Log to `$WORKDIR/tmp/quickstart-validate/validate.log`; a subagent summarizes the log. If the published example needs AWS-only wording, the validated variant plus a "endpoint differs" note is acceptable — but the SQL must run verbatim.

- [ ] **Step 4: Style-gate all six, review, commit**

```bash
python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/*.md docs/en/antalya/cas/architecture/{correctness,design-history}.md
git add docs/en/antalya/cas
git commit -m "cas-docs: antalya/cas — correctness, design history, user-facing core (quick-start validated live)"
```

---

### Task 12: Operations runbooks

**Files:**
- Create: `docs/en/antalya/cas/operations/migration.md`
- Create: `docs/en/antalya/cas/operations/monitoring.md`
- Create: `docs/en/antalya/cas/operations/troubleshooting.md`
- Create: `docs/en/antalya/cas/operations/debugging.md`

**Interfaces:**
- Consumes: `runbook-fact`/`metric` clusters; `08-testing-and-soak.md`; existing pages `docs/en/operations/system-tables/content_addressed_*.md` (linked, NOT moved); `garbage-collection.md` anchors (Task 10).
- Produces: four runbooks.

- [ ] **Step 1: Author the four runbooks**

`migration.md`: scenario steps — add CAS disk to config, add policy, `ALTER TABLE ... MOVE PARTITION ... TO DISK`, verify (`system.parts` query), roll back. `MOVE PARTITION` SQL validated live exactly like quick-start (same harness, reuse `$WORKDIR/tmp/quickstart-validate/`). `monitoring.md`: table of the three system tables (`content_addressed_log`, `content_addressed_garbage_collection_log`, `content_addressed_mounts`) with links to their existing `docs/en/operations/system-tables/` pages, key-metrics table (metric → healthy range → what a spike means), sourced only from `doc-fact` verified clusters. `troubleshooting.md`: symptom → diagnosis → action table ("what if" rows from `runbook-fact` clusters). `debugging.md`: `clickhouse-disks ca-fsck`, `ca-gc-dryrun`, relevant `SYSTEM` commands (names verified against parser sources at HEAD), how to read a GC round summary, what to collect before filing a bug.

- [ ] **Step 2: Style-gate, review, commit**

```bash
python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/operations/*.md
git add docs/en/antalya/cas/operations
git commit -m "cas-docs: antalya/cas — operations runbooks"
```

---

### Task 13: Roadmap + BACKLOG regroom

**Files:**
- Create: `docs/en/antalya/cas/roadmap.md`
- Modify: `docs/superpowers/cas/BACKLOG.md` (full regroom, IDs preserved)

**Interfaces:**
- Consumes: ALL verdicts (`done`/`rejected`/`open`), walkthrough §17.2 known gaps.
- Produces: public roadmap; regroomed internal backlog. Task 15 uses the strike-through/carry-over lists.

- [ ] **Step 1: Author `roadmap.md`**

Sections: shipped (from `done` verdicts, grouped by area, NO internal issue IDs), in-progress/planned (from `open` verdicts that are user-visible), known limitations (from `unverifiable`-with-caveat + platform caveats), deliberately-rejected directions (one-line table, links to `design-history.md`). Style-gate applies.

- [ ] **Step 2: Regroom `BACKLOG.md`**

Mechanical first pass (script or codex): for every issue ID in BACKLOG, join against verdicts by `issue_ids`; `done`/`rejected`/`stale` items move to a terminal "closed by the 2026-08 consolidation" section (one line each, verdict + evidence pointer); `open` items stay, re-verified status line updated. New `open` clusters WITHOUT an existing ID get new IDs continuing each series (never renumber existing ones). Grooming header updated to 2026-08 with a pointer to `$WORKDIR/verdicts/`.

- [ ] **Step 3: Cross-check, commit**

Check: no issue ID appears in both the live and closed sections; every `open` verdict with an ID is present. Then:

```bash
python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/roadmap.md
git add docs/en/antalya/cas/roadmap.md docs/superpowers/cas/BACKLOG.md
git commit -m "cas-docs: public roadmap + BACKLOG regroom from verified verdicts"
```

---

### Task 14: Agent working guide — `docs/superpowers/cas/AGENTS.md`

**Files:**
- Create: `docs/superpowers/cas/AGENTS.md` (persistent survivor — never a deletion candidate)

**Interfaces:**
- Consumes: `docs/superpowers/cas/INTENT.md` and `02-methodology.md` (absorbed here before Task 16 deletes them); the assistant's cross-session memory items (provided by the controller in the dispatch prompt — a subagent cannot read the controller's memory directory); `runbook-fact`/`contract` clusters tagged `keep-in-place`.
- Produces: the one file a future agent (any model, any session) reads before touching this branch. Listed as `KEEP-IN-PLACE` in the Task 15 matrix, alongside `BACKLOG.md`.

- [ ] **Step 1: Author `AGENTS.md` (~250 lines max; same compactness discipline — tables over prose)**

Controller (not a blind subagent) drafts it, because half the sources live in the controller's memory. Sections, each entry one-two lines + a pointer:

1. **Orientation** — what the branch is, where the spec/plans/BACKLOG/public docs live, what `consolidation-2026-08/COVERAGE-MATRIX.md` is.
2. **Hard invariants (never violate; user-vetoed classes)** — revival = fresh re-upload only, never GET a condemned object; GC never throws on a 404 during fold (record + continue); no CA-specific fields in generic Replicated/Keeper code or formats; protocol steps (e.g. HEAD-before-PUT) are not "cheap optimization" targets — user veto, consult first; no compat scaffolding pre-release; fail-close, no fallback paths; the S3 LIST-trust verdict is SETTLED — read the verdict doc, do not re-argue.
3. **Build & test recipes** — `Cas*` gtest name filter is the gate (watch for suites that escape the filter); `LOGICAL_ERROR` sites need the death-test split (naive `EXPECT_THROW` aborts sanitizer lanes); praktika invocation lines for stateless/integration + binary at `ci/tmp/clickhouse`; ca-soak: real soak = phase 3 `--duration Nm`, `down -v` for a clean restart; local praktika runs prune docker — never overlap with a live soak.
4. **Delegation & git policy** — mechanical work → `codex exec -m gpt-5.6-luna`, prompt via file; review stays with Claude; never push unprompted, a push request is not standing; commit discipline in a shared worktree (verify HEAD after commit); never `git add -A` on artifact dirs.
5. **Known traps** — `/tmp` inode exhaustion under concurrent gates (`df -i`); `grep -a` for NUL-embedded logs; unquoted `;` breaks mermaid; jemalloc profiling needs `jemalloc_enable_global_profiler` + restart; `LogSeriesLimiter` "accepted series X/N" counts the whole logger.
6. **Reporting conventions** — scenario results as a table: № / description / result / artifacts / planned fix; "no known reds" rule: any red gets an RCA or a tracked return-item.

Every claim fact-checked at authoring time: recipes are re-verified by running the referenced command's `--help`/dry form or grepping the referenced flag/symbol at HEAD; anything unverifiable is dropped or marked stale-suspect. Frontmatter per CLAUDE.md docs rules (slug `/superpowers/cas/agents`).

- [ ] **Step 2: Style-gate and commit**

```bash
python3 $WORKDIR/tools/check_page.py docs/superpowers/cas/AGENTS.md
git add docs/superpowers/cas/AGENTS.md
git commit -m "cas-docs: AGENTS.md — persistent agent working guide (absorbs INTENT + methodology + memory items)"
```

---

### Task 15: Cross-page consistency review + coverage matrix (Gate D packet)

**Files:**
- Create: `$WORKDIR/tools/build_coverage.py`
- Create: `$WORKDIR/COVERAGE-MATRIX.md`
- Modify: any page with consistency findings

**Interfaces:**
- Consumes: everything.
- Produces: the Gate D packet the user reviews: `COVERAGE-MATRIX.md` mapping EVERY corpus file → destination(s) | `ephemeral: reason` | `KEEP-IN-PLACE: reason`.

- [ ] **Step 1: Cross-page consistency review**

One reviewer agent (Fable) over the whole `docs/en/antalya/cas/` tree: terminology drift (the Task 9 vocabulary is canonical), broken relative links (`grep -o '](\.[^)]*)'` + existence check — script it), duplicated content between pages, tone violations (spec §3). Fix findings inline; re-run style gate on touched pages.

- [ ] **Step 2: Build the coverage matrix**

`build_coverage.py`: for every `corpus-manifest.tsv` row, resolve its fate via the chain file → its records (`extracted/*.jsonl` by source path) → their clusters (`member_ids`) → verdicts → destination. Fate rules: all records `ephemeral`/file had `ephemeral` manifest row → `ephemeral: <reason>`; any cluster routed to a page → list the page(s)#anchor; any `open` verdict → `BACKLOG`; file explicitly marked keep (see next step) → `KEEP-IN-PLACE`. Output one markdown table per group, plus a summary count. HARD RULE: exit non-zero if any file has no fate.

- [ ] **Step 3: Nominate KEEP-IN-PLACE files**

From clusters with `target: keep-in-place` + judgement pass over `specs/` files whose verdict profile is mostly `open` (still-actual designs likely to be implemented — the user's criterion). Also always-keep: `docs/superpowers/models/**` (sources AND `*_RESULTS.md`), `docs/superpowers/cas/BACKLOG.md`, `docs/superpowers/cas/AGENTS.md` (Task 14), `.claude/agents/ca-*.md` (active tooling, not docs). Cross-check before Gate D: `INTENT.md` and `02-methodology.md` may be marked deleted only if `AGENTS.md` exists and the matrix lists it as their destination. List each with a one-line reason in the matrix.

- [ ] **Step 4: USER CHECKPOINT — Gate D review, commit the packet**

Present to the user: the matrix summary (N files → deleted, N keep-in-place, N ephemeral), the full `COVERAGE-MATRIX.md` path, cumulative token spend. Do NOT delete anything yet.

```bash
git add docs/superpowers/cas/consolidation-2026-08 docs/en/antalya/cas
git commit -m "cas-docs: consistency review + coverage matrix (Gate D packet)"
```

---

### Task 16: Deletion (only after explicit user approval of the matrix)

**Files:**
- Delete: everything the matrix marks for deletion (grouped commits)
- Delete: `$WORKDIR` contents except `COVERAGE-MATRIX.md` and `tools/` gate scripts referenced by the matrix

**Interfaces:**
- Consumes: user-approved `COVERAGE-MATRIX.md`.

- [ ] **Step 1: WAIT for the user's explicit go-ahead on the matrix. Not before.**

- [ ] **Step 2: Grouped deletion commits (surgical revert per spec §8)**

One commit per category, each message referencing the matrix:

```bash
# 1. sdd task reports (pure play-by-play)
git rm -r .superpowers/sdd && git commit -m "cas-docs: delete sdd task reports (covered: consolidation-2026-08/COVERAGE-MATRIX.md)"
# 2. dated specs/plans/reports/worklogs marked deleted in the matrix (script emits the git rm list from the matrix)
# 3. dated loose files inside docs/superpowers/cas/
# 4. the absorbed core set: 01–11, README.md, ROADMAP.md, INTENT.md, 11-walkthrough.md, how-we-got-here.md, CONSOLIDATION-COVERAGE.md
# 5. untracked root notes (plain rm — they are untracked; list them in the commit message of #3 for the record)
```

The `git rm` lists for commits 2–3 are generated by a 15-line script reading `COVERAGE-MATRIX.md` — never hand-typed, and the script cross-checks each path is NOT `KEEP-IN-PLACE` and NOT under `models/`.

- [ ] **Step 3: Post-deletion sanity**

Run: `python3 $WORKDIR/tools/check_page.py docs/en/antalya/cas/**/*.md` (all still pass); `grep -rn 'superpowers/cas/\(0[1-9]\|1[01]\|README\|ROADMAP\|how-we-got-here\|11-walkthrough\)' docs/en/ src/ tests/ utils/ .claude/ --include='*.md' --include='*.cpp' --include='*.h'` — zero references to deleted files from surviving files (untruncated output, per the exhaustiveness rule). Then clean `$WORKDIR` (keep matrix + gate scripts), final commit:

```bash
git add -A docs/superpowers/cas/consolidation-2026-08
git commit -m "cas-docs: consolidation complete — workdir reduced to coverage matrix"
```

---

## Self-review notes

- Spec §2 corpus additions (untracked root notes, `utils/ca-soak`, `.superpowers/sdd`) → Task 1. Non-doc debris line item → Task 1 Step 3.
- Spec §5 schema/verdicts/blindness → Tasks 2, 6 (blindness enforced by prompt construction: clusters JSON only).
- Spec §6 scoped verification → Task 6 Step 1; audits → Tasks 4/7; live example validation → Tasks 11/12.
- Spec §7 resource model → codex in Tasks 3/5/13, sonnet in 6/9–12, Fable only in 7/9-review/15; token reporting at every gate.
- Spec §8 end state → Tasks 13 (BACKLOG stays), 14 (`AGENTS.md` survivor absorbing `INTENT.md`/`02-methodology.md` + memory items), 15 (KEEP-IN-PLACE incl. `models/*_RESULTS.md`), 16 (grouped deletes, matrix-driven).
- Spec §9 risks: moving target → `checked_at` HEAD pinning (Task 6 gate re-queues stale verdicts); false done/stale → Task 7; map summarization → Task 4 audit; bloat → Task 8 volume limits.

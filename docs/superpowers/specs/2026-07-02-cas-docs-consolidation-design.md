# CAS documentation consolidation — design

**Status:** DESIGN (2026-07-02, unattended). Autonomous per the user's directive ("do specification for that
documentation work, then plans, and then implement with subagents, with a sanity check that nothing important
is lost, then commit new files and remove all obsolete").
**Branch:** `cas-layout-hot-cold-split`.

## Problem

`docs/superpowers/` has grown to ~24 MB across 10 top-level docs + 59 specs + 78 plans + 13 reports +
15 model READMEs/RESULTS + 161 model files (`.tla`/`.cfg`) + worklogs + night logs. It faithfully records
the *story and evolution* of the content-addressed (CAS) MergeTree feature over ~2 months, but it is
unnavigable: the same invariant/decision is restated across many dated docs, superseded designs sit beside
current ones, and ephemeral logs (night logs, unattended worklogs, handoff dossiers) dominate by volume.

We want ONE canonical, structured documentation set that captures — without the noise — every load-bearing
invariant, architectural decision, failed path, and final path with the necessary technical detail, so that
**all the old narrative/log/superseded docs can be deleted from the tree and from git without losing anything
important.**

## What we KEEP untouched (not in scope for deletion)

- **TLA+ model sources**: all `docs/superpowers/models/*.tla` and `*.cfg` — the formal artifacts. The new
  docs REFERENCE them; they are not rewritten. (Their prose `*_RESULTS.md` / `*_README.md` are consolidated —
  see below.)
- **The active D1 work** (recent, in-flight): `specs/2026-07-01-cas-shard-incarnation-and-registry-removal-design.md`,
  `plans/2026-07-01-cas-shard-incarnation-and-registry-removal.md`, `models/CaGcShardIncarnationCore*`. These
  stay as the live spec/plan until D1 fully lands; their durable content is ALSO summarized in the new docs.
- Anything referenced by `.claude/CLAUDE.md` or the tooling.

## Target structure — a new folder `docs/superpowers/cas/`

A small set of durable, topic-structured documents (NOT dated, NOT per-milestone). Each section carries an
explicit status stamp where relevant: **DONE / TODO / REJECTED (why) / DESIRABLE (why)**.

- `README.md` — entry point: what this folder is, a reading guide, and a **status dashboard** (one line per
  major area → DONE/partial/TODO + the canonical doc). Links every doc below.
- `01-architecture.md` — what CAS is ("git for MergeTree", shared-nothing not shared-state), the object model
  (content-addressed **blobs**; per-`(ns,shard)` **ref** shards; immutable **part-manifests**; the
  **incarnation** identity), the pool **layout** (hot/cold split; `blobs/`, `cas/refs/`, `cas/manifests/`,
  `roots/`, `gc/`), and the **approaches tested and REJECTED** with reasons (e.g. Merkle `treeId` tree layer —
  removed; EBR GC core — replaced by incarnation tokens; integer refcount — replaced by the source-edge set;
  the namespace registry — removed by D1; zero-copy replication — replaced by content addressing).
- `02-methodology.md` — the disciplined method that DROVE the decisions: TDD, subagent-driven development,
  **TLA+ as a pre-implementation gate** (counterexamples that changed the design), the scenario/soak suite as
  an empirical oracle, systematic debugging. Why each big pivot happened.
- `03-writer-protocol.md` — the write path in full: mount + `server_root_id` + owner claim; durable-monotone
  `writer_epoch` / `build_sequence`; **build → precommit → (upload) → promote**; mutable vs immutable files;
  renames; the fold barrier; resource requirements (memory, scratch, multipart). Per-operation **S3 budget**
  (cross-links `07-s3-budget.md`). Ends with DONE/TODO/REJECTED/DESIRABLE.
- `04-gc-protocol.md` — GC **leader election / lease + advisory heartbeat**; the **round** (fold → retire →
  fence → recheck → trim/reclaim); **orphan removal** (blobs/part-manifests, in-degree via the source-edge
  set); **ref removal** (owner journal, drops, tombstones); **shard-object reclaim** + **incarnation** +
  **registry removal (D1)**; attempt-scoped generations; snap prune; concurrent-leader safety. Explicit
  **TLA+ references** (which model proves which invariant). S3 budget cross-link. DONE/TODO/REJECTED/DESIRABLE.
- `05-formats-and-backend.md` — object kinds + **envelope/one-header** + codecs + **determinism**
  (`putDeterministicArtifact`); the **layout keys**; the `Cas::Backend` abstraction; **exact-token deletes**;
  the **rustfs** testbed; **AWS / GCS / Azure** support status + gaps (LIST consistency, generation vs ETag
  tokens). DONE/TODO/REJECTED/DESIRABLE.
- `06-tla-models.md` — an index of every TLA+ model that survives: what each proves (its invariants), the
  **counterexamples that drove design decisions**, and code-currency notes. Points to the `.tla`/`.cfg` files
  (which remain). Supersedes the per-model `*_RESULTS.md`/`*_README.md` prose.
- `07-s3-budget.md` — THE consolidated, detailed **S3 op-count breakdown per protocol part**. Write budget
  (e.g. 1 PUT per new file >1 MiB + 1 HEAD per file + 1 PUT precommit + 1 PUT commit + 1 PUT part-manifest,
  …), read budget, GC budget (fold/fence/recheck/reclaim/prune), and the **reduction history** (dedup cache,
  adaptive HEAD-before-PUT, precommit-first, snap-prune, LIST-token skip). Notes what is measured (soak
  ProfileEvents) vs modeled.
- `08-testing-and-soak.md` — the scenario suite (S01–S35: what each stresses), the soak harness, `fsck` /
  `ca-gc-dryrun`, the event/gc audit logs, and the standing findings/backlog pointers. DONE/TODO/DESIRABLE.
- `ROADMAP.md` — the consolidated cross-area **DONE / TODO / REJECTED (why) / DESIRABLE (why)** roll-up
  (a single place to see the whole state), linking into the section docs.

## Sourcing map (old → new; built precisely in the plan)

- `content_addressed_mergetree.md`, `incarnation-tagged-cas.md`, `content_addressed_shared_mergetree_design.md`,
  `specs/2026-06-10-ca-incarnation-store-design.md`, `specs/*core-refactor*`, `specs/*layout-hot-cold-split*`
  → `01-architecture.md`.
- Night logs, `cas-gc-unattended-execution-log.md`, `cas-unattended-work-log-*`, `UNATTENDED-WORKLOG-*`,
  `worklogs/*`, `reports/2026-06-13-unattended-progress.md`, `B140-dangle-HANDOFF.md`, `CAS_MERGETREE_CODE_REVIEW_GUIDE.md`
  → mined for durable decisions/failed-paths into `01`/`02`/`04`; the logs themselves are then DELETED.
- Writer specs/plans (`*build-root-precommit*`, `*build-watermark*`, `*precommit-first*`, `*precommit-inline-closure*`,
  `*resurrect*`, `*revival-consolidation*`, `*manifest-commit-lock-scope*`, `*rootshard-protobuf-codec*`,
  `m2/m3/m5*` plans) → `03-writer-protocol.md`.
- GC specs/plans (`ca-gc-convergence`, `ca-gc-s1..s4*`, `ca-core-m-c3`, `*gc-lease-heartbeat*`, `*gc-snap-prune*`,
  `*gc-snap-generation-retention*`, `*attempt-scoped-generation*`, `*b140-dangle*`, `*gc-introspection*`,
  `*streaming-sharded-redesign*`, `*gc-root-local-part-manifest*`, D1) → `04-gc-protocol.md`.
- `s3-ops-cost-model.md`, `2026-06-14-ca-reduce-s3-op-count-*`, `*dedup-cache-head-before-put*`, `*head-after-put-etag*`,
  `reports/*opcount*`, `*rustfs-overwrite-leak*` → `07-s3-budget.md` + `05-formats-and-backend.md`.
- All `models/*_RESULTS.md` + `*_README.md` + `INDEX.md` + `MODEL_CURRENCY_REVIEW*` → `06-tla-models.md`.
- Formats/envelope/merkle specs (`*merkle-store*`, `*envelope-one-header*`, `*merkle-tree-id*`, `*proto-rename*`,
  `*schema-evolution*`) → `05-formats-and-backend.md` (merkle/treeId recorded under REJECTED).
- Soak/fsck/vfs (`ca-soak-test*`, `ca-fsck-readonly*`, `ca-vfs-*`) → `08-testing-and-soak.md`.
- The various backlogs (`deferred_backlog/*`) → summarized into `ROADMAP.md` TODO/DESIRABLE; the current
  `deferred_backlog/*.md` may stay if still actionable, else fold + delete.

## Sanity protocol (MANDATORY before any deletion)

1. Build a **coverage matrix**: every doc slated for deletion → the new doc + section that captures its
   durable content (invariant / decision / failed-path / technical detail). No obsolete doc is deleted unless
   the matrix names where its important content now lives.
2. A dedicated **sanity-check subagent** (fresh context) audits the matrix against the actual old + new docs:
   for a sample of the largest/most-important obsolete docs, confirm each load-bearing item is present in the
   new set. Report any gap; a gap blocks deletion of that doc until filled.
3. Only after the matrix is complete AND the audit passes: `git rm` the obsolete docs (and remove from the
   working tree). Commit the new folder + the removals together, with the coverage matrix included (as
   `docs/superpowers/cas/CONSOLIDATION-COVERAGE.md`) so the mapping is auditable in history.

## Non-goals / constraints

- Do NOT delete or rewrite the `.tla`/`.cfg` model sources, the active D1 spec/plan/model, or CLAUDE.md-referenced tooling.
- Do NOT invent facts: every technical claim in the new docs must be traceable to an old doc or the code.
- The new docs must be detailed but MANAGEABLE — synthesis, not concatenation. If a section would exceed
  ~600 lines, split it, but keep the folder small (≤ ~10 docs).
- Anchors on every heading (`{#kebab}`) and a frontmatter block per new doc, per the repo doc convention.

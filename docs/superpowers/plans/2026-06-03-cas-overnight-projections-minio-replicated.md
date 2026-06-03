# CAS Overnight Plan — Projections → minio+CA → ReplicatedMergeTree

> Autonomous execution plan (≈10h). No user interaction. Use subagents/codex for any "discussion".
> Commit regularly. Log findings/deferrals to `docs/superpowers/deferred_backlog/cas-mergetree-integration.md`.
> Operational safety: every build/test runs INSIDE a subagent with a bounded `timeout`; never leave a
> `ninja`/test process hanging in the background. Foreground + bounded only. `TaskStop` runaways.

**North star:** all stateless tests — including ReplicatedMergeTree — pass on a content-addressable
**S3 (minio)** disk used as the default, EXCEPT genuinely disk-specific tests.

Branch: `cas-mergetree-poc` (never master). Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.

---

## Stage A — Finish projections (CA disk as default)

Source plan: `docs/superpowers/plans/2026-06-03-cas-mergetree-projections.md` (Approach A, flat manifest, nested `<proj>.proj/<file>` keys). Phase 1 (metadata-storage subdir branches + gtests) and Phase 2 (gate lift + `04299` inline-disk stateless test) are DONE on-branch. Remaining:

- **A1 (plan Task 7):** extend `04299` with `ALTER ADD/DROP/MATERIALIZE PROJECTION` + `DETACH/ATTACH`; run under CA-default; diagnose the temp-projection (`.tmp_proj`) flow; add a `moveDirectory` `.tmp_proj`→`.proj` re-key branch ONLY if the run shows it's needed (template: `republishDetachedStagingIntoActive`, commit `6e076f1feb8`).
- **A2 (plan Task 8):** enumerate the ~40 projection stateless tests tagged `no-content-addressed-storage`, un-tag, run under CA-default via the `cas-test-triage` skill, fix real CA-projection bugs (likely point back to a Phase-1/3 gap), re-gate orthogonal failures with a precise reason, update backlog B5 (projection half DONE), push.

**Done when:** all projection stateless tests pass under the CA-default config except orthogonal/disk-specific ones (each re-gated with a documented reason); B5 projection half closed.

---

## Stage B — minio + content-addressed stateless run (the real-S3 target)

The CA-default stateless job today uses a **local** object storage. The north star is **S3 (minio)**. So:

- **B1:** Find the existing minio/s3 stateless config + job definition. Stateless S3 lives in `ci/` (praktika job configs in `ci/defs/job_configs.py`) and the server storage config templates under `tests/config/` (e.g. `tests/config/config.d/storage_conf.xml` and the `s3`/`s3_with_keeper` disk definitions) + the install script that selects them (`tests/config/install.sh`). The existing CA-default config is whatever makes `metadata_type=content_addressed` the default disk — find it (grep `content_addressed` under `ci/` and `tests/config/`).
- **B2:** Create a NEW praktika stateless config + job: a disk that is `type=object_storage, object_storage_type=s3, endpoint=<minio>, metadata_type=content_addressed` as the DEFAULT — i.e. the existing minio/s3 config but with the CA metadata type. Mirror how the local-CA-default config was wired. Register the new job in `ci/defs/job_configs.py` with a name like `Stateless tests (…, content_addressed s3 storage, …)`.
- **B3:** Run the FULL stateless suite under the new minio+CA job (in batches/shards if needed; bounded `timeout` per batch; via subagents). This is large — expect many failures.
- **B4:** Triage at scale. Bucket failures: (a) **real CA-on-S3 bugs** (e.g. S3 multipart/`writeObject` append semantics, `removeObject` having no dir-prune so different from local, `tryGetObjectMetadata` tag behavior, eventual-consistency vs If-None-Match) — fix; (b) **already-gated/orthogonal** (replication — Stage C/D/E; FREEZE/BACKUP — B4/B34; etc.); (c) **flaky/infra/minio-setup**; (d) **disk-specific** (the allowed exception — tag/skip). Fix the real CA-on-S3 bugs; defer or tag the rest with documented reasons.
- **B5:** Re-run until the only failures are orthogonal (replication, awaiting Stage E) / disk-specific / documented-deferred.

**Done when:** the minio+CA stateless suite is green except replication (pending Stage E), disk-specific, and documented deferrals. Findings → backlog.

> Note: S3 differs from the local object storage in ways CA cares about — no directory concept (so no
> `removeObject` empty-dir prune → B57's local-only race does not occur, but other code paths assumed
> local FS), conditional-create via `If-None-Match`, multipart uploads, object tags. Watch for write
> paths that worked on local but not S3, and vice-versa. Record each in the backlog.

---

## Stage C — Brainstorm ReplicatedMergeTree on CA

Goal: `ReplicatedMergeTree` on a content-addressed disk, where a **Fetch** (replica pulling a part it lacks)
becomes a **metadata-only re-link** instead of a byte download — IFF the source and target are the **same
content-addressed pool**. Conceptually the CA analogue of zero-copy replication's fetch.

Use the `superpowers:brainstorming` flow (run the dialogue with a subagent/codex acting as the
sounding-board since the user is unavailable — capture decisions, don't block). Key questions to resolve:

- **Current gate:** `ReplicatedMergeTree` is rejected on a CA disk (backlog B33). Where, and what does
  lifting it expose?
- **Fetch protocol:** `DataPartsExchange` (`Service`/`Fetcher`) streams part files over HTTP. The CA
  fetch should instead transfer the part's **manifest/part_id + checksums** and have the target
  **publish a ref to the already-present blobs** (the blobs are in the shared pool — same `blobs/`),
  i.e. `getStorageObjects`-by-reference rather than download. Design the protocol extension: a
  capability handshake ("both sides are the same CA pool"), the part_id/manifest payload, and the
  target-side "publish ref + per-ref sidecar (uuid/txn/metadata_version)" step. Fall back to a normal
  byte fetch when the pools differ or the peer lacks the capability (fail-safe, not fail-open).
- **What "same pool" means:** same object-storage endpoint + key prefix (the pool identity). How does a
  replica know the peer shares its pool? (pool id in the replica's part-exchange handshake / ZooKeeper.)
- **No-op file operations:** with the blobs already present, the existing fetch-then-rename flow's file
  copies become no-ops (the CA metadata storage sees the content already exists). Identify which
  `IDataPartStorage` operations the fetch path calls and which become no-ops vs which must still run
  (the ref publish + mutable sidecar MUST run; blob downloads must NOT).
- **Mutable per-part state across replicas:** `uuid.txt`/`txn_version.txt`/`metadata_version.txt` are
  per-ref sidecars (not content-addressed). A fetched part on the target needs its OWN sidecar values
  (e.g. a fresh uuid). Design how the fetch sets these.
- **GC / cross-replica safety:** a part referenced by replica B but written by replica A — the blobs
  must not be GC'd while any replica references them. The M8 WriteSession pin + the refs-are-roots model
  + the shared-pool single-source-of-truth need to extend to multi-replica. This is the hard part — may
  need the Keeper-backed ref index (B1/B11) or a careful refs-union-across-replicas reachability.
  Decide the M-replication-1 scope vs deferrals.
- **DDL/queue ops:** REPLACE/MOVE/ATTACH PARTITION, mutations, merges via the replication queue —
  confirm they route through the same whole-part transaction (most already work locally); the
  background-queue clone paths (B33) need the same gating/handling as the partition-clone ALTERs.

Write the spec to `docs/superpowers/specs/2026-06-04-cas-mergetree-replication-design.md`. Decompose if
too large (likely: M-repl-1 = single-pool fetch-by-relink + lift the gate for a 2-replica same-pool
cluster; defer cross-pool, multi-region, Keeper-accelerated refs).

---

## Stage D — Implement the ReplicatedMergeTree plan

Write the implementation plan to `docs/superpowers/plans/2026-06-04-cas-mergetree-replication.md`
(writing-plans skill), then execute it (subagent-driven). Phasing likely:
1. Pool-identity handshake in the part-exchange protocol + capability negotiation.
2. Fetch-by-relink: transfer manifest/part_id + checksums; target publishes a ref + fresh sidecar; no
   blob bytes move when pools match; byte-fetch fallback otherwise.
3. Lift the `ReplicatedMergeTree`-on-CA gate (B33) for the same-pool case.
4. Cross-replica GC safety (refs-union reachability or the chosen mechanism).
5. gtests + an integration test (2-replica same-pool cluster) proving a fetch moves no blob bytes and
   the replica reads the part back.

---

## Stage E — Un-gate ReplicatedMergeTree stateless tests

- Enumerate stateless tests tagged `no-content-addressed-storage` that use `ReplicatedMergeTree`
  (and the `Replicated*` engines). Un-tag.
- Run under the minio+CA job (Stage B's config). Triage/fix. Plan extra rounds if needed (record them
  here and in the backlog).
- **Done when:** the Replicated stateless tests pass on minio+CA, except disk-specific/documented ones.

---

## Operating rules (autonomous)

- One implementer subagent at a time (no parallel `ninja` on the same build dir). Reviews may run after.
- Every test run: `timeout <N>` (≤1800s), foreground inside a subagent, non-empty `--test` selector
  asserted, never `clickhouse local`, `TaskStop` (not kill-9) any runaway.
- Build: `ninja -C build <target> > build/<log>` (no `-j`, no `nproc`); a subagent summarizes the log.
- After each green sub-step: COMMIT. Push at phase boundaries.
- Anything non-trivial that can't be resolved now → backlog with a plug-in point; do NOT block the night
  on a single hard failure — defer and move on, then return if time permits.
- If a whole stage is blocked, record why in this file + backlog, and proceed to the next stage so the
  night is productive regardless.

## Progress log (append as we go)
- Stage A: Phase 1 + Phase 2 of the projections plan DONE (commits through `84272e9774f`). Starting A1.
- Stage A DONE (2026-06-04). Projections work on CA (Approach A): CREATE/INSERT/SELECT/merge + ALTER
  ADD/DROP/MATERIALIZE + durability across DETACH/ATTACH. Fixes: temp-projection `.tmp_proj` rekey
  (Phase 3), B58 merge/mutate durability (route projection sub-part through the parent whole-part
  transaction, CA-conditional — `24f89d7ce78`), B60 projection-subdir removal noise (`5f1422f748c`).
  Cleaned up stale gate-rejection tests (04281 removed, 04285 corrected — `d85f289c754`). **131
  projection stateless tests un-gated and passing**, ~24 orthogonal re-gated (replication/ATTACH-MOVE/
  BACKUP), and **B59 (7 tests) DEFERRED** — a real correctness gap: the CA whole-part transaction can't
  read its own in-flight staged temp projection sub-parts during a mutation/MATERIALIZE that merges
  multiple temp blocks. B59 needs a design decision (in-flight read-overlay vs early standalone
  sub-commit) — documented with both options; not a 1am hack. B5 projection half = DONE except B59.
  Pushed through `5f1422f748c`. → Starting Stage B (minio + CA stateless config).

---
description: 'CAS memory and S3-request-budget optimizations: introspection-first, manifest-trust relink adoption, absence-means-Clean blob meta, configurable cache validation, dedup-cache sizing, GC fold buffer right-sizing'
sidebar_label: 'CAS Memory & S3 Budget Optimizations'
sidebar_position: 20260714
slug: /superpowers/specs/cas-memory-s3-budget-optimizations-design
title: 'CAS Memory & S3 Budget Optimizations (Round B)'
doc_type: 'reference'
---

# CAS Memory & S3 Budget Optimizations (Round B) {#cas-mem-s3-opt}

**Date:** 2026-07-13 (brainstormed during the task-5 prod-scale scenario campaign)
**Status:** approved design, awaiting spec review
**Baseline:** the 2026-07-13 metrics audit
([report](../reports/2026-07-13-cas-soak-metrics-audit.md)): ~$10/h at the synthetic worst-case
2h chaos soak; read class 15.3M ops (GET 5.7M + HEAD 9.6M), PUT class 1.13M (all controlled
conditional writes), GC fold IO-buffer churn 1.96 GB/round, write-path buffer churn ~130 GB/30 min.

## Goals and non-goals {#goals}

Target: cut the read class by ~40-50% and the PUT class by ~25-30% at the measured workload, and
eliminate the GC fold buffer churn — WITHOUT touching the ref-protocol publish path (owned by the
rev.6 plan), Ring-2/upstream write buffers, or GC round cadence.

Non-goals (explicitly out of scope, recorded so they are not lost):
- Write-path per-part-file buffer churn (`MergeTreeWriterStream`/`CaContentWriteBuffer`/
  `CaInlineWriteBuffer` constructors) — shared upstream code, Ring-2; a later round.
- Adaptive GC round cadence (by-design O(pool) enumeration per round stays; cadence is an
  operational knob).
- Anything overlapping
  [rev.6 lease exclusivity](2026-07-13-cas-ref-lease-exclusivity-rev6-design.md): grace machinery,
  snapshot publication, lease reclaim, seals.

## Measurement methodology {#methodology}

Every lever ships behind current-behavior-preserving defaults where semantics allow, and its effect
is measured by SHORT COMPARATIVE SOAKS: 10-minute phase-3 runs, same seed, one variable per run,
verdict = deltas of the per-family ProfileEvents (via `metric_log` sums — `system.events` resets on
chaos restarts). The full matrix runs on ONE side of the rev.6 landing (see §Sequencing), never
straddling it.

## §0 Introspection first {#s0-introspection}

The audit had to derive its largest PUT class by subtraction and found four attribution artifacts.
This section is the first commit of the round; its counters are also useful to the rev.6 soak task.

1. `CasMetaPut` / `CasMetaCas` ProfileEvents at the `.meta` write choke points
   (`putMetaIfAbsent` / `casMeta`), with distinguishable creation reasons (create-Clean after body,
   resurrect refresh, GC condemn/spare/delete marks) — via separate counters or the audit event's
   `reason`.
2. Attribution-artifact closures: dedicated counters for GC's bounded-meta-pool operations
   (`CasGcMetaOps`) and the async-lister enumeration pages (`CasGcEnumerationPages`), incremented so
   they land in the GC round's `ProfileEvents` map (or, failing that, as clearly-named globals).
3. Dedup-cache hit/miss counters if absent (`CasDedupCacheHits`/`Misses`).
4. For every request class a later lever ELIMINATES, a counter of avoided operations
   (`CasBlobAdoptTrusted`, `CasPartFolderValidateSkipped`, ...) — savings become directly visible,
   not subtraction-derived.
5. No new system tables; ProfileEvents + `system.content_addressed_log` only.

## §1 GC fold read-buffer right-sizing (memory) {#s1-fold-buffers}

Fold body GETs open a fresh ~1 MiB `ReadBufferFromS3` per object while the average fold body is
~3.7 KB (271x oversize; 1.96 GB allocation churn per round). Size the read buffer from the KNOWN
body size (listing/meta provides it) + small slack, capped at the current default. Mechanical, no
setting. Expected: fold buffer churn −99%; no behavior change.

## §2 Dedup-cache sizing (measured, no code beyond §0 counters) {#s2-dedup-cache}

The workload recycles content hashes, yet dedup probes ran ~7M HEADs. Run the soak matrix over
`dedup_cache_bytes` (current default, x4, x16) and judge by `CasBlobHead` delta + hit rate. Outcome
is a default-tuning decision recorded here after measurement; no semantic change.

## §3 Configurable cache validation (user decision) {#s3-validate-setting}

Facts established during brainstorm: every local ref mutation already pushes invalidation THROUGH
the part-folder cache (each primitive owns its `eraseView` side effect; `dropRefBestEffort` erases
even on swallowed failure), namespaces are per-server so no remote actor can remove this node's
refs, and plain cache hits (`CachedForLoad`) already skip the manifest `HEAD`. The mass `HEAD`s come
from `ForceFresh` call sites (mutable per-part reads and write-path source reads — merges), where
the part is locally pinned and the body cannot legally vanish: the mandatory `HEAD` there
(review 2026-07-08: "a fresh ref resolve proves ref currency, not body existence") is a fail-closed
`INV-NO-DANGLE` bug net, not healthy-protocol correctness.

New setting `part_folder_validate` applied to the `ForceFresh` body re-proof:
- `always` — mandatory `HEAD` on every `getView` (today's behavior, **default**);
- `age <X>` — skip the `HEAD` when the view's last successful validation is younger than X seconds
  (timestamp kept on the retained view); `CasPartFolderValidateSkipped` counts skips;
- `never` — no body re-proof (optimized; bug detection falls to fsck and actual body GETs).
`StrictValidate` callers are untouched in all modes. Trade-off being purchased: a GC over-delete bug
surfaces up to X seconds later (or at the next non-cache read) instead of instantly. Soak matrix:
`always` vs `age 5` vs `age 60` vs `never`.

## §4 Manifest-trust relink adoption {#s4-relink-trust}

Today a relink fetch pays ~36 GET + 68 HEAD per part (per-file `loadMeta` + occupancy probes) with
zero byte copy — the largest single read-class consumer (~30%). Replace per-file observation with
manifest trust:

- Trust argument (two independent guards): the SOURCE pins the part for the whole download (RMT
  serves a fetch from a live part; it cannot vanish mid-fetch), and the FETCHER's precommit edge is
  durable before adoption (EDGE-BEFORE-OBSERVE holds literally — edge, then no observe at all).
  While the source's refs live, every blob has in-degree >= 1 and cannot be condemned, so the
  condemned-occupant check (and its displacement branch) does not apply on this path. Matches the
  D4-documented relink trust model (ordinary ReplicatedMergeTree interserver trust).
- No manifest format change and NO per-entry tokens: durable manifest edges are hash-based;
  adopted-dep tokens served only the displacement branch (gone on this path), rollback (never
  touches adopted entries), and the B170 token-join (which pins exactly the race the trust argument
  excludes; it remains intact on the write/dedup path where the race exists).
- Audit: adoption events carry `reason="manifest-trust"` and an empty token — a distinguishable
  class; `CasBlobAdoptTrusted` counts them.
- Plan gate: grep-proof that no other consumer of adopted-dep tokens exists; if one surfaces, this
  section returns to design.

Expected: fetch read cost drops from O(files) probes to one manifest read; ~30% of the read class.

## §5 Blob meta: absence means Clean {#s5-meta-absence-clean}

The largest PUT class (~54%) is `.meta` freshness-object traffic; its biggest sub-class is
create-as-Clean after every new body (~263k per audit window, 23% of all PUTs) — written "so
point-readers never fall back to a HEAD-only guess" while a fallback for absent meta ALREADY exists
(pre-protocol blobs). This lever makes absence the steady state:

- A blob with NO `.meta` object is Clean by definition; `.meta` becomes a pure TOMBSTONE. The full
  transition model (user-formulated, 2026-07-13):
  1. **Create** = body PUT only — no meta write and no meta read on the fresh-create path (the
     dedup path's occupied-key point-read stays).
  2. **GC condemn (retire)** = write the tombstone (meta CAS, round-stamped); body untouched.
  3. **GC delete** = delete the body (exact token) THEN delete the meta (exact etag).
  4. **Resurrect** = fresh body first (re-upload from the writer's own source / displacement with a
     fresh token) THEN delete the tombstone (If-Match on the observed etag).
  5. **Healing rule**: a GC recheck that meets a tombstone with nonzero in-degree CLEARS the
     tombstone (spare = clear).
  Ordering rationale: every crash window then fails toward "tombstone present while alive" — safe
  (at worst one extra resurrect cycle) and self-healing via rule 5. The reverse order in resurrect
  would open "absence (=Clean) while the body is dying" — adoption of an object with a queued
  exact-token delete = dangling; the only unsafe direction, excluded by construction. Both meta
  deletions are CONDITIONAL (If-Match) so resurrect cannot stomp a later round's re-condemn and GC
  cannot stomp a resurrect's clear. GC deleting the meta AFTER the body is mandatory: a recycled
  hash must not be born under a stale tombstone (create no longer reads meta); the crash window
  (tombstone without body) heals via rule 5 at the hash's next birth.
  The TLA+ gate models exactly these five transitions plus the crash points between step pairs,
  with the invariant "meta absence implies no queued exact-token delete on a live token".
- SEMANTIC change touching condemn/resurrect invariants: **TLA+ gate required** (the blob
  condemn/resurrect model), lands as the LAST commit of the round, one-command revertible, after a
  baseline soak with §0 counters proves the class decomposition.

Expected: −23% of the PUT class immediately (create-Clean), plus the resurrect-refresh share where
delete-on-resurrect applies.

## Sequencing vs rev.6 {#sequencing}

1. **§0 lands first, now** (small, zero overlap; also serves the rev.6 soak task).
2. **The rev.6 plan executes in full** (approved and written; deletes the interim publish patch —
   by design; heavier protocol risk goes while context is hot).
3. **Then this round: §1 → §2 → §3 → §4 → §5** with the soak matrix on the post-rev.6 baseline.
   §4 also gets simpler on rev.6's single-writer-by-construction foundation.

Code-collision surface with rev.6 is trivial (`ProfileEvents.cpp` appends); this round deliberately
avoids `CasStore` publish/lease code entirely.

## Testing {#testing}

- Per-lever gtests, RED-first where the lever changes behavior (§1 buffer sizes asserted via
  allocation-observing stub or buffer-capacity introspection; §3 mode matrix incl. "genuine
  divergence still fails"; §4 trusted-adopt path incl. rollback-leaves-adopted-alone and the
  audit-class assertions; §5 absent-meta reads, GC condemn on absent-meta blobs, resurrect
  clears/deletes).
- TLA+ gate for §5 before implementation.
- The 10-minute soak matrix is each lever's acceptance criterion (deltas in the §0 counters).
- Name-set sweep regression (`*Cas*:RefWriter*:*RefTableCache*:CaWiring*:CaPartPathParser*`)
  after every commit.

## Decision log {#decisions}

- Round scope = B (structural levers, no Ring-2, no publish-path) — user, 2026-07-13.
- `.meta` semantic lever approved WITH TLA gate, last-commit placement — user.
- Relink trust confirmed; NO manifest tokens (audit keeps a distinguishable trusted class instead)
  — user challenge "а нафиг они нам?" upheld by dep-consumer analysis.
- Cache validation = three-mode setting, default `always`, effect measured by soak matrix — user.
- Introspection-first section added — user ("не нужна ли ещё интроспекция?").
- Sequencing after rev.6 (except §0) — user.

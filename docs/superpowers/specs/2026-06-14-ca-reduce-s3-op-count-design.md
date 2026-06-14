---
description: 'Root performance design for content-addressed MergeTree: reduce the per-operation S3 round-trip count (the soak proved a HEAD storm + 66% S3 error/retry storm is the dominant scalability bottleneck, not snap bytes). Two pillars — (A) resident-snap incremental GC making rounds O(churn) and eliminating the per-round whole-snap GET+PUT and the per-candidate retire HEAD; (B) resolveRef decode-cache that skips the per-access HEAD on a warm hit (single-flight + bounded TTL) — plus zstd compression of the snap blob.'
sidebar_label: 'CA reduce S3 op-count'
sidebar_position: 7
slug: /superpowers/specs/ca-reduce-s3-op-count
title: 'CA Performance — Reduce S3 Op-Count (Incremental GC + resolveRef Decode-Cache + zstd Snap)'
doc_type: 'guide'
---

# CA Performance — Reduce S3 Op-Count {#ca-reduce-s3-op-count}

**Status:** approved direction (brainstormed 2026-06-14; backlog B149). Builds on B147 part-1 (binary `gc/snap` codec, committed `11fd32b2854`).

**Goal:** Stop a content-addressed (CA) ClickHouse node from becoming unresponsive under sustained load by **cutting the per-operation S3 round-trip count** — the measured root cause — at the source, not with patches.

## 1. The measured problem {#problem}

Detailed live profiling of the soak's degraded GC-leader node (S3 event-counter deltas + `system.stack_trace` + code) established:
- **HEAD dominates**: ~5,100 HEAD/s (16.1 M cumulative) vs ~610 GET/s ≈ ~690 PUT/s — HEADs outnumber GETs ~10:1.
- **66% of read requests ERROR** (11.7 M) → an **S3 retry storm** the single-disk RustFS test store can't absorb (16.9 GB RSS, 300–520% CPU), which stalls `system.parts` (it `resolveRef`s → HEADs, queuing behind the storm) — the unresponsiveness.
- **HEAD sources** (stack + code confirmed): (1) `Store::resolveRef` → `readShardDecoded` HEADs the shard manifest on **every** call (`CasStore.cpp:163`) to validate the head-token decode cache — a cache *hit* still costs a HEAD; (2) `Gc::retire` HEADs **every zero-in-degree candidate, every round**; (3) head-after-put per conditional write (B127).
- Separately, GC re-reads + re-writes the **whole-pool snap every churned round** (B147) — bytes-heavy, multipart PUT that monopolizes the shared S3 connection pool.

Op-count is the bottleneck. Real S3 bills per request and caps req/s per prefix, so cutting op-count is a genuine CA efficiency win, not only a single-disk-test-store workaround. No correctness issue was ever observed (no dangling, no loss).

## 2. Pillar A — resident-snap incremental GC {#pillar-a}

Today `Gc::fold` calls `loadSnap` (GET the whole snap) **every round**, and a churned round re-persists the whole snap (PUT); `Gc::retire` HEADs every candidate. The snap is a *derived cache* of the manifest journals (reconstructible via `folded_cursor`); deletes are authoritative in the round-keyed retired/outcome sets, **not** the snap. So:

1. **Resident snap.** Keep the decoded `GcSnap` resident in the long-lived per-leader `Gc` (the scheduler owns one). Each round, while we are the continuing leader at the current generation, **fold only new journal records** (read the changed root-shard manifests past `folded_cursor`) into the resident snap — **no per-round `loadSnap`**.
2. **Decoupled periodic checkpoint.** Every round still CAS-writes the small `gc/state` (round/fence) + retired sets — the durable delete authority, unchanged. The **whole snap + `folded_cursor` are persisted only at a checkpoint** — when ≥ K folded records accumulated **or** ≥ M rounds elapsed (K caps recovery re-fold; M caps staleness). Between checkpoints: zero snap I/O. The existing snap-PUT-then-`gc/state`-CAS ordering (orphan-snap-at-gen+1 ignored) stays crash-safe.
3. **Recovery / lease handoff.** A new/recovering leader loads the last checkpoint snap and **re-folds the journal delta from `folded_cursor` to now** (bounded by K). Resident snap is dropped on any `gc/state` CAS failure or lease loss (the existing detection) and reloaded next round.
4. **`retire` without per-candidate HEAD — DEFERRED (see below).** Originally: the resident snap carries each known object's last-observed token/size so `retire` skips the per-candidate `head`. **Deferred 2026-06-14** after a code audit (see note) showed it is only partially achievable and likely secondary. Kept out of this plan's scope.

> **Code-audit finding (2026-06-14) — why retire-without-HEAD is deferred.** `GcSnap` stores **no token/size** — only reachability (`edges`/`indeg`/`known`/`expanded`). During `fold`, GC `GET`s tree/pack objects to expand them (their token is free from the `GetResult`), but it **never reads blobs** — blob hashes come from tree entries, never their tokens. `retire` needs the **current live** token for `deleteExact`, so for blobs (which dominate a real pool) the token is only obtainable via a HEAD. Thus retire-without-HEAD could skip the HEAD only for tree/pack candidates, never blobs — partial benefit, non-trivial new snap-token-storage surface, and the riskiest element for INV-NO-RETURN. The soak's retire HEAD storm was also likely **secondary**: a melted store stalled `deleteExact`, growing the zero-in-degree candidate backlog that `retire` re-HEADs every round; relieving the store via Pillar B (kills the `resolveRef` HEAD storm) + Pillar A1 (kills per-round snap GET+PUT) should drain that backlog so retire HEADs shrink without this change. Revisit only if the soak shows retire HEADs still dominate once the store is relieved.

Net steady-state per-round S3 ≈ O(churn): fold the delta + write small gc/state + (retire/fence/delete the candidates, still HEAD-per-candidate for now). The O(pool) snap I/O happens only at checkpoints.

## 3. Pillar B — `resolveRef` decode-cache: no HEAD on a warm hit {#pillar-b}

`readShardDecoded` (`CasStore.cpp:154-176`) HEADs the shard key every call to get the current token, then returns the cached decode if the token matches. The decode (GET+parse) is cached; the **HEAD is not** — so a warm hit still costs an S3 HEAD. Under high `resolveRef` rate (the read path + everywhere), this is the biggest HEAD amplifier.

Two reducers, composable:
- **Single-flight (no staleness, always on):** coalesce concurrent `readShardDecoded` of the same shard key — N threads resolving the same shard in flight share ONE HEAD (+ at most one GET). Cuts the concurrent-resolve HEADs with zero staleness (all callers get the same fresh result).
- **Bounded-TTL freshness (tunable, bounded staleness):** if this shard was validated < TTL ago (a short default, e.g. 100–250 ms, configurable), return the cached decode **without** a HEAD. Cuts back-to-back sequential resolves. Staleness ≤ TTL.
- **Fail-safe default — TTL is opt-IN.** Single-flight (zero staleness) is always on. The bounded-TTL skip is **opt-in per caller**: the default behavior stays force-fresh (HEAD-validated, today's semantics), and the implementation **audits `resolveRef`/`readShardDecoded` callers and opts ONLY the staleness-tolerant read paths into the TTL** (e.g. mutable-per-part-file reads). Strict-freshness callers (the publish gate, MVCC `txn_version`, read-your-writes) keep the HEAD. Defaulting to safe-and-opting-in (rather than fast-and-opting-out) means a missed caller is merely slower, never incorrect.

**Correctness (the review crux):** bounded staleness must never let a safety decision act on a stale manifest. The publish path's manifest read (`mutateShard`) is its own read-modify-CAS (CAS detects a concurrent change → re-runs), so it is not weakened. The retire-view freshness is separate (RetireView, already fence-gated). The audit + force-fresh ensures no strict-consistency caller relies on the TTL'd hit. Default TTL is small; at quiescent checkpoints (writers paused) the cache is fresh regardless.

## 4. zstd-compress the snap blob {#zstd}

On top of the B147-part1 binary codec, **zstd-compress the encoded snap** using ClickHouse's existing compression infrastructure (`ZstdDeflatingWriteBuffer`/`ZstdInflatingReadBuffer` or the `CompressionCodecZSTD`/zstd contrib). The snap is the big-bytes object; the binary form is already ~3.5× smaller than JSON, and zstd on the structured binary (many repeated UInt128/enum patterns) should shrink it further — cutting the checkpoint PUT and the recovery GET bytes (and what the S3 connection pool must move). A format/version byte already fronts the snap (bumped in part-1); add a codec marker so decode picks raw-vs-zstd. Pre-release: no on-disk compat needed.

## 5. Correctness obligations & review gate {#correctness}

Delete safety is **unchanged** in both pillars — it rides the durable retire→fence→recheck→`deleteExact` with round-keyed retired sets; the snap is a derived candidate-selector and `resolveRef` freshness is bounded only where audited safe. Obligations to verify under adversarial review (INV-NO-LOSS / INV-NO-RETURN):
- **A:** re-fold from `folded_cursor` reproduces the exact resident snap (fold is deterministic/idempotent — set semantics, already relied on); resident snap dropped on every leadership/generation discontinuity; checkpoint snap-PUT + cursor-CAS crash-consistent; `retire`-without-HEAD never retires an object the snap wrongly believes exists/absent (the fallback HEAD covers uncertainty; a wrongly-skipped HEAD must not cause a wrong delete — verify the snap's token is only trusted when freshly folded).
- **B:** no force-fresh-requiring caller uses the TTL'd hit; single-flight returns identical results to all coalesced callers; the publish gate cannot pass a condemned dep due to a stale resolveRef.

## 6. Testing {#testing}

- **gtest (`src/Disks/tests/`):** (A) a continuing leader runs many churned rounds and persists the snap only at checkpoints (assert snap-PUT count ≪ round count); a forced fresh-`Gc`/lease-handoff reloads checkpoint + re-folds to a **byte-identical** snap; crash-replay yields identical retire decisions; `retire` issues **zero** per-candidate HEADs when the resident snap has the tokens (assert via a HEAD-counting backend). (B) a warm decode-cache hit issues **no** HEAD within the TTL but does after it / on force-fresh (HEAD-counting backend); single-flight coalesces concurrent resolves to one HEAD; a force-fresh caller always HEADs. (zstd) snap round-trips through zstd; size assertion. Full GC battery + `CasGcLeak`/`CasTruncateReclaim` stay green.
- **Soak re-validation:** rebuild + re-run the aggressive config (6 workers / 25 GB), measure: GC-leader node `system.parts` stays bounded, S3 **HEAD-rate and read-error-rate drop sharply** (the B148 counters), no per-round whole-snap I/O, checkpoints complete. Compare against the B148 baseline (16 M HEADs, 66% error).

## 6a. Protocol impact & model retesting {#protocol-impact}

None of the three elements introduce a NEW protocol invariant; each is below the model's abstraction line or aligns with already-modeled behavior. Delete safety (INV-NO-LOSS / INV-NO-RETURN / INV-OVER-COUNT-ONLY) is preserved by construction. Two Pillar-A aspects touch model-relevant behavior and must be re-confirmed against the scenario battery (not modeled from scratch):
- **Crash-recovery / lease-handoff via `folded_cursor` re-fold** (Pillar A): the snap-persistence *cadence* is an on-disk detail, but recovery now re-folds from the checkpoint cursor. This is exactly what `folded_cursor` was specified for (M-C3) — it aligns the impl with the modeled design. Re-confirm: re-fold is **deterministic + idempotent → identical retire decisions** (the fold-idempotence the model already relies on).
- **retire-without-HEAD token trust** (Pillar A): safe under the *existing* exact-token-delete invariant — a stale token ⇒ `deleteExact` `TokenMismatch` ⇒ spared (INV-OVER-COUNT-ONLY), never a wrong delete. Obligation: trust the snap's token only when freshly folded; else fall back to HEAD.

Pillar B is protocol-relevant **only at the publish gate**, which stays force-fresh — so the modeled publish-vs-GC-fence interlock is unchanged; the bounded read staleness lives on non-safety read paths (a MergeTree/MVCC consistency concern, guarded by the caller audit, not the GC safety model). zstd is a pure encoding change.

**Practical note:** there is no current TLA+ model of the incarnation GC (the old `CaGcCore.tla` was invalidated by the incarnation rewrite); the M-C3 verification tier IS the gtest fault-injection scenario battery + adversarial review. "Model retesting" therefore concretely means the §6 scenario gtests (byte-identical re-fold on handoff, crash-replay identical retire decisions, retire-without-HEAD safety, publish-gate never-stale) plus the adversarial-review gate — NOT a separate TLA+ run. (If a TLA+ re-model of the cursor-recovery is later wanted, it can be added, but it is not a prerequisite.)

## 7. Scope / non-goals {#scope}

- In scope: Pillar B (single-flight + opt-in bounded-TTL decode cache), Pillar A1 (resident-snap read-cache + decoupled periodic checkpoint), and zstd snap compression. Implementation order is **biggest-win-first**: Pillar B → Pillar A1 → zstd.
- **Out of scope (deferred 2026-06-14):** retire-without-HEAD (Pillar A §2.4) — only partially achievable (tree/pack only; blobs dominate) and likely secondary; revisit after soak re-validation. See the code-audit note in §2.
- **Out of scope (separate future levers):** the publish/manifest write path redesign (log-structured manifest / batched ref-deltas / piggybacked fence — the per-publish manifest CAS), snap-sharding (`snap_shards>1`, persist only dirty shards — the deeper O(churn)-writes amplifier), object-count collapse / packing (M-F), and a separate S3 connection pool / QoS for background vs query I/O. The RetireView per-publish refresh cache already landed.
- No on-disk compat (pre-release pool format).

## 8. Risks & open questions {#risks}

- **Pillar B TTL vs CA consistency model:** the audit of `resolveRef` callers is load-bearing — a missed strict-freshness caller reading a TTL-stale manifest is the main risk. Mitigation: default to force-fresh and opt specific staleness-tolerant read paths into the TTL, rather than the reverse (fail-safe to correctness). Confirm during implementation which callers (read-your-writes, MVCC txn_version, publish gate) need fresh.
- **A retire-without-HEAD token trust:** the snap's recorded token may be stale vs the live object; `retire` must only skip the HEAD when the token was folded this round / is confidently current, else fall back to HEAD — verify this cannot produce a wrong-token delete (INV-NO-RETURN).
- **Checkpoint K/M tuning:** recovery re-fold cost is bounded by K; pick K so recovery is seconds, not minutes, at large pools.
- **RustFS test-store limit:** some error rate is the single-disk store failing at an op rate real S3 absorbs; the re-validation should show op-count down even if RustFS remains the absolute ceiling — record the op-count delta, not just wall-clock.

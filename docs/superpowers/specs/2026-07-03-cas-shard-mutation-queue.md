# CAS: flat-combining shard mutation queue — spec + plan

**Status:** APPROVED 2026-07-03 (brainstorm in-session; user chose the queue over a plain mutex).
**Branch:** `cas-shard-mutation-queue` off `cas-copy-forward`.

## Problem

`Store::mutateShard` has NO intra-server serialization: every concurrent writer thread runs its own
GET(shard body)+CAS loop against S3. Soak evidence (2026-07-03, 1h, per replica): up to **156
threads mutate shards within one second** (6 INSERT workers + background merge commits + the
outdated-parts cleanup pool — `ref_drop` came from 214 distinct threads), all hashed onto 64 shard
keys. Result: 637k casPut attempts for 380k landed mutations — **40% conflict rate, 92% during
storms** (a slow pool stretches the GET+CAS window, raising the collision probability — a positive
feedback loop). Every conflict re-reads a ~280 KB body; every LANDED rewrite leaks a data dir on
RustFS ([rustfs#3231](https://github.com/rustfs/rustfs/issues/3231)) until the upstream fix.

Two independent leak/cost factors: `landed_writes × body_size`. This spec attacks the first
(landed_writes AND wasted attempts). Body size (journal-tail length) is fold-cadence work, out of
scope here.

## Design: flat combining with a leader-caller

Key invariant that bounds everything: `mutateShard` is SYNCHRONOUS for its caller — every queued
item is a blocked thread waiting on its promise. There is no fire-and-forget producer, so the total
number of queued items across ALL shards is bounded by the number of concurrently writing threads
(dozens), independent of pool size. Queues cannot bloat by construction.

- **Owner:** `Store`. One `std::unordered_map<ShardKey, ShardMutationQueue>` under ONE
  `std::mutex queue_map_mutex`. `ShardKey = (ns.string(), shard)`.
- **Entry lifetime:** created by the first waiter, erased by the leader when drained. An idle pool
  holds an EMPTY map — no persistent per-prefix structures.
- **Leadership:** the caller that inserts into an idle queue becomes the leader (no background
  thread, no lifecycle). Others enqueue and wait on their `std::future`.
- **Leader loop:** under the map mutex carve a batch PREFIX (scope rule + cap below); release the
  mutex; run ONE read→apply-all→casPut cycle; complete the batch's promises; re-lock; repeat until
  the queue is empty, then erase the entry. Arrivals during a flush form the next batch — natural
  two-phase pipelining ("collect new while flushing old").
- **Degenerate case:** a single writer = today's exact path (batch of 1), zero added latency.
- **Self-regulation:** a slow pool makes flushes longer → batches larger → FEWER writes per unit of
  work. The current positive feedback loop becomes negative.

```
struct Pending
{
    MutationScope scope;                 /// Ref{name} | WholeShard
    std::function<void(RootShard &)> mutate;
    RootMutationOrigin origin; RootMutationKind kind;
    ShardIncarnation birth_incarnation; std::function<uint64_t()> birth_floor_provider;
    std::promise<uint64_t> committed_version;
};
struct ShardMutationQueue { std::deque<Pending> items; bool leader_active = false; };
```

### Scope rule (user decision 2026-07-03)

`MutationScope` is `Ref{ref_name}` or `WholeShard`. A batch prefix stops BEFORE:
- an item whose `Ref` name already appears in the batch (≤1 mutation per ref per flush — the set of
  durable states stays a strict subset of today's; per-ref semantics are bit-identical to today);
- any `WholeShard` item (flushes SOLO: trim, GC fence, `dropNamespace`).

`precommit → promote` of the same part can never co-batch even without the rule: `promote` is
called only after `precommitAdd` RETURNED, i.e. after its flush landed (INV-2 orders precommit
durability before blob uploads before promote). The rule guards the unnatural pairs: a background
`dropRef(A)` racing `promote(A)`, double drops, `updateRefPayload(A)` vs drop.

### Semantics pinned

1. **`transition_version`:** the leader applies closures in queue order, bumping
   `root.shard_version` after EACH closure (not once per flush). Events keep unique, ordered
   transition versions; one casPut carries N events — indistinguishable to fold/trim from N fast
   sequential writes. Each item's `committed_version` future resolves to its own post-bump version.
2. **Per-closure fault isolation:** snapshot the in-memory `RootShard` before each closure; a
   throwing closure (promote owner-check, validation) restores the snapshot, its promise gets the
   exception, the batch continues. A ~280 KB memcpy per item is noise next to S3 RTTs.
3. **CAS conflict** (now only cross-server: the GC leader on the other replica): re-read, re-apply
   ALL not-yet-completed closures — identical to today's single-mutation retry semantics.
   `MAX_CAS_ATTEMPTS` exhaustion fails the whole batch (every promise gets the exception).
4. **Fence (`mayMutate`)** checked per flush attempt; a trip fails the whole batch with the same
   `ABORTED` every caller would have gotten individually.
5. **Backpressure / limits** per flush attempt on the final encoded body: soft-limit delay fires
   once per flush when ANY batched item has `origin == Writer` (same pacing intent as B164b).
   **Hard limit with batch > 1:** do NOT fail everyone — degrade to SOLO re-flush item by item, so
   exactly the offending mutation gets `LIMIT_EXCEEDED` and innocents proceed. (Rare path.)
6. **Create-if-absent** (`token == nullopt`, first write to a shard): flush SOLO with that item's
   `birth_incarnation`/`birth_floor_provider` — preserves today's birth-stamping exactly.
7. **`view_gate`:** the leader holds the SHARED side for the whole flush (a batch is one in-flight
   mutation for the beat/drain protocol — unchanged semantics).
8. **GC trim coalesces for free:** trim already goes through `mutateShard`; as a `WholeShard` item
   it rides the same queue (solo), and its rewrite no longer races writers.

### TLA+ posture

No new model. A batch is a SEQUENCE of today's transitions committed atomically: any crash/conflict
leaves either none or all of the batch durable — both are prefixes of a sequence that today's
protocol could produce with N fast sequential casPuts (a crash today can also leave any prefix).
The reachable durable states are a SUBSET of today's; the scope rule additionally keeps per-ref
histories bit-identical. `CaGcAckFloorCore`'s `WLand` abstraction (one landed ref per action) is
unaffected — N events in one object version fold identically.

## S3 effect (soak numbers)

- Conflicts: 257k/h wasted attempts (each with a ~280 KB GET) → ~0 intra-server; only rare
  cross-server GC-trim conflicts remain.
- Landed rewrites: bursts compress by the batch factor (~2–5 at healthy latency, tens under storm —
  exactly when it matters). Expected: 380k/h → well under ~150k/h at the same workload.
- Every avoided rewrite is an avoided rustfs#3231 data-dir leak until the upstream fix.

## Plan

### Task 1 — `MutationScope` threading (mechanical, no behavior change)
- `Core/CasStore.h`: `struct MutationScope { enum class Kind : uint8_t { Ref, WholeShard }; Kind kind; String ref_name; }`
  + static helpers `MutationScope::ref(String)`, `MutationScope::wholeShard()`.
- `mutateShard` gains the `scope` parameter (first after `shard`); thread through ALL 8 call sites:
  `precommitAdd`/`promote` (`Ref{final_ref_name}`), `dropRef` (`Ref`), `updateRefPayload` (`Ref`),
  `dropNamespace`/GC fence/trim (`WholeShard`), reclaim (`Ref` of the reclaimed binding's name).
- No queue yet; parameter is stored/asserted only. Suite green. Commit.

### Task 2 — the queue (TDD)
- RED tests first in `src/Disks/tests/gtest_cas_store.cpp` (suite `CasShardQueue`), using a
  latency-injecting/counting backend (delay casPut so batching windows are deterministic):
  1. two threads, same shard, different refs ⇒ ONE casPut, both futures get distinct consecutive
     versions, journal holds both events in enqueue order;
  2. same-ref two ops ⇒ TWO casPuts (scope cut);
  3. `WholeShard` item flushes solo between ref batches;
  4. a throwing closure: its caller gets the exception, the OTHER item in the batch lands, the
     journal contains only the survivor's event (snapshot isolation);
  5. external casPut between read and flush (conflict) ⇒ batch replays once, all land;
  6. fence tripped mid-batch ⇒ every waiter gets `ABORTED`, nothing landed;
  7. first-ever write to a shard (create-if-absent) flushes solo and stamps birth incarnation/floor;
  8. stress: 16 threads × 200 mutations over 4 shards ⇒ all versions unique and dense
     (`shard_version == events == 3200`), `casPut count << 3200`, no lost/duplicated journal events.
- GREEN: implement the leader loop in `Store::mutateShard` per the sketch (map + one mutex +
  promises; leader re-locks only for carve/complete/erase). Keep the existing loop body as the
  flush core (read, birth-stamp, apply, limits, casPut, cache invalidation, committed versions).
- Full `Cas*` suite green. Commit.

### Task 3 — metrics + docs
- ProfileEvents: `CasShardBatchFlushes`, `CasShardBatchedMutations` (avg batch = ratio),
  `CasShardBatchScopeCuts`, `CasShardQueueWaitMicroseconds` (sum over items).
- Docs: `03-writer-protocol.md` §shard-mutation-queue (design + scope rule + subset argument),
  `07-s3-budget.md` (conflict/rewrite budget update), ROADMAP row (DONE) + close the B157 axis row
  pointing here.
- Memory update; full suite + full link. Commit.

### Validation (queued)
Next soak run on a clean pool: `CasOtherCasConflict` ≈ 0 intra-server; landed `CasOtherCas` drops
2–5×; batch-size counters visible in the per-round ProfileEvents map of the GC round log.

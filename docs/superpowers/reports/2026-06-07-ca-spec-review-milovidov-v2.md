# CA Merkle-store spec — virtual A. Milovidov, second review (2026-06-07)

Second pass over `docs/superpowers/specs/2026-06-07-ca-merkle-store-design.md` after D6 was cut and orphans moved
to a periodic condemn-not-delete reconcile. Persona: ClickHouse CTO, blunt, simplicity- and performance-obsessed.

## Verdict {#verdict}
Yes — finally the right shape. D6 is genuinely gone (checked: not renamed). The hot path is "upload the bytes +
append a coalesced edge-delta + write one ref", **zero data-proportional Keeper traffic, zero per-object orphan
bookkeeping**. Orphans go through a rare reconcile that condemns-not-deletes and reuses the existing quiescence +
in-degree + fence gate. Four gears collapsed to two in prose. It's a GC, not a database, not a thesis. Дожали.

## Did they fix what I flagged — yes, all six {#fixed}
D6 really gone (§4.1 step 3 "no write-ahead orphan tracking"; §9 records the rejection *with the correct
"hashes not known up front" argument*). Observability, reconcile-at-scale, generation compaction, stuck/flapping
writer, `checksums.txt` coverage, part-load read path — all present in §11.

## Adopt the §4.5 merge-sort / seen-marker refinement {#adopt}
It is the correct version and *better* than current §4.5, on both my axes:
1. **It deletes a code path** — reconcile writes only into `gc/log`; the single existing fold stays the only
   thing that computes in-degree-0 and condemns. One condemner, not two writers of `gc/condemned`.
2. **The "seen" marker is not the toxic born-marker.** The poison was *who emits it and how often* (per-commit,
   per-object, on the writer hot path = D6). Emitted rarely, by reconcile, from a bulk `LIST`, off the hot path,
   it's clean — reusing the one mechanism the design already trusts.
3. **It's the only `O(1)`-memory framing at 10⁹–10¹¹ objects.** Current §4.5 "compute REACHABILITY from all
   roots" implies materializing a reachability set that OOMs. The streaming merge-sort (LIST ⋈ sorted stream)
   never materializes it. This makes §11 item 2 *answerable* instead of hand-waving.
Caveat: the seen-marker must be **idempotent across passes** (`(node,gen)`-keyed, deduped via `event_id`), else
overlapping reconciles double-emit → phantom non-zero in-degree → orphan leaks forever (over-count, but a
leak-forever bug). State the keying.

(Note: the distributed reviewer rightly insists the merge input be `refs/`-reachability, not the raw snapshot —
that preserves authority + snapshot-rebuild while staying streaming/`O(1)`-memory. Adopt that combined form.)

## Performance — honest count {#perf}
Per part with `F` files, all-new: `F` blob PUTs + 1 tree + 1 ref = `F + 2` S3 PUTs (the bytes you upload anyway +
two constant CA objects); Keeper = `O(active writers)` (epoch-cache read, `writers/<S>` updates), pool-size
independent; `+` deltas coalesced (`O(commits)`); zero hot-path LIST in the `g=0` common case. **Strictly better
than zero-copy**, whose `/zero_copy/.../<blob>` lock load scales with data and melts Keeper at scale. Say this
louder in §1.
The honesty gap: reconcile at 10¹¹ objects (≈10⁸ `LIST` calls, days of wall-clock) is *acknowledged* in §11 but
not *designed*. The streaming-merge framing is what makes it tractable (bounded memory, spend LIST calls). Write
the one paragraph that says the schedule + per-pass LIST budget — don't leave it an adjective.

## Epoch cache — disprefer, but the decision is made {#epoch}
With the sole-writer rule normative (§3.2/§6.2/§9-D1) the safety hole is closed; §6.2's lag-self-adjusting
`safe_epoch` argument is genuinely elegant (limbo `+2` and cache lag collapse into one worry). I'll stop arguing.
But the cost is a human-remembered invariant forever; **put an assertion in the code that writes the znode**
("if you are not the fenced leader, you have a bug"), not just prose.

## Still on my list {#risks}
1. Adopt the §4.5 refinement (merge against `refs/`-reachability, streaming, seen-markers, idempotent) — shorter,
   one condemner, `O(1)`-memory.
2. Reconcile-at-scale: specify schedule + LIST budget + the streaming-merge (not "incremental/bounded/pausable"
   adjectives).
3. Generation compaction: add a `count of live g>0 nodes` metric before "tolerate high-g" is safe; or compact at
   reconcile.
4. `checksums.txt` coverage guard must check **both** directions (missing file → dangle, loud; extra file →
   silent double-storage).
5. Epoch sole-writer assertion in code.

## Good — credit {#good}
Keeper-only + `INV-S3-COMPLETE`; sharded `O(delta)` fold; coalesced deltas; one `gc/condemned/<e>` set; no
`active` hint; over-count-only + fenced single deleter + ref-last; Merkle core; D5. New: the §9 rejection record
*with the why*; the two-gears framing; §6.2's lag-self-adjusting-`safe_epoch` (two worries collapse into one —
the good kind of simple).

## Bottom line {#bottom}
Done in shape. Ship after (a) adopting the §4.5 merge-sort/seen-marker refinement (merge against
`refs/`-reachability) and rewriting §4.5/§8/§11-item-2 around it — closes the only real hand-wave (reconcile
memory at scale) and removes the second condemner; (b) turning §11 items 3/5 from "decide later" into "decide
later + here's the metric/guard". Everything else is implementation. No academic complexity left to cut.

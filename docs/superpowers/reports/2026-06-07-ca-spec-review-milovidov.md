# CA Merkle-store spec — virtual A. Milovidov review (2026-06-07)

Blunt simplicity- and performance-focused review of `docs/superpowers/specs/2026-06-07-ca-merkle-store-design.md`
(the then-current revision, with the Keeper `epoch` cache and the D6 write-ahead intents). Persona: ClickHouse
CTO, allergic to over-engineering and to any data-proportional traffic on the hot path.

## Verdict {#verdict}
Third iteration; the core finally stopped trying to lose data and the two hard requirements from last round —
Keeper-only, and a sharded `O(delta)` fold — are in. Это уже не диссертация. But two pieces are still more than
the problem deserves: **D6** (write-ahead lease intents) and the **Keeper epoch cache**. Дожмите оба.

## CUT {#cut}
- **D6 write-ahead lease intents.** It's the S3-log-pin complexity renamed. The intent exists only to enumerate
  debris from a build that crashed before committing — a crash-during-upload event that essentially never
  happens — and you keep the full-`LIST` sweep as backstop *anyway*. Worse, it puts `O(files)` **persistent**
  Keeper writes on *every* commit (multi-create before the PUTs, multi-delete on commit), hitting the Raft log —
  which **re-introduces the data-proportional Keeper traffic that made zero-copy painful**, the exact thing this
  design exists to kill. CUT it; crash debris goes to the periodic Retention-guarded sweep. If a daily sweep is
  too coarse for big uploads, use **one per-build znode holding the build's `tmp/<uuid>/` prefix** + a prefixed
  `LIST` on owner-death — `O(in-flight builds)`, not `O(files)`.
- **The Keeper `epoch` cache.** It saves one tiny `GET` per commit and costs a whole durable-ordering invariant
  to babysit. Cache the S3 epoch in **writer process memory** with a short TTL — lag-only, which §6.2 already says
  is the safe direction. Identical safety, zero new znode, zero new invariant.

## SIMPLIFY {#simplify}
- The `e+2` / quiescence / condemned-set / deferred-cascade stack reads as four gears; it's two — quiescence is
  the real gate, `e+2` is slack, and the deferred cascade is just "a decrement is an edge `-` folded next round."
  Collapse the prose.
- Generations `g`: correct idea (clean ABA answer), but a node ever resurrected pays `GET`-`404`-`LIST` forever
  with no compaction story. Bound it.

## GOOD (credit) {#good}
Keeper-only + S3-is-sole-truth (`INV-S3-COMPLETE`); the sharded `O(delta)` fold; coalesced `+`/`-` deltas
(`O(commits)`, not `O(blobs)`); per-object tombstones removed → one `gc/condemned/<e>` set; the `active` hint
removed (`GET g=0 → 404 → LIST`); over-count-only bias + fenced single deleter + ref-written-last/removed-first;
the Merkle-DAG core; D5 (trust `checksums.txt`, `cityHash128`) — the keystone that makes fetch/ATTACH free.

## RISKS at 3 ночи {#risks}
1. D6's persistent-znode storm under high commit rate (self-inflicted; cut D6).
2. The full-`LIST` orphan-sweep on a 10⁹–10¹¹-object bucket — needs a real spec (incremental, no-OOM, pausable,
   `LIST` ceiling).
3. Generation-heavy nodes paying the `404→LIST` tax forever (no compaction).
4. A single stuck/flapping writer pins `safe_epoch` and stalls pool-wide reclamation — needs an alertable metric.
5. **Observability absent** — no way to answer "I dropped a table, why didn't S3 shrink?" Add `system.*` for
   epoch / `safe_epoch` / reclaim-lag / oldest-pinning-writer before implementation.
6. Codebase reality: the set of "files that are blobs" must exactly match `IMergeTreeDataPart::checksums`; some
   metadata files are not in `checksums.txt`. And verify part-load doesn't trip the cold `404→LIST` en masse.

## Disposition (what the spec did with this) {#disposition}
- **D6 CUT entirely** (orphans → periodic Retention-guarded sweep). The rejected-intent rationale is recorded in
  §9; §8 notes the sweep cost.
- **Epoch cache kept** (user's call) but with the **sole-writer rule** made normative (§3.2, §6.2, §9 D1).
- Framing collapse (two gears, deferred cascade = edge `-`) applied to §4.2.
- All RISKS 2–6 captured as explicit open items in the new §11; the generation read-cost is noted in §8.

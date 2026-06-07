# CA storage — resolved design decisions (2026-06-07)

Decision log for the Merkle-folder CA store + Keeper-only EBR GC. These are settled; the layout/protocol
docs are updated to match.

## D1 — Keeper is REQUIRED for GC coordination {#d1}
The S3-only coordination mode is **dropped**. It had two proven data-loss modes (the `2·Δ_skew` clock-skew
lease, and the fence-steal visibility race, review R3). Keeper-only removes both *and* deletes a whole class of
S3 workarounds (fence counter, lease skew math, strong-read fence, steal-out-wait). ClickHouse already depends
on Keeper for replication, so this is not a new operational dependency in practice. **S3 remains the sole
durable truth (`INV-S3-COMPLETE`)**; Keeper holds only `O(active-writers)` ephemeral coordination, and a
Keeper wipe is non-destructive (writers go read-only, GC stops, restore rebuilds from S3).
*Rejected:* "Keeper recommended + S3-only weaker mode" — shipping a clock-skew-dependent data-loss mode is not
acceptable for a storage backend.

## D2 — Generations are DECOUPLED from epochs (per-node resurrection counter) {#d2}
A node key is `(content_hash, gen)` where `gen` is a **small per-node counter, bumped only on real
resurrection** (the GC condemned generation `g` and a writer needs the content → it recreates at `g+1`). In the
common case `gen` stays `0` forever. This is independent of the global **epoch** (the EBR clock).
- Resolves the model's **CE-2** tension: the `generation == epoch` unification forced a "reuse only `e ≥ O_W`"
  rule that *broke long-lived dedup* (a stable `g=0` blob keeps generation 0 while `O_W` climbs unboundedly).
- The reuse rule becomes simply: **reuse the present generation iff it is not in the condemned cache; else
  resurrect to `gen+1`** (with the `flush-+-then-advance` + re-check + `-`-on-resurrect discipline). No
  `e ≥ O_W` constraint.
- The epoch/quiescence/`e+2`-limbo/reclaim machinery is unchanged — `epoch` is the EBR clock, `gen` is the ABA
  counter; the condemnation epoch is recorded by the `gc/condemned/<epoch>` filename, not on the node.
*Rejected:* `generation == epoch` — elegant on paper, but it was the source of the only real dedup-correctness
tension we found.

## D3 — A table's active set is FLAT refs (one ref per part) {#d3}
Keep one mutable `refs/<part_name>` per part; no giant table-level tree. *Pro:* simple, parts independent, no
giant object, matches today. *Accepted cost:* no single atomic whole-table snapshot ("list parts" = `LIST` the
refs). *Rejected for now:* one tree-of-parts (huge object, rewrite-on-change) and a sharded trie (premature).
Revisit the trie only if atomic table snapshots / time-travel become a hard requirement.

## D4 — Decrement cascade on tree delete is DEFERRED {#d4}
Deleting a tree appends its children's `-` edge-deltas; children whose in-degree reaches 0 are condemned and
reclaimed in **subsequent** GC rounds. *Pro:* simple, idempotent, crash-safe (a partial cascade is recovered
by re-folding the durable `-`s; over-count-only). *Accepted cost:* a depth-`D` dead subtree takes ~`D` rounds
to fully reclaim — fine for a background GC. *Rejected:* synchronous whole-subtree delete (a large non-atomic
multi-delete that is harder to make crash-safe).

## Treated as settled / out of scope of the ask {#settled}
- **Hash + serialization:** reuse ClickHouse's existing 128-bit hashes — `cityHash128` over blob content
  (already the part file checksum) and `SipHash-128` over canonical, name-sorted tree entries (matches today's
  `part_id`). Content is internal (non-adversarial), so 128-bit non-cryptographic is sufficient; no new dep.
- **Timing parameters** (`T_session`, epoch period ≈10 min, heartbeat, `e+2` limbo, `Retention` for reconcile):
  tuning, not design; pick conservative defaults and validate.

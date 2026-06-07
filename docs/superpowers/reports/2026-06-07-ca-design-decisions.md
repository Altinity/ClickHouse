# CA storage — resolved design decisions (2026-06-07)

Decision log for the Merkle-folder CA store + Keeper-only EBR GC. These are settled; the layout/protocol
docs are updated to match.

## Canonical design set (the "final plan", latest-greatest) {#canonical}
1. `2026-06-07-ca-merkle-folders-design.md` — data model (blob + tree + ref Merkle DAG; part == tree).
2. `2026-06-07-ca-layout-and-protocols.md` — concrete S3 + Keeper layout and the write/read/GC/recovery protocols.
3. `2026-06-07-ca-gc-keeper-only-profile.md` + `2026-06-07-ca-gc-ebr-design-plan.md` — the EBR reclamation layer.
4. `docs/superpowers/models/` — the TLC-checked core (single-node; multi-child pass pending).
5. THIS file — the resolved decisions D1–D5.
(Intermediate journey: `reports/obsolete/`. Discuss only the above.)

## Remaining work before implementation {#remaining}
- **Model pass:** multi-child commit atomicity + deferred decrement cascade (D4) + decoupled reuse (D2) in TLA+
  (re-verifies CE-2 under the decided design). The one untested safety surface.
- **Folder API** `putBlob/putTree/getBlob/getTree/setRef/removeRef` + the `IDataPartStorage` adapter.
- **Checksum reuse (D5) wiring**; canonical tree serialization format.
- **Keeper-first phased implementation plan.**

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

## D5 — when a tree IS a part, trust `checksums.txt` (reuse the part's own checksums) {#d5}
A MergeTree part already stores a per-file checksum (`cityHash128` + size) in `checksums.txt`. **The CA blob
hash MUST use the same algorithm (`cityHash128`)**, so for part-shaped trees the blob hash of each file is read
**directly from `checksums.txt`** rather than re-hashing the bytes. This makes **copy-part / fetch / ATTACH /
clone** operations near-free: the destination builds the part-tree (`name → (cityHash128, size)`) straight from
`checksums.txt`, `setRef`s it, and uploads only the blobs that are not already present (dedup by hash) — no
re-read or re-hash of file content. For non-part trees (or files without a trusted checksum) the writer hashes
normally. (Early draft of this is in the backlog `cas-mergetree-integration.md`.) This is the concrete payoff
of choosing `cityHash128` in §settled, and it is why the hash choice is *not* free to change.
Guard: verify the `checksums.txt` algorithm/format matches the CA blob-hash definition at attach time; fall
back to re-hash (fail-safe) if it does not.

## Treated as settled / out of scope of the ask {#settled}
- **Hash + serialization:** reuse ClickHouse's existing 128-bit hashes — `cityHash128` over blob content
  (already the part file checksum) and `SipHash-128` over canonical, name-sorted tree entries (matches today's
  `part_id`). Content is internal (non-adversarial), so 128-bit non-cryptographic is sufficient; no new dep.
- **Timing parameters** (`T_session`, epoch period ≈10 min, heartbeat, `e+2` limbo, `Retention` for reconcile):
  tuning, not design; pick conservative defaults and validate.

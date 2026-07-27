# Recommendation

Replace the LIST-discovered sparse journal with a conventional CAS-rooted commit chain:

- one exact-readable pool catalog defines the namespace universe;
- one fixed mutable head per namespace defines its committed frontier;
- immutable nodes contain ref transactions and point to their predecessor;
- the head CAS—not the immutable node PUT—is the transaction’s commit point;
- `LIST` is used only to discover garbage candidates. An omission can therefore delay reclamation but can never hide ownership or authorize deletion.

This removes the need to explain absence. That is the root simplification.

## Diagnosis: why the current invariants cause accretion

The source confirms the problematic combination:

- Ref IDs come from the pool-wide `next_ref_sequence`, so a namespace’s IDs are inherently sparse; rejected writes also burn safe gaps. See [CasRefLedger.h:533](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:533) and [CasRefLedger.cpp:2188](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2188).
- Replay validates only strict increase, not contiguity. A numeric hole says nothing. See [CasRefProtocol.cpp:433](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:433).
- The immutable log PUT is treated as durability, while whether it semantically counts depends on later in-memory installation. An ambiguous PUT therefore creates “possibly durable but not applied” state and the per-namespace wedge. See [CasRefLedger.cpp:1911](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1911) and [CasRefLedger.cpp:2154](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2154).
- Recovery discovers both its state and its frontier through `LIST`, then retroactively declares part of the ID space void with a synthetic recovery seal. See [CasRefLedger.cpp:465](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:465) and [CasRefLedger.cpp:579](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:579). Current code only emits this seal for a listed, nonempty dead region in a live table; `NeverBorn` is draft machinery, not a current invariant.
- GC enumerates the ref universe and its records by global `LIST`, then folds only listed IDs. See [CasGc.cpp:1013](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1013) and [CasGc.cpp:1457](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1457).
- Namespace discovery is itself LIST-derived. This infects REBUILD and fsck even if middle-record repair is perfect. See [CasGc.cpp:2393](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2393), [CasPool.cpp:1321](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1321), and [CasFsck.cpp:314](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:314).
- The orphan sweep combines a reconstructed LIST view with a separate build-floor authority. Worse, `PartWriteTxn` retires the build unconditionally even when an owner grant may remain unresolved. See [CasOrphanManifestSweep.cpp:205](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp:205) and [CasPartWriteTxn.cpp:119](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:119).

This creates four different notions of truth:

1. durable immutable objects;
2. writer RAM state;
3. GC’s folded cursor;
4. mount/build lease state.

`LIST` is asked to reconcile them. Because absence is undecidable in a sparse ID space, each counterexample demands another certificate: `prev`, a seal pointer, seal history, birth authority, generations, `R*`, lineage tombstones, quarantine, and so on.

The invariant to delete is not merely “LIST is complete.” It is:

> An immutable object becomes semantically committed merely by existing, even though no authoritative point-readable object names it.

## Candidate reformulations

| Candidate | Shape | Benefit | Cost |
|---|---|---|---|
| **1. Per-namespace CAS head plus catalog** | Exact catalog; immutable transaction nodes; fixed head points to committed tip | Local recovery, no cross-namespace append contention, removes every LIST proof | One additional head CAS per committed batch; catalog lifecycle object |
| 2. Fixed sharded global journals | A fixed number of CAS heads; each namespace hashes to a shard | No namespace catalog; GC reads a fixed head set | Cross-namespace CAS contention; recovery and snapshots become shard-wide |
| 3. Fixed full-state objects and mark/sweep | CAS-overwrite each namespace’s complete ref state; recompute reachability | Almost trivial correctness argument; no incremental cursor | O(table) writer rewrites and O(pool) GC scans; operationally unacceptable |

Pick candidate 1. It preserves the useful current invariant—one append lane per namespace—while replacing the flawed commit and discovery invariants.

## Chosen format and invariants

### Keys

```text
cas/ref_catalog

cas/refs/<ns>/_head

cas/refs/<ns>/<incarnation>/_node/
    <sequence16>-<sha256-of-canonical-node>.zst

cas/refs/<ns>/<incarnation>/_snapshot/
    <covered-sequence16>-<sha256-of-canonical-snapshot>.zst

cas/manifests/<ns>/<incarnation>/<build-id>/...
roots/<ns>/<incarnation>/...
```

Namespace incarnation must qualify every namespace-scoped physical object. Debris from an old life can then never alias a reborn namespace.

`ref_catalog` is created empty during pool creation. Once the pool format exists, a missing or undecodable catalog is corruption: all mutations and destructive work stop.

Catalog contents are a sorted map:

```text
logical namespace -> random 128-bit incarnation
```

It contains active namespaces and namespaces waiting for their terminal removal to fold. It does not retain permanent tombstones.

A node contains:

```text
namespace
incarnation
sequence
previous = {sequence, hash} | null
writer_epoch                 # audit/fencing, not ordering
operations[]
```

Committed-chain sequences are dense and per-incarnation: `node.sequence = parent.sequence + 1`. Failed candidate nodes do not create gaps because they are not part of the head-rooted chain.

The fixed head contains:

```text
namespace
incarnation
tip = {sequence, node_hash} | null
checkpoint = {covered_sequence, snapshot_hash} | null
writer_epoch
```

The backend token on `_head` is the serialization token.

### Core invariant

> A ref transaction is committed iff it is reachable from the namespace’s exact-read `_head`.

An immutable node that landed but was never installed into `_head` is inert garbage. It has no owner semantics, requires no void interval, and is never folded.

Head updates must be monotone:

- a transaction update changes the tip to a node whose `previous` is the old tip;
- a checkpoint update preserves the tip and only advances checkpoint coverage;
- a recovery fence preserves both tip and checkpoint and changes `writer_epoch`;
- any regression or unrelated tip is corruption.

## End-to-end protocol

### Namespace creation

1. Choose a new random incarnation.
2. Create or CAS-replace `_head` with an empty head for that incarnation.
3. CAS-add `{namespace, incarnation}` to `ref_catalog`.
4. Only after the catalog entry is exact-read back may the first owner transaction commit.

Thus a namespace cannot acquire an owner before it belongs to the authoritative universe.

Concurrent creators serialize through the head and catalog CAS operations. A mismatch is loud; no “assume virgin” branch exists.

### Writer append

Under the existing single namespace lane:

1. Read the cached head and prepare the candidate `RefTableState`.
2. Encode a node with:
   - `sequence = head.tip.sequence + 1`;
   - `previous = head.tip`;
   - the batched operations.
3. Conditional-create the content-derived node key.
   - If the result is ambiguous, exact-GET that key.
   - Same validated bytes mean the inert node is ready.
   - Different bytes or an unresolved read closes the lane.
4. Token-CAS `_head` from the old token to the new tip. This CAS is the sole commit point.
5. Acknowledge only after the exact head shows the proposed tip and the local candidate is installed.

If local installation throws after the head committed, do not attempt clever repair in place: close the lane and recover from `_head`. The durable operation cannot be lost.

An ambiguous head CAS closes the namespace and invokes the normal recovery-fence path. There is no special persistent wedge object and no retroactive voiding. The operation may remain unacknowledged, but acknowledged data cannot disappear.

### Recovery and old-writer races

Recovery never lists:

1. Exact-GET `ref_catalog`; verify the namespace/incarnation.
2. Exact-GET `_head`.
3. Load the exact snapshot named by `checkpoint`, if any.
4. Follow exact node links backward from `tip` to the checkpoint, verifying:
   - namespace and incarnation;
   - body hash;
   - dense decreasing sequence;
   - no cycles.
5. Replay forward.
6. Token-CAS `_head` to the same tip/checkpoint with the new `writer_epoch`.
7. Install only after that fence CAS commits.

This fence resolves every late old-writer CAS:

- If the old append CAS commits first, the fence conflicts, recovery rereads and adopts that node.
- If the fence commits first, the old CAS is permanently invalid because its expected token is stale.

Therefore every durable old transaction is either committed and replayed or inert and ignored. There is no “ghost,” no dead interval, and no timing assumption.

### GC fold

1. Exact-GET `ref_catalog`, retaining its token.
2. For every catalog entry, exact-GET `_head`.
3. Read the previous GC cursor `{incarnation, sequence, node_hash}`.
4. Follow the immutable chain from the sampled tip back to that exact cursor.
5. If the cursor is not an ancestor, or any node is missing, malformed, or over budget:
   - do not advance that namespace;
   - suppress all destructive work for the round;
   - report the exact head, node, and cursor loudly.
6. Otherwise fold every node forward, transaction-atomically.
7. Publish the new cursor as exactly the sampled tip.
8. Exact-GET `ref_catalog` again. If its token changed, abandon the fold result and retry.

A bounded vector of node keys is enough to reverse the backward walk; if its hard budget is exceeded, the round holds. No quarantine is required because the exact catalog guarantees the namespace is visited again—there is no vacuous “listing omitted the offender” round.

This proves the required property directly:

> A cursor equals a sampled authoritative head, and every node on the unique path from the old cursor to that head was exact-read and folded.

### Snapshots and ref-object cleanup

A snapshot is an immutable checkpoint:

1. Encode the full state at current tip `T`.
2. Conditional-create its content-derived key.
3. CAS `_head`, preserving `T`, to point at the new checkpoint.

Recovery trusts only the checkpoint named by `_head`.

`LIST` may still find old nodes and snapshots for cleanup, but each listed object is deleted only when point-readable facts prove:

- the GC cursor covers it;
- the current head checkpoint covers it;
- it is not the current tip or current checkpoint.

A LIST omission therefore leaks an old object. It cannot remove live history.

### Orphan-manifest sweep

Keep the sweep, but change its proof:

1. `LIST` supplies manifest deletion candidates only.
2. Exact-recover the namespace from `_head`.
3. Protect:
   - every current committed/precommit manifest;
   - every removed manifest named by exact chain nodes above the GC cursor, whose body GC still needs.
4. A current-epoch build prefix is eligible only after its build duty is conclusively settled.
   - `PartWriteTxn` must not retire a build while a head CAS capable of granting ownership is ambiguous.
   - On ambiguity, leave the durable `min_active` floor pinned and close the mount.
5. An old-epoch prefix is eligible only after that namespace head has been recovery-fenced to a newer `writer_epoch`.
6. Delete only a listed, eligible, unprotected manifest using its exact current token.

This deletes `R*`. The simpler invariant is:

> The build floor never passes a transaction that could still grant ownership.

For a retired namespace incarnation, perform best-effort physical cleanup while its catalog entry and terminal head still prove retirement. After unregistering it, later-listed unknown-incarnation debris is retained, not guessed dead.

### Namespace removal and rebirth

1. Commit one terminal removal node containing every owner `-1` and `RemoveNamespace`.
2. No later operation is legal in that incarnation.
3. GC folds through that exact terminal head.
4. GC performs one best-effort cleanup pass over incarnation-qualified manifests/files.
5. CAS-remove the catalog entry.
6. A rebirth creates a new incarnation and new empty head.

Physical emptiness is no longer a rebirth precondition. Omitted old objects cannot collide with the new incarnation. This removes cleanup markers, tombstone lineage, and physical-empty polling.

### REBUILD

REBUILD no longer needs a maintenance-fence or operator-attestation protocol:

1. Exact-read the catalog.
2. Exact-read and recover every head at its sampled tip.
3. Emit baseline `+1` edges for current owners.
4. Set each baseline cursor to that sampled tip.
5. Refuse on a missing committed manifest or any broken chain.
6. Publish the baseline with destructive work suppressed for that operation.
7. A physical blob `LIST` may nominate zero-edge blobs for condemnation; omitted blobs merely remain leaked.
8. Subsequent head commits are above the sampled cursors and fold normally.

A namespace added after the catalog sample is discovered by the next exact catalog read and starts from cursor zero. A removed namespace can leave the catalog only after its terminal node was already folded.

### fsck

fsck should separate logical integrity from physical garbage enumeration:

- Exact catalog plus exact heads define the complete logical universe.
- For each sampled head, recover its state and exact-GET every committed manifest and referenced blob.
- If the head changes while checking a namespace, retry that namespace.
- Missing catalog/head/node/checkpoint state makes the result non-clean and `unchecked`; it never becomes an empty namespace.
- Physical LIST results are only “garbage candidates observed.” fsck must not claim that no unaccounted physical objects exist, because that negative statement is unavailable under the store contract.

A clean logical result is still meaningful: every committed reference in the authoritative catalog/head graph was exact-read and validated.

## What this deletes

From current code:

- pool-wide ref-log ID allocation and safe-gap reasoning;
- LIST-based writer recovery and restart-on-vanish;
- recovery seals, synthetic `{epoch-1, UINT64_MAX}` IDs, `sealed_from`, and late-log classification;
- two global ref-prefix walks per GC fold;
- Probe A as a correctness gate—the standalone backend diagnostic may remain, but not in GC;
- LIST-derived namespace discovery;
- LIST-based fsck recovery and snapshot oracle;
- physical-empty namespace cleanup gating and `_cleanup` markers;
- old-life/rebirth lineage logic based on prior transaction IDs;
- unconditional build retirement while an owner grant is ambiguous.

From the rejected v4 design:

- `NeverBorn`;
- `_seal_latest`;
- seal-descriptor history;
- birth-time authority per namespace;
- recovery generations;
- owner bound `R*`;
- removal-admission object;
- permanent lineage tombstones and handoff;
- sticky recovery floors;
- durable quarantine records;
- recovery-seal retention pins;
- REBUILD’s proposed maintenance-fence dependency.

Keep the existing per-hash condemned metadata, multi-round condemnation, and exact-token blob deletion. Those address the independent dedup-adoption versus deletion race and become trustworthy once ref intake is complete.

## Residual risks

- Every committed ref batch adds one head CAS after the node PUT.
- `ref_catalog` is a high-value control object and a namespace-create/drop contention point. Missing or corrupt catalog state stops the pool; it must never be reconstructed from LIST.
- The catalog has an encoded-size limit. Admission must fail loudly before exceeding it. Since completed removals leave the catalog, the bound is on concurrently active/removing namespaces, not lifetime churn.
- A long unsnapshotted chain increases recovery/fold work. Budget exhaustion holds the namespace and suppresses deletion rather than guessing.
- Failed candidate nodes and incarnation-qualified objects omitted by LIST can leak permanently.
- Old build debris may be retained until its namespace head is recovery-fenced to a newer writer epoch.
- The design relies directly on the stated exact-GET and token-CAS contract. A backend violating those primitives is outside the admissible pool contract and must fail its capability probe.

The simplification is structural: two point-readable authorities—the catalog for “which streams exist” and each head for “which commits exist.” Everything else is immutable data reachable from them. No safety decision depends on proving that a listing contained everything.

v5 is substantially smaller, but it still has four independent correctness blockers and several major lifecycle gaps.

1. **Blocker — recovery still trusts LIST to identify its snapshot base.**

   **Claim attacked:** LIST is only a hint; recovery and fsck rely solely on computable point reads.

   **Proof:** Recovery can point-read a log only after it knows an epoch and sequence, and can point-read a snapshot only after it knows the snapshot ID. The catalog stores only `Live | Removing`, not a checkpoint or tail ([v5 §2](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:74)). Current recovery obtains the “newest snapshot” exclusively from `LIST` and `snapshots.back()` ([CasRefProtocol.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:735), [CasRefProtocol.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:764)), while GC legitimately deletes covered logs once that snapshot and the fold cursor cover them ([CasRefProtocol.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:697)).

   Consider logs `(E,1..100)`, snapshot `(E,100)`, and GC cleanup of those logs. A later recovery LIST omits the snapshot. The observation is indistinguishable from a legitimately empty catalog entry: recovery knows neither `E` nor an exact snapshot key. Proceeding empty loses state; failing closed bricks the legitimate-empty case. The same ambiguity covers an unclean first epoch whose hidden first append is still in flight, and burned epochs make guessing `my_epoch-1` invalid.

   **Smallest fix:** add a point-readable per-namespace authority containing at least incarnation, authoritative checkpoint ID, and last epoch seal; or adopt `_head`. Alternatively, covered logs cannot be cleaned. The head design’s exact checkpoint is not optional information ([simplification consult](/home/mfilimonov/workspace/ClickHouse/master/tmp/codex_simplify_design.md:95)).

2. **Blocker — `DefiniteFailure` does not prove that the ID is reusable.**

   **Claim attacked:** an ID is reused after `DefiniteFailure` because its key is proven unwritten.

   **Proof:** The controller may send attempt A, receive an ambiguous timeout, exact-GET absence, and reissue. If attempt B returns a whitelisted definite rejection, it immediately returns `DefiniteFailure` without considering A ([CasRequestControl.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:297), [CasRequestControl.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:329)). `attempt_timeout_ms` is only a scheduling check and cannot stop A from landing later ([CasRequestControl.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h:136)).

   Thus chunk N commits; chunk N+1 receives ambiguous A followed by definite B; the next flush reuses N+1 for different bytes; then A lands. Two records cannot coexist, but the wrong transaction may occupy the key and be adopted as durable. The historical contract correctly says only definite failure of *every* attempt permits a gap ([2026-07-11 design](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-11-cas-ref-table-snapshot-log-design.md:497)).

   `NoAttemptSent` and local encode/apply failures before the PUT are safe; the defect is specifically a definite result after an earlier ambiguous send.

   **Smallest fix:** once any attempt was ambiguous, a later definite rejection may stop reissuing but the logical outcome must remain `Unresolved`; retain the wedge until exact occupancy resolves it. Reuse only if no request was sent or every sent attempt has its own conclusive non-application proof.

3. **Blocker — catalog entries need namespace incarnations.**

   **Claim attacked:** `Live | Removing` plus physical cleanup safely supports entry deletion and namespace rebirth.

   **Proof:** Namespace cleanup still enumerates manifests and files with LIST and declares a pass complete after the returned pages end ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2182)). If LIST omits an old manifest or verbatim file, GC may delete the catalog entry and allow rebirth while that object remains under the same namespace-qualified key ([CasLayout.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:175), [CasLayout.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:198)). The stale file can then be read as part of the new namespace.

   Ref keys also become reusable on a warm rebirth: after GC removes the old stream, cache eviction discards `greatest_applied` ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:762)); recovery sees empty and the same writer epoch starts again at sequence one. A delayed old retry can then occupy the reborn stream’s key. Current cleanup explicitly relies on recreated keys having a greater writer epoch ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2105)), which warm rebirth does not guarantee after this invariant change.

   This also regresses historical invariant I8, which required incarnation isolation ([2026-07-10 design](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-10-cas-ref-snapshot-log-design.md:151)).

   **Smallest fix:** store a random incarnation in each catalog entry and qualify ref logs, snapshots, manifests, files, fold cursors, and cleanup state with it, as the alternative already specifies ([simplification consult](/home/mfilimonov/workspace/ClickHouse/master/tmp/codex_simplify_design.md:63)).

4. **Blocker — the catalog does not make `REBUILD` condemnation safe.**

   **Claim attacked:** after catalog adoption, physical blob LIST omissions can only leak and can never cause condemnation.

   **Proof:** `REBUILD` has a second logical universe: unowned manifests belonging to builds not proven dead. It discovers those through manifest LIST and adds their edges to `edge_bearing` ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2708)). It then LISTs physical blobs and condemns each listed blob absent from that set ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2739)).

   If the manifest LIST omits a live unowned manifest while the blob LIST includes one of its blobs, `REBUILD` seeds that blob as zero-edge. The catalog fixes ref-stream discovery, but not build/manifest discovery. v5 specifically removes the maintenance fence, so a live build may exist during this comparison.

   **Smallest fix:** do not perform zero-edge condemnation during `REBUILD` unless all edge-bearing manifest/build inputs have an exact authoritative universe. The immediate safe repair is to disable this condemnation and accept leakage; the stronger repair needs a writer/build maintenance fence or registry.

5. **Major — `maybeSweepStalePrecommits` does not reclaim current-epoch manifest bodies.**

   **Claim attacked:** live-epoch orphans are already the writer’s job, including builds that die mid-flight.

   **Proof:** `PartWriteTxn` destruction only retires the build sequence ([CasPartWriteTxn.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:119)). `maybeSweepStalePrecommits` removes only owner bindings whose manifest epoch is *older* than the live epoch ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2677)). It neither enumerates nor deletes current-epoch manifest bodies. Explicit cleanup is best-effort and swallows errors because GC is documented as its backstop ([CasPartWriteTxn.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:1438)).

   Repeatedly staging manifests and destroying builds without `abandon` therefore leaks without bound during a long-lived mount, while v5’s sweep refuses those bodies until the epoch is sealed.

   **Smallest fix:** add a durable writer cleanup queue retried to completion, or retain a durable build-duty authority that lets the sweep prove current-epoch builds settled. Do not retire a build while cleanup or an owner-grant outcome remains uncertain.

6. **Major — the two-state catalog has unreconciled partial transitions and a capacity deadlock.**

   **Claim attacked:** add-before-append and `Live → Removing` before removal are complete lifecycle protocols.

   **Proof:** If creation CASes `Live` and the first append never occurs, the ref table remains never-born. Current `dropNamespace` treats that state as a successful no-op ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2897)), while v5 permits only GC to delete the catalog entry after a terminal removal has folded. No such record exists.

   Conversely, if `Live → Removing` commits and the terminal append fails, ordinary mutations are barred, GC has no terminal removal to fold, and no actor is assigned to finish or roll back the transition.

   The cap rule worsens this: v5 explicitly allows the cap to refuse a `Live → Removing` transition. A catalog full of `Live` entries can then admit neither creation nor removal, so its size can never decrease. Namespace length is currently unbounded ([CasLayout.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.cpp:258)), and each catalog entry also induces a fold-seal cursor under a 256 MiB object cap ([CasFoldSealFormat.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:91), [CasFormat.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:102)).

   **Smallest fix:** define durable `Creating`, `Live`, and `Removing` reconciliation, including who idempotently completes each interrupted transition. Charge each new entry its worst-case `Removing` plus fold-seal representation so removal can never be refused for capacity, and impose or account for exact namespace byte length.

7. **Major — the CAS-walk’s outcome mapping does not match the actual controller.**

   **Claim attacked:** when the seal loses, recovery adopts the occupying record; when it wins, the old append is conclusively rejected and fenced.

   **Proof:** The ordinary immutable controller maps `PreconditionFailed` to exact GET, but throws `CORRUPTED_DATA` when that GET finds different bytes ([CasRequestControl.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:315), [CasRequestControl.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:246)). Therefore a seal racing an existing ref record does not produce “occupied, adopt these bytes”; it throws.

   In the other direction, an old append that reads the seal as different bytes enters the current catch arm, which declares a safe gap and leaves the lane usable rather than applying v5’s stated interference/fence policy ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2019)). The predecessor’s expired mount fence should prevent a later append, but that is different from the outcome contract v5 claims and tests.

   **Smallest fix:** introduce a dedicated slot-occupy primitive returning `Created`, `Occupied(GetResult)`, or `Unresolved`. It must preserve the actual occupant bytes/token, decode an occupied `RefLogTxn` or `EpochSeal`, and terminate immediately when it adopts a seal. Test both directions through the real retry controller.

8. **Major — clean epoch transitions contradict the seal rule.**

   **Claim attacked:** every epoch crossing is mediated by an `EpochSeal`, while recovery costs a seal only on an unclean remount.

   **Proof:** The fold refuses to cross epochs without a consumed seal, and recovery step 3 appears unconditional. But the cost section says `+1` seal PUT only per *unclean* remount ([v5 §8](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:198)). Current mounting explicitly distinguishes clean predecessors and does not set the unclean boundary ([CasPool.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:665)); current lazy recovery publishes a seal only when that boundary matches ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:611)).

   If that condition remains, an epoch with acknowledged records can close cleanly without a seal and the next epoch is unreachable to the fold. If recovery seals clean predecessors too, the correctness story works but the cost claim is false.

   **Smallest fix:** require the first recovery/touch after every writer-epoch transition—clean or unclean—to CAS-walk and seal the previous nonempty stream. Specify that an adopted `EpochSeal` terminates the walk. Correct the cost to one seal per touched namespace per epoch transition.

9. **Major — consuming a build’s epoch seal is insufficient for manifest deletion.**

   **Claim attacked:** after the cursor consumes build epoch `E`’s seal, “no owner” is complete enough to delete the build’s manifest.

   **Proof:** New owner grants do stay in their build’s epoch: `requireAlive` rejects a pre-remount build ([CasPartWriteTxn.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:126)). But owner removals do not. After remount, `maybeSweepStalePrecommits` appends an old build’s precommit removal in epoch `E+1` ([CasRefLedger.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2714)).

   If the orphan sweep deletes the manifest after consuming only epoch `E`’s seal, a later fold of that `E+1` `-1` cannot read the body. Current folding treats a missing removed-precommit body as “never activated” and skips the decrement ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1521)), stranding the earlier `+1`. v5 names the S42 fix but does not specify it normatively.

   **Smallest fix:** retain every manifest named by any unconsumed removal record, reading that tail arithmetically, until the fold cursor passes the removal itself. Alternatively, persist the blob-edge decrement independently of the manifest body.

10. **Minor — the one-enumeration and verification plans are not yet falsifiable as written.**

    **Claim attacked:** the second walk existed only for Probe A, and §10’s tests demonstrate the new guarantees.

    **Proof:** The current second scan is the strict `groupRefKeys` input and supplies ref intake plus cleanup planning, not just Probe A ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1006)). The first scan is lenient and retains only log IDs for defer/Probe A ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2449)). One enumeration is attainable, but only by making the first scan strict and retaining all key classes.

    The proposed tests also miss the counterexamples above: `DefiniteFailure` after an earlier ambiguous attempt; hidden latest snapshot after covered logs were cleaned; catalog cleanup omitting old objects before rebirth; asymmetric manifest/blob omissions during `REBUILD`; and current-epoch body growth after `PartWriteTxn` destruction. A single-attempt reuse test or stale-binding count can remain green while these failures persist, contrary to INTENT’s “green that cannot go red” rule ([INTENT.md](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/INTENT.md:18)).

    **Smallest fix:** retain and strictly group the first enumeration, and add negative controls for each interleaving above using actual controller and cleanup paths.

The A1 audit itself survived: ref-log/snapshot/cleanup keys include the namespace ([CasLayout.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:126)); fold coverage and cleanup items are namespace-qualified ([CasFoldSealFormat.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:74)); and the late-log dedup key contains namespace plus rendered ID. B1/B2 can also represent `EpochSeal` correctly if B1 counts it and B2 leaves `produced=false`. Those local successes do not repair the missing stream authority or incarnation boundary.

REJECT

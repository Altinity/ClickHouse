1. **Blocker — `_seal_latest` forgets earlier, still-unconsumed void intervals.**

   **Claim attacked.** The latest pointer is sufficient to certify every late record without trusting `LIST` ([spec §4](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:88), [rule 1](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:146)).

   **Proof.** Let the fold cursor be `A`. Recovery 1 publishes `S1` with `sealed_from=A`, so `(A,S1]` is void. Before the fold consumes that interval, recovery 2 publishes `S2`; because recovery 1 advanced the recovered state to `S1`, recovery 2 naturally has `sealed_from=S1`. `_seal_latest` is overwritten with only `{S2,S1}`. A first-recovery ghost `V`, with `A<V<=S1` and `V.prev=A`, then surfaces. It is outside the only retained interval `(S1,S2]`, so rule 2 folds it as a plain continuation.

   Current seal construction confirms that `sealed_from` is the recovered greatest-applied ID, not a cumulative interval root ([CasRefLedger.cpp:601](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:601)). Consecutive seals are adjacent, but the pointer is not a history. The §11 tests omit consecutive unclean recoveries before folding.

   **Smallest fix.** Make the pointer the head of an immutable seal-descriptor chain: every seal records the previous seal descriptor ID independently of ordinary log ancestry. Before folding, walk all unconsumed seal descriptors whose intervals overlap the cursor. Pin that chain until the cursor passes each interval. A convex cumulative interval is not sufficient because valid records can exist between recovery intervals.

2. **Blocker — §5c’s containment mechanism does not exist, and rule 0 prevents the promised detection.**

   **Claim attacked.** Every mount acknowledges past the minting round, and the recovering mount performs a pointer-aware fold before its acknowledgement lands ([spec §5c](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:195)).

   **Proof.** `MountLease` contains `min_active`, but no GC-round acknowledgement or recovery generation ([CasServerRootFormats.h:43](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h:43), [codec](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.cpp:119)). The GC explicitly says graduation is paced by GC rounds, “not on heartbeat acks” ([CasGc.cpp:321](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:321)). A condemned row graduates solely when `condemn_round < current_round` and is deleted the following pass ([CasBlobInDegree.cpp:441](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:441)).

   Concrete schedule:

   - Round `R` reads the old pointer and folds a ghost `-1`, falsely condemning a still-owned blob.
   - Recovery publishes the new pointer, but the recovering mount remains down, is a GC follower, or never runs a GC fold.
   - `R+1` publishes `delete_pending`.
   - `R+2` executes the exact-token deletion.

   Worse, rule 0 skips a namespace with no candidates ([spec:137](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:137)). Once the ghost became the cursor and the namespace is quiet, no later fold reads the new pointer, so the breach is never detected. A third mount is irrelevant; a fenced or expired recovering mount is excluded from liveness handling anyway.

   **Smallest fix.** Specify the fallback now, not after TLA finds the counterexample. One workable form is a durable pool-wide recovery generation: recovery updates it after the pointer and before install; folds sample and recheck it before adoption; a generation change forces point-reading every carried namespace, including quiet ones, before the fold seal acknowledges that generation. Graduation requires the condemned generation to equal this acknowledged generation. Merely comparing a number without forcing that revalidation would retain forever without repairing the false fold.

3. **Blocker — absent-pointer-equals-clean is a fail-open corruption path.**

   **Claim attacked.** An absent `_seal_latest` proves that no unclean boundary ever existed ([spec:137](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:137)).

   **Proof.** Delete, roll back, or lose the pointer after a seal was published. A subsequent point `GET` returns absent and a ghost folds normally. Requiring “no listed seal and no fold-seal entry” does not fix this: both corroborating facts are obtained through the same untrusted listing or can have been legitimately retired. This directly violates INTENT’s fail-closed rule ([INTENT:31](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/INTENT.md:31)).

   The only absence test in §11 is the healthy clean-history case; there is no RED test for pointer deletion, rollback, or undecodable bytes ([spec:288](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:288)).

   **Smallest fix.** Give every namespace a permanent fixed-key authority from first birth, including clean birth. Thereafter, missing or undecodable authority is a hold/corruption condition, never clean history. If avoiding an extra first-append request is mandatory, an independently point-readable namespace registry is required; `LIST` cannot prove virginity.

4. **Blocker — the `min_active`/`R*` pair has no specified linearization point.**

   **Claim attacked.** “Retire build → sample allocator → publish lease” is enforced ([spec:106](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:106)).

   **Proof.** Today `minActive` is read under `builds_mutex` ([CasMountRuntime.cpp:144](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:144)), while ref IDs come from a separate ledger atomic ([CasRefLedger.h:533](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:533)). The heartbeat callback samples only `min_active` ([CasMountRuntime.cpp:226](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:226), [CasServerRoot.cpp:745](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:745)).

   A heartbeat can sample highest allocated ID `9`; active build `B` then allocates grants `10` and `11` and retires; the heartbeat then observes the advanced floor and publishes it paired with `9`. A chain cut through `9` can authorize sweeping `B` while grant `11` later resolves durable. Sampling raw `next_ref_sequence` instead introduces an undocumented off-by-one and covers at most the immediately next allocation.

   The first-lease remedy is also not wired through the two writers: `claimMount` constructs the initial body ([CasServerRoot.cpp:267](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:267)), then keeper adoption rewrites a separately constructed body ([CasServerRoot.cpp:757](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp:757)).

   **Smallest fix.** Maintain a durable-ready pair inside `CasMountRuntime`. Under `builds_mutex`, derive the new floor first and then sample an explicitly defined inclusive highest-allocated ID; retirement updates that pair before releasing the lock. Heartbeat publishes only the stored pair. Initialize `{E−1,MAX}` in both claim and keeper adoption, with a RED interleaving test that pauses between allocator sampling, retirement, and lease publication.

5. **Major — a later first pointer invalidates a valid clean-history genesis.**

   **Claim attacked.** `{0,0}` is accepted only while the pointer is absent ([spec anchor table](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:120)).

   **Proof.** A fresh clean namespace writes genesis record `G(prev=0)`. Before GC folds it, a later unclean recovery publishes `S` with `sealed_from=G` and creates the namespace’s first pointer. `G` is valid and is not void—the interval is `(G,S]`—but the acceptance table now rejects `G.prev` solely because the pointer exists. Full replay and `fsck` encounter the same contradiction even if an incremental cursor had previously passed `G`.

   A related format hole exists when a clean-history namespace is removed before any seal: handoff must create a lineage-only pointer, but `{seal_id,sealed_from,lineage}` defines no “no seal component” state.

   **Smallest fix.** Persist an immutable birth mode/anchor in the fixed authority: clean genesis ID versus `NeverBorn` seal ID. Base `{0,0}` acceptance on that birth fact and lineage, not current pointer presence. Make the seal component optional for lineage-only pointers.

6. **Major — seal retry and semantic pointer CAS are not a complete state machine.**

   **Claim attacked.** Seal → pointer CAS → install is restart-safe, monotone by `seal_id`, and field-merging ([spec:93](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:93)).

   **Proof.** The available primitive is token CAS only; it neither compares `seal_id` nor merges fields ([CasBackend.h:238](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasBackend.h:238), [CasObjectStorageBackend.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:897)). Those semantics must be implemented by a read/decode/merge/CAS loop.

   There is also a retry trap:

   - Immutable seal `S` commits.
   - Pointer CAS fails or is ambiguous.
   - Recovery restarts, but `LIST` omits `S` and now exposes another late record.
   - Re-encoding the same seal key produces different bytes.

   The current exact-key resolver accepts only byte-identical bytes and treats different bytes as corruption ([CasRequestControl.cpp:246](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.cpp:246)). Current recovery also skips seal publication when it believes the seal is already present ([CasRefLedger.cpp:603](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:603)); pointer reconciliation must not accidentally sit only inside the “new seal” branch.

   **Smallest fix.** Define one idempotent publication routine: exact-`GET` the derived seal key and adopt its decoded bytes if present; otherwise publish it; then repeatedly `GET` the pointer, reject a newer seal as supersession, merge all fields, CAS by token, and resolve ambiguous outcomes by exact re-read. Install only after the current pointer contains the exact adopted seal descriptor. Run this routine even when the seal pre-existed.

7. **Major — `fsck` is called “seal-aware” without requiring it to read the authoritative pointer.**

   **Claim attacked.** The new oracle cannot produce a false clean ([spec §10](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:255)).

   **Proof.** Current `fsck` discovers snapshots and logs solely by `LIST` ([CasFsck.cpp:197](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:197)), silently skips when the expected evidence is unavailable ([CasFsck.cpp:210](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:210)), and `clean` has no unchecked term today ([CasFsck.h:127](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.h:127)). “Seal-aware” could be implemented merely by special-casing synthetic snapshot IDs, while still missing a pointer/seal disagreement and accepting a listed ghost.

   **Smallest fix.** Require a point read of the authority for every known namespace; traverse the complete seal-descriptor history from finding 1; exact-`GET` and verify every referenced seal; apply void/breach rules during replay. Missing, malformed, regressed, or pointer/seal-mismatched state must be fatal `unchecked`/corruption. Without a maintenance fence or operator attestation, namespace-universe completeness remains unproven and the hot whole-pool scan must stay `unchecked`.

8. **Major — quarantine revalidation can clear on a vacuous listing.**

   **Claim attacked.** A later round whose reads succeed and whose chain verifies safely clears quarantine ([spec §5b](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:185)).

   **Proof.** The specification persists the offending link/anchor but never requires revalidation to start from it. If a later `LIST` omits the offender and yields no candidates, rule 0 skips the namespace; treating that empty walk as “chain verifies” would clear the only pool-wide destructive suppression. The §11 test checks carried suppression but has no negative test for vacuous or stale-pointer revalidation ([spec:302](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:302)).

   **Smallest fix.** Clearing must exact-`GET` the persisted offender and anchor, reread the current pointer/generation, and verify a chain from the durable pre-hold cursor through that evidence. Omission, 404, pointer change, or budget exhaustion carries quarantine unchanged.

9. **Note — the backend CAS primitive is feasible, but the support matrix should be stated accurately.**

   **Claim checked.** `gc/state` uses `Backend::casPut`; native mode maps arbitrary keys to `If-Match`/`If-None-Match`, while local/emulated mode serializes and token-checks in process ([CasObjectStorageBackend.cpp:897](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:897)). `Cas::Probe` verifies create, stale-token conflict, and current-token replacement ([CasProbe.cpp:129](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasProbe.cpp:129)).

   RustFS/AWS and the S3-compatible GCS path can therefore carry the pointer. Local uses `EmulatedSingleProcess`. Azure/non-S3 native object storage is not currently an admitted writable CAS backend: mount rejects it when it cannot honor the single-attempt retry profile ([CasObjectStorageBackend.cpp:86](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasObjectStorageBackend.cpp:86)). No primitive change is needed, but the spec must not imply Azure support and must implement the semantic/retry layer from finding 6.

Conditional on a correct semantic pointer merge, §6’s deposed-leader handoff is sound because only the fold seal adopted by `gc/state` can drop the entry and later pointer updates cannot erase lineage. The precise `NeverBorn` codec exception, ledger-level sticky floor, final supersession check, cut-scoped B1 accounting, retention pins, REBUILD refusal/attestation, and narrowed performance statement otherwise apply the round-3 remedies coherently. They do not repair the blockers above.

REJECT

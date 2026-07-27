v7 resolves much of round 6, especially `_ckpt`, seal grammar, and recovery fencing. It still has two acknowledged-data-loss counterexamples in the new fold algorithm, so it has not converged.

1. **blocker — “quiet = zero cost” still trusts LIST to say whether a namespace has new records.**

   **Claim attacked:** §5 says arithmetic walking happens only “per namespace with hinted candidates”; an unhinted namespace carries its cursor verbatim ([v7:131](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:131)). The frontier probe is only optional in the cost model ([v7:185](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:185)).

   **Proving scenario:**

   1. Blob `b` initially has one folded source edge from namespace `B`.
   2. Namespace `A` durably commits and acknowledges a new ref to `b` at its exact next slot.
   3. The round’s strict LIST omits every new key for `A`, but returns `B`’s removal of its edge.
   4. v7 carries `A`’s old cursor without issuing `GET(A,cursor+1)`, folds `B`’s `-1`, and observes no anomaly.
   5. `b` reaches zero, passes the normal two-phase pipeline, and is deleted while the acknowledged ref in `A` still names it.

   This is the original defect across two namespaces. Per-namespace cursor immobility is insufficient when blob in-degree and deletion are pool-wide. The current intake is indeed driven solely by keys returned to `groupRefKeys` ([CasRefProtocol.cpp:646](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:646)).

   **Smallest fix:** every destructive round must exact-GET `cursor+1` for every `Live` or `Removing` catalog entry. If present, walk arithmetically until an absent frontier; if the common budget expires before every catalog namespace has a frontier proof, partial cursor advances may be sealed but all destructive work must remain suppressed. Quiet namespaces therefore cost one exact 404 per round. The alternative is an authoritative per-namespace head object.

2. **blocker — the per-namespace hold has no specified durable carrier.**

   **Claim attacked:** impossible shapes cause a hold and pool-wide suppression “while carried,” while unhinted namespaces carry verbatim ([v7:137](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:137)).

   **Proving scenario:**

   1. Round `R` observes a higher record but finds a missing or malformed expected slot for `A`. It holds `A` at cursor `C` and suppresses deletion.
   2. The adopted seal persists only cursor `C`; v7 defines no durable held/backoff state.
   3. Round `R+1` receives no hint for `A`, carries `C`, reconstructs no anomaly, and resumes destructive work.
   4. A visible `-1` elsewhere can now delete a blob protected by the unaccounted acknowledged `+1` behind `A`’s hold.

   The existing `CasFoldSeal` schema carries coverage, run references, and cleanup items, but no quarantine/hold record ([CasFoldSealFormat.h:84](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:84)). Existing `suppress_destructive` is recomputed only from anomalies in the current pass ([CasGc.cpp:1817](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1817)). `ShardCoverage::classification == 4` could be the carrier, but neither v7 nor current code defines “OR suppression over carried classification.”

   **Smallest fix:** persist `{Held, reason-code, retry/backoff state}` per `(namespace, incarnation)` in the fold seal—or explicitly make classification `4` that state. Every carried hold must force an exact retry even without a hint and must keep pool-wide destruction suppressed until a successful frontier walk or an explicit operator repair clears it.

3. **major — orphan-blob nomination is neither failure-atomic nor representable by the current reducer contract.**

   **Claim attacked:** deleting a proven-dead orphan manifest nominates its blobs through the normal condemn pipeline with exact-token discipline unchanged ([v7:162](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:162)).

   **Proving scenario:** the sweep exact-deletes manifest `M`, then the process dies before the nominations are adopted by `gc/state`. `M` is no longer rediscoverable, its blobs have no in-degree rows, and condemn-nothing REBUILD will preserve the leak forever.

   A synthetic `BlobDelta` is not a drop-in solution. Every delta carries a ref-transaction ordinal ([CasBlobInDegree.h:138](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.h:138)), and the reducer unconditionally indexes B2’s `out_applied_by_txn_ordinal` for consumed deltas ([CasBlobInDegree.cpp:594](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:594)). A reserved unmatched `-1` would also pollute the unmatched-removal correctness signal.

   **Smallest fix:** exact-GET and decode `M`, feed its `BlobRef`s through a separate nomination input that performs a neutral “touch and current-count recheck” without B2 or unmatched-remove accounting, adopt those results in the fold seal/`gc/state` CAS, and only then exact-delete `M`. A lost CAS leaves `M`; process death after adoption but before deletion merely causes an idempotent retry.

   The other sweep directions hold conditionally:

   - Advancing the fold cursor between scan rounds is sound because removals at or below the adopted cursor have already folded. Deletion must explicitly occur after adoption, and “affected candidates” must mean every candidate in a namespace whose frontier was not reached.
   - The successor eligibility argument works: the successor closes epoch `E`; after GC consumes that seal, an unowned epoch-`E` manifest with no unconsumed removal is eligible. The remaining liveness dependency is finding a successor/decommission actor.

4. **major — incarnation wiring still lacks a closed handle and physical-identity protocol.**

   **Claim attacked:** every namespace-scoped family/state is qualified, and a stale cached handle is rejected by catalog mismatch without adding a hot-path catalog read ([v7:65](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:65)).

   The current surfaces are broader than v7’s enumeration:

   | Surface | Current identity | v7 status |
   |---|---|---|
   | Ref log/snapshot keys | namespace + transaction ID ([CasLayout.h:110](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:110)) | Key qualification named; body/key binding still repeats only namespace and ID |
   | `_ckpt` | absent today | Named and qualified |
   | Verbatim files | namespace + filename ([CasLayout.h:175](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:175)) | Key named; call-chain handle not designed |
   | Manifests | namespace + mount-global build ID ([CasLayout.h:198](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.h:198)) | Omitted from v7’s “every key family” list |
   | `ManifestId` / manifest body | namespace only ([CasTypes.h:131](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h:131), [CasPartManifestFormat.h:67](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h:67)) | Omitted |
   | `PartWriteTxn` | `RootNamespace`, epoch, build sequence ([CasPartWriteTxn.h:361](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.h:361)) | Only a generic recreation gate is mentioned |
   | Ref-table cache | `ref_tables[ns.string()]` ([CasRefLedger.cpp:385](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:385)) | Generic “cached handles” named, no owner/check algorithm |
   | Fold cursor/cleanup/hold | namespace-based ([CasGcShardPlan.h:138](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGcShardPlan.h:138)) | Cursor and cleanup named; hold is missing |
   | Decommission/fsck/sweep | LIST-derived namespace or `ManifestId` | Catalog migration not fully wired |

   Nobody caches a namespace handle today. `RootNamespace` is only an opaque string ([CasTypes.h:42](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Primitives/CasTypes.h:42)). `CasPlainObjects` accepts only that string, and reads are deliberately not fence-gated ([CasPlainObjects.h:45](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.h:45)). Above it, `namespaceFilesReadable` separately checks the ref-table lifecycle and callers later perform get/list operations by namespace, leaving a rebirth TOCTOU ([ContentAddressedMetadataStorage.cpp:1234](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1234)).

   Qualifying old files structurally prevents aliasing, but it does not make “catalog mismatch rejected” true: a hot reader cannot know another process replaced the catalog entry without a read, a locally observable fence transition, or a bounded refresh.

   **Smallest fix:** introduce one typed `NamespaceHandle{namespace, incarnation, local_life_generation}` and require it throughout `Pool`, `CasPlainObjects`, `PartWriteTxn`, ref recovery/publishing, cleanup, and upper metadata operations. Maintain a cached catalog snapshot in `Pool`; local lifecycle changes invalidate the shared life token, and remotely initiated changes are safe only after the old mount’s locally checkable fence/lifecycle becomes terminal. Either incarnation-qualify manifests and their body identity too, or explicitly exclude them with a proof and a cleanup/sweep protocol that cannot let a deposed old-life worker enumerate new-life manifests.

5. **major — catalog CAS serialization does not by itself reserve aggregate fold-seal capacity.**

   **Claim attacked:** creation atomically checks the catalog plus “the entry’s” worst-case lifetime cost against both caps ([v7:73](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:73)).

   Concurrent creations cannot both win against one catalog token; that part is sound. The missing piece is an additive predicate. If each CAS checks:

   ```text
   encoded_catalog(post) + reserve(new_entry) <= fold_seal_cap
   ```

   every creation can pass while the eventual fold seal exceeds its 256 MiB cap ([CasFormat.cpp:102](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:102)). Existing entries’ cursor, cleanup, token, and now hold reservations are not represented by their catalog bytes.

   **Smallest fix:** define and test:

   ```text
   encoded_catalog(post).size <= catalog_cap
   fold_fixed
     + Σ reserve_cursor(entry)
     + Σ reserve_removing_cleanup(entry)
     + Σ reserve_hold(entry)
     <= fold_seal_cap
   ```

   The sum is computable at admission once namespace length, token encoding, reason strings, and all record counts have fixed maxima. No separate reservation object is needed; the catalog itself is the serialized admission ledger.

6. **major — permanently decommissioned owners can strand `Removing` indefinitely.**

   **Claim attacked:** the owner or a fenced successor completes removal on its next mount ([v7:111](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:111)).

   The required successor already exists conceptually: the pool-member decommission command claims the victim as a temporary writer ([decommission design:58](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-13-cas-pool-member-decommission-design.md:58), [CasPool.cpp:720](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:720)). But current decommission discovers namespaces by LIST ([CasDecommission.cpp:116](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp:116)). A hidden namespace whose catalog entry is already `Removing` can therefore be missed; the command then retires the server-root slot, eliminating the only legal sequencer and permanently consuming catalog capacity. v7 instead labels decommission fencing out of scope ([v7:276](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:276)).

   **Smallest fix:** name the dependency on `2026-07-13-cas-pool-member-decommission-design.md` and update it in the same rollout: after claiming the victim, enumerate its catalog entries exactly, treat `Removing` without a terminal record as resumable writer work, and forbid slot retirement while any entry owned by that server root remains.

7. **major — the only safe migration for existing soak pools is recreation, but v7 does not say so.**

   **Claim attacked:** “pre-release; no compat scaffolding” plus the wiring order is a complete migration rule ([v7:190](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:190)).

   An existing pool has:

   - sparse, pool-wide ref IDs;
   - no catalog or `_ckpt`;
   - legacy namespace-only keys;
   - cleanup that may delete snapshots using the old listing-derived rule.

   It cannot safely bootstrap `_ckpt` or the catalog from the same incomplete LIST this design distrusts. Starting new publishers while an old cleanup binary can still delete at-or-above-checkpoint candidates recreates the dangling-pointer race. The verification rule says missing catalog/`_ckpt` fails closed, so existing soak pools simply become unopenable without an explicit procedure.

   **Smallest fix:** state that there is no in-place migration: bump the pool format, require a quiesced upgrade, and recreate existing pre-release/soak pools. Startup against the old format should fail with a message that names pool recreation. If preserving a pool is required, that is a separate offline migration protocol, not compatibility scaffolding.

8. **major — wedge retry lacks an operation envelope, though the successor race itself is safe.**

   **Claim attacked:** an all-ambiguous wedge retries under the original fence generation with bounded backoff ([v7:39](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:39)).

   `CasMountRuntime` already has the needed generation primitive ([CasMountRuntime.h:136](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h:136)), but `RefAppendWedge` currently stores only ID, key, and bytes ([CasRefLedger.h:381](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:381)). Each call to the request controller establishes a fresh 90-second deadline ([CasRequestControl.h:141](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Backend/CasRequestControl.h:141)); repeatedly creating calls in a background retry loop defeats that contract.

   The self-remount race is safe if generation checks are respected:

   - old PUT wins first → successor `slot-occupy` observes and adopts the transaction;
   - successor seal wins first → the old conditional create is rejected, and the seal occupies the slot;
   - rearm before the old send → the generation mismatch refuses the old send.

   A successor’s valid `EpochSeal` at the key must be treated as conclusive rejection of the old transaction, not impossible corruption.

   **Smallest fix:** store the admitted fence generation in the wedge. Let each foreground lane flush perform at most one normal bounded controller operation with that generation; unresolved returns uncertainty and retains the wedge for the next caller/remount. If an autonomous worker is desired, give it one non-renewing overall deadline rather than resetting `operation_deadline_ms` indefinitely.

9. **minor — `_ckpt` is directionally sound, but its convergence bounds need two explicit clauses.**

   The adversarial merges now work:

   - an older snapshot publisher racing a newer sealer preserves both fields;
   - two publishers do not require single-flight for correctness because semantic maximum makes the lower publication a no-op;
   - strictly-below retention prevents the PUT-before-pointer dangling race;
   - missing base plus changed token restarts, while unchanged authority fails closed.

   Two implementation-level requirements remain:

   - If merging the proposed field values produces the exact current body, return without a same-bytes CAS; otherwise redundant writers rotate tokens and create avoidable conflict/restart churn.
   - Bind the retry/restart loop to the existing recovery deadline and bounded-restart accounting. A legitimate hostile publisher should be excluded by the same owner/fence-generation serialization, and the specification should state that fact rather than permitting an unbounded “advanced again” loop.

   The old listing-based deletion rule during rollout is not compatible; finding 7’s recreate-only migration is the smallest resolution.

10. **minor — §8–§10 do not yet test or cost the protocol v7 actually needs.**

   **Claim attacked:** the deletions, cost model, and negative tests are internally complete.

   Problems:

   - §9 says carry-forward is deleted as a special case while §5 relies on unhinted namespaces carrying state verbatim ([v7:195](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:195)).
   - The strict parser currently accepts only `_log`, `_snap`, and `_cleanup`, deriving namespace from everything before the kind segment ([CasLayout.cpp:96](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasLayout.cpp:96)). v7 needs a canonical right-to-left grammar for `<ns>/<incarnation>/{_log,_snap,_ckpt}` and an explicit legacy-key refusal policy.
   - The cost model’s optional frontier probe becomes one mandatory point GET per catalog namespace because of finding 1. Deep arithmetic walks still dominate backlog cost; the three-write creation accounting is now correct.
   - Missing negative controls include:
     - wholly hidden `A:+1` versus visible `B:-1` on the same blob;
     - carried hold followed by a round omitting the offending namespace;
     - process death before and after nomination adoption;
     - many serialized catalog admissions exceeding aggregate fold reservation;
     - decommission with a `Removing` catalog entry omitted by LIST;
     - opening a legacy soak pool;
     - stale-handle rebirth between `namespaceFilesReadable` and the subsequent verbatim operation.

   These must be go-red controls under `HoleyListBackend`, consistent with `INTENT`’s requirement that a guarantee fail without its mechanism ([INTENT.md:18](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/INTENT.md:18)).

## Round-6 disposition

| Round-6 finding | Status in v7 | Proof |
|---|---|---|
| 1. Incarnation alternative deferred | **Partial** | v7 chooses explicit incarnations and names refs, `_ckpt`, verbatim, cursors, cleanup, and handles, but omits manifest/`PartWriteTxn` identity and the cheap stale-handle check protocol. |
| 2. `_ckpt` state machine missing | **Resolved** | Semantic-max CAS, strict-below retention, authority-token revalidation, and exact-delete-before-catalog ordering are all explicit. Only retry bounds/no-op optimization remain. |
| 3. GC is an illegal removal sequencer | **Partial** | GC no longer appends and owner/fenced-successor authority is correct; decommission adoption and catalog enumeration are not connected. |
| 4. Recovery overlaps self-remount | **Resolved** | Generation capture gates `slot-occupy`, `_ckpt`, and install; remount must cancel/wait recovery before rearm. |
| 5. Cleanup queue called durable without representation | **Partial** | v7 honestly calls it in-memory and names successor sealing, but never-touched/decommissioned namespaces still lack a guaranteed adopter. |
| 6. Unbounded removal-tail scan | **Resolved** | One shared scan per namespace, common budget, retain-on-incomplete, and resume from the fold cursor are specified; post-adoption ordering should be made explicit. |
| 7. Permanent absent-key wedge | **Partial** | Same-key/same-bytes retry under the original generation is stated, but the retry actor and non-resetting operation envelope are not. |
| 8. Catalog reconciliation/capacity assertions | **Partial** | Creator terminality and exact-token reconciliation are resolved; aggregate fold-seal reservation is not. |
| 9. Orphan blobs never enter condemnation | **Partial** | Nomination is named, but no failure-atomic ordering or reducer input compatible with B2/exact-token semantics is specified. |
| 10. `EpochSeal` grammar missing | **Resolved** | Solo seal operation and exact sequence-1 `prev_epoch_seal` rules, including the empty-epoch case, are explicit. |
| 11. `_cleanup` deletion order and `_ckpt` parsing | **Partial** | Landing order and `_ckpt` classification are stated; canonical incarnation grammar and legacy-pool handling remain absent. |
| 12. Probe A sampled role unclear | **Resolved** | Deterministic cadence, durable due/performed/skipped observability, and separation from the mount gate are explicit. |
| 13. Costs/tests could be vacuous | **Resolved for the round-6 requests** | Three-write creation, conflict/tail costs, and the requested fault interleavings are present; findings 1–3 introduce new mandatory controls and costs. |

REJECT

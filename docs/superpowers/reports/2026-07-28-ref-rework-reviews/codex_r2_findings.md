## Findings

1. **[blocker] Void-first is not decidable from the proposed data and is keyed to the wrong epoch**

   **Attacks:** round-1 remedy 1; spec §4 seal adoption and §5 rule 1.

   **Proof:** recovery does not seal the predecessor’s actual epoch. It writes `{my_epoch - 1, UINT64_MAX}` ([CasRefLedger.cpp:601](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:601)), while mount retries can burn epochs before adopting one ([CasPool.cpp:581](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:581), [CasPool.cpp:608](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:608)).

   Scenario: epoch 7 has applied through `A={7,10}` and has unresolved `V={7,11}`. Epoch 8 is burned during mount recovery; epoch 9 wins and publishes `S={8,MAX}`, `sealed_from=A`. `V` lands late. The rule asks for “a seal for epoch 7”; none exists, and `V.prev=A`, so plain continuation folds writer-unapplied data.

   Even without a burned epoch, hot `LIST` can return `V` while omitting `S`. The spec never requires an exact-key seal lookup before continuation, and an absent seal lookup can race seal publication.

   **Smallest fix:** define voidness by a recovery-seal interval:

   `seal.sealed_from < X.id && X.id <= seal.snapshot_id`

   Do not infer death from idleness. A dead-epoch record without an authoritatively discovered covering interval must hold. If frontier progress is required, add a fixed per-namespace current-anchor object; the existing keys cannot derive the applicable seal from `X.id`.

2. **[blocker] The sweep remedy defers the missing protocol primitive rather than designing it**

   **Attacks:** round-1 remedy 2; spec §8’s “possible owner-granting window”.

   **Proof:** the deletion target provides a build epoch/sequence, while the fold cut is a separate `RefTxnId` sequence. Builds use `next_build_seq` ([CasMountRuntime.cpp:165](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:165)); ref transactions use the independent pool-wide `next_ref_sequence` ([CasRefLedger.h:533](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.h:533)). The durable `MountLease` contains `writer_epoch` and `min_active`, but no ref-transaction cut ([CasServerRootFormats.h:43](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasServerRootFormats.h:43)).

   Therefore no current field can prove that every transaction capable of granting a particular manifest an owner lies below cursor `C`. Exact-token deletion does not help: granting an owner does not replace the manifest body or change its token.

   **Smallest fix:** disable orphan-manifest deletion for live namespaces. A complete implementation needs either an owner-grant ref ID bound persisted with the build/floor transition, or a fenced quiescent reconstruction. “Plan-phase task” is not sufficient for a release-blocking design.

3. **[blocker] An empty recovery seal is still not encodable**

   **Attacks:** round-1 remedy 4; spec §4’s “explicit empty-region encoding”.

   **Proof:** an unseen namespace is represented as `Removed` with no `remove_txn_id` ([CasRefProtocol.h:147](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.h:147)). Overriding `snapshot_id` and allowing `sealed_from={0,0}` does not change that state. The snapshot codec rejects every `Removed` snapshot without `remove_txn_id` ([CasRefSnapshotFormat.cpp:75](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasRefSnapshotFormat.cpp:75)).

   Faking the seal as `Live` is also wrong: recovery would later reconstruct a live namespace, and its real first `NamespaceBirth` would be rejected.

   **Smallest fix:** add an explicit `NeverBorn` recovery-seal state, or use a separate seal format that carries the predecessor and table state independently. Merely permitting zero `sealed_from` is insufficient.

4. **[blocker] `SYSTEM ... GC REBUILD` remains a destructive hot-LIST consumer**

   **Attacks:** the spec’s §3 claim that destructive listing consumers are bounded by proven cuts; round-1 remedy 8’s whole-namespace negative control.

   **Proof:** rebuild discovers its entire universe solely through `LIST(cas/refs/)` ([CasGc.cpp:2393](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2393)), then creates coverage only for namespaces returned ([CasGc.cpp:2647](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2647)). It condemns every physical blob missing from the rebuilt edge set ([CasGc.cpp:2739](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2739)).

   A wholesale-omitted live namespace therefore loses all owners in the rebuilt baseline and its blobs are condemned. Permanent tombstones in the previous fold seal do not help because rebuild is precisely the path that replaces lost/untrusted GC state.

   Fsck has the same universe problem: it iterates `store.listNamespaces` ([CasFsck.cpp:314](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:314)), which is itself LIST-derived ([CasPool.cpp:1321](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp:1321)). It cannot mark a namespace `unchecked` when it does not know that namespace exists.

   **Smallest fix:** rebuild must first establish pool-wide writer quiescence/fencing and use the cold-LIST trust boundary. Fsck must make the entire run `unchecked` without the same proof; a per-discovered-namespace flag cannot detect wholesale omission.

5. **[major] Vanish-restart can erase chain evidence and then report successful recovery**

   **Attacks:** round-1 remedy 7; spec §9.

   **Proof:** the current restart loop discards the candidate and starts from a fresh listing after any selected object vanishes ([CasRefLedger.cpp:536](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:536), [CasRefLedger.cpp:557](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:557)). The brake fails after three restarts in one invocation ([CasRefLedger.cpp:446](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:446)).

   If the first listing exposes successor `X` but repair of predecessor `P` returns 404, a lying second listing can omit both `X` and `P`. Recovery then succeeds from an older snapshot rather than reaching the brake. This violates the claim that a cold-contract violation with a trace becomes loud and violates INTENT’s rejection of fallbacks that hide errors.

   **Smallest fix:** carry `required_snapshot_floor=P` across restarts. The retry may succeed only after selecting a snapshot covering at least `P`; omission of `X` is not resolution. Preserve that floor in the runtime across repeated touches, with backoff/quarantine after the brake.

6. **[major] Permanent seal-carried tombstones are not bounded**

   **Attacks:** round-1 remedies 3 and 9; spec §6.

   **Proof:** the proposed rule retains every namespace ever visited, not merely “namespaces ever removed”. `CasFoldSeal::per_ns_shard` is serialized in full into every generation ([CasFoldSealFormat.cpp:75](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.cpp:75)). Fold seals have a 256 MiB control-object limit ([CasFormat.cpp:102](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFormat.cpp:102)). Thus “bounded by count ever removed” is not a bound; eventual encode/decode failure is guaranteed under unbounded namespace churn.

   The `Pending` remedy is also not enforceable: raising an event after exceeding a configured bound, while admission “may” be refused, permits further growth.

   **Smallest fix:** keep active cursors in the fold seal and move retired lineage to one fixed per-namespace lineage object, point-read when an old/rebirth record appears. Enforce the cleanup backlog cap before accepting a new `remove_namespace`, not after exceeding it.

7. **[major] The snapshot pin safely prevents deletion but creates unbounded ordinary snapshots**

   **Attacks:** round-1 remedy 5; spec §7.

   **Proof:** current cleanup retains only the newest snapshot and deletes every listed older snapshot ([CasRefProtocol.cpp:720](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:720)). Under the proposed rule, if cursor `C` is held for months while the writer continues publishing, every ordinary snapshot above `C` is pinned. Each may be up to 64 MiB.

   This is unnecessary: only recovery seals are chain anchors. `UINT64_MAX` is being reserved precisely so their IDs are syntactically distinguishable.

   **Smallest fix:** retain the newest ordinary snapshot as today, and additionally pin only recovery-seal IDs above the durable cursor. This also covers the EMPTY-region seal once its encoding is fixed.

8. **[major] B1 remains undefined for holds after speculative repair or void classification**

   **Attacks:** round-1 remedy 6; spec §5a.

   **Proof:** current B1 recomputes intent only over IDs between the parent and final sealed cursor ([CasGc.cpp:1638](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1638)). The revision instead says repaired records count as accounted and a void record is recorded immediately.

   Scenario: cursor `A`; void record `V` is certified; successor `B` then clamps or repair returns 404, leaving the final cursor at `A`. Counting `V` as `certified_void` makes `accounted = applied + void` fail if accounted is cut-scoped. Counting all examined/repaired candidates instead can make the identity pass over records above the sealed cut.

   **Smallest fix:** make accounting two-phase. B1 covers exactly records in `(parent_cursor, final_cursor]`; speculative repaired bodies and void observations above the final cut are separate metrics. A certified void joins B1 only when a later accepted anchor places it below the final cut. B2 applies only to transactions whose deltas were committed.

9. **[major] A “per-namespace” hold is still a pool-wide reclamation wedge**

   **Attacks:** spec §5 rule 5 and §12’s repeated-GET treatment.

   **Proof:** any anomaly currently sets `suppress_destructive` globally ([CasGc.cpp:1833](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:1833)); ref cleanup immediately returns under that flag ([CasGc.cpp:2075](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2075)). This global suppression is safety-critical because an unknown `+1` may name any shared blob.

   Therefore one permanently held namespace lets other namespaces fold but prevents pool-wide graduation, blob deletion, and ref cleanup indefinitely. It also repeats its anchor/repair GETs on every folding round with no per-namespace backoff. The work is bounded per round, not over time or namespace count.

   **Smallest fix:** state the global consequence honestly and add a persistent quarantine/backoff plus operator recovery path. Do not simply allow other namespaces’ deletes; that would be unsafe without an uncertainty-protection index.

10. **[minor] Repair observability is itself unbounded**

    **Attacks:** round-1 remedy 10 and spec §§5/11.

    **Proof:** the repair walk has a step bound, but the spec requires one durable `gc_anomaly` row per repaired hole. Across many namespaces, that can still generate an operationally unbounded audit burst, unlike probe A’s explicit 32-row cap.

    **Smallest fix:** apply the same per-round cap as probe A, carry the true total and truncation status in every emitted row, and retain aggregate counters for all omitted rows.

## Round-1 remedy disposition

| Round-1 finding | Disposition |
|---|---|
| 1 seal adoption / void precedence | **REFUTE in part.** Adopting the seal high-water is sound; void precedence is not usable without authoritative interval discovery and is wrong under burned epochs. |
| 2 sweep | **REFUTE.** §8 states a requirement but defers the absent protocol field. |
| 3 monotone rebirth / tombstones | Monotone rebirth is sound; **REFUTE** permanent embedding in every fold seal. |
| 4 empty seal | **REFUTE.** Zero `sealed_from` alone cannot encode the never-born state. |
| 5 snapshot pin | Safety idea upheld; **REFUTE** pinning every snapshot above the cursor. Pin seals only. |
| 6 B1/B2 | Shared intake primitive upheld; **REFUTE** the non-cut-scoped accounting definition. |
| 7 recovery restart | Mutator inventory upheld; **REFUTE** restart without a sticky required-coverage floor. |
| 8 fsck unchecked | Principle upheld, implementation incomplete: without a universe/quiescence proof it must be whole-run `unchecked`. |
| 9 pending liveness | **REFUTE.** “May refuse after exceeding” is not a bound and does not bound completed tombstones. |
| 10 bounded repair / cost | Bounded-memory repair upheld; cost claim is incomplete because sound void detection and persistent holds add requests. |
| 11 ID hygiene | **UPHOLD**, provided allocation uses a saturating atomic CAS and never wraps the shared counter. |

## A–J cross-examination

- **A:** Synthetic high-water adoption itself is sound. Trial IDs already special-case `{dead_epoch,MAX}` ([CasRefLedger.cpp:1659](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1659)); snapshot publication skips when the newest snapshot equals the state high-water ([CasRefLedger.cpp:2488](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:2488)). Admission uses it only for framing. Rebuild may treat a validated seal as a certified cut. The current fsck oracle assumes a snapshot ID is also a log ID and skips a recovery seal ([CasFsck.cpp:220](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasFsck.cpp:220)); it must not report that skip as checked.
- **B:** No. See finding 1. Deadness must come from a durable mount/seal fact, never idleness.
- **C:** It does not currently break an emptiness branch, but it destroys the seal-size bound and changes “empty universe” into “no current entries, but permanent history”. See finding 6.
- **D:** Yes, ordinary snapshots accumulate without bound. The pin is sufficient for an empty seal only after the encoding is fixed. See findings 3 and 7.
- **E:** Not implementable from current fields. See finding 2.
- **F:** Repeated identical evidence fails closed within one invocation, but evidence can disappear on restart and repeated callers can re-drive forever. See finding 5.
- **G:** Not well-defined under a mid-repair/mid-anchor hold. See finding 8.
- **H:** No existing consumer semantically relies on nonzero `sealed_from`; the codec is the direct rejection. The real obstacle is the never-born lifecycle.
- **I:** One GET per held namespace per folding round is rate-bounded only by the scheduler; it has no cumulative bound or backoff. Correct void discovery may require more than the single anchor GET claimed.
- **J:** The restart fallback can hide evidence, §8 presents an unspecified mechanism as a completed safety design, and blanket `unchecked` tests risk being green-that-cannot-go-red. These conflict with INTENT. The loud repair/hold counters and fail-closed cursor behavior do align with INTENT.


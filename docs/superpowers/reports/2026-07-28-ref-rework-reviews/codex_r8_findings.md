V8 closes round 7’s two ordinary-round data-loss scenarios, but it has not fully converged: `REBUILD` can discard a known hold and recreate the cross-namespace deletion path.

## Findings

1. **Blocker — `REBUILD` can forget the durable hold and certify the same broken chain as quiet.**

   **Claim attacked:** a hold “can never be forgotten” ([spec §5](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:137)), while `REBUILD` creates a baseline only from catalog, `_ckpt`, and arithmetic tails ([spec §7](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:174)).

   **Proving scenario:**

   1. Namespace `A` is held at cursor `C`: exact `GET(C+1)` is absent, but a higher exact witness `C+2` contains an acknowledged `+1` for blob `b`.
   2. The adopted seal carries classification `4`, so ordinary rounds suppress deletion.
   3. An operator runs forced `REBUILD`. The current command explicitly allows replacing healthy bookkeeping with `FORCE` ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2549)); the rebuilt seal is freshly constructed rather than inherited ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:2651)).
   4. `A`’s `_ckpt` is below `C`. Arithmetic recovery gets 404 at `C+1`; the unreliable hint also omits `C+2`. The rebuilt baseline therefore contains neither `A`’s edge nor its hold.
   5. A later destructive round again exact-gets `C+1`, sees absent, and treats `A` as proven quiet.
   6. A visible `B:-1` on `b` reaches zero and eventually deletes `b` while `A:C+2` still names it.

   “Condemns nothing” during `REBUILD` only postpones the loss; it does not prevent later regular rounds from condemning from the incomplete baseline.

   **Smallest fix:**

   - A forced rebuild with a readable prior seal must carry every hold verbatim into the new baseline.
   - If the prior hold authority is missing or undecodable, publish a pool-wide destructive hold—or refuse the rebuild—not an ordinary deletion-capable baseline.
   - Define “successful frontier walk” as folding through the offending position and adopting that result in `gc/state`, not merely reaching another absent expected-next.
   - A disappeared above-cursor witness is corruption, not evidence for clearing. Current cleanup occurs after the state CAS ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:866)); a deposed leader cannot legitimately clean an unfolded above-cursor record.
   - Add the exact `held A → FORCE REBUILD → hint hides witness → B:-1` go-red test.

2. **Major — the destructive-frontier mechanism is sound, but its temporal proof is not stated.**

   **Claim attacked:** one frontier proof per catalog namespace is sufficient for the entire destructive round ([spec §5](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:126)).

   The result by variant is:

   | Variant | Exposed window | Result |
   |---|---|---|
   | `(a)` `+1` lands after that namespace’s absent probe | Probe until the round finishes | Safe, but not from two-phase pacing alone. A newly condemned object cannot be deleted that round. If it was already delete-pending, its `Condemned` meta forces the writer to rematerialize from source ([CasPartWriteTxn.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:349)), and the old exact-token delete cannot remove the new incarnation ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:516)). |
   | `(b)` namespace already in HOLD | None, provided suppression is computed before every destructive site | Safe. The OR must suppress pre-CAS pending deletes, condemnation/graduation, manifest deletion, ref cleanup, and sweep deletion. The current one-pass order folds before pending deletes ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:481)). |
   | `(c)` `Creating→Live` after catalog GET | Catalog sample until first append | Safe only if `Creating` categorically forbids publication. Once `Live`, the same late-arrival/meta/token argument as `(a)` applies. Current text does not state this late-admission lemma. |
   | `(d)` `Removing` mid-teardown | Probe until terminal append/catalog removal | Safe if `Removing` forbids new positive ownership and the entry remains cataloged until terminal folding and cleanup. A late terminal record contains removals, so omission delays reclamation rather than hiding protection. |

   **Smallest fix:** add this temporal lemma normatively, state that writer condemnation/meta and exact-token replacement complete the deletion-phase proof, and test delayed `+1` after the probe during condemnation, graduation, and exact-deletion rounds, plus catalog admission and `Removing` interleavings.

3. **Major — the remote-transition argument is true for writes, not for reads.**

   **Claim attacked:** remotely initiated transitions are safe because the old fence is terminal, allowing hot reads to check only a local generation ([spec INV-3](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:70)).

   The current signals are:

   - Local drop and rebirth have synchronous control points where a new life generation can be published.
   - Self-remount and fence loss already bump `fence_generation` ([CasMountRuntime.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp:87)).
   - A remote decommission claim has no synchronous signal in the victim process; it becomes locally visible only through lease/fence expiry or `tripMountLost`.

   `CasPlainObjects` deliberately does not fence reads ([CasPlainObjects.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPlainObjects.h:88)), and `namespaceFilesReadable` is a separate pre-check with a TOCTOU before the later read ([ContentAddressedMetadataStorage.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:1234)).

   The exact read-side guarantee available without a catalog/fence refresh is therefore: a stale handle remains pinned to old incarnation-qualified objects and cannot alias the new life, but it may return stale old-life bytes or `NotFound`. It is not guaranteed to be rejected.

   **Smallest fix:** state that weaker read contract explicitly, or add a locally checkable lease/fence gate or bounded catalog refresh to reads. Separately require destructive cleanup to revalidate life and fence immediately before every delete and drain/cancel admitted old-life enumeration before rebirth.

4. **Major — the named decommission dependency remains contradictory and lacks the `_ckpt`-absent branch.**

   **Claim attacked:** decommission exactly enumerates catalog entries and resumes every `Removing` namespace ([spec §3](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md:97)).

   The dependency still mandates scoped LIST enumeration ([decommission design](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/specs/2026-07-13-cas-pool-member-decommission-design.md:74)), and current code does exactly that, including a one-key LIST existence check ([CasDecommission.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp:116)).

   The requirements are implementable after claim, but “`Removing` without a terminal record is resumable writer work” is incomplete:

   - `_ckpt` present: recover and append the missing terminal under the claimed fence.
   - `_ckpt` absent: normal recovery is impossible. By the stated deletion order, this represents the finalization window after terminal folding/cleanup and `_ckpt` deletion but before catalog removal. It should exact-CAS-remove the catalog entry, or fail as corruption if that ordering cannot be established.

   **Smallest fix:** update the dependency now, define those two branches, and require a final exact catalog GET/token check before slot retirement.

5. **Major — hold encoding is extensible, but neither its strict grammar nor the seal-side cap is closed.**

   Classification `4` currently only means “clamped,” with no hold fields ([CasFoldSealFormat.h](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.h:32)). The decoder rejects every unknown `cov` key ([CasFoldSealFormat.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasFoldSealFormat.cpp:179)). This is implementable under the format bump, but the new grammar must be explicit: required bounded hold fields for classification `4`, forbidden otherwise, with unknown/duplicate/invalid combinations rejected.

   The aggregate reservation correctly pre-pays the worst-case hold for every catalog entry, so fold-time creation need not overflow logically. However, current raw-object sealing does not enforce the object cap on write; the cap is checked when opening the object ([CasTextFormat.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasTextFormat.cpp:362)). A boundary-sized catalog could therefore produce an unreadable fold seal if the estimator is wrong.

   **Smallest fix:** define the exact wire fields and maxima, make the reservation estimator include JSON escaping/framing, and check `encodeFoldSeal(...).size() <= fold_seal_cap` before every PUT.

6. **Minor — nomination safety is resolved, but guaranteed post-adoption deletion retry is overstated.**

   The neutral nomination input can ride the existing one-pass commit: discover and decode `M` before `fold`, add neutral touches to the candidate seal/runs, then adopt through the single `gc/state` CAS ([CasGc.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp:772)). The current reducer confirms why a synthetic `BlobDelta` is invalid: it unconditionally marks B2 ordinals and counts unmatched removals ([CasBlobInDegree.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasBlobInDegree.cpp:591)).

   Alternating leaders are safe:

   - A loser of `gc/state` must not delete `M`.
   - Re-nomination after a winner is a neutral current-count touch.
   - Manifest keys are not content-addressed; they are immutable, monotonically minted identities and cannot legally be recreated ([CasPartWriteTxn.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPartWriteTxn.cpp:850)).
   - A different token at the same key is illegal ABA; exact-token deletion must retain it and surface corruption.

   But after process death following adoption, no durable manifest-delete debt is specified, so an incomplete LIST may omit `M` forever. That is a manifest leak, not blob data loss.

   **Smallest fix:** either carry exact `{key, token}` deletion debt in the seal, or weaken “retries the delete” to “is safe to retry when rediscovered.”

7. **Minor — verification does not yet exercise the newly exposed temporal and rebuild cases.**

   The current `CaRefDeltaIntakeCore` has two tables, which is sufficient cardinality for cross-namespace sabotage, but its state contains no shared blob, in-degree, condemnation phase, deletion, catalog sample, or hold ([model](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/models/CaRefDeltaIntakeCore.tla:51)). Merely retaining its two-table shape cannot express the property.

   **Smallest fix:** the rewrite must model one shared blob and pool-wide in-degree/retirement state. Add go-red controls for:

   - late `+1` after a namespace’s probe in all retirement phases;
   - `Creating→Live` and `Live→Removing` after catalog sampling;
   - hold followed by forced `REBUILD`;
   - strict classification-4 codec and maximum-size seal;
   - local versus remote stale-read semantics;
   - duplicate nomination and token-ABA handling;
   - `_ckpt`-absent decommission finalization.

   The cost model should also name carried-hold exact retries and full rebuild/quarantine cost.

8. **Note — the wedge is demand-driven, not autonomously live.**

   A permanent wedge with no subsequent callers performs no retry; remount is only an escape if a remount independently occurs. That is acceptable because the unresolved operation was not acknowledged, and the next caller retries it. The wording should say “a later caller or an independently occurring remount,” rather than implying remount is guaranteed.

9. **Note — recreate-only migration is otherwise credible.**

   I found no test or CI contract that requires opening a persisted pool across this generation bump. Existing format handling already has a fail-closed recreation message ([CasPoolMetaFormat.cpp](/home/mfilimonov/workspace/ClickHouse/master/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPoolMetaFormat.cpp:109)). Implementation must bump both the writer generation and backward floor, update version goldens, and explicitly require a quiesced recreation so an already-running old writer cannot touch the reused prefix.

## Round-7 disposition

| Round-7 finding | Status in v8 | Proof |
|---|---|---|
| 1. Quiet namespaces trust LIST | **Resolved** | Every destructive round exact-probes every `Live`/`Removing` namespace; incomplete coverage suppresses all destruction. |
| 2. No durable hold carrier | **Partial** | Classification `4` carries ordinary rounds, but `REBUILD` does not preserve or conservatively replace it. |
| 3. Nomination not failure-atomic/B2-compatible | **Resolved** | Exact GET/decode, neutral input, state-CAS adoption, then exact delete are specified. |
| 4. No closed namespace handle | **Partial** | Typed handle and physical identity are present, but remote read rejection is false and destructive-task draining/rechecks remain unspecified. |
| 5. Capacity predicate not additive | **Resolved** | The catalog CAS now sums all per-entry reservations, including holds. Write-side seal-cap enforcement remains an implementation fix. |
| 6. Decommission can strand `Removing` | **Partial** | V8 names the correct dependency and retirement rule, but the dependency still uses LIST and lacks `_ckpt`-absent resumption. |
| 7. Migration unspecified | **Resolved** | Recreate-only and fail-closed old-format startup are explicit. |
| 8. Wedge envelope missing | **Resolved** | One bounded foreground attempt, captured generation, no deadline-resetting worker, and successor-seal resolution are explicit. |
| 9. `_ckpt` no-op/retry bounds | **Resolved** | Exact-body no-op and existing recovery deadline/restart accounting are explicit. |
| 10. Costs/tests/grammar/TLA incomplete | **Partial** | Mandatory 404 cost, grammar, and original controls are present; rebuild/temporal controls and shared-blob TLA state are still missing. |

## INTENT sweep

V8 aligns well with `INTENT` on recreate-over-compatibility, exact-token mutation, visible holds, bounded ambiguity, refusal of fallback paths, and acknowledging the mandatory quiet-namespace cost.

The blocker violates the overriding rule directly: a known anomaly can be erased by an administrative state transition and later permit deletion of an object still named by an acknowledged reference ([INTENT](/home/mfilimonov/workspace/ClickHouse/master/docs/superpowers/cas/INTENT.md:63)). The remote-read claim and nomination retry wording also violate the “no silence” principle by promising stronger behavior than the represented state can provide. The missing go-red rebuild control would allow that discrepancy to survive implementation.

REJECT

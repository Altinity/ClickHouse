# Adversarial review: converged v9 CAS ref-chain implementation plans

1. **[BLOCKER]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Staging contract and Task 13 Step 1 (`new-namespace residual`).
   **Defect:** The residual names a missing-universe condition but falsely asserts that it cannot destroy data: because blob in-degree is pool-wide, an acknowledged hidden `A:+1` in a namespace absent from both the hint and `gc/state` can coexist with a visible `B:-1`, letting Stage A prove only the known namespaces' frontiers, drive the shared blob to zero, and delete it while `A` still owns it, so Stage B Task 4's catalog universe is load-bearing for the claimed acked-loss closure.
   **Concrete fix:** Do not certify or deploy Stage A as a standalone deletion-capable stage; either move the authoritative catalog universe into Stage A, merge the stages, or keep all destructive work globally suppressed until Stage B is installed, and replace Task 13's “no destruction can occur” case with the hidden-`A:+1`/visible-`B:-1` regression whose required verdict is zero deletes.

2. **[BLOCKER]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Tasks 1, 4, and 6, ordinary sequence-1 append path.
   **Defect:** Task 1 makes `prev_epoch_seal` mandatory on sequence 1 of every non-genesis epoch, but the only plan steps that populate it are recovery-generated seals in Task 6, while Task 4 never puts the recovered predecessor seal into an ordinary `{E,1}` transaction, so the first normal append after a transition must fail the encoder's own grammar.
   **Concrete fix:** Have recovery install the exact last-seal identifier into the writer runtime and require `commitRefChunk` to copy it into every ordinary non-genesis sequence-1 transaction; add a failing test that seals `E-1`, installs live epoch `E`, performs an ordinary first append, and round-trips the exact predecessor link.

3. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 2 Steps 1 and 3 (`slotOccupy`).
   **Defect:** The proposed composition cannot implement its declared result: current `putIfAbsentControlled` retries internally and returns only `CasWriteOutcome`, while current `resolveByExactGet` compares against expected bytes and throws `CORRUPTED_DATA` on a different occupant instead of returning its bytes and token, contradicting both “one conditional create / never retries internally” and `Occupied(bytes, token)`.
   **Concrete fix:** Specify and implement a dedicated raw slot operation that performs exactly one fence/deadline-gated backend `putIfAbsent` followed, only when resolution is required, by exactly one raw exact `GET` returning the occupant bytes/token without compare-and-throw; add backend operation-count assertions for every outcome.

4. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 5 Steps 1–3 and Task 6 Step 7.
   **Defect:** The plan builds and tests a shared `publishCkpt` helper and calls it from the sealer, but no task modifies `CasRefLedger::trySnapshotPublishOnce` to publish `checkpoint_snapshot_id` after the snapshot body is durable, so one of INV-4's two writers and the checkpoint witness/cleanup authority are absent.
   **Concrete fix:** Add `CasRefLedger.cpp` and its constructor plumbing to Task 5, invoke the same `publishCkpt` helper after a committed snapshot PUT and before treating the snapshot as cleanup-authoritative, and test the body-PUT/cleanup/`_ckpt` race, identical-body no-op, token conflict, and stale-generation refusal at that real call site.

5. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 9 Step 1 and Task 14 integration battery.
   **Defect:** The plans cover only the temporal lemma's third arm (delete-site in-degree re-read) and contain no C++ step for the two writer-side arms that the TLA phase explicitly delegates to the implementation plan: SOURCE-BACKED/TOKENED adoption must read `Condemned` and rematerialize, and TOKENLESS `adoptEvidence` must make the receiver's `+1` durable before the source releases its edge; the newly-condemned-not-deleted-in-the-same-round arm is also not pinned directly.
   **Concrete fix:** Add explicit C++ regressions for (a) a post-probe `+1` followed by same-round condemnation, (b) source-backed/tokened adoption of an already-pending blob with fresh-incarnation rematerialization and a delayed old-token delete, and (c) tokenless relink with an operation journal proving receiver `+1` durability precedes source release and that no deletion gap occurs.

6. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 9 Files, Behavior, and Step 1.
   **Defect:** The claimed inventory of “three ungated sites” is incomplete because current `CasGc.cpp` also runs the post-CAS `handoff_reclaim` wholesale generation-prefix delete at lines 793–833 before `suppress_destructive` is even assigned at line 879, in addition to the listed prune, manifest-delete, and orphan-sweep paths.
   **Concrete fix:** Perform a tree-wide destructive-call inventory, compute/transport the gate before every pre- and post-CAS destructive phase, gate `handoff_reclaim` explicitly, and give every individual delete family a zero-delete-under-hold/frontier-incomplete test rather than asserting coverage of only three sites.

7. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 3 “Three-site same-value obligation,” Steps 1–3 (and Stage A Task 6 Steps 1, 6–8).
   **Defect:** Task 3 misreads the model ledger by defining one `{admission generation, expected token}` pair for slot-occupy, `_ckpt`, and catalog CAS even though the proven three same-value sites are slot-occupy, `_ckpt` CAS, and recovery install sharing one captured generation, while the slot, `_ckpt`, and catalog objects necessarily have different tokens and catalog-token CAS is a separate ZombieGoLive credential.
   **Concrete fix:** Define a generation-only recovery context captured once and passed through Stage A's slot-occupy, `_ckpt`, and install checks, test fence bumps between those three actual sites, and keep the catalog entry's observed token as a distinct Task 3 credential combined with the creator generation only at `Creating → Live`.

8. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 4 `resolveWedgeOnce`, Steps 1 and 3.
   **Defect:** Task 4 checks the original generation only as a pre-attempt predicate and compares an occupied result by body bytes, but it omits the r9-5 post-I/O generation/operation recheck under `state_mutex` immediately before adoption, acknowledgement, or unwedge and has no test in which the old result returns after a successor has sealed the slot.
   **Concrete fix:** Preserve and compare the exact stored `(admission generation, transaction/operation identity, bytes)` under the install lock after I/O, permit a valid successor seal to remain the conclusive rejection while forbidding the superseded runtime from acknowledging/installing/publishing, and add a deterministic blocked-I/O test that bumps/rearms, seals, then releases the old result.

9. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 8 Steps 1 and 3; `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 2 Capacity admission and Step 1.
   **Defect:** Stage A conflates `FoldSeal`'s 64 KiB per-line cap with its 256 MiB object cap and names only the normal PUT although REBUILD has a second `encodeFoldSeal(seal)` PUT, while Stage B applies a combined catalog-plus-reservation quantity to both unrelated caps and says the exact boundary is refused despite the required boundary/boundary-plus-one convention.
   **Concrete fix:** Use one shared byte-arithmetic helper with separate predicates `encoded_catalog <= catalog_object_cap` and `fold_fixed_bytes + Σ worst_case_entry_reservation <= fold_object_cap`, separately test per-row 64 KiB framing, accept exact object-cap equality and refuse cap+1, and place the whole-seal pre-PUT check at both the regular fold and REBUILD write sites.

10. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 6 new recovery sequence and tests.
    **Defect:** A generation check on final install is not the spec's separate self-remount rule: the plan leaves `recovery_in_progress` local to the old table runtime and contains no remount step that cancels or waits for outstanding recovery before `CasMountRuntime` rearms.
    **Concrete fix:** Add the remount orchestration files and an explicit cancel-or-join barrier before fence rearm, then test an old recovery paused in I/O across self-remount and assert rearm cannot complete until it is cancelled/drained and no old `_ckpt` or install mutation follows.

11. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Global Constraint 4 and Task 3 Step 3.
    **Defect:** Bumping the writer generation/backward floor and rejecting old-format startup does not implement the normative migration requirement that recreation be quiesced, because an already-running old binary can continue writing the reused prefix without ever reopening it.
    **Concrete fix:** Add an operational recreation precondition and enforcement step—preferably a fresh prefix, otherwise exact owner/lease terminalization and writer drain—with an integration test showing an old writer is fenced before prefix reuse; Stage A cannot receive PASS without this proof.

12. **[MAJOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 8 TLA files and Step 1.
    **Defect:** The plan makes `CaRefFoldClampRecoveryCore` conditional on whether the implementer thinks clamp interaction changed and gives RED-first model coverage only to R2, weakening §9's requirement that the register models be extended when R2/R3 land and leaving R3's neutral nomination/adopt-before-delete ordering without a model gate.
    **Concrete fix:** Require a named R3 sabotage/control pair in the appropriate fold/clamp model before C++—at minimum delete-before-adoption and nomination-contaminates-B2/unmatched-remove variants—with a name-asserting runner and a separate green model commit.

13. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 5 removal sequence and Step 1.
    **Defect:** Task 5 goes directly from terminal-folded to exact `_ckpt` deletion and catalog-entry deletion, omitting INV-3's required bounded best-effort cleanup pass before the entry is removed.
    **Concrete fix:** Insert one explicitly bounded, suppression-aware best-effort cleanup attempt after the terminal fold and before `_ckpt`/entry deletion, defer failures as leak-only janitor work, and assert that ordering in the backend journal.

14. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 6 read-side contract and Step 1.
    **Defect:** “Immediately before every delete” is weakened to one catalog GET per multi-key batch even though the current backend exposes only per-key `deleteExact` and no atomic batch-delete primitive, allowing the catalog/fence credential to change between separate deletes.
    **Concrete fix:** Either define a batch as one future atomic backend deletion operation or revalidate the exact catalog entry token/fence immediately before each current per-key delete, with a race injected between two keys rather than only between planning and the first delete.

15. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 14 Step 3 soak PASS criteria.
    **Defect:** “Zero wedged-forever lanes” contradicts Task 4 and accepted register R6, under which a permanently quiet unacknowledged wedge may remain until a later caller or independent remount, and the unqualified criterion incentivizes the forbidden autonomous retry loop.
    **Concrete fix:** Define failure as a wedge that remains after a foreground flush or remount resolution opportunity, explicitly allow/report quiet unacknowledged wedges, and assert that no background deadline-resetting retry exists.

16. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 9 Step 1, late-`+1` delete-site regression.
    **Defect:** That individual C++ test pins behavior already present in `CasBlobInDegree::settleEntry`, so Task 9's aggregate filter can be red only because unrelated frontier symbols/behaviors are missing and does not prove the newly normative delete-site guard can itself fail.
    **Concrete fix:** Add a test-only sabotage/fault seam or scratch mutation that bypasses the final in-degree read, run only the targeted test to recorded RED, then restore the guard and record GREEN.

17. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 10 Steps 1–4.
    **Defect:** One review unit combines the `listedTok` semantic audit, expectations for 123 configurations across nine drivers, four runnerless-model decisions, and five phase-runner classifier fixes, so an evidence-sensitive model retirement can be hidden inside a mechanically enormous diff.
    **Concrete fix:** Split it into independently reviewed tasks/commits for the `listedTok` verdict, driver expectations by model family, runnerless models, and classifier-only phase reruns, each with its own before/after results artifact.

18. **[MINOR]** `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-b-catalog.md` — Task 1 Step 1 and Task 7 Files; `docs/superpowers/plans/2026-07-28-cas-ref-chain-stage-a-streams.md` — Task 5 Files.
    **Defect:** The site map is not executable as written: the compile-time assertion names nonexistent `CasLayout` and unqualified `refLogKey` although the current type is `Layout` with a member method, Stage A points at nonexistent `ContentAddressed/CasLayout.h` instead of `Formats/CasLayout.h`, and Stage B points at nonexistent `ContentAddressed/CasDecommission.cpp` instead of `Tools/CasDecommission.cpp`.
    **Concrete fix:** Correct every path, express the deleted-overload check as a dependent `requires`/concept over `Layout::refLogKey`, and add explicit `CasRefLedger`/`CasPool` constructor callback plumbing for generation capture/check rather than assuming the ledger owns a `CasMountRuntime`.

PLANS VERDICT: REJECT

The plans carry several ledger obligations correctly—the one semantic-max helper and per-field tests, token-exact stale-creator reconciliation, generation-plus-catalog-token creator install, deposited-incarnation cleanup, the honest retirement verdict framing, R5, R7, and most hold vocabulary—but they are not implementation-ready: Stage A's own residual test asserts a false safety argument and makes Stage B's catalog unexpectedly load-bearing, while the missing ordinary `prev_epoch_seal` path makes the strict format unusable after the first transition. The incompatible slot API, absent snapshot `_ckpt` writer, incomplete temporal-lemma tests/destructive-site inventory, mistaken three-site credential, and missing post-I/O/remount/migration controls are material enough that patching during implementation would change task boundaries and safety claims; revise and re-review both plans before code starts.

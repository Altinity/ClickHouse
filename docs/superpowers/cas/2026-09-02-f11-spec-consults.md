---
description: 'Consult and review reports on the CAS relink-confirm liveness design (F11), 2026-09-02: three codex rounds and one independent Claude consult, kept verbatim so the spec can cite them.'
sidebar_label: 'F11 spec consults'
sidebar_position: 92
slug: /superpowers/cas/f11-spec-consults-2026-09-02
title: 'F11 relink-confirm liveness: spec consults (2026-09-02)'
doc_type: 'reference'
---

# F11 relink-confirm liveness: spec consults (2026-09-02) {#f11-spec-consults}

Verbatim reports produced while the design in
[relink confirm liveness](/superpowers/specs/cas-relink-confirm-liveness-design) moved from revision 1
to revision 5. Kept because the spec cites them and because two of them contain the evidence walks
(attempt lifetime, committed-ref mutation inventory) that the spec summarises. Model names and effort:
codex `gpt-5.6-sol` at high reasoning, read-only sandbox; the Claude consult is an independent `ca-arch`
agent with no access to the codex transcripts. Line numbers refer to the tree at the time of each round.


## Codex round 1, revision 1: REQUEST CHANGES {#codex-round-1}

1. **CODE — blocker:** The proposed broken-lane-only rule permits the existing stale-cache counterexample for a live committed-ref repoint.

   `publishStaging` explicitly supports standalone writes and removals on an already committed ref and calls `repointRef` without consulting the `MergeTree` part state (`ContentAddressedTransaction.cpp:352-405`). `PartWriteTxn::promote` then retires the old committed binding and promotes the replacement in the same ref-log record (`CasPartWriteTxn.cpp:863-913`). The part can remain `Active` or `Outdated`; for example, `CHECK TABLE` selects those states and may call `writeChecksums`, which commits a storage transaction (`StorageMergeTree.cpp:3344-3377`, `IMergeTreeDataPart.cpp:1725-1745`).

   The unsafe execution is:

   1. The sender offers manifest `m1` while the part is `Active`.
   2. A live repoint from `m1` to `m2` becomes durable before receiver T1, retiring an edge of `m1`, but the sender’s row still reads `m1` while the lane is `Writing` or publishing its frontier.
   3. GC folds that removal and advances deletion far enough that a later receiver edge cannot prevent the already-decided deletion.
   4. Receiver T1 publishes its edge.
   5. Gate 0 still sees `Active`; the proposed rule admits `Writing`; rule 5 reads stale `m1` and answers `Yes`; T3 promotes a dangling manifest.

   This is already the existing `_sab_stalecache` trace: the model permits `SenderAdmit(Other)`, makes the repoint durable while retaining the stale row (`CaRelinkConfirmCore.tla:166-188,367-370`), and records the resulting deletion and false `Yes` (`CaRelinkConfirmCore_RESULTS.md:126-143`). The claim that T1 “keeps them alive regardless” at spec lines 69-71 applies only when the removal is admitted after T1; it does not repair a removal durable before T1.

   The different gate-0 and ledger locks do not produce a separate counterexample for ordinary whole-part removal. Gate 0 releases the parts lock before entering the ledger (`DataPartsExchange.cpp:246-268`), but if it reads `Active` before the transition to `Deleting`, the later removal append is after T1 and therefore protected. If the removal predates T1, rollback can expose `Outdated` only after the append has returned applied, conclusively failed, or fenced its lane. The live-repoint path breaks that reasoning because it performs no part-state transition.

   The complete committed-ref mutation inventory is:

   | Path | Part state when the removal/repoint append is issued | Result |
   |---|---|---|
   | Standalone committed-file write/removal through `publishStaging` → `repointRef` | May remain `Active` or `Outdated` | **Unsafe under the proposed rule** if the replacement retires a blob edge |
   | Normal old-part cleanup through `IMergeTreeDataPart::remove` | `Deleting`; `DeleteOnDestroy` for the old side of MOVE | Gate 0 refuses |
   | Destructor cleanup of a temporary part | `Temporary`, generally not registered in `data_parts` | Gate 0 cannot find it |
   | `delete_tmp_*` rename and subsequent deletion | `Deleting` or `DeleteOnDestroy` | Destination ref is published before source drop |
   | Force-detach | Changed to `Deleting` before `renameToDetached` (`MergeTreeData.cpp:6031-6060`) | Safe |
   | Ordinary detach/attach | Detached destination is cloned/published before the live source is outdated; attach operates before the new part is registered (`MergeTreeData.cpp:5903-5916,8895-8926`) | Safe |
   | Merge/mutation temporary result rename | `Temporary`/`PreActive`, or only staged; committed-source variants publish destination first | Safe |
   | MOVE PART TO DISK/VOLUME | Old part becomes `DeleteOnDestroy`, replacement becomes `Active` under the parts lock (`MergeTreeData.cpp:6429-6457`) | Safe; gate 0 also checks the routed disk |
   | RENAME TABLE | Source parts remain `Active`/`Outdated` (`MergeTreeData.cpp:4105-4135`) | Safe only because each destination ref is published before its source is dropped (`ContentAddressedTransaction.cpp:1260-1298`) |
   | DROP/TRUNCATE | Parts become `Outdated` and then `Deleting`, or directly `Deleting` for table teardown (`MergeTreeData.cpp:3541-3609,4151-4204`) | Safe |
   | Projection removal | Non-temporary projections cannot be removed independently; they are nested in and removed with the parent manifest (`IMergeTreeDataPart.cpp:2448-2461`) | Parent state governs |
   | FREEZE/backup shadow cleanup | Drops shadow refs, not the live part ref (`ContentAddressedTransaction.cpp:1069-1107`) | No live part object involved |
   | Restore/ATTACH | Uses detached or staged refs before activation | Safe |
   | Partial multi-ref commit rollback via `dropRefIfMatches` | Newly created, not-yet-activated ref; existing repoints are deliberately not dropped (`ContentAddressedTransaction.cpp:482-535`) | Safe |
   | Detached/moving container cleanup | No `Active`/`Outdated` part object | Safe |
   | Namespace decommission | Administrative mount refuses a live or contended server lease (`CasPool.cpp:668-697`) | No live answering sender |
   | Zero-copy removal | Uses the same `Outdated` → `Deleting` removal path (`MergeTreeData.cpp:3498-3517`) | No additional CAS-ref path |

2. **CODE — blocker:** The proposed TLA plan would prove safety by constraining away real transitions.

   Spec lines 123-132 allow removal journal admission only while the modelled part is `Deleting`. That excludes both the blocker above and several legitimate code paths:

   - Direct `Active → Deleting`, used by `dropAllData` and force-detach (`MergeTreeData.cpp:4183,6057`).
   - `Active → DeleteOnDestroy` and replacement `DeleteOnDestroy → Active` during MOVE (`MergeTreeData.cpp:6451-6457`).
   - Direct temporary-part destruction (`IMergeTreeDataPart.cpp:993-1028`).
   - Live `OwnerTransition` repoints which retire an old committed binding without changing part state (`CasPartWriteTxn.cpp:890-913`).
   - `republishRef` as two distinct durable steps, destination publication followed by source removal (`PartFolderAccess.cpp:506-533`).
   - RENAME TABLE source removals while parts are still `Active`/`Outdated` (`MergeTreeData.cpp:4105-4135`).

   A green result under the proposed `Deleting`-only constraint would therefore not re-derive `ConfirmedRelinkNeverDangles` for the implementation. The model must represent protector ordering, not just a four-state part lifecycle.

3. **PROSE — medium:** The stated removal invariant and cited call sites are factually inaccurate.

   - “The only entry to physical removal is `asMutableDeletingPart`” at spec lines 58-61 is false. `removeIfNeeded` directly invokes `remove` for temporary and `DeleteOnDestroy` parts (`IMergeTreeDataPart.cpp:969-1030`).
   - Ordinary fast part removal drops the ref through `removeDirectory` at `ContentAddressedTransaction.cpp:1031-1037`; the cited line 1075 is the FREEZE shadow-part branch of `removeRecursive`.
   - “For every path … the part has left `{Active, Outdated}`” at lines 77-79 is false for live repoints and RENAME TABLE.
   - The claim that every `republishRef` source is staged, `Deleting`, or detached is false for RENAME TABLE, although that path remains protected by publish-before-drop.
   - Temporary parts are not universally moved to `Deleting`; destructor cleanup may remove them directly.

   These distinctions matter because only some paths are made safe by gate 0; other paths depend on an existing destination protector or on there being no registered part.

4. **PROSE — medium:** The `rollbackDeletingParts` taxonomy does not match the lane state machine.

   An ambiguous append that was sent initially transitions `Writing → Wedged`, not directly to `NeedsRecovery` (`CasRefLedger.cpp:3935-4006`). `NeedsRecovery` is used when a transaction is known durable but its frontier or in-memory installation cannot be completed (`CasRefLedger.cpp:2169-2182,3750-3788`). Wedge resolution can later apply the transaction and return `Ready`, remain `Wedged`, become `NeedsRecovery`, or terminate `Closed`/`Faulted` (`CasRefLedger.cpp:2357-2389,2432-2496,2534-2543`).

   The omitted fourth case is a sent operation conclusively proven not durable: `DefiniteFailure` clears the attempt and restores `Ready` (`CasRefLedger.cpp:3916-3933`). There is also a distinct pre-send `Unresolved` result which proves no request was sent and restores `Ready` (`CasRefLedger.cpp:3962-3991`).

   I did not find an additional unsafe rollback outcome once those states are represented: an applied removal makes rule 5 return `No`; an ambiguous one remains fenced; and a definite/pre-send failure leaves the old row valid. The prepared-relink abandon path also retains its terminal obligation and retries after a thrown removal append (`ContentAddressedMetadataStorage.cpp:2260-2277`, `CasPartWriteTxn.cpp:1079-1083`).

5. **PROSE — medium:** The verification plan misses the exact unsafe race.

   The proposed unit test only confirms that an unrelated ref can return `Yes` while the lane is busy (`spec:151-155`). It must also exercise the same ref while an old committed binding has become durable but the stale row is still visible. A deterministic test should:

   - Offer old manifest `m1`.
   - Durably repoint the same live ref to a manifest that omits one old blob.
   - Pause after the log PUT or during frontier publication, before row installation.
   - Advance GC past the old edge.
   - Publish receiver T1 late.
   - Verify that confirm cannot authorize `m1`.

   Tests should also cover `republishRef` destination-before-source, RENAME TABLE with `Active` parts, MOVE’s `DeleteOnDestroy` transition, and the distinct rollback outcomes above. The delayed-`_ckpt` integration test is useful for liveness but does not deliberately create a same-ref edge-retiring repoint.

   The rollout compatibility claim itself is supported: this changes only sender answer selection and neither the wire token nor persisted formats (`ContentAddressedExchange.h:142-145`). It is unsafe to roll out only because the new `Yes` set is currently too broad.

6. **PROSE — low:** The documentation and observability updates are incomplete.

   The implementation plan mentions changing comments only at gate 0 and `asMutableDeletingPart`, but the new semantics would also contradict the `RefLaneState` and `ConfirmAnswer` documentation saying only `Ready` certifies a row (`CasRefLedger.h:40-60,147-159`) and the exchange interface saying a busy table returns `Unknown` (`ContentAddressedExchange.h:170-176`).

   The proposed “per refusing rule” counters also omit `state_mutex` contention, which independently returns `Unknown` at `CasRefLedger.cpp:453-462`. It needs its own reason or an explicit mapping to one of the named counters.

REQUEST CHANGES

Exact changes required:

- Retain refusal for a potentially ref-affecting `Writing` attempt, or design a narrower predicate that proves the active attempt cannot retire the queried manifest’s blob edges. A table-wide broken-state predicate is insufficient.
- Either prohibit/enforce blob-set-monotone repoints on `Active`/`Outdated` parts, or treat an edge-shrinking live repoint as a removal requiring an earlier state transition or another already-durable protector.
- Revise the TLA model to include live `OwnerTransition` repoints, `DeleteOnDestroy`, temporary/unregistered parts, direct `Active → Deleting`, and the two-step `republishRef` protocol. The existing stale-cache sabotage must remain a counterexample until the implementation has a real guard.
- Add the deterministic same-ref, durable-before-T1 stale-row regression described above, plus coverage for rename/MOVE/rollback state variants.
- Correct the removal-path prose, citations, rollback state taxonomy, header/interface documentation, and refusal-counter classification.


## Claude consult, revision 2: APPROVE WITH NITS; revision 3 check: APPROVE {#claude-consult}

### Consult: CAS relink-confirm liveness design, revision 2 (F11) {#consult-cas-relink-confirm-liveness-design-revision-2-f11}

Consultant: Claude (Fable 5.1), independent of the other model's consult. Read-only.
Spec: `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md` (revision 2).
Code read at branch `cas-gc-rebuild`, HEAD `9041853b181`. Every line number below is from that tree.

#### Verdict {#verdict}

**APPROVE WITH NITS.** I tried to refute the design on all six attack lines and could not. The
load-bearing claim, that every durable mutation which can make the sender's committed row lag is an
armed `append_attempt` whose ops name the affected refs, is true of the code in front of me. The nits
are prose defects in the spec that would misdirect the implementer or leave a false invariant in a
code comment; none changes the design. Four of them (F1, F2, F4, F6) must land in revision 3 before
the implementation task is cut.

#### Failure asymmetry, stated first {#asymmetry}

Over-refusal (`Unknown`) costs the receiver one retry. Under-refusal (`Yes` on a stale row) lets a
receiver promote a manifest whose blob GC may already have deleted. Every ambiguity in the design must
therefore resolve toward refusing, and revision 2 does: unknown op kinds are namespace-wide,
`Wedged` keeps refusing for its refs, `NeedsRecovery`/`Closed`/`Faulted` refuse everything.

#### Attack results {#attacks}

##### A. Is `append_attempt` armed for the whole window in which the store can hold a transaction the row does not reflect? {#attack-a}

Yes. Verified transitions, all under `state_mutex`:

- **Arm before first send.** `commitRefChunk` arms the attempt and sets `Writing` at
  `CasRefLedger.cpp:3585-3596`; the only ref-log `PUT` of the commit path is at `:3625`, after it. A
  failed arm (`!attempt_armed`) sends nothing (`:3598-3614`).
- **During the PUT.** Attempt armed, `Writing`.
- **Committed → frontier publication → install.** The frontier `_ckpt` publish (`:3766-3795`) runs
  with the attempt still armed. A publish throw or `FencedOut` calls `requireRecovery`
  (`:3783`, `:3794`), which sets `NeedsRecovery` and does NOT clear the attempt (`:2169-2176`). The
  install swaps the state, swaps the attempt out and sets `Ready` in one critical section
  (`:3855-3859`), preceded by a `state_unchanged` check that the armed attempt is still this one
  (`:3822-3829`). A stale runtime refuses to install and goes `NeedsRecovery` (`:3818-3821`, `:3840`).
- **Unresolved after a real send.** `Wedged`, attempt retained (`:3993-3998`); same on a
  non-`CORRUPTED_DATA` exception after the send boundary (`:3636-3643`).
- **Unresolved with nothing sent** (`NoAttemptSent`, `attempts_sent == 0`). Attempt cleared, `Ready`
  (`:3964-3971`). Nothing reached the network, so the store cannot hold it.
- **DefiniteFailure.** Attempt cleared, `Ready` (`:3919-3926`). The controller's contract
  (`Backend/CasRequestControl.h:469-476`) returns it only when every attempt of the call was proven
  never applied; an ambiguous predecessor forces `Unresolved`. Pre-existing, unchanged by the design.
- **Different object at our key.** `Faulted` (`:3686-3690`, `:3734-3738`) or `Closed` (`:3708-3712`),
  both refuse everything.
- **Wedge resolution.** Reads the same attempt (`:2216-2229`), re-sends the same bytes (`:2286`),
  publishes the frontier with the attempt still armed, installs and unwedges in one hold
  (`:2530-2538`). Every non-adoption arm leaves the attempt in place or moves to a refuse-all state.
- **Recovery.** `ensureRefTableRecovered` copies `retained_attempt` (`:1434`) and releases
  `state_mutex` around I/O (`:1486`, `:1582`) with `recovery_in_progress = true`, which rule 2 refuses
  (`:456-460`). `installRecoveryResult` clears the attempt and sets `Ready` only after installing the
  replayed state (`:1638-1640`).
- **Chunked flush.** Between chunk N's install and chunk N+1's arm the lane is `Ready` with no attempt
  (`:3006-3044`); the row reflects N, N+1 has not been sent. `flushRefBatch` itself never writes
  `rt->state` or the attempt (grep: the only `rt->state` writers are `:1625`, `:2534`, `:3855`).
- **The `Faulted` arm at new-id allocation** (`:3365-3400`) clears the attempt but goes `Faulted`.

No path clears or swaps the attempt while the row is behind the store.

##### B. Can a committed ref be mutated by anything that does not pass through the lane? {#attack-b}

No binding change can. Every ref-binding writer funnels into `appendRefOps`:
`PartFolderAccess.cpp:660` (`dropRefIfMatches`; `republishRef`/`repointRef`/`dropRefIfPresent` reach
the same ledger `dropRef`/publish path), `CasPartWriteTxn.cpp:657,769,1044` (publish, promote,
precommit), `CasPool.cpp:1524` (writer-cleanup precommit abandon), `CasPool.cpp:1883`. `RENAME`,
detach/attach, `MOVE`, `DROP`/`TRUNCATE`, FREEZE cleanup all reduce to these (the spec's inventory
matches what I read; `republishRef` is indeed publish-then-drop, two transactions,
`PartFolderAccess.cpp:506-536`).

Direct ref-log writers outside the lane, exhaustively (grep for `refLogKey(`, `putIfAbsentControlled(`,
`slotOccupy(` outside tests): the recovery walk's epoch seal (`makeEpochSealTxn` `:137-150`, sent via
`slotOccupy` at `:1132`). It carries no table content, so it changes no binding, and it runs while
`recovery_in_progress` is true, which rule 2 refuses. The other two `putIfAbsentControlled` callers in
the ledger are staging (`:251`) and snapshot publish (`:4427`), neither a ref-log transaction. GC
(`CasGc.cpp:3375`) deletes checkpointed ref-log objects and never changes a binding; fsck and the
orphan sweep only read (`CasFsck.cpp:378`, `CasOrphanManifestSweep.cpp:255,304`). A foreign writer after
fence loss is rule 6's business, unchanged.

##### C. Is the touched-ref set derivable and complete for every `RefOpKind`? {#attack-c}

Yes. `RefOpKind` has five kinds (`Formats/CasRefLogFormat.h:35-42`). The apply rules
(`Pool/CasRefProtocol.h:368-393`) show that the only ops that change a committed binding are
`OwnerTransition` remove (exact `old_binding` must exist) and promote (same `ref_name` on both sides,
never displaces another row implicitly); add creates a precommit only. `SetPublishedAt` names
`ref_name` and cannot change the manifest edge (`:279-280`, `:389-390`). `RemoveNamespace` requires both
maps already empty (`:391-393`), so it unbinds nothing itself. `EpochSeal` and `NamespaceBirth` carry no
`RefOp` fields. No op unbinds a ref without naming it. Marking the three field-less kinds namespace-wide
is conservative and correct.

##### D. Locking {#attack-d}

The confirm holds `ref_queue_mutex` and `try_to_lock`s `state_mutex` (`:438`, `:447-449`). Under the
new rule everything rule 3 reads (`lane_state`, `append_attempt`, `state`) lives under `state_mutex`,
and every writer of those three takes `state_mutex` (`:3585`, `:3855`, `:2530`, `:1638`, `:2169` callers).
Arming precedes the send, so there is no instant at which a touching transaction has been sent and the
confirm can observe `Ready` or a non-touching attempt. The leader reads the attempt's `key`/`bytes`
outside the lock through `active_attempt` (`:3616`) but never mutates it after arming; new fields
inherit that. `ref_queue_mutex` is no longer needed for the rule-3 conjunction, only for the slot lookup
(`:443-446`); see S5.

##### E. What did `pending`/`leader_active` protect that the sent-transaction check does not? {#attack-e}

Nothing that the safety argument needs:

- **Queued, unsent removal of the queried ref.** It becomes durable only after the confirm (T2), and
  the receiver's `+1` was durable at T1 < T2 (the receiver's `appendRefOps` returns after install and
  frontier publication). The blob is protected continuously. This is the protocol's existing argument
  and does not depend on the sender's queue being empty.
- **Leader between two chunks.** Row equals store (A above).
- **Leader inside `build_ops` or catalog reads** (`:3013`, `:2966`; `:3289-3350`). Nothing sent.
- **Partially durable tenure** (the old comment's justification, `:474-478`). Committed chunks are
  installed; the in-flight chunk is armed; the rest is unsent. Row plus attempt describe it exactly.
- **A live repoint** (`ContentAddressedTransaction.cpp:392` → `repointRef`). Its `OwnerTransition` names
  the ref on both sides, so the confirm refuses for that ref while it is armed and answers `No` after
  install (mint-tightening makes the new `ManifestRef` differ).
- **`Wedged` with a non-touching attempt.** A wedge is an ambiguous PUT, not evidence of deposition;
  deposition is `Closed` or a lost fence, both still refused.

##### F. Can the model and tests miss the failure they are meant to catch? {#attack-f}

The model can, as currently planned, in one way (F6). The tests can, in one way (F4). Details below.

#### Findings {#findings}

1. **PROSE, medium — the "recovery re-arm at `CasRefLedger.cpp:1876`" does not exist.** Line 1876 is
   inside `forceWedgeForTest` (`:1866-1878`), a test seam. Production has exactly one arming site,
   `:3592`. Recovery never re-arms an attempt: it copies `retained_attempt` (`:1434`) for
   adjudication and `installRecoveryResult` clears it (`:1638`). Spec §rule-3 ("filled where an
   attempt is built: in `prepareRefChunk` ... and on the recovery path that re-arms a wedged attempt")
   and §consult ("does `Wedged` ever hold an attempt whose ref set is stale") must be rewritten: one
   production fill site in `prepareRefChunk` (`:3213-3216`, from `chunk_txn.ops`); `forceWedgeForTest`
   sets `touches_namespace = true` because its bytes are test-supplied and need not decode.

2. **PROSE, medium — the docs-to-change list misses the comment that becomes false.** The
   function-header paragraph at `CasRefLedger.cpp:426-435` says the two-mutex snapshot exists because
   "`pending`/`leader_active` live under `ref_queue_mutex`" and concludes "There is no interleaving in
   which a removal is admitted and this function still answers `Yes`." Under the design a removal CAN
   be admitted (queued) while the confirm answers `Yes`, by design and safely. Also `CasRefLedger.h:40`
   ("`Ready` is the only state that ... certifies a cached row"), `:60` ("busy or non-`Ready` table
   answers `Unknown`") and `:156-159` ("lane state `Ready`" and "the two-mutex hold"). Add all four to
   §docs; the spec currently names only "the rule 3 comment block" (`:471-480`).

3. **PROSE, low — "Rule 4 (apply-pending poison)" is not a separate check in the code.** The poison of
   the model (`sPoison`) is `NeedsRecovery`, entered via `requireRecovery` (`:2169-2176`); the confirm
   covers it through `lane_state` alone. Say so, or the implementer will look for a rule 4 to preserve.

4. **PROSE/TEST, medium — a table-driven completeness test cannot see a kind it does not list.** §tests
   proposes "a unit test that `touched_refs` is complete for every `RefOpKind`, driven from a table of
   one op per kind, so a future kind without a mapping fails the test." A hand-maintained table fails
   open for the exact case it is meant to catch. The codebase's existing tool is a `switch` over
   `RefOpKind` with no `default` (`CasRefLogFormat.cpp:80`, `:193`; `CasRefProtocol.cpp:405`), which turns
   a new enumerator into a compile error under the project's warnings-as-errors. Require the derivation
   to be written that way; keep a per-kind semantic test if wanted, but it is no longer the guard.

5. **PROSE, low — "an epoch-seal attempt in flight" is not a production scenario.** `EpochSeal` is
   minted only by the recovery walk (`:1107`) and written with `slotOccupy` (`:1132`), never through
   `commitRefChunk`. The namespace-wide test is constructible only via `forceWedgeForTest` with seal
   bytes and then tests the flag, which is fine; reframe it as "a forced wedge whose bytes cannot be
   attributed to refs refuses every ref".

6. **PROSE, medium — the model plan under-specifies the extension and can pass vacuously.** Today's
   model has no armed state: `SenderAdmit` (`CaRelinkConfirmCore.tla:166`) sets `sPending` and
   `sLeader` together, `SenderDurable` (`:180`) is atomic, and rule 3 is `~sPending /\ ~sLeader`
   (`:269`). Revision 3 must state: (a) a new `sArmed` set by an arm step that `SenderDurable` requires
   and `SenderApply` (`:192`) clears, mirroring "arm before first send" at `CasRefLedger.cpp:3585-3625`,
   stated as a code invariant the model assumes; (b) a `sTouches` choice at admit, so a non-touching
   transaction leaves `sDurableRef` unchanged (no second ref needed); (c) rule 3 becomes
   `~(sArmed /\ sTouches)`; (d) `SabotageStaleCache` drops that conjunct, and a second sabotage forces
   `sTouches = FALSE`; (e) a new non-vacuity witness "a confirm answered `yes` while `sLeader /\ sPending`",
   without which the relaxation is not shown to fire and `_main` could pass with the old behaviour.

7. **CODE observation, info — one direct ref-log writer outside the lane, harmless.** The recovery
   seal (`:1132`) bypasses `append_attempt` but changes no binding and runs under
   `recovery_in_progress`. Name it in the inventory so "every mutation goes through the lane" reads
   exactly as verified rather than as an over-claim.

8. **PROSE, low — inventory qualifier "never a live part's ref" for `dropRefIfMatches`.**
   `PartFolderAccess.cpp:646-670` removes a `RefOwnerKind::Committed` binding; the qualifier is about
   callers, not enforced, and not load-bearing under the design because the op names the ref. Drop it.

9. **Limit, info — residual `Unknown` sources the design does not remove.** The `try_to_lock` fails
   while `listRefs`/`hasAnyRefWithPrefix` iterate the whole table under `state_mutex`
   (`CasRefLedger.cpp:380`, `:400`), while the snapshot publisher copies the state, and during an
   install. Bounded, not a livelock. A confirm about a ref whose own `SetPublishedAt` or repoint is in
   flight is `Unknown` for one flush, seconds on GCS. The proposed `StateLockBusy` and
   `SentTxnTouchesRef` counters are the right instruments; if either dominates on the live gate, that is
   a different fix.

#### Exact changes required for revision 3 {#changes}

- §rule-3, §consult: replace the recovery re-arm sentence with the single fill site; state that
  `forceWedgeForTest` marks the attempt namespace-wide (F1).
- §docs: add `CasRefLedger.cpp:426-435` and `CasRefLedger.h:40`, `:60`, `:156-159` (F2).
- §rule-3 or §safety: one sentence that the model's rule 4 is `NeedsRecovery` in code (F3).
- §tests: the touched-set derivation is a `switch` with no `default`; the table test is optional (F4).
- §tests: reframe the epoch-seal case (F5).
- §model: the five items in F6.
- §inventory: add the recovery-seal row; drop the "never a live part's ref" qualifier (F7, F8).

#### Simplifications {#simplifications}

- **S1.** One fill site, not two (F1). Removes the "decode the transaction to derive the set" branch.
- **S2.** Compile-time completeness via `switch` replaces a test (F4).
- **S3.** Keep `touches_namespace` as a plain bool; do not fold it into the set. It is the fail-closed
  default for kinds without names and costs one byte.
- **S4.** `SetPublishedAt` could be excluded from the touched set safely (it cannot change the manifest
  edge, `CasRefProtocol.h:279-280`). Not recommended: including it is the simpler rule with no
  special case, and the over-refusal is one flush per publish.
- **S5.** `ref_queue_mutex` is no longer part of the rule-3 conjunction. Keep holding it (it protects
  the slot lookup and the hold is O(1)), but the comment must stop claiming the two-mutex snapshot is
  what makes `Yes` sound. No code motion.
- **S6.** Model: two booleans and one witness, no second ref, no part state.
- **S7.** The seven `ProfileEvents` are observability for the live gate, not part of the fix. They are
  justified by the gate's oracle; if the change must be smaller, ship `SentTxnTouchesRef`,
  `LaneBroken` and `StateLockBusy` and leave the rest.

#### What this consult did not establish {#limits}

- The GC LIST-completeness assumption (`MaxHoles = 0`) and the mount-fence timing of rule 6 are
  unchanged and were not re-examined.
- I did not run TLC or any test. The model claims above are about what the plan must say, not about a
  run.
- I did not read the other model's consult (`tmp/gcs_live_20260902/codex_review_f11_spec.log`), to
  keep this one independent.
- The claim "no path clears the attempt while the row is behind" is about the code at HEAD
  `9041853b181`; it is not a statement about future edits, which is why F4's compile-time guard and
  the `state_unchanged` check at `:3822-3829` matter more than the prose.

#### Revision 3 check {#revision-3-check}

Re-read revision 3 at commit `ed475cd2015` only. Every new citation and every new factual claim was
checked against the tree at that commit.

##### Required changes from the revision 2 consult {#rev3-required}

| Finding | Status | Where in revision 3 |
|---|---|---|
| F1 single fill site; `forceWedgeForTest` namespace-wide | CLOSED | §rule-3: "Production has exactly one fill site, `prepareRefChunk`"; recovery copies (`:1434`) and clears (`:1638-1640`); `forceWedgeForTest` (`:1866-1878`) sets `touches_namespace`. All three citations verified. |
| F2 docs list names the comments that become false | CLOSED | §docs: `CasRefLedger.cpp:426-435`, `:471-480`; `CasRefLedger.h:40`, `:60`, `:156-159`, `:577-608`. `:577-608` verified as the `RefAppendAttempt` comment and fields. |
| F3 rule 4 is `NeedsRecovery` in code | CLOSED | §safety, last paragraph. |
| F4 derivation is a `switch` with no `default` | CLOSED | §rule-3 and §tests. The new citation `CasRefProtocol.cpp:112-154` is `OwnerTransitionShape` and `classifyOwnerTransitionShape`, the four legal shapes as stated. |
| F5 epoch-seal case reframed | CLOSED | §tests: `WedgedLaneIsUnknown` stays `Unknown` because `forceWedgeForTest` marks the attempt namespace-wide. |
| F6 model plan: `sArmed`, `sTouches`, rule, two sabotages, witness | CLOSED | §model, all five items present, plus the between-chunks state. |
| F7 recovery-seal inventory row | CLOSED | §inventory, last row. |
| F8 qualifier dropped | CLOSED | §inventory, `dropRefIfMatches` row now "`OwnerTransition` removals naming their ref". |
| F9 residual `Unknown` sources | CLOSED | §tests, observability paragraph. |
| S1-S7 | REFLECTED | S5 in §rule-3 (the `ref_queue_mutex` sentence); S7 in §tests (three counters, others optional). |

##### The four named tests {#rev3-tests}

Verified in `src/Disks/tests/gtest_cas_confirm_exact_ref.cpp` at that commit:

- `InFlightAppendIsUnknown` (`:472-503`): same-ref `dropRef("x")`, leader parked by the pre-carve latch,
  confirm about `x`. The spec's phase expectations (`Yes` queued, `Unknown` armed through `Writing`, `No`
  installed) follow from the design; the "armed" phase needs the existing `PostDurableInstall` hook
  (`CasRefLedger.cpp:3797`), which the spec's regression test already uses.
- `MidTenureChunkBoundaryIsUnknown` (`:512-586`): confirm about `seed`, untouched by the two co-batched
  items, issued at `ChunkReseed`. That hook fires at `:3036`, before the reseed takes `state_mutex` at
  `:3038` and after chunk 1's install cleared the attempt, so `Yes` is the correct new expectation.
- `WedgedLaneIsUnknown` (`:589-609`): `forceWedgeForTest` with the bytes `"synthetic"`, so
  namespace-wide, `Unknown` for every ref. Correct.
- `ConcurrentAppendIsOrderedAfterTheSnapshot` (`:672-728`): same-ref removal, three phases via the
  pre-carve latch. The spec's four-phase rewrite is correct; the "admitted but unsent `Yes`" phase is
  the one the test's header comment currently calls impossible, and that comment must go with it.

No other gtest calls `confirmExactRef`. The two integration tests that wait for a refusal
(`tests/integration/test_cas_replicated_relink/test.py:792-797`, `:884-889`) induce it by merging the
source part away until no part of that name remains (`:756-766`), so the answer comes from gate 0 or
rule 5 and is unaffected by the design. Four is the complete count.

##### Newly noticed, none blocking {#rev3-new}

- **N1, PROSE, low.** §tests says the real-wedge sibling leaves a `PUT` `Unresolved` "through the
  request-control seam". Request control has only a sleep seam (`CasRequestControl.h:604`). The seams
  that produce a real wedge in existing tests are test backends: `ChunkFaultBackend::Mode::Unresolved`
  (`gtest_cas_ref_install_safety.cpp:236-262`, `UnresolvedAlwaysRecordsTheWedge`) and
  `CasInMemoryBackend::injectAmbiguousPutIfAbsent` (`CasInMemoryBackend.h:98-108`). Name one of those so
  the implementer greps for the right thing.
- **N2, PROSE, low.** §consult cites `tmp/gcs_live_20260902/` for the two consult reports. That
  directory is not tracked (`git ls-files tmp/gcs_live_20260902` is empty), so a committed spec points
  at a scratch path that will not exist for the next reader. Either commit the two reports beside the
  ledger under `docs/superpowers/cas/` or drop the path and keep the sentence.
- **N3, info.** The claim that the ambiguous-`PUT` sibling test is constructible holds: a real wedge is
  reached today from `commitRefChunk`'s `Unresolved` arm with the attempt built by `prepareRefChunk`, so
  its touched set is the derived one, which is what the sibling needs.

##### Verdict for revision 3 {#rev3-verdict}

**APPROVE.** Every required change from the revision 2 consult is closed, and every new citation in
revision 3 is correct. N1 and N2 are editorial and can be folded into the next touch of the document;
neither needs a revision 4 before the model variant or the code task is cut.


## Codex round 2, revision 2: REQUEST CHANGES (prose) {#codex-round-2}

1. **PROSE — medium:** The model plan overstates the existing model’s expressiveness. The journal records have no ref identity, the sender has only one modeled ref, and `sPending` spans admission through durability; there is no armed/sent/Wedged phase (`docs/superpowers/models/CaRelinkConfirmCore.tla:101`, `:114-120`, `:164-188`). Consequently the model cannot currently distinguish queued-unsent work from a sent attempt, or an attempt touching the queried ref from another ref, despite the claim at `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:143-150`. Add explicit attempt phase, touched-ref/namespace-wide state, and at least two sender refs; model the between-chunks no-attempt state. Define the two sabotage flags distinctly or remove the duplicate. `_sab_stalecache` will still counterexample when the touching attempt is ignored: its durable `m1 → m2` repoint leaves `sCacheRef = m1` (`CaRelinkConfirmCore_RESULTS.md:126-139`).

2. **PROSE — medium:** The test plan does not explicitly replace the existing rule-3 expectations that revision 2 reverses. A queued same-ref removal before carving currently expects `Unknown` (`src/Disks/tests/gtest_cas_confirm_exact_ref.cpp:467-503`), an untouched ref at a chunk boundary expects `Unknown` (`:507-582`), and the concurrency test expects `Unknown` from admission onward (`:663-708`). Under the proposed rule these must be `Yes` until the touching attempt is armed, then `Unknown` through `Writing`/`Wedged`, and `No` after install. Add those phase-separated assertions, plus both same-ref and unrelated-ref cases in `Writing` and `Wedged`; the current synthetic wedge test (`:586-607`) can only exercise a conservative namespace-wide fallback unless it carries or decodes a valid transaction.

3. **PROSE — medium:** “One op per `RefOpKind`” is insufficient coverage for `OwnerTransition` (`docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:175-176`). The state machine has four legal shapes—new-only precommit add, old-only precommit removal, old-only committed removal, and old+new promotion (`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefProtocol.cpp:112-154`). Test every shape and assert extraction from both optionals; the old-only committed removal is the safety-critical case.

4. **PROSE — low:** The cited “recovery path that re-arms a wedged attempt” is actually the test-only `forceWedgeForTest` seam (`docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:72-75`; `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasRefLedger.cpp:1866-1877`). Production recovery copies the retained attempt before its unlocked walk and clears it only when installing recovered state (`CasRefLedger.cpp:1432-1434`, `:1619-1640`). Correct the citation and describe how the test seam supplies conservative or decoded touch metadata. Also include the `RefAppendAttempt` field/identity comments in the documentation updates (`CasRefLedger.h:577-608`).

REQUEST CHANGES — specify the model’s ref/attempt-phase extension, update the reversed rule-3 tests with explicit queued/Writing/Wedged/chunk-boundary expectations, cover every legal `OwnerTransition` shape, and correct the line-1876 recovery claim.


## Codex round 3, revision 3: REQUEST CHANGES (prose) {#codex-round-3}

1. **PROSE — MAJOR — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:162-179`**  
   The model extension remains under-specified. Currently `SenderAdmit` admits only `Other`/`"none"`, `SenderDurable` always emits the sender-edge deletion, and `SenderApply` clears both `sPending` and `sLeader` (`CaRelinkConfirmCore.tla:166-198,367-370`). Thus there is no honest non-touching durable transaction and no installed-between-chunks state. The proposed witness can succeed merely between admission and the new arm step, without exercising either `sArmed /\ ~sTouches` or the post-install chunk boundary. Specify an honest non-touching/noop transaction, derive `sTouches` from its actual shape except under the new sabotage, split apply from tenure completion, and add explicit witnesses for armed-non-touching `Yes` and post-install/pre-next-chunk `Yes`. With that mapping, both stale-cache sabotages will reproduce the intended counterexample.

2. **PROSE — MAJOR — `docs/superpowers/models/CaRelinkLaneComposition.tla:3-9,111-133,209`; `docs/superpowers/models/CaRefLaneCore.tla:714-722`; `docs/superpowers/models/CaRefLaneCore_RESULTS.md:68-91`**  
   The repository’s companion composition model still defines certification as `Ready`-only and treats any confirmation in `Writing`/`Wedged` as sabotage. Revision 3 changes that public seam but schedules only `CaRelinkConfirmCore.tla`. Extend the lane/composition model with attempt touch scope—or explicitly replace that obsolete contract—and rerun/update its configs and results.

3. **PROSE — MINOR — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:236-247`**  
   The comment inventory is still incomplete. Add `CasRefLedger.h:616`, whose “COMPLETE” field enumeration will omit `touched_refs` and `touches_namespace`; `gtest_cas_confirm_exact_ref.cpp:40-46`, which explicitly forbids the newly allowed admitted-unsent `Yes`; and the affected `CaRelinkConfirmCore_RESULTS.md:85-92,126-142`. The listed `CasRefLedger.cpp:426-435,471-480` set is otherwise correct.

4. **CODE — INFO (closed) — `CasRefProtocol.cpp:112-154`; `CasRefLedger.cpp:1434,1638-1640,1866-1878,3213-3216`**  
   The four `OwnerTransition` shapes, exhaustive no-default `RefOpKind` derivation, single production fill site, retained recovery attempt, and namespace-wide `forceWedgeForTest` treatment are now described correctly. The recovery-seal inventory row is also accurate (`CasRefLedger.cpp:137-152,1107-1133`).

5. **CODE — INFO (closed) — `gtest_cas_confirm_exact_ref.cpp:467-708`; `cas_test_helpers.h:1961-2035`; `CasRefLedger.cpp:2034-2120,3748-3907`**  
   The phase expectations are correct, and `ChunkFaultBackend::Mode::Unresolved` with a single-attempt budget can construct the real wedge sibling. The queued-unsent `Yes` is sound: a sender touching transaction is armed before its first send, while receiver `appendRefOps` returns only after frontier publication, cache install, and waiter completion, so the receiver’s T1 is durable before T2.

REQUEST CHANGES — fully specify and witness the honest non-touching and between-chunks model transitions; update the `CaRefLaneCore`/`CaRelinkLaneComposition` contract and results; add the omitted comment/result files to the documentation inventory.

## Codex round 4, revision 4: REQUEST CHANGES (model plan prose) {#codex-round-4}

1. **PROSE — MAJOR — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:171-188`**  
   Finding 1 is not fully closed. `NoOp` is a valid abstraction—the model already has edge-neutral records (`CaRelinkConfirmCore.tla:91-101`)—and splitting installation from tenure end matches the implementation (`CasRefLedger.cpp:3855-3859,2165`). However, the proposed transitions remain under-specified: current `SenderApply` requires `sDurableRef # Token` (`CaRelinkConfirmCore.tla:192-199`), which never becomes true for `NoOp`; revision 4 names no replacement durable-phase marker, does not state that `SenderInstall` requires durability and clears `sPending`, and does not distinguish the post-install state from the pre-arm state. The witnesses also need history flags set by `RConfirm`; otherwise a stored earlier answer can later coexist with the desired sender state without that confirm occurring in it.

2. **PROSE — MAJOR — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:164,192-202`**  
   Finding 2 is only partly closed. The companion modules are now in scope, but the proposed rewrite is incomplete. `CaRefLaneCore::Certify` currently protects not only `Ready`, but also current-runtime authority and cache/durable equality (`CaRefLaneCore.tla:714-722`). Replacing that property solely with “no certification while the armed attempt touches the identity” would discard those checks; retaining global `cache_id = durable_id` would instead reject a legitimately non-touching landed transaction. Preserve `CurrentRuntime` and identity-specific row currency while replacing only the `Ready` restriction. Also rerun `run_reflane.sh`, which owns the `CaRefLaneCore` battery (`run_reflane.sh:16-43`); `run_relinklane.sh` runs only `CaRelinkLaneComposition`. The composition is a separate abstract model, not a TLA component instance (`CaRelinkLaneComposition.tla:3-9,21-38`), so its touch observable must be modeled independently. Line 164 should say three modules, not two models.

3. **CODE — INFO (closed) — `src/Disks/tests/gtest_cas_ref_install_safety.cpp:241-275`; `src/Disks/tests/cas_test_helpers.h:1967-2024`**  
   `ChunkFaultBackend::Mode::Unresolved` with the single-attempt budget is the correct real-attempt wedge seam.

4. **PROSE — INFO (closed) — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:260-275`**  
   Finding 3 is closed: the inventory now includes `CasRefLedger.h:616`, the gtest header, both affected `CaRelinkConfirmCore_RESULTS.md` passages, and the companion lane results/README material.

5. **PROSE — INFO (closed) — `docs/superpowers/specs/2026-09-02-cas-relink-confirm-liveness-design.md:12-20`; `docs/superpowers/cas/2026-09-02-f11-spec-consults.md:20,123,420,433`**  
   The status and committed consult record agree with the history: three Codex rounds and one independent Claude consult. Describing revision 2 as passing “on the design itself” is accurate; the objections were prose/model-plan specification findings.

REQUEST CHANGES — define an explicit durable/install phase for both transaction shapes; make `SenderInstall` require durability, clear `sPending` and `sArmed`, and retain `sLeader`; record all three witnesses at the actual `RConfirm` transition; preserve authority and identity-specific cache correctness in `CaRefLaneCore`; model the composition observable independently; rerun both `run_reflane.sh` and `run_relinklane.sh`; and correct “two models” to three modules.

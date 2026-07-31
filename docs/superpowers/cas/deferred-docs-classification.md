# Classification of `deferred-docs-fixes.md` D1..D29 (read-only pass, 2026-07-31)

Every row below was produced by opening the file and line named and reading the sentence **as it
reads today**. Line numbers are today's — several entries quote line numbers that have since shifted
(D18 in particular).

Verdicts: STALE / VALID-PROSE / ACTUALLY-CODE / DELETE-INSTEAD / UNRESOLVABLE.

## Table

| id | verdict | evidence |
|----|---------|----------|
| D1 | STALE | `src/.../Formats/CasLayout.h:274-277`. Reads today: *"It THROWS `CORRUPTED_DATA` naming the key in the same one situation the ref parsers do: the key carries the reserved segment, but the segment where its incarnation belongs is missing, non-canonical or zero."* The "IS one of our namespace files" clause is gone; `grep -rn "one of our namespace files" src/` → zero hits. Mirrored `CasLayout.cpp` comment likewise carries no such clause. |
| D2 | STALE | `src/.../ContentAddressedTransaction.cpp:1070-1074`. Reads today: *"For a ref-family key that is a test's claim rather than this comment's: `CasRefGc.UnIncarnatedRefKeyAbortsRefFoldingWithoutWedgingTheRound` … For a `_files`-family key the round never gets the chance: `Cas::Gc`'s fold only LISTs `casRefsPrefix()`, never `rootsPrefix()`."* Citation is scoped per family exactly as the entry's fix asked. |
| D3 | STALE | `src/Disks/tests/gtest_cas_namespace_life_id.cpp:421-425`. Reads today: *"the type declares no conversion operator and no `RootNamespace` constructor takes a `NamespaceLifeId`, so nothing interconverts in either direction today"*. The `explicit` clause is gone. |
| D4 | STALE | `src/.../Tools/CasFsck.h:230-234`. Reads today: *"The rule is also restated in `docs/superpowers/cas/08-testing-and-soak.md` and in the soak harness's comments and messages; those restatements have gone stale before -- more than once, about the exit set … (No count is given, for the same reason the paragraph above gives none…)"*. Count dropped; "docstrings" replaced by "comments and messages". |
| D5 | STALE | `.superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/task-1c-report.md:643`. Reads today: *"…explicitly that no count is given **and why**; `InterpreterSystemQuery.cpp`'s drops the count without…"* — the sentence now separates the two sites. |
| D6 | STALE | `docs/superpowers/cas/08-testing-and-soak.md:106-111`. Reads today: *"`stale_edge` is a `clean()` term that `CommandFsck::executeImpl` never exits nonzero on, in EITHER mode … **a zero exit code is therefore not by itself proof of a clean pool, in either mode: `stale_edge` needs its own check.**"* "summary" dropped, `stale_edge` named. |
| D7 | STALE | `src/.../Formats/CasRefCatalogFormat.h:136-143`. Reads today: *"…an OVER-ESTIMATE per entry, not a claim about how many `cov`/`nsc` rows one namespace can carry in total (`ns_cleanup_items` is keyed per removal, so a namespace removed more than once can carry more than one `nsc` row)."* |
| D8 | STALE | `src/.../Formats/CasFormat.cpp:146-150`. Reads today: *"the line cap is tight (4 KiB) because one entry's record is ordinarily small -- but not always small enough: a namespace or `server_root_id` near their own byte bounds, worst-case escaped, can push a single line past 4 KiB, and `encodeRefCatalog` REFUSES that entry (`LIMIT_EXCEEDED`, `CasRefCatalogFormat.cpp`'s `checkLineBytes`)…"* |
| D9 | STALE | `src/.../Formats/CasRefCatalogFormat.h:127-133`. Reads today: *"A FLOOR on the fold seal's fixed frame cost (header + meta + trailer, zero entries), not a constant: measured at `generation = 0` and `n = 0`…"* Meta line named, floor stated. |
| D10 | **ACTUALLY-CODE** | Report half is fixed — `task-2-report.md:52-53` now reads *"predicate (2) exact-ENTRY-COUNT boundary/`+1` entry"*. The residual is a **test name**: `src/Disks/tests/gtest_cas_ref_catalog.cpp:364` `TEST(CasRefCatalogAdmission, Predicate2AcceptsEqualityRefusesOneEntryOver)` — the body asserts `checkFoldSealReservation(max_entries)` vs `max_entries + 1`, i.e. an entry-count boundary, not byte equality. The honest fix is a rename, which is a code edit. The entry itself says so and defers it; it must not be silently re-filed as prose. |
| D11 | STALE | Closed by side effect and its own note forbids touching it. Verified today: `src/.../Pool/CasRefLedger.cpp:3013-3026` has exactly one `attempt_armed` arm and the removed call site is gone. **Do not edit.** (Separate, uncovered: the *other* "THREE call sites (corrected here from 'two'…)" comment at `CasRefLedger.h:960` — see nearby defects.) |
| D12 | STALE | `grep -rn "never reaches the object store" src/` → zero hits (only this deferred-fixes file). `src/.../Pool/CasServerRoot.h:543-550` reads today: *"`fence_generation` is deliberately NOT one of the two scalars this function takes, and not because it is unavailable -- the catalog persists it … but because it is not the property this predicate needs … comparing it across actors compares two unrelated counts that happen to share a name."* The non-comparability reason is the one on record. |
| D13 | STALE | `src/.../Pool/CasServerRoot.h:566-570`. Reads today: *"an ABSENT mount slot (`Backend::get` returning `nullopt` answers nothing about liveness…) and an UNDECODABLE body (… mirroring `probeNonTerminalMountSlots`'s own stated discipline for that case) both return `false`"* — the citation sits only beside the undecodable-body case. |
| D14 | STALE | `grep -rn "nothing is lost by removing it" src/` → zero hits; `grep -rn "cleanupOrphanedBirthCkpt" src/` → zero hits. The function and its clause are both gone from the tree. |
| D15 | STALE | `src/.../Pool/CasRefCatalog.h:259-268`. Reads today: *"NOT YET ENFORCED ON THE PRODUCTION REF-WRITE PATH -- state this plainly rather than let the spec sentence above read as a claim the tree already satisfies. `CasRefLedger::commitRefChunk` … does not call this function…"* The disclosure exists. (The paragraph it added now itself cites `plan 0cf11354aa0` and "Task 4" — see nearby defects.) |
| D16 | VALID-PROSE | `src/Disks/tests/gtest_cas_list_liar_end_to_end.cpp:427-428`. Reads today: *"`hidden` was born through the real writer path (`publishAt`, `birth=true`), so it is `Live` in the catalog…"*. Verified false: `cas_test_helpers.h:1093 publishAt` → `writeTxnAt` (:1066) → `writeRefLogTxnRaw` (:998), whose own doc says it *"Admits `txn.ns` into the catalog first (`casAdmitEntry`…)"*. Catalog-`Live` comes from the test-only admission shim, not production birth. Fix is a comment rewrite only. |
| D17 | VALID-PROSE | `src/Disks/tests/cas_test_helpers.h:944-948`. Reads today: *"…does NOT itself admit an entry, so a namespace this helper is the ONLY writer for stays exactly as invisible to the catalog as it was before Task 4-C (unchanged from this helper's own pre-existing scope)."* No pointer to `casAdmitEntry`; the sibling at :998 has one. Adding the pointer is doc-only. Note against the policy: the added sentence must not carry the "Task 4-C" citation the surrounding text already carries. |
| D18 | **DELETE-INSTEAD** | `src/Disks/tests/gtest_cas_gc_shard_incarnation.cpp:336-337` (entry cited 108-109; shifted). Reads today: *"Task 4: listNamespaces is LIST-based; no registry involved. / Publishing into ns A makes it appear in listNamespaces(""); ns B absent."* The bare task number is exactly the forbidden branch-local citation, and the second sentence restates what the test body does. `Pool::listNamespaces` (`CasPool.cpp:1396-1399`) is verified still LIST-based, so nothing false survives deletion. Delete both lines rather than qualify which Task 4 is meant. |
| D19 | STALE | No-action entry, and its one tracked item landed: `task-4c-report.md:45` now records the real experiment (*"…to its pre-session content (`git checkout 19ae69f64cb --`)…"*) and :177 states M3 is resolved in substance. Nothing to edit. |
| D20 | **DELETE-INSTEAD** | `src/.../Gc/CasGc.cpp:2019-2022`. Reads today: *"`life` below is still needed for logging/key-construction parity with the catalog-named case further down this loop, but `catalog_names_this_namespace` is what gates whether this namespace is ever actually walked at it."* Verified: `life` is first read at :2287 (`layout.refLogKey(life, *expected)`), inside `while (expected)` at :2282; the un-cataloged branch at :2220 does `expected.reset()` at :2227, so that path never reads `life`. The paragraph's real content (why the sentinel must not gate a GET) stands on its own; delete the false clause rather than narrow it. Optional code alternative that would remove the need for any sentence: compute `life` only on the catalog-named branch. |
| D21 | **ACTUALLY-CODE** | Two of the three sites are **runtime strings**, not comments: `src/.../Gc/CasGc.cpp:1481-1483` — *"ref intake: key {} names a namespace with no catalog entry (Live/Removing) this round -- dropped rather than folded (expected on an ordinary removal too, not only on damage, until Task 5's removal-evidence check lands)"* — and `:2222-2226` — *"…than walked at a fabricated one (expected until Task 5's removal-evidence check lands, on an ordinary removal too, not only on damage)"*. Both are `report.recordAnomaly` payloads: editing them changes program output (and they ship a branch-local task number to operators). Third site is a comment: `src/Disks/tests/gtest_cas_part_folder_access.cpp:1141` — *"the catalog entry survives until Task 5's last step, so it is still readable here."* Verified premise: `CasRefCatalog` still has no entry-deletion primitive (`CasGc.cpp:2044` says so in-tree). Route through a code review, not the prose pass. |
| D22 | VALID-PROSE | `src/Disks/tests/gtest_cas_ref_gc.cpp:868-870`. Reads today: *"the item as uncataloged (also correct, but for the WRONG reason -- this test is specifically about the round-freshness guard, and an uncataloged skip would let it pass without ever reaching, let alone exercising, that guard."* — `(` at "(also" never closed. Pure punctuation. |
| D23 | **ACTUALLY-CODE** | `src/.../Pool/CasPool.h:519-521`. Reads today: *"NEVER creates a namespace: an uncataloged one is answered from a catalog-only lookup that writes nothing, so a probe or an `if_exists` unlink against a table that was never opened cannot admit an entry into the pool-wide catalog."* Verified false at operation granularity: `ContentAddressedMetadataStorage::listDirectory`'s `TableDir` branch (`ContentAddressedMetadataStorage.cpp:1673-1679`) calls `store()->listRefs(ns)` **before** `readableNamespaceFilesLife(ns)`, and both `CasRefLedger::resolveRef` (`CasRefLedger.cpp:281-282`) and `CasRefLedger::dropNamespace` (`:4188-4189`) call `ensureRefTableRecovered`, whose step 0 mints. The comment is false *because the code mints on read/removal paths*; the code half is already placed as an executing plan step ({#d23-code-half-is-placed}), so a prose narrowing done now goes stale the moment that step lands. Sequence the two, do not fix the sentence in isolation. |
| D24 | VALID-PROSE | `src/Disks/tests/gtest_cas_namespace_file_request_profile.cpp:406-409`. Reads today: *"A GC round touches all three of `cas/ref_catalog`, `cas/refs/` and `cas/manifests/`, so with the background scheduler enabled these zeros would hold only because the first tick (60s) outlives the test. A timer is not a fence."* Verified: the storage two lines down is constructed with a null context (`…, "srv1", "", nullptr, settings`), so no scheduler was ever started. Setting stays; only the reason is rewritten. Comment-only. |
| D25 | **DELETE-INSTEAD** | `src/Disks/tests/gtest_cas_namespace_file_request_profile.cpp:52-53`. Reads today: *"The two `CasNamespaceFileDiskProfile` cases at the bottom of this file fence it there…"* — there are three (`:442`, `:479`, `:512`). The entry already proposes dropping the number rather than correcting it; that is the right call and matches rule 2. Drop "two", keep the sentence. |
| D26 | VALID-PROSE (with a code obligation) | `src/.../Pool/CasRefLedger.h:588-591`. Reads today: *"Resolved ONCE per table-open, on the FIRST recovery attempt of this runtime's lifetime (`ensureRefTableRecovered`), and never re-resolved by a later re-recovery of the SAME runtime…"* Verified second writer: `CasRefLedger.cpp:4145-4157` (`namespaceFilesLifeIfReadable` step 1) takes `state_mutex`, reads `CasRefCatalog::lifeIfCataloged` and sets `rt->life` without recovering. Nothing executes wrongly today (no entry-removal API exists, so no cache to invalidate), so the fix is doc-only — **but** the future "invalidate the cached life on entry removal" step must cover both writers, including the pure-read one. Record that where the step lives, not only here. |
| D27 | STALE | The cited sentence no longer exists. `grep -rn "same role it plays" src/` → zero hits. It was deleted by `59cbe85640d` ("move the life_epoch decrease refusal out of the merge, to the publish site"), which restored `mergeCkpt` to its unordered/commutative form and removed the whole `stored`/`contribution`/`what` paragraph. `CasRefCkpt.h:30-36` today carries no `what` parameter or parity claim; `checkRefCkptInvariants`'s own doc (`CasRefCkptFormat.h:112-113`) simply says *"`what` identifies the direction in the exception message."* |
| D28 | VALID-PROSE (coverage residual flagged) | `src/.../Formats/CasRefCkptFormat.h:52-53`. Reads today: *"Both halves are fenced by `gtest_cas_ref_ckpt_join.cpp`, which also carries the compile-time `std::is_trivially_copyable_v<RefCkpt>` assertion that a heap-owning field would break."* Both bullets above it ARE fenced (`EncodedCkptSizeIsIndependentOfCardinality:440`, `EncodedCkptSizeHasAConstantCeilingAcrossTransactionsAndEpochs:472`), so the sentence is literally true; what it over-implies is invariant completeness. Verified gap: the ceiling test constructs `RefCkpt worst{…}` naming today's three fields, so a future fixed-capacity, non-heap field written only by the sealer/publisher changes neither `CKPT_WORST_CASE_ENCODED_BYTES` (if it encodes as absent) nor `is_trivially_copyable_v`. Prose fix (name which writer the fences drive) closes the entry; closing the *gap* is a test change and is not in this pass. |
| D29 | VALID-PROSE | `src/.../Formats/CasRefCkptFormat.h:30`. Reads today the short ragged line *"/// Writing the whole body is what makes a stale field dangerous, and"* followed by a full-width line at :31. Pure reflow. |

## Contradictions between entries

1. **The `{#policy-change}` note vs D17 (and, retroactively, D15).** The note makes deletion the
   default and says an entry that asks for a sentence may now be answered by structure or by nothing.
   D17 asks to **add** a sentence to `writeRefSnapshotRaw`'s doc; D15's already-executed fix added a
   paragraph to `CasRefCatalog.h` that now cites `plan 0cf11354aa0` and "Task 4" — precisely what the
   note forbids. The pass must decide D17 under the new note, not the old one, and should treat D15's
   landed paragraph as needing the citation stripped even though D15 itself is closed.
2. **D23's two halves pull opposite ways in time.** D23 asks to narrow `CasPool.h`'s sentence to the
   namespace-file surface; {#d23-code-half-is-placed} schedules code that removes the mint from
   `listRefs`/`resolveRef`/`dropNamespace`, at which point the *unnarrowed* sentence becomes true again
   and the narrowed one becomes an understatement. Not two entries disagreeing, but two obligations on
   one sentence that cannot both be satisfied at once.

No two entries request opposite edits to the same sentence at the same time.

## Nearby defects of the same kind, covered by no entry

- `src/.../Pool/CasPool.cpp:1412`: *"this function has four callers"* — there are at least six non-test
  call sites (`CasFsck.cpp:587,908,1091`, `ContentAddressedMetadataStorage.cpp:1479`,
  `ContentAddressedTransaction.cpp:1078`, `CasDecommission.cpp:131`). A stale count, the stage's
  recurring defect, in a load-bearing rationale.
- `src/.../Pool/CasRefLedger.h:960`: *"THREE call sites (corrected here from "two": … and was missing
  from this count)"* — a count **plus** the review-correction provenance, both forbidden; this is the
  same shape D11 was kept as a closed lesson about.
- `src/.../Gc/CasGc.cpp:2044-2045`: a code comment cites *"`deferred-docs-fixes.md` D19/NEW-4-6"* — code
  pointing at this very branch-local file.
- `src/.../Pool/CasRefCatalog.h:265-266`: *"Task 4's catalog-backed universe (plan `0cf11354aa0`, …)"* —
  a plan commit hash in a public header, added by D15's own fix.
- `src/.../ContentAddressedTransaction.cpp:1043` (*"Remove gate (rev.7 §1)"*),
  `src/.../Formats/CasLayout.h:276` (*"Behind Stage B's format bump"*),
  `src/Disks/tests/gtest_cas_namespace_life_id.cpp:421` (*"Directive §1's remaining requirements"*),
  `src/.../Pool/CasRefLedger.cpp:3027` (*"review C1, and the whole class removed by increment review
  Critical B"*, plus a BACKLOG anchor), `src/Disks/tests/gtest_cas_gc_shard_incarnation.cpp:107`
  (*"Increment review NEW-1"*), `src/Disks/tests/gtest_cas_part_folder_access.cpp:1140` (*"review C2's
  whole point"*): spec-revision, stage, directive, review-round and BACKLOG citations in comments —
  the class the policy bans, and far more numerous than the entry list suggests.

## Counts

| verdict | count | ids |
|---------|-------|-----|
| STALE | 16 | D1, D2, D3, D4, D5, D6, D7, D8, D9, D11, D12, D13, D14, D15, D19, D27 |
| VALID-PROSE | 7 | D16, D17, D22, D24, D26, D28, D29 |
| DELETE-INSTEAD | 3 | D18, D20, D25 |
| ACTUALLY-CODE | 3 | D10, D21, D23 |
| UNRESOLVABLE | 0 | — |

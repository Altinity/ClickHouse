# T3 arc review — `laneg/t3-finish` (`30108b5ee64`, `babea62289b`, `53e7b4c8588`)

Base `8e2d46ba38d`. Reviewed read-only from git objects; builds and runs done in the
`/home/mfilimonov/workspace/ClickHouse/lane-g` worktree, which is checked out at `53e7b4c8588`
with no source modifications.

## Verdicts

| commit | subject | verdict |
|---|---|---|
| `30108b5ee64` | draft: renamed arm-(a) test + new arm-(b) test | **APPROVE** |
| `babea62289b` | fsck slice: janitor-pending split, observe-then-cut | **REJECT** (2 blocking CODE findings) |
| `53e7b4c8588` | T3 closure: two-phase heal + UX message + report | **APPROVE-WITH-NONBLOCKING** |

## My own numbers (not the report's)

Rebuilt `unit_tests_dbms` in both `build` and `build_asan` at the tip of the three commits, under
`flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`:

- `build/t3rev_build_release.log` — `NINJA_REL_EXIT=0`
- `build_asan/t3rev_build_asan.log` — `NINJA_ASAN_EXIT=0`

Filter `*CasFsck*:*CasDecommission*:*CasLayout*:*CasNamespaceLife*:*CasNamespaceJanitor*:*Fsck*:*Decommission*`:

- release (`build/t3rev_tests_release.log`): **144/144 passed**, exit 0
- ASan (`build_asan/t3rev_tests_asan.log`): **144/144 passed**, exit 0

Both runs include all five new/renamed tests by name
(`VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot`,
`CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`,
`CanonicalDeadLifeResidueIsJanitorPendingNotHardFinding`,
`LifeAdmittedBetweenNamespaceListingAndLaterCutIsNotResidue`,
`MalformedNamespaceTreeShapesStayHardFindings`). The ASan run lists one extra suite name
(`CasNamespaceLifeIdDeathTest`) at the same 144 total — a gtest suite-naming artifact, not an extra
test. `MalformedNamespaceTreeShapesStayHardFindings` prints exception stack traces into the log while
passing; cosmetic.

I did **not** run the integration lane or the stateless suite (MAIN owns the praktika slot); finding
F2 below is proved from the code and the reference file, not from a run.

---

## F1 — CODE, BLOCKING (`babea62289b`)

**The ambiguous-catalog case now aborts the whole audit instead of being reported, contradicting this
commit's own three statements that it is a `lifeless_keys` finding.**

In `runFsckImpl`'s unscoped physical scan, the new post-listing classification loop calls
`post_listing_cut.life_index.resolve(candidate.life_id)` with no handler.
`CatalogLifeIndex::resolve` **throws** `CORRUPTED_DATA` when the queried life id is shared by two
current rows (see `CatalogLifeIndex::resolve` in `CasRefProtocol.cpp`). `runFsck`'s only catch
converts `TIMEOUT_EXCEEDED`; everything else propagates. So an ambiguous post-listing cut destroys
the entire report.

This is a regression introduced by this commit. In the pre-image the same `resolve` call sat **inside**
the `catch (const Exception & e) { if (e.code() != CORRUPTED_DATA) throw; recordLifelessKeys(...); }`
block, so ambiguity was recorded as a hard finding and the scan continued. The commit moved the call
out of that block.

Three surfaces in this same commit assert the behaviour the code no longer has:

- `FsckClass::LifelessKey`'s new doc: "*OR a catalog incarnation that is ambiguous or otherwise
  unreadable*";
- `FsckReport::lifeless_keys`'s new doc: "*OR a catalog incarnation that is ambiguous or unreadable*";
- `CommandFsck`'s reworded exception: "*malformed or unresolvable … or their catalog incarnation is
  ambiguous/unreadable*".

It also contradicts the design comment a few lines above `recordLifelessKeys` ("*FINDING and not an
abort: an audit that died on the first bad key would report nothing about the healthy namespaces it
never reached*") and the consult's §2B ("*A catalog reverse-index ambiguity is a separate hard catalog
finding*").

**Untested.** `CasFsck.DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses` is the
only ambiguity test; its duplicated incarnation `UInt128{777}` has **no** physical object under
`cas/ns/`, so no candidate ever carries the ambiguous id into the new loop and the abort path is never
reached. That is why the suite is green.

Fix: wrap the `resolve` in the same `catch (const Exception &) { if code != CORRUPTED_DATA throw; recordLifelessKeys(...); continue; }`
shape the pre-image used, and add a case to
`DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses` (or a sibling) that writes a
physical `_ckpt`/`_files` object under the duplicated life id, asserting `lifeless_keys >= 1` and
`ASSERT_NO_THROW`.

## F2 — CODE/TEST, BLOCKING (`babea62289b`)

**`05020_content_addressed_fsck` will fail: the three new SQL columns were never added to the
reference.**

`tests/queries/0_stateless/05020_content_addressed_fsck.sh` runs
`SYSTEM CONTENT ADDRESSED FSCK '<disk>'` with `--format TSVWithNames`, so both the header row and the
value row land in the golden file. `contentAddressedFsckColumns` now declares **20** columns (I
counted; `appendContentAddressedFsckRow` inserts a matching 20), while
`05020_content_addressed_fsck.reference`'s header line still has **17** names and its data line 17
values. The reference is untouched across all three commits.

The `kFsckHardFindings` static_assert cannot catch this: it fires only when a *hard* finding is
added, and `namespace_janitor_pending` is deliberately soft. The CA gate is unit tests only, which is
why the report's green gate did not surface it.

Fix: regenerate the two golden lines (header gains `namespace_janitor_pending`,
`namespace_janitor_pending_bytes`, `namespace_janitor_pending_lives` after `lifeless_keys`; data row
gains three zeros in the same position).

## F3 — CODE, non-blocking (`53e7b4c8588`)

**The reworded arm-(a) message asserts a state that the count does not check — and its own regression
test exercises the counterexample.**

`victim_owned_count` is `std::count_if` over every `retirement_catalog_cut` entry matching
`victim_srid` or `victim_namespace_prefix`, in **any** `NsState`. The message then says all N
"*are marked for removal*". `CasDecommissionCatalogDuties.VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot`
admits its late entry with `.state = NsState::Live` and asserts on the string "*all 1 namespace(s)*" —
so the pinned case is precisely one where the claim is false. `NsState::Creating` is the other.

The retired message named the states it had evidence for ("*in Removing/Creating state*"); the
replacement drops that qualification while widening the claim. Operationally this can present a
live-or-creating namespace under a decommissioned member's prefix — a genuine anomaly — as routine
"decommission underway" progress. Fix: either drop the removal claim ("*N namespace(s) are still owned
by this member*") or count and name the states.

## F4 — TEST, non-blocking (`53e7b4c8588`)

**The integration lane's janitor-pending assertion is fail-open on an absent key.**

`assert "janitor_pending=0" not in fsck` passes both when the count is nonzero **and** when the field
is missing entirely (an older binary, a renamed field). This repo fails closed on exactly this
distinction elsewhere and says so: `stale_edge_verdict`'s `absent` verdict exists because "*a missing
key is not a zero, it is the absence of an answer*". The dispatch's own wording for this step is
`janitor_pending >= 1`.

Fix: parse the token (`re.search(r"\bjanitor_pending=(\d+)", fsck)`), fail if absent, then assert
`>= 1`. The `lifeless_keys=0` assertion on the same line is fine — it is a positive substring match,
so an absent key fails it.

## F5 — PROSE (report completeness), non-blocking (`53e7b4c8588`)

Plan T3 Step 2 requires a sensitivity check for the **new** arm-(b) test (arm (b)'s token/value
comparison commented out; mutation applied, output captured, reverted, mandatory wording). The draft
report prescribes it for the finisher. The closure report's mutation section covers only (i) the
`_ckpt` throw, (ii) the `SCOPE_EXIT` wake, and (iii) `victim_still_owned` — arm (a). The arm-(b)
demonstration was not performed, and its absence is not disclosed under "Deviations".

I verified the sensitivity analytically instead, and it holds: with arm (b)'s block removed there is
no second `get("p/cas/ref_catalog")`, so `backend->fired()` never becomes true and no warning is
produced (arm (a) computed count 0 at that cut) — `EXPECT_TRUE(backend->fired())`,
`EXPECT_FALSE(report.slot_removed)` and the message assertion all fail. The test is load-bearing; only
the recorded evidence is missing.

## F6 — CODE (message text), non-blocking (`babea62289b`)

`CommandFsck`'s new note tells the operator "*the perpetual namespace janitor deletes them on a later
page — expected, no action needed*". Under the current Stage-A posture the janitor's deletes are
suppressed, so the residue does **not** drain — the closure report proves it empirically (6 explicit GC
rounds, the count held at exactly 12). The note describes the designed steady state as if it were
today's behaviour. Defensible as forward-looking (and T6 flips the posture), which is why this is
non-blocking, but the sentence is currently untrue for the operator reading it.

---

## What I verified and found CORRECT

Recorded so a later reader does not re-derive it.

**Classifier soundness (consult §2).**

- **(A) complete writer grammar.** `isCleanRelativeNamespaceFileName` is used by both
  `Layout::namespaceFileKey` (writer) and `Layout::parseNamespaceFileKey` (reader); the writer's
  inline predicate was deleted, not duplicated. The four families under `cas/ns/` are exactly `_log`,
  `_snap` (`parseRefObjectKey`), `_ckpt` (`parseRefCkptKey`) and `_files/` (`parseNamespaceFileKey`) —
  enumerated from `CasLayout.h`'s key builders, so the classifier's parts do sum to the whole and the
  "unrecognized key" arm cannot swallow a healthy shape. Doubled slash, leading/trailing slash, `..`
  in any position, empty relative name, zero/uppercase/short life id, wrong `_ckpt` suffix and unknown
  kind directory all reach a hard verdict; I traced each through the parser by hand.
- **No remaining asymmetry in the touched pair.** (Out of scope, pre-existing: `mountpointObjectKey`
  keeps its own inline clean-path check that rejects empty/leading/trailing/doubled slash but **not**
  `..`, so that family's writer grammar is looser than the new helper. Untouched by this arc.)
- **(B) observe-then-cut.** Candidates are buffered in `canonical_candidates` during
  `forEachListedKey`, and the cut is `CasRefCatalog::read` **after** the callback loop returns.
  Malformed keys are classified inline, which cannot misclassify anything cut-dependent: a parser
  refusal or `CORRUPTED_DATA` is a property of the key alone. `resolve` returning a value for
  `Creating`/`Live`/`Removing` alike means all three protect (`CatalogLifeIndex`'s ctor indexes every
  entry regardless of state).
- **(C) no body decode.** Classification uses only the key and `ListedKey::size`.

**Hard findings preserved.** `kFsckHardFindings` still has its 5 rows with `lifeless_keys` among them;
the `static_assert(kFsckHardFindings.size() == 5, ...)` is intact and needed no bump;
`FsckReport::clean` is unchanged; `JanitorPending` is the only new enumerator and the only switch over
`FsckClass` (`CommandFsck`) handles it. The catalog-cut-**unreadable** case stays hard by propagation
(`CasRefCatalog::read` throws on an absent/undecodable catalog, as before). The catalog-cut-**ambiguous**
case is F1.

**Parser posture flip audited for wedges.** `parseNamespaceFileKey` now throws where it used to return
`nullopt`. Only two production call sites exist. `CasFsck` catches `CORRUPTED_DATA` and records.
`NamespaceJanitor::runOnePage` already wraps the parse in
`catch (const DB::Exception & e) { anomalies.push_back(...); continue; }` and does **not** clear
`page_decided`, so the cursor still advances — no wedge, no phase-lock. Behaviour change worth knowing:
a dirty `_files` key used to parse and could be exact-deleted as dead-life debris; it is now skipped
forever and needs an operator. That is the ruling's intent ("*malformed keys stay hard; the janitor
deliberately reports/skips unparseable keys*"), so not a finding.

**Soak harness.** `parse_fsck_summary` skips unknown tokens, and the checkpoint gate is
`exit_code` + `dangling` + `stale_edge_verdict`; the three new summary tokens neither break parsing
nor newly fail a soak checkpoint. No harness change was owed.

**New fsck tests are non-vacuous.**

- `CanonicalDeadLifeResidueIsJanitorPendingNotHardFinding` — a regression to the old hard
  classification fails three independent assertions (`lifeless_keys == 0`,
  `namespace_janitor_pending >= 1`, `clean()`).
- `LifeAdmittedBetweenNamespaceListingAndLaterCutIsNotResidue` — the injection really lands between
  the listing and the later cut. `cas/ns/` is listed in exactly two places in the tree (`CasFsck` and
  `NamespaceJanitor`), and the pool-open path lists neither, so the backend's
  `prefix.ends_with("/cas/ns/")` hook can only fire inside fsck's own physical listing, which is after
  the pre-listing `catalog_cut` and before `post_listing_cut`. Under either regression (classify
  against the older cut, old or new code shape) an assertion fails.
- `MalformedNamespaceTreeShapesStayHardFindings` covers the dirty `_files` name, zero id, uppercase id
  and unknown kind. Trailing-slash / doubled-slash / 31-33-digit shapes are **not** in this table; they
  are pinned at the parser level in `gtest_cas_namespace_life_id.cpp` and `gtest_cas_layout.cpp` and
  reach the identical fsck arm, so the four representatives are adequate. Neither commit claims the
  table is exhaustive.

**SQL surface.** Both `contentAddressedFsckColumns` (20 entries) and `appendContentAddressedFsckRow`
(20 inserts) place the three new columns immediately after `lifeless_keys`, in the same order. Only
the golden file is missing (F2).

**Arm-(b) injection seam (dispatch item 5) — the draft's ordering claim holds at this tree.**
`decommissionPoolMember`'s last `list` before the retirement tail is
`deleteListedPrefix(admin->backend(), layout.serverRootDataPrefix(victim_srid), ...)` =
`list("p/roots/victim/", "", ...)`, which is exactly the fixture's anchor. Between that anchor and
`retirement_catalog_cut` there is no catalog read; between `retirement_catalog_cut` and
`fresh_retirement_catalog` there is only `admin->layout()` / `admin->poolBackendPtr()`. And
`CasRefCatalog::read` issues **exactly one** `get(refCatalogKey())` (via
`readOptionalForBootstrap`), so "second catalog GET after the anchor" is `fresh_retirement_catalog`
and nothing else. `casAdmitEntry` re-enters `get`, but `added = true` is set before the call, so the
nested read cannot re-trigger. A wrong seam would have made the test fail (arm (a) would fire), not
pass falsely.

**Two-phase heal test (dispatch item 6).** Phase 1 asserts `namespaces_removed >= 1`,
`slot_removed == 0` and the arm-(a) substring. The GC-drive loop is bounded at 30 with a
`for/else: pytest.fail` (correct Python; `pytest` is imported). Phase 2's `slot_removed == 1` is
established by the `break` condition and followed by `assert fields[9] == ""`. The mounts-table check
moved after phase 2, correctly. The `STAGE-A CONTRACT` banner and its reclamation assertions are
untouched — T6's flip is not pre-empted.

**UX message plumbing (dispatch item 7).** The count is real, taken over `retirement_catalog_cut`'s
entries at the retirement re-scan. All three live consumers of the retired string are updated (one
gtest, two integration sites); arm (b)'s message is untouched. No task ids, plan references or finding
ids in any new comment or message — the comment policy holds across all three commits, including the
long new doc comments in `CasFsck.h`. (The `CasFsck.h` and `CasFsck.cpp` comments do cite
`docs/superpowers/specs/2026-07-27-cas-ref-chain-complete-cut-design.md`, a shipped spec rather than a
branch-local artefact — pre-existing style in this file.)

**Mutation records (dispatch item 8).** Mandatory wording present. Mutation (i)'s finding — that the
`_ckpt`-absence guard is *shadowed* and its deletion does not go red — is disclosed rather than
buried, and the claim checks out: `CasRefLedger::dropNamespace` reaches
`chooseRecoveryGrounding`, which throws the same `CORRUPTED_DATA` with the message
"*CAS recovery grounding: a Live or Removing namespace requires a readable _ckpt with life_epoch*"
(`CasRefCkpt.cpp`). The `-Wunreachable-code` reshaping from `if (false && ...)` to block deletion is
recorded. Mutations (ii) and (iii) name the tests that caught them.

**Report completeness (dispatch item 9).** Both STOP findings, their rulings, and the
suppression code-gate verification are recorded; the metrics half is explicitly recorded as not
obtained, with the one-attempt cap named. Only F5's omission is missing.

## PROSE, deferred to `docs/superpowers/cas/deferred-docs-fixes.md`

1. **FALSE (was true before this arc):** `docs/superpowers/cas/BACKLOG.md` (the entry ending
   "*Consequence: `ca-decommission` refuses fail-close and `ca-fsck` posts a hard `lifeless_keys`*")
   describes the exact behaviour `babea62289b` removed. `ca-decommission` never refused on these keys —
   the retired `CommandFsck` message said it did, and the consult calls that text stale.
2. **FALSE as a live instruction:** `docs/superpowers/plans/2026-08-02-cas-stage-b-remaining.md`'s T3
   Step 1 tells the implementer to assert arm (a)'s warning string
   "*catalog still owns victim namespaces*", which `53e7b4c8588` retired. The test correctly asserts
   the new string instead; the plan text now instructs the opposite of the landed code.
3. **IMPRECISE:** `docs/superpowers/cas/2026-08-02-stage-b-midpoint-audit.md` quotes the same retired
   string verbatim as the current arm-(a) warning. As a dated audit record this is defensible history,
   but it reads as a statement about the present.
4. `docs/superpowers/cas/08-testing-and-soak.md`'s exit-code list (`dangling`, `chain_broken`,
   `corrupted_runs`, `lifeless_keys`) is still **correct** after the split — no fix owed. Recorded so
   nobody "fixes" it.

---

# Re-verification of the fix round — `d7673bd9ede`

Scoped to F1/F2 (blocking) plus a spot-check of F3/F4/F6. Not a re-review of the arc.

## Verdict

**APPROVE** — fsck slice (`babea62289b`) + fix round (`d7673bd9ede`) as a unit. Both blocking
findings are closed, and the three non-blocking fixes match the findings' intent. No new findings.

## My own numbers, at `d7673bd9ede`

Rebuilt and reran both binaries in `lane-g` under
`flock "$(git rev-parse --git-common-dir)/unit_tests.lock"`, same filter as before:

- release: `build/t3rev2_build_release.log` `NINJA_EXIT=0`; `build/t3rev2_tests_release.log`
  **145/145 passed**, `TESTS_EXIT=0`
- ASan: `build_asan/t3rev2_build_asan.log` `NINJA_EXIT=0`; `build_asan/t3rev2_tests_asan.log`
  **145/145 passed**, `TESTS_EXIT=0`

145, up one from my 144 at the previous tip, and
`CasFsck.AmbiguousLifeUnderAPhysicalKeyIsRecordedNotAborted` appears `[ OK ]` by name in both. I ran
ASan as well even though the dispatch scoped it to release, because the fix lands on an
exception-handling path.

Provenance note on the round's own logs: `build/t3fix_run_release.log` (12:58, 145/145) and
`build_asan/t3fix_run_asan.log` (12:59, 145/145) both predate the commit timestamp (13:03), i.e. they
were run from the working tree before it was committed — legitimate. I checked for the
relink-inside-the-window trap: my 13:05 rebuild reported 19 steps, but reading them they are all
cargo utility re-runs (`_cargo-build_*`, symbol localizing, stripping) plus the final
`Linking CXX executable src/unit_tests_dbms` — **no C++ translation unit was recompiled**, so their
binary and mine were built from identical objects.

## F1 — CLOSED

The `catch (const Exception & e) { if (e.code() != ErrorCodes::CORRUPTED_DATA) throw; recordLifelessKeys({{candidate.key, e.message()}}); continue; }`
shape is restored around `post_listing_cut.life_index.resolve(candidate.life_id)`, which is exactly
the pre-image's shape and what the class's own doc comments describe. The recorded key is the
candidate's physical key with `resolve`'s own message; duplicates across several candidates sharing
one ambiguous life collapse through the existing `lifeless_seen` set, so the counter stays a count of
defects.

**Red-first log validated, not relayed.** `build/t3fix_red_run.log` (12:54, before the 13:03 commit)
runs the single new test and ends `[ FAILED ] 1 test` / `1 FAILED TEST`, with gtest's
`Actual: it throws DB::Exception with description "CAS ref catalog: life_id …309 is shared by current
namespaces 'bad/a' and 'bad/b' -- both rows are unresolvable"` and a stack frame at
`gtest_cas_fsck.cpp:647`. Line 647 in the committed file is the `ASSERT_NO_THROW(report = runFsck(...))`
call, and `…309` is `0x309 = 777`, the duplicated incarnation the fixture installs. That is the F1
abort itself, not a proxy for it.

**The new test drives a real candidate into the loop.** It writes
`namespaceFilesPrefix(NamespaceLifeId::fromCatalogEntry(RootNamespace{"bad/a"}, UInt128{777})) + "format_version.txt"`
— a canonical `_files` key with a nonzero life id, so it parses and enters `canonical_candidates` —
and only then makes `777` ambiguous by appending `bad/a` (`Live`) and `bad/b` (`Removing`). That is
precisely the gap I named: `DuplicateLifeIdIsReportedWhileAnUnrelatedUniqueNamespaceStillProgresses`
has no physical object under its duplicated id.

Non-blocking strengthening suggestion (not a finding): `EXPECT_GE(report.lifeless_keys, 1u)` is also
satisfied by the pre-existing `walk_lives` row (keyed `refCatalogKey()#<hex>`), so `ASSERT_NO_THROW`
is the sole assertion that discriminates the regression. The red log proves it fires, so the test is
sound; asserting a detail row whose `key` equals the physical key would additionally pin the new
code path.

## F2 — CLOSED

The golden's header and data lines are both 20 fields (`awk -F'\t'` on lines 3 and 4). I diffed the
header against the column names extracted in declaration order from `contentAddressedFsckColumns` and
they are **byte-identical**, including the position immediately after `lifeless_keys` — so this is not
"three names appended somewhere", it matches the emission order.

`build/t3fix_stateless_05020.log` shows
`[1 / 1] 05020_content_addressed_fsck: [ OK ] 1.40 sec` / `1 tests passed. 0 tests skipped.` under
`--options "amd_binary, content_addressed storage, parallel"`. Provenance: the log's harness lines
resolve the binary through `ci/tmp/clickhouse`, a symlink to `lane-g/build/programs/clickhouse`,
relinked 12:59:38 — after the slice that added the columns (12:34) and before the run (log mtime
13:02). And the result is self-validating: a stale 17-column server against the new 20-column golden
would have failed, so the pass proves binary and golden agree at 20. I did not rerun it (MAIN owns
the praktika slot) and did not need to.

## F3 / F4 / F6 — match intent

- **F3.** Now "*pool member decommission underway: N namespace(s) are still owned by this member; …*".
  The universal "marked for removal" claim is gone; what remains is exactly what `count_if` over
  `retirement_catalog_cut` establishes, for every `NsState`. The one gtest pinning the old wording was
  updated to "*underway: 1 namespace(s)*", and the integration lane asserts only the stable
  "*pool member decommission underway*" substring, so no consumer is left behind.
- **F4.** `re.search(r"\bjanitor_pending=(\d+)", fsck)` with an explicit assert that the match exists
  before `>= 1` — fail-closed on absence, which was the point. The pattern cannot be satisfied by
  `janitor_pending_bytes=` (the literal `=` follows immediately) and the `\b` matches the
  space-delimited `janitor_pending=` token that `formatFsckSummary` emits. `import re` added.
- **F6.** The note now names the janitor as "*the sole intended reclaimer*" whose "*deletes can be
  deferred (e.g. a destructive-round suppression policy)*", with an investigate-only-if-persistent
  qualifier and no timeline promise. It states the ownership fact without asserting today's drain.

## Housekeeping verified

`t3-report.md` records the fix round with all six findings, its own red-first evidence and the
stateless run. F5 and my four PROSE items are batched into
`docs/superpowers/cas/deferred-docs-fixes.md` as `{#d42-t3-review-prose}`, including the
"NO FIX OWED" entry for `08-testing-and-soak.md`. No fix round was opened for prose, per policy.

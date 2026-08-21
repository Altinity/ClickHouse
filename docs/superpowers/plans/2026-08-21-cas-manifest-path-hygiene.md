---
description: 'Implementation plan for rejecting undecodable manifest paths at encode time and stopping one poison object from wedging garbage collection pool-wide'
sidebar_label: 'CAS manifest path hygiene'
sidebar_position: 1
slug: /superpowers/plans/cas-manifest-path-hygiene
title: 'Manifest entry-path hygiene and a non-wedging orphan sweep — implementation plan'
doc_type: 'guide'
---

# Manifest entry-path hygiene and a non-wedging orphan sweep — Implementation Plan {#cas-manifest-path-hygiene-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop a single unlucky `CREATE TABLE` from permanently disabling reclamation for a whole content-addressed pool.

**Architecture:** Two independent halves, both needed. The encoder gains a control-character check, so a manifest that nothing can decode is never written — and the decoder's rule set is left exactly as it is, which is what keeps every manifest already in a pool readable. And the orphan sweep treats an undecodable manifest as a recorded anomaly it walks past, instead of letting the exception escape and abort every round forever.

**Tech Stack:** C++ (ClickHouse fork), gtest, one stateless `.sh` test, `SYSTEM CAS FSCK` for the "nothing was left behind" assertion.

**Spec anchor note:** this plan cites `{#manifest-entry-path-newline-banner}` in five places (the Spec line above, Task 3's file list, and Task 3 Steps 2, 3 and 4 — this note is not one of them). Task 3 deletes that anchor. The citations here are provenance for a plan that is itself history once executed, so they stay as they are — but do not add new ones, and do not "fix" them into links that will dangle.

**Spec:** `docs/superpowers/cas/BACKLOG/formats-and-storage.md` `{#manifest-entry-path-newline-banner}`, with the adjudication at `docs/superpowers/cas/2031-triage.md` `{#cas-040}`. CONFIRMED and reproduced live on HEAD. **Read the spec-delta section below before starting.**

## Spec delta — what this plan adds {#spec-delta}

The spec's mechanism and fix shape are correct. Four things it does not say, all verified against the tree, and the first is a deliberate narrowing of the spec's own wording:

1. **The check goes on the encoder ONLY, and covers control characters ONLY.** The spec reads as though one shared validator should serve both sides. It must not, for a reason that is invisible until you read how lines are parsed: `decodePartManifest` reads records with `readLine`, which splits on `\n` alone, and compares the payload-zone banner byte-wise. So a path containing `\t`, `\r`, or any other non-LF control byte **round-trips correctly today**, and such a path is reachable by the same ordinary DDL that produced the reported bug. Tightening the decoder would make manifests that are readable now unreadable after an upgrade, for no gain: an embedded `\n` can never reach the decoder as part of a path — it splits the line, and the decoder already refuses the result as malformed. Fixing the writer fixes the defect; touching the reader only breaks readers. **Do not extend the decode-side hygiene check, and do not lift it into a shared function.**
2. **Because of (1), no existing test changes.** `gtest_cas_part_manifest_format.cpp`'s `DecodeRejectsMalformedEntryPaths` encodes `"../evil"`, `"/abs"`, `""`, `"a//b"`, `"a/./b"` and then expects decode to throw; none of those is a control character, so the encoder still accepts them and the test still reaches its assertion. Its comment — that `encodePartManifest` does not itself reject *these* — stays true. `grep -P '\\[nrt0]|\\x0'` over that file returns nothing, so no other case in it is affected either. If any test in the format file does start failing, that is a finding: something about the character class is wider than intended.
3. **The error code comes from the sibling check in the same function, not from first principles.** `encodePartManifest` already rejects duplicate paths with `CORRUPTED_DATA`. Use `CORRUPTED_DATA` for the new check too. It is also in the deterministic-local-failure set the write controller propagates instantly without retry, so the failure stays fast and loud.
4. **The sweep cannot delete the poison object, only walk past it.** Deciding an orphan manifest is safe to delete requires reading its body to derive the source edges — exactly what fails. So half two retains the object, records it, and advances the cursor: one visibly-leaked object instead of a pool-wide wedge. `SYSTEM CAS FSCK` counts such a body as **`unreachable`** — `CasFsck.cpp`'s manifest-debris scan increments `report.unreachable` for any `cas/manifests/` body no committed ref owns, with no eligibility precondition on the increment. It is *not* `unaccounted`: that counter is produced only inside blob-object classification and would stay `0` here, which is exactly why an earlier revision of this plan had a stateless assertion that would have passed on the broken tree.
5. **Part (3) of the spec's fix list is out of scope here.** Escaping the projection directory name lives in generic MergeTree code (`ProjectionsDescription::getDirectoryName`) and the spec itself routes it through the upstream-consult step. Do not touch it. This plan makes the CAS layer refuse the input; it does not change what MergeTree names a directory.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **The worktree is shared.** Other sessions hold uncommitted work in this checkout. Before every commit, re-run `git status --short <the exact paths>` and stage only paths whose diff is yours. Never `git add -A`, `git add .`, or `git commit -a`.
- **No `LOGICAL_ERROR` anywhere in this work.** Both new failure paths are reachable from ordinary user DDL, so they are `CORRUPTED_DATA`. Before writing any `EXPECT_THROW`, check the error code at the site; if a site you touch throws `LOGICAL_ERROR` on an input-reachable condition, stop and report it rather than testing it.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- Every build and every test run goes to its own uniquely named log under `build/`, with an exit marker appended, and the status is read from the marker rather than from the shell.
- The test number `05026_cas_manifest_path_newline` is already reserved by `add-test`; both files exist as untracked stubs, the reference empty. Do not renumber, do not re-run `add-test`, do not `chmod`.

---

## Task 1: Reject an undecodable path at encode time {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp` — `encodePartManifest` only; **do not touch the decode-side hygiene block**
- Modify: `src/Disks/tests/gtest_cas_part_manifest_format.cpp` — add tests; no existing test needs repair
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.sh`
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.reference`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a file-local `rejectControlCharacters(std::string_view)` in the format translation unit, called from `encodePartManifest` only. No public signature changes, and no change to what the decoder accepts.

- [ ] **Step 1: Write the failing gtest**

Add beside the existing path tests. Both fail today because the encoder accepts the path and the round trip only breaks at decode.

The case vector must be `std::vector<String>`, not a `const char *` initialiser list. Two independent
reasons, and both silently produce a test that passes while checking nothing: `std::string(...).c_str()`
in a braced list hands out a pointer to a temporary that dies at the end of the full expression, and a
`const char *` converted to a `std::string_view` truncates at the first NUL — so the NUL case would
test the path `"nul"`, which contains no control character at all and would make the loop fail for the
wrong reason.

```cpp
/// A path is not just a name: `bannerFor` writes it verbatim into the payload-zone banner line, so a
/// control character inside it breaks the banner's own line framing and nothing can decode the result.
/// Rejecting at encode is what keeps such an object from ever being written.
TEST(CASPartManifestFormat, EncodeRejectsControlCharactersInEntryPath)
{
    const std::vector<String> paths{
        String("p\nq.proj/columns.txt"),
        String("a\rb.txt"),
        String("tab\there.txt"),
        String("nul\0byte.txt", 12),   /// length-explicit: a NUL must survive into the String
    };
    for (const String & path : paths)
    {
        SCOPED_TRACE(path);
        ASSERT_EQ(path.find('\0') != String::npos, path.starts_with("nul"))
            << "the NUL case must actually carry a NUL, or this loop checks a path with no control "
               "character in it and passes for the wrong reason";
        expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA, [&] { encodePartManifest(manifestWithSinglePath(path)); });
    }
}

/// The banner is `==> <path> il=<n> <==`; a newline in the path used to split it across lines. Pin the
/// specific case the reproducer used, so a future relaxation of the character set cannot let it back.
TEST(CASPartManifestFormat, EncodeRejectsTheProjectionNewlineReproducer)
{
    expectThrowsCode(DB::ErrorCodes::CORRUPTED_DATA,
                     [&] { encodePartManifest(manifestWithSinglePath("p\nq.proj/columns.txt")); });
}
```

`manifestWithSinglePath` takes the path by a type that must preserve a NUL — check its signature before
writing the loop, and if it takes `const char *`, add a `String` overload rather than dropping the case.

- [ ] **Step 2: Run it and confirm it fails for the right reason**

```bash
ninja -C build unit_tests_dbms > build/040_unit_build_1.log 2>&1; echo "EXIT=$?" >> build/040_unit_build_1.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_red.log 2>&1; echo "EXIT=$?" >> build/040_fmt_red.log
```

Expected: both new tests FAIL because `encodePartManifest` returns normally — no throw at all. A failure reporting a *different* thrown code means the encoder already rejects something and the case needs re-picking, not the fix.

- [ ] **Step 3: Add the encode-side check**

Add one file-local function to `CasPartManifestFormat.cpp` and call it from `encodePartManifest`. Do
**not** move, extend, or refactor the decode-side hygiene block — see spec-delta item 1 for why
touching the reader is the one change that would make this fix harmful.

```cpp
/// A path reaches the payload-zone banner verbatim, so a control character inside one breaks the
/// banner's line framing and produces an object nothing can ever decode -- including the writer
/// itself, one transaction later. Refusing it here means such an object is never written.
///
/// Encode side only. The decoder splits records on '\n' and compares the banner byte-wise, so every
/// other control byte round-trips correctly; rejecting them on read would make manifests that are
/// readable today unreadable after an upgrade, and would buy nothing -- an embedded '\n' cannot reach
/// a decoded path at all, because it splits the line and the record fails as malformed.
void rejectControlCharacters(std::string_view path)
{
    for (const char c : path)
    {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b == 0x7f)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS part manifest: entry path contains a control character, which cannot be carried "
                "in the payload-zone banner: '{}'", path);
    }
}
```

Call it in `encodePartManifest` beside the existing duplicate-path rejection, over every entry, before
any byte is written — the manifest is refused whole rather than half-emitted. Both checks run off the
same sorted entry vector, so put the new loop next to the duplicate scan rather than in a third pass.

- [ ] **Step 4: Confirm the new tests pass and nothing else moved**

```bash
ninja -C build unit_tests_dbms > build/040_unit_build_2.log 2>&1; echo "EXIT=$?" >> build/040_unit_build_2.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_green.log 2>&1; echo "EXIT=$?" >> build/040_fmt_green.log
```

Expected: the two new tests pass and **every pre-existing test in that filter still passes unchanged** —
in particular `DecodeRejectsMalformedEntryPaths`, which reaches the decoder by encoding traversal paths
that are not control characters. If it now fails, the character class is wider than intended: report it,
do not adapt the test.

- [ ] **Step 5: Write the stateless test — the user-visible half**

The gtests prove the format refuses the path. This proves the improvement a user can see: the failure is
loud, and **nothing is left behind**. That second half is the whole point of fixing the encode side, and
it is what a message-matching test would miss.

The fsck read is not invented here — copy the idiom from `tests/queries/0_stateless/04290_cas_no_leftovers.sh`,
which already asserts exactly this pair of counters. Two details in it are load-bearing: the statement is
`SYSTEM CAS FSCK '<disk>'` (there is no `ON DISK` in the grammar — `ParserSystemQuery` parses the disk as a
plain target), and the `awk` indexes columns **by name** out of the `TSVWithNames` header, so a column added
to the summary later cannot silently move the one being read.

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# A projection name is used verbatim as a part-relative directory, and a part-relative path is written
# verbatim into the manifest's payload-zone banner. A control character in one therefore produced an
# object that nothing could decode -- not even the writer, one transaction later.
#
# The INSERT failed before this fix too, so a test that only checks "the INSERT fails" would have
# passed on the broken tree. What changed is WHEN it fails: before any object is written, so the pool
# is left clean. That is what the fsck counters assert here.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="05026_cas_newline"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_newline_proj;"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE t_cas_newline_proj (k UInt32, v String, PROJECTION \`p
q\` (SELECT k ORDER BY k))
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05026',
    name = '${DISK_NAME}',
    path = '05026_cas_newline_pool/');"

# The INSERT must fail, and it must name the control character rather than a banner mismatch --
# a banner mismatch would mean the object was written and read back, which is the old behaviour.
echo 'insert_error'
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_newline_proj VALUES (1, 'a');" 2>&1 \
    | grep -c 'control character'

# Nothing was written, so the pool holds no manifest body without a committed owner. On the broken tree
# the failed INSERT left one behind, and that object wedged every later collection round.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${DISK_NAME}'" --format TSVWithNames \
    | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                  { print "unreachable", $col["unreachable"]; print "dangling", $col["dangling"] }'

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_newline_proj;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Reference file:

```
insert_error
1
unreachable	0
dangling	0
dropped_ok
```

Match the reference to what the `awk` above actually prints — one line per counter, name and value
separated by a space or a tab depending on how you print it. Generate it by running the test and reading
the output, then check the values are the ones written here rather than whatever the run produced.

- [ ] **Step 6: Prove the stateless test fails on the pre-fix binary**

This step is mandatory and it is not a formality: the previous revision of this plan asserted a counter
(`unaccounted`) that stays `0` on the broken tree, so its stateless test would have passed against the
defect it was written for. A gtest failing-first run does not cover this — the gtest and the stateless
test have different oracles.

```bash
git stash push src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp
ninja -C build clickhouse > build/040_prefix_build.log 2>&1; echo "EXIT=$?" >> build/040_prefix_build.log
./tests/clickhouse-test 05026_cas_manifest_path_newline > build/040_stateless_red.log 2>&1; echo "EXIT=$?" >> build/040_stateless_red.log
git stash pop
```

Expected: FAIL, and the diff must show a **non-zero `unreachable`** — that is the leftover object, and it
is the only part of this test that distinguishes fixed from broken. If `unreachable` is `0` on the pre-fix
binary, the oracle does not discriminate: stop, report it, and do not proceed by weakening the assertion.
The `insert_error` line failing to match instead means only that the message changed, which proves nothing.

- [ ] **Step 7: Build the fixed binary, run the whole gate, read each marker**

```bash
ninja -C build clickhouse > build/040_clickhouse_build.log 2>&1; echo "EXIT=$?" >> build/040_clickhouse_build.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/040_gate_1.log 2>&1;   echo "EXIT=$?" >> build/040_gate_1.log
./tests/clickhouse-test 05026_cas_manifest_path_newline > build/040_stateless.log 2>&1; echo "EXIT=$?" >> build/040_stateless.log
```

Expected: the stateless test passes all four assertions; the full gate is unchanged from before the task. **Any `CAS*` test that fails here is a finding, not noise** — the encoder just became stricter for every caller, and a test that was relying on a permissive encoder is exactly what this step is for.

- [ ] **Step 8: Commit**

```bash
git status --short src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp \
                   src/Disks/tests/gtest_cas_part_manifest_format.cpp \
                   tests/queries/0_stateless/05026_cas_manifest_path_newline.sh \
                   tests/queries/0_stateless/05026_cas_manifest_path_newline.reference
git add <only the paths whose diff is yours>
git commit -m 'Refuse a manifest entry path that cannot survive the payload-zone banner'
```

---

## Task 2: Stop one undecodable manifest from wedging every round {#task-2}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h` (one counter, ~line 121)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp` (the unguarded decode at line 878)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp` (~line 1272 — the phase-metric block; a counter with no metric line is invisible on the round row)
- Modify: `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp` — the sweep's own test file; add there rather than starting a new one
- Read first: that file's `OrphanFixture` (line 47). **Not** `src/Disks/tests/cas_sweep_test_support.h`: that header holds only the `sweepManifestCursorPageForTest` wrapper and builds no pool, no catalog, and no watermark

**Interfaces:**
- Consumes: nothing from Task 1. **This half must work on a pool that already contains a poison object**, which is precisely the case Task 1 can no longer create — so it is tested by planting bytes at the pool level, not through DDL.
- Produces: `ManifestSweepResult::undecodable`, a `uint64_t` beside `listed`/`deleted`/`skipped`.

**Why not a `retained_*` counter.** The `retained_*` family is documented in the header as "the §6
premise's share of `skipped`, by reason class", and each member has a `SweepRetainClass` enum entry that
`topRetainReason` ranks and `reportSweepRetention` narrates. An undecodable body was never judged by the
deletion premise at all — it failed before the premise could be evaluated — so filing it as a retain
reason would corrupt what "the top retain reason" means. It belongs with `skipped`, whose own contract
already reads "counts malformed, protected, ineligible, budget-exhausted, or race-spared keys". So:
increment **both** `skipped` (the contract requires it) and the new `undecodable` (the reason), add **no**
`SweepRetainClass` member, and leave `topRetainReason` and `reportSweepRetention` untouched.

Task 1 stops new occurrences; this unwedges a pool that already has one. Neither substitutes for the other: a pool wedged today stays wedged after Task 1 alone, because the object is already there.

- [ ] **Step 1: Write the failing gtest**

Copy `OrphanFixture` (line 47) as the starting point rather than reaching for the support header. Its
setup is what makes the poison object *reachable by the decode at all*, and every piece of it is
load-bearing: `openPoolForTest`, then `casAdmitRecoverableEntry` for the recovery frontier, then the
manifest body, then `setWatermarkMinActive(..., /*min_active*/ 6)` so the build prefix is ELIGIBLE. On top
of that, seed the fold cursor into the **next** epoch — `seedFoldCursorForTest(..., RefTxnId{kBuildEpoch + 1, 1})`,
the value `AConsumedEpochSealWithACleanTailDeletes` uses — so the premise admits the deletion and the code
path actually reaches the decode. Without the watermark and the consumed seal, the object is retained by an
earlier branch, the decode never runs, and the test passes on the broken tree while proving nothing.

Plant the poison body **in place of** the fixture's `writeManifestRaw` line, not on top of it. This is why
the instruction is to copy `OrphanFixture` into a local variant rather than instantiate the shared one: the
backend's only write verb here is `putIfAbsent`, which on an existing key changes nothing and returns
`PreconditionFailed`. Planting poison over the fixture's legal body would therefore be a silent no-op, and
the test would pass while exercising a manifest that decodes perfectly well. Delete that one line from your
copy and put the plant where it was.

`writeManifestRaw` encodes, so it cannot be used for the poison body: write the bytes directly, and note the
**order** — the seal wraps the payload, so corrupt the encoded manifest first and seal afterwards. Sealing
first and corrupting after would fail the seal check, which is a different code path than the one being fixed.

```cpp
    /// A payload-zone banner that no longer matches its entry path: the exact shape the reproducer
    /// produced, reached without the encoder, which refuses to make one after Task 1.
    PartManifest good;
    good.ref = f.orphan;
    good.root_namespace_id = f.ns;
    good.entries = {<one INLINE entry, path "a.txt", so a banner line exists>};
    good.payload_digest = computePayloadDigest(good);
    String bytes = encodePartManifest(good);
    const size_t at = bytes.find("==> a.txt");
    ASSERT_NE(at, String::npos) << "no banner line to corrupt -- the entry must be Inline, not Blob";
    bytes[at + 4] = 'X';   /// same length, so no other offset shifts
    const PutResult put = f.backend->putIfAbsent(f.orphanKey(), sealObject(FormatId::PartManifest, bytes));
    /// `putIfAbsent` over an existing key is a no-op, and a silently-legal body would make every
    /// assertion below pass against the wrong object.
    ASSERT_EQ(classifyPutOutcome(put), PutClass::Created) << "the poison body was not the one planted";
```

Check the name of the outcome classifier beside `classifyDeleteOutcome` before writing that assertion — what
matters is that the plant is *verified*, not which helper verifies it. A bare `head(...).exists` is not
enough: the fixture's own body would satisfy it.

Plant a **second, legal, deletable** orphan at a key that sorts *after* the poison key, and give the page
a list budget large enough for both. This is what turns "did not throw" into "walked past":

```cpp
    const auto legal = writeManifestRaw(*f.backend, f.store->layout(), f.ns, ref(5, 0xCD),
                                        {blobEntryFor("b", DB::UInt128(2))});

    ManifestSweepResult result;
    ASSERT_NO_THROW(result = sweepManifestCursorPageForTest(*f.store, "", /*list_budget*/ 8, /*delete_budget*/ 8));

    EXPECT_EQ(result.undecodable, 1u) << "the anomaly must be recorded, not silently swallowed";
    EXPECT_GE(result.skipped, 1u) << "a key the sweep declined to nominate counts as skipped";
    EXPECT_TRUE(f.orphanExists()) << "an undecodable body is retained, never deleted on a guess";
    /// The page reached the end of the keyspace, so the cursor did not stall on the poison key. Assert
    /// `wrapped` rather than a moved `next_cursor`: `InMemoryBackend` leaves `next_cursor` EMPTY when no
    /// keys remain, so a moved-cursor assertion fails after a correct fix, not before it.
    EXPECT_TRUE(result.wrapped);
    /// And the strong form: the object BEYOND the poison key was still decided this page.
    EXPECT_FALSE(f.backend->head(f.store->layout().manifestKey(legal)).exists)
        << "the sweep stopped at the poison key instead of walking past it";
```

Check `ref(...)`'s ordering before relying on "sorts after": if `0xCD` does not sort after `0xAB` in the
manifest key encoding, swap the two so the legal object is genuinely beyond the poison one, and say in a
comment which is which. An assertion that depends on key order must state the order it depends on.

- [ ] **Step 2: Run it and confirm the failure is the wedge**

```bash
ninja -C build unit_tests_dbms > build/040_sweep_build_1.log 2>&1; echo "EXIT=$?" >> build/040_sweep_build_1.log
build/src/unit_tests_dbms --gtest_filter='CASSweepDeletionPremise*' > build/040_sweep_red.log 2>&1; echo "EXIT=$?" >> build/040_sweep_red.log
```

Expected: the test fails on the `ASSERT_NO_THROW` with the banner-mismatch `CORRUPTED_DATA` escaping the sweep. Two other failures mean the test, not the sweep, is wrong, and neither may be papered over:

- it fails on `result.undecodable` with **no throw** — the planted bytes decoded, so fix the plant;
- it fails on `EXPECT_FALSE(...legal...exists)` with no throw — the page never reached either object, so the fixture is not admitting the deletion and the watermark or the fold cursor is wrong.

- [ ] **Step 3: Make the decode non-fatal to the round**

Wrap the decode at line 878 — and only the decode. The branch mirrors the `if (!got)` branch a few lines
above it exactly, which is where `++result.skipped` and the `decided_through` advance come from; that
advance is the mechanism that moves the cursor past the key.

```cpp
        std::optional<PartManifest> body;
        try
        {
            body = decodePartManifest(openObject(FormatId::PartManifest, got->bytes));
        }
        catch (const Exception &)
        {
            /// A body we cannot decode cannot be shown safe to delete: proving that needs the source
            /// edges this decode would have produced. So retain it, record it, and walk on. The object
            /// stays visible to fsck, which counts it as unreachable; the alternative -- letting this
            /// escape -- aborts the round and every later round on the same object, which stops
            /// reclamation for the whole pool rather than for this one key.
            LOG_ERROR(getLogger("CasOrphanManifestSweep"),
                "CAS orphan sweep: manifest at {} cannot be decoded and was retained; run cas-fsck to "
                "enumerate such objects", parsed->key);
            ++result.undecodable;
            ++result.skipped;
            decided_through = listed.key;
            continue;
        }
```

There is no `log` in scope in this translation unit — every log statement in it names its logger inline
as `getLogger("CasOrphanManifestSweep")`. Follow that.

Making `body` an optional changes its three uses below, all inside the same loop iteration, and the
compiler will not catch a missed one if you leave the old declaration in place — so change all three in
the same edit: `refMatchesBody(id.ref, *body)`, `manifestNamespaceMatches(id.root_namespace, *body)`, and
`for (const ManifestEntry & entry : body->entries)`.

Keep the identity-mismatch throw immediately below as it is: it fires on a manifest that decoded fine but
names something else, which is a different and genuinely unexpected state — do not widen the catch over it.

Add the counter next to `skipped` in the header, with a comment saying that a non-zero value is an operator
signal rather than a transient, and that the body is retained rather than deleted.

- [ ] **Step 4: Give the counter a surface on the round row**

`CasGc.cpp` renders the sweep phase's numbers as `t.metric(...)` lines around line 1272 — `listed`,
`deleted`, `skipped`, then the four `retained_*`. Add one line beside them:

```cpp
        t.metric("undecodable", sweep.undecodable);
```

That is the whole surface. Do **not** add a `SweepRetainClass` member or touch `reportSweepRetention` or
`topRetainReason` — see the Interfaces note above for why an undecodable body is not a retain reason. The
`LOG_ERROR` and fsck's `unreachable` count are the operator's other two paths to the same object.

- [ ] **Step 5: Build, run, read the markers**

```bash
ninja -C build unit_tests_dbms > build/040_sweep_build_2.log 2>&1; echo "EXIT=$?" >> build/040_sweep_build_2.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/040_gate_2.log 2>&1; echo "EXIT=$?" >> build/040_gate_2.log
```

Expected: the new sweep test passes; the gate is otherwise unchanged. A GC test that now sees a different counter total is a finding — report it rather than adjusting the expectation. `skipped` now counts one more key in the poison case only, so a pre-existing test whose `skipped` total moved means something else changed too.

- [ ] **Step 6: Commit**

```bash
git status --short src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h \
                   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp \
                   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasGc.cpp \
                   src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp
git add <only the paths whose diff is yours>
git commit -m 'One undecodable manifest no longer stops reclamation for the whole pool'
```

---

## Task 3: Retire the entry and update the records {#task-3}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md` — **delete** the `{#manifest-entry-path-newline-banner}` section, only if the file is clean
- Modify: `docs/superpowers/cas/2031-triage.md` — five lines mention CAS-040 (`grep -n "CAS-040" docs/superpowers/cas/2031-triage.md`; read the whole output, do not trust this number)
- Modify: `docs/superpowers/cas/final-checks-todo.md` — item 8, which also links the dying anchor

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

The backlog files carry only open work — history lives in git — so a fixed entry is deleted rather than relabelled. The triage document is the opposite: it IS the history, so its section is historicised in place, never removed.

- [ ] **Step 1: Check whether each file is yours to touch**

Run `git status --short` on all three. Any file another session is holding: **stop and report it**, do not edit it, and do not relocate its content to get around it. The other files can still proceed.

- [ ] **Step 2: Delete the live-backlog entry**

Remove the whole `{#manifest-entry-path-newline-banner}` section from `BACKLOG/formats-and-storage.md`. Every link to that anchor must die in the same commit, which is why item 8 of `final-checks-todo.md` is in this task and not a later one.

- [ ] **Step 3: Historicise the triage**

`grep -n "CAS-040" docs/superpowers/cas/2031-triage.md` returns five lines when this plan was written. Re-run it untruncated and read every line rather than trusting that number — a sibling plan claimed four for its own issue because the grep behind it had been piped through `head`, and the site it missed was the largest. The five known sites map to the four edits below plus the closing note; if your grep returns a sixth line, it is a real site and it is yours to handle.

1. The verdict row, currently `подтверждено | P1`. Mark it fixed and name the commits. **Its link is already broken today** — it points at `BACKLOG.md#manifest-entry-path-newline-banner` while the section actually lives in `BACKLOG/formats-and-storage.md`. Repoint it at this plan rather than reproducing the mistake at a new address.
2. The `{#p1-list}` table — drop the CAS-040 row.
3. The paragraph after that table naming which P1s the triage found first. CAS-040 is called out there as the one where the triage added the reproduction and corrected the stated impact; that sentence has to survive the row's removal and still make sense.
4. The priority tally line. One fewer P1.
5. The full `{#cas-040}` section, whose heading still ends `(подтверждено, P1)`. Historicise it in place — do not delete it. The triage IS the history, which is the opposite of the live backlog's contract.

One of those five lines is a note near the end of the file correcting an unrelated one-liner's reference to CAS-040. Check whether it still reads correctly once the verdict changes; it may need nothing, and "needs nothing" is a finding to state rather than a step to skip silently.

- [ ] **Step 4: Correct the two remaining anchor references**

`grep -rn "manifest-entry-path-newline-banner" docs/ | cat` returns, besides the section itself: the triage verdict row from Step 3, a triage line announcing the section as newly created, and `final-checks-todo.md` item 8. The announcement line describes a moment in the triage's own history and can keep its wording, but its link dies with the anchor — point it at this plan or drop the link and keep the sentence.

- [ ] **Step 5: Close the scheduling item**

`final-checks-todo.md` item 8 is the pre-release line for this fix. Another session closed item 2 for a different issue and shows the format: the heading gains `DONE —` and the body names the commits.

- [ ] **Step 6: Commit**

Re-check dirtiness across all three files, stage only what is yours, then:

```bash
git commit -m 'Retire the manifest path-hygiene gap from the live backlog'
```

## Documentation checked and deliberately NOT changed {#docs-not-changed}

Recorded so it is not re-derived, and so a later reader knows the sweep happened:

- **`docs/en/antalya/cas/`** — nothing to change. This was a bug reachable only through a pathological identifier; no user-facing page promised or forbade it, and the roadmap's limitations list never mentioned it.
- **`ProjectionsDescription::getDirectoryName`** — the upstream half of the spec's fix list. Left alone deliberately; it is generic MergeTree code and the spec routes it through the upstream-consult step. Note in the Task 3 commit that the CAS layer now refuses the input while the upstream naming is unchanged, so a future consult starts from the right state.

## Self-review {#self-review}

**Spec coverage.** The spec's fix (1), encode-side validation, is Task 1 — narrowed to the control-character class on the writer only, with the reason in spec-delta item 1. Its fix (2), the non-wedging sweep, is Task 2. Its fix (3), upstream projection escaping, is explicitly out of scope and recorded as such. The test gap the spec names — that the format tests pin `\n` in inline BYTES but never in the entry PATH — is closed by Task 1 Step 1.

**Placeholders.** Three remain, all narrow and each flagged in place: the Inline entry's construction in Task 2 Step 1 (the helper is in `cas_test_helpers.h` beside `blobEntryFor`, and naming the wrong one here would be worse than a one-line look), the put-outcome classifier in the same step, and the reference file's exact whitespace in Task 1 Step 5, which is generated from a run and then checked. Every other step carries its actual content.

**Type consistency.** `rejectControlCharacters(std::string_view)` is file-local, void, throwing, called from `encodePartManifest` only. `undecodable` is a `uint64_t` on `ManifestSweepResult`, beside `skipped`, and is deliberately not a `SweepRetainClass`. The decode result becomes `std::optional<PartManifest>` at one call site, with three deref sites named.

**What an earlier revision got wrong, so it is not reintroduced.** Two defects, both of the same kind — an oracle that does not discriminate:

- the stateless test asserted `unaccounted = 0`. That counter is produced only by blob classification; an orphan manifest lands in `unreachable`. The assertion would have passed on the broken tree. Task 1 Step 6 now makes a pre-fix stateless run mandatory, because reading the counter's definition is what caught this and running the test is what would have caught it sooner.
- the poison plant went on top of the fixture's own legal body. `putIfAbsent` over an existing key changes nothing, so every assertion in that test would have run against a manifest that decodes fine. The plant now replaces the fixture's write and its outcome is asserted, because "the object exists" is satisfied by the wrong object.
- the sweep test asserted the cursor had moved via `EXPECT_NE(next_cursor, …)`. `InMemoryBackend` returns an empty `next_cursor` at the end of the keyspace, so on a single-object page that assertion fails *after* a correct fix. It is now `EXPECT_TRUE(wrapped)` plus a second object planted beyond the poison key — the second is the assertion that actually distinguishes walking past from stopping.

**The risk this plan cannot remove.** Task 1 makes the encoder stricter for every caller in the tree. `grep -P '\\[nrt0]|\\x0'` over the format test file returns nothing and no other test constructs a control-character path, but only a full gate run covers the callers that build paths from data rather than from literals. Task 1 Step 7 says such a failure is a finding rather than noise, because the tempting response — relaxing the new check until the suite goes quiet — would undo the fix.

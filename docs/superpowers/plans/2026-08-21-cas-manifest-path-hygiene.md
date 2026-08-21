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

**Architecture:** Two independent halves, both needed. The encoder gains the path-hygiene check the decoder already applies, extended to control characters, so a manifest that nothing can decode is never written. And the orphan sweep treats an undecodable manifest as a recorded anomaly it walks past, instead of letting the exception escape and abort every round forever.

**Tech Stack:** C++ (ClickHouse fork), gtest, one stateless `.sh` test, `SYSTEM CAS FSCK` for the "nothing was left behind" assertion.

**Spec:** `docs/superpowers/cas/BACKLOG/formats-and-storage.md` `{#manifest-entry-path-newline-banner}`, with the adjudication at `docs/superpowers/cas/2031-triage.md` `{#cas-040}`. CONFIRMED and reproduced live on HEAD. **Read the spec-delta section below before starting.**

## Spec delta — what this plan adds {#spec-delta}

The spec's mechanism and fix shape are correct. Four things it does not say, all verified against the tree, and the first is the one that will surprise an implementer mid-task:

1. **Half one breaks two existing tests, by construction, and that is the point.** `gtest_cas_part_manifest_format.cpp`'s `DecodeRejectsMalformedEntryPaths` **encodes** `"../evil"`, `"/abs"`, `""`, `"a//b"`, `"a/./b"` successfully and only then expects `decodePartManifest` to throw. Its preceding comment states outright that "`encodePartManifest` does not itself reject these … so each case must fail closed at decode time instead". Once the encoder rejects them, that test cannot even reach its assertion and the comment is false. Both must be repaired in the same task as the fix, and **the decode check must stay**: manifest bytes also arrive over the interserver relink channel, where a remote peer — not our encoder — chose the path.
2. **The error code comes from the sibling check in the same function, not from first principles.** `encodePartManifest` already rejects duplicate paths with `CORRUPTED_DATA`. Use `CORRUPTED_DATA` for the new check too. It is also in the deterministic-local-failure set the write controller propagates instantly without retry, so the failure stays fast and loud.
3. **The sweep cannot delete the poison object, only walk past it.** Deciding an orphan manifest is safe to delete requires reading its body to derive the source edges — exactly what fails. So half two retains the object, records it, and advances the cursor: one visibly-leaked object instead of a pool-wide wedge. `SYSTEM CAS FSCK` already surfaces such an object as `unaccounted`, so it does not become invisible.
4. **Part (3) of the spec's fix list is out of scope here.** Escaping the projection directory name lives in generic MergeTree code (`ProjectionsDescription::getDirectoryName`) and the spec itself routes it through the upstream-consult step. Do not touch it. This plan makes the CAS layer refuse the input; it does not change what MergeTree names a directory.

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
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp`
- Modify: `src/Disks/tests/gtest_cas_part_manifest_format.cpp`
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.sh`
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.reference`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: a private validator in the format translation unit, used by both `encodePartManifest` and the existing decode-side check. No public signature changes.

- [ ] **Step 1: Write the failing gtest**

Add beside the existing path tests. It fails today because the encoder accepts the path and the round trip only breaks at decode:

```cpp
/// A path is not just a name: `bannerFor` writes it verbatim into the payload-zone banner line, so a
/// control character inside it breaks the banner's own line framing and nothing can decode the result.
/// Rejecting at encode is what keeps such an object from ever being written.
TEST(CASPartManifestFormat, EncodeRejectsControlCharactersInEntryPath)
{
    for (const char * path : {"p\nq.proj/columns.txt", "a\rb.txt", "tab\there.txt", std::string("nul\0byte.txt", 12).c_str()})
    {
        SCOPED_TRACE(path);
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

If `std::string("nul\0byte.txt", 12).c_str()` reads awkwardly in the loop, hoist the four cases into a `std::vector<String>` instead — a NUL inside a `const char *` literal loop is the one case that needs care, and it is worth covering because the check is about control characters generally, not about newlines specifically.

- [ ] **Step 2: Run it and confirm it fails for the right reason**

```bash
ninja -C build unit_tests_dbms > build/040_unit_build_1.log 2>&1; echo "EXIT=$?" >> build/040_unit_build_1.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_red.log 2>&1; echo "EXIT=$?" >> build/040_fmt_red.log
```

Expected: both new tests FAIL because `encodePartManifest` returns normally — no throw at all. A failure reporting a *different* thrown code means the encoder already rejects something and the case needs re-picking, not the fix.

- [ ] **Step 3: Extract the path validator and call it from both sides**

In `CasPartManifestFormat.cpp`, lift the decode-side hygiene check into one file-local function and extend it with a control-character rule. The existing decode check enforces: non-empty, no leading `/`, and no empty, `.` or `..` segment. Add: no byte below `0x20` and no `0x7f`.

```cpp
/// One rule, two callers. The decoder needs it because manifest bytes also arrive over the
/// interserver relink channel, where a remote peer chose the path; the encoder needs it because a
/// path reaches the payload-zone banner verbatim, so a control character inside one breaks the
/// banner's line framing and produces an object nothing can ever decode -- including the writer
/// itself, one transaction later. Rejecting here means such an object is never written.
void validateEntryPath(std::string_view path)
{
    if (path.empty() || path.front() == '/')
        throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS part manifest: invalid entry path '{}'", path);
    for (const char c : path)
    {
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) == 0x7f)
            throw Exception(ErrorCodes::CORRUPTED_DATA,
                "CAS part manifest: entry path contains a control character, which cannot be carried "
                "in the payload-zone banner: '{}'", path);
    }
    for (std::string_view rest = path; !rest.empty();)
    {
        const size_t slash = rest.find('/');
        const std::string_view seg = rest.substr(0, slash);
        if (seg.empty() || seg == "." || seg == "..")
            throw Exception(ErrorCodes::CORRUPTED_DATA, "CAS part manifest: invalid entry path '{}'", path);
        rest = (slash == std::string_view::npos) ? std::string_view{} : rest.substr(slash + 1);
    }
}
```

Call it from `encodePartManifest` alongside the existing duplicate-path rejection — validate every entry before any byte is written, so the manifest is refused whole rather than half-emitted. Replace the inline block in the decoder with a call to the same function, so the two can never drift.

- [ ] **Step 4: Repair the two tests the change breaks**

`DecodeRejectsMalformedEntryPaths` currently reaches decode by encoding a malformed path. It cannot any more. Split it in two, and keep both:

- `EncodeRejectsMalformedEntryPaths` — the same five cases, now asserting `encodePartManifest` throws.
- `DecodeRejectsMalformedEntryPaths` — the same five cases, reaching the decoder through **hand-forged bytes** rather than the encoder. The file already establishes the technique next to `DecodeRejectsOutOfOrderEntries`: this is a text format and lines carry no per-line checksum, so a record line can be edited in place. Encode a legal path of the same byte length and substitute the malformed one, keeping the length identical so no other offset shifts.

Then correct the comment above them. It currently says `encodePartManifest` does not reject these, so each case must fail closed at decode instead. Both halves now check, and the reason they both must is worth stating: the encoder guards what this server writes, the decoder guards what a peer sends.

- [ ] **Step 5: Write the stateless test — the user-visible half**

The gtests prove the format refuses the path. This proves the improvement a user can see: the failure is loud, and **nothing is left behind**. That second half is the whole point of fixing the encode side, and it is what a message-matching test would miss.

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
# is left clean. That is what fsck's unaccounted count asserts here.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS t_cas_newline_proj;"

${CLICKHOUSE_CLIENT} --multiquery --query "
CREATE TABLE t_cas_newline_proj (k UInt32, v String, PROJECTION \`p
q\` (SELECT k ORDER BY k))
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05026',
    name = '05026_cas_newline',
    path = '05026_cas_newline_pool/');"

# The INSERT must fail, and it must name the control character rather than a banner mismatch --
# a banner mismatch would mean the object was written and read back, which is the old behaviour.
echo 'insert_error'
${CLICKHOUSE_CLIENT} --query "INSERT INTO t_cas_newline_proj VALUES (1, 'a');" 2>&1 \
    | grep -c 'control character' 

# Nothing was written, so the pool has no unaccounted objects. On the broken tree this was 1 or more,
# and that object wedged every later collection round.
echo 'unaccounted'
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK ON DISK '05026_cas_newline' FORMAT TSV;" \
    | ${CLICKHOUSE_LOCAL} --input-format TSV --structure "$(${CLICKHOUSE_CLIENT} --query "SELECT 'x'" >/dev/null; echo 'dummy String')" --query "SELECT 1" >/dev/null 2>&1 || true
${CLICKHOUSE_CLIENT} --query "SELECT unaccounted FROM (SELECT * FROM viewIfPermitted(SELECT 1 ELSE null('unaccounted UInt64'))) LIMIT 0;" >/dev/null 2>&1 || true

${CLICKHOUSE_CLIENT} --query "DROP TABLE t_cas_newline_proj;"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

**The fsck read above is deliberately left unfinished, and you must finish it — do not ship it as written.** `SYSTEM CAS FSCK ON DISK <name>` returns a one-row summary whose columns include `unaccounted UInt64`; find how another test in the tree consumes a `SYSTEM`-statement result set (the freeze tests pipe through `clickhouse-local` with an explicit `--structure`) and read just that column. Two constraints on how you do it: the assertion must be on `unaccounted` being `0`, and it must not depend on the column ORDER of that summary, because a new column added later must not silently move the one being read.

Reference file:

```
insert_error
1
unaccounted
0
dropped_ok
```

- [ ] **Step 6: Build both binaries, run everything, read each marker**

```bash
ninja -C build clickhouse      > build/040_clickhouse_build.log 2>&1; echo "EXIT=$?" >> build/040_clickhouse_build.log
ninja -C build unit_tests_dbms > build/040_unit_build_2.log 2>&1;    echo "EXIT=$?" >> build/040_unit_build_2.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_green.log 2>&1; echo "EXIT=$?" >> build/040_fmt_green.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/040_gate_1.log 2>&1;             echo "EXIT=$?" >> build/040_gate_1.log
./tests/clickhouse-test 05026_cas_manifest_path_newline > build/040_stateless.log 2>&1;           echo "EXIT=$?" >> build/040_stateless.log
```

Expected: the new format tests pass; the split decode/encode pair passes; the stateless test passes both assertions; the full gate is otherwise unchanged. **Any other `CAS*` test that fails here is a finding, not noise** — the encoder just became stricter for every caller, and a test that was relying on a permissive encoder is exactly what this step is for.

- [ ] **Step 7: Commit**

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
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h` (one counter)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp` (the unguarded decode)
- Modify: `src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp` — the sweep's own test file; add there rather than starting a new one
- Read first: `src/Disks/tests/cas_sweep_test_support.h` — the shared fixture both sweep test files use for pool setup. `gtest_cas_gc_round_defer.cpp` is the other consumer and shows a second usage shape if the first is not a fit

**Interfaces:**
- Consumes: nothing from Task 1. **This half must work on a pool that already contains a poison object**, which is precisely the case Task 1 can no longer create — so it is tested by planting bytes at the pool level, not through DDL.
- Produces: `ManifestSweepResult::retained_undecodable`, a `uint64_t` alongside the existing `retained_*` counters.

Task 1 stops new occurrences; this unwedges a pool that already has one. Neither substitutes for the other: a pool wedged today stays wedged after Task 1 alone, because the object is already there.

- [ ] **Step 1: Write the failing gtest**

Plant an orphan manifest object whose bytes cannot be decoded, then run one sweep page. Use the shared fixture in `cas_sweep_test_support.h` for pool setup, and write the bad object through the pool's own object storage rather than through a transaction — a transaction would refuse it after Task 1, which is the whole reason this half is tested here and not through DDL.

The manifest bytes only need to be undecodable in the way the reproducer produced — a payload-zone banner that does not match the entry path. Build them by hand: encode a legal single-entry manifest with an Inline entry, then corrupt the banner line in place (this is a text format and carries no per-line checksum). Do NOT try to produce them through `encodePartManifest` with a bad path — after Task 1 that throws, and a test that depends on the encoder being permissive would break the moment Task 1 lands.

Assertions, and the second is the one that matters:

```cpp
    /// The round must COMPLETE rather than throw ...
    ASSERT_NO_THROW(... one sweep page ...);
    /// ... the anomaly must be recorded rather than silently swallowed ...
    EXPECT_EQ(result.retained_undecodable, 1u);
    /// ... and the cursor must have MOVED PAST the poison key, which is what makes the wedge
    /// impossible: a cursor that stalls here fails every future round on the same object.
    EXPECT_NE(result.next_cursor, <the cursor the page started from>);
```

- [ ] **Step 2: Run it and confirm the failure is the wedge**

```bash
ninja -C build unit_tests_dbms > build/040_sweep_build_1.log 2>&1; echo "EXIT=$?" >> build/040_sweep_build_1.log
build/src/unit_tests_dbms --gtest_filter='CASSweepDeletionPremise*' > build/040_sweep_red.log 2>&1; echo "EXIT=$?" >> build/040_sweep_red.log
```

Expected: the test fails on the `ASSERT_NO_THROW` with the banner-mismatch `CORRUPTED_DATA` escaping the sweep. If instead it fails on the counter with no throw, the planted bytes are decodable and the plant needs fixing — not the sweep.

- [ ] **Step 3: Make the decode non-fatal to the round**

At the unguarded `decodePartManifest` call in `planManifestCursorPage`, wrap the decode — and only the decode — so an undecodable body becomes a recorded, retained candidate instead of an escaping exception:

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
            /// stays visible to fsck as unaccounted; the alternative -- letting this escape -- aborts
            /// the round and every later round on the same object, which stops reclamation for the
            /// whole pool rather than for this one key.
            LOG_ERROR(log, "CAS orphan sweep: manifest at {} cannot be decoded and was retained; "
                           "run cas-fsck to enumerate such objects", parsed->key);
            ++result.retained_undecodable;
            decided_through = listed.key;
            continue;
        }
```

Keep the identity-mismatch throw immediately below it as it is: that one fires on a manifest that decoded fine but names something else, which is a different and genuinely unexpected state — do not widen this catch over it.

Add the counter next to the other `retained_*` fields in the header, with a comment saying what it means and that a non-zero value is an operator signal rather than a transient.

- [ ] **Step 4: Check whether the counter needs a reporting surface**

Find how the sibling `retained_*` counters reach an operator — `grep -rn "retained_hold\|retained_no_coverage" src/` across the round's result rendering and any system table. If they are rendered somewhere, render `retained_undecodable` in the same place and in the same style. If none of them are, do not invent a surface for this one: say so in your report, and note that the `LOG_ERROR` plus fsck's `unaccounted` are the operator's path in that case.

- [ ] **Step 5: Build, run, read the markers**

```bash
ninja -C build unit_tests_dbms > build/040_sweep_build_2.log 2>&1; echo "EXIT=$?" >> build/040_sweep_build_2.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/040_gate_2.log 2>&1; echo "EXIT=$?" >> build/040_gate_2.log
```

Expected: the new sweep test passes; the gate is otherwise unchanged. A GC test that now sees a different counter total is a finding — report it rather than adjusting the expectation.

- [ ] **Step 6: Commit**

```bash
git status --short src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.h \
                   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/CasOrphanManifestSweep.cpp \
                   src/Disks/tests/gtest_cas_sweep_deletion_premise.cpp
git add <only the paths whose diff is yours>
git commit -m 'One undecodable manifest no longer stops reclamation for the whole pool'
```

---

## Task 3: Retire the entry and update the records {#task-3}

**Files:**
- Modify: `docs/superpowers/cas/BACKLOG/formats-and-storage.md` — **delete** the `{#manifest-entry-path-newline-banner}` section, only if the file is clean
- Modify: `docs/superpowers/cas/2031-triage.md` — five CAS-040 sites plus two anchor references
- Modify: `docs/superpowers/cas/final-checks-todo.md` — item 8, which also links the dying anchor

**Interfaces:**
- Consumes: Tasks 1 and 2 landed.
- Produces: nothing later tasks depend on.

The backlog files carry only open work — history lives in git — so a fixed entry is deleted rather than relabelled. The triage document is the opposite: it IS the history, so its section is historicised in place, never removed.

- [ ] **Step 1: Check whether each file is yours to touch**

Run `git status --short` on all three. Any file another session is holding: **stop and report it**, do not edit it, and do not relocate its content to get around it. The other files can still proceed.

- [ ] **Step 2: Delete the live-backlog entry**

Remove the whole `{#manifest-entry-path-newline-banner}` section from `BACKLOG/formats-and-storage.md`. Every link to that anchor must die in the same commit, which is why item 8 of `final-checks-todo.md` is in this task and not a later one.

- [ ] **Step 3: Historicise the triage, all five CAS-040 sites**

`grep -n "CAS-040" docs/superpowers/cas/2031-triage.md` returns five lines. Read the whole output rather than trusting this count — a sibling plan claimed four for its own issue because the grep behind it had been piped through `head`, and the site it missed was the largest.

1. The verdict row, currently `подтверждено | P1`. Mark it fixed and name the commits. **Its link is already broken today** — it points at `BACKLOG.md#manifest-entry-path-newline-banner` while the section actually lives in `BACKLOG/formats-and-storage.md`. Repoint it at this plan rather than reproducing the mistake at a new address.
2. The `{#p1-list}` table — drop the CAS-040 row.
3. The paragraph after that table naming which P1s the triage found first. CAS-040 is called out there as the one where the triage added the reproduction and corrected the stated impact; that sentence has to survive the row's removal and still make sense.
4. The priority tally line. One fewer P1.
5. The full `{#cas-040}` section, whose heading still ends `(подтверждено, P1)`. Historicise it in place — do not delete it.

There is also a sixth mention of the id at the end of the file, in a note correcting an unrelated one-liner's reference to CAS-040. Check whether it still reads correctly once the verdict changes; it may need nothing.

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

**Spec coverage.** The spec's fix (1), encode-side validation, is Task 1. Its fix (2), the non-wedging sweep, is Task 2. Its fix (3), upstream projection escaping, is explicitly out of scope and recorded as such. The test gap the spec names — that the format tests pin `\n` in inline BYTES but never in the entry PATH — is closed by Task 1 Step 1.

**Placeholders.** One deliberate exception, flagged in place: Task 1 Step 5's fsck read is left unfinished with an explicit instruction not to ship it as written, because the column-reading idiom must be taken from a working example in the tree rather than guessed here. Every other step carries its actual content. One step names a surface to be found by grep rather than by path — where the sibling `retained_*` counters are rendered, if anywhere — because guessing it would be worse than a one-line search. The sweep test file and its shared fixture are named outright.

**Type consistency.** `validateEntryPath(std::string_view)` is file-local, void, throwing; called from `encodePartManifest` and from the decoder's entry loop. `retained_undecodable` is a `uint64_t` on `ManifestSweepResult`, matching its `retained_*` siblings. The decode result becomes `std::optional<PartManifest>` at that one call site only.

**The risk this plan cannot remove.** Task 1 makes the encoder stricter for every caller in the tree, and only a run can tell whether some other test or path was relying on the old permissiveness. Task 1 Step 6 says explicitly that such a failure is a finding rather than noise, because the tempting response — relaxing the new check until the suite goes quiet — would undo the fix.

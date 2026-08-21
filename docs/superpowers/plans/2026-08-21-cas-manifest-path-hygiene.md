---
description: 'Implementation plan for carrying a part-file path through one escaper everywhere the manifest writes it, and stopping one poison object from wedging garbage collection pool-wide'
sidebar_label: 'CAS manifest path hygiene'
sidebar_position: 1
slug: /superpowers/plans/cas-manifest-path-hygiene
title: 'Manifest entry-path escaping and a non-wedging orphan sweep — implementation plan'
doc_type: 'guide'
---

# Manifest entry-path escaping and a non-wedging orphan sweep — Implementation Plan {#cas-manifest-path-hygiene-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a content-addressed disk accept every part-file path an ordinary `MergeTree` disk accepts, and stop one undecodable manifest from disabling reclamation for a whole pool.

**Architecture:** Two independent halves. The manifest format carries the entry path through **one** escaper everywhere it writes it — today the entry-record line escapes it and the payload-zone banner does not, and that asymmetry is the whole bug. And the orphan sweep treats an undecodable manifest as a recorded anomaly it walks past, instead of letting the exception escape and abort every round forever.

**Tech Stack:** C++ (ClickHouse fork), gtest, one stateless `.sh` test, `SYSTEM CAS FSCK` for the "nothing was left behind" assertion.

**Spec:** `docs/superpowers/cas/BACKLOG/formats-and-storage.md` `{#manifest-entry-path-newline-banner}`, with the adjudication at `docs/superpowers/cas/2031-triage.md` `{#cas-040}`. CONFIRMED and reproduced live on HEAD. **The spec's fix shape is superseded — read the spec-delta section below before starting.**

**Spec anchor note:** this plan cites `{#manifest-entry-path-newline-banner}` in five places (the Spec line above, Task 3's file list, and Task 3 Steps 2, 3 and 4 — this note is not one of them). Task 3 deletes that anchor. The citations here are provenance for a plan that is itself history once executed, so they stay as they are — but do not add new ones, and do not "fix" them into links that will dangle.

## Spec delta — this plan does NOT reject the path {#spec-delta}

The spec diagnoses the mechanism correctly and then proposes the wrong remedy: validate the path and refuse it. **Do not implement that.** `MergeTree` accepts a projection whose name contains a newline, so refusing it in the CAS metadata layer would mean the same `CREATE TABLE` works on one disk type and fails on another, permanently and by design. That divergence is worse than the bug it prevents. Six things, all verified against the tree:

1. **The defect is asymmetric escaping inside one format, and it only bites Inline entries.** The entry-record line escapes the path: `writeEntryRecord` writes it through `writeStringValue` → `CasJsonWriter::stringValue`, a quoted JSON string with full escaping, and `r.readString` restores the real bytes on the way back. That line survives the round trip intact. The break is in the **payload zone**, where `bannerFor` (`CasPartManifestFormat.cpp:68`) concatenates the path **raw** into `==> <path> il=<n> <==`; an LF splits that banner across two physical lines, and the reader's `readLine` comparison then mismatches. A **Blob** entry has no banner at all, so a path with an LF decodes end to end without complaint today. So "a newline cannot reach a decoded path" is false — do not write that into a comment.
2. **One function fixes both sides, and that is why this is the right fix rather than the cheap one.** `bannerFor` is called from exactly two places: the encoder at `:116` writes its result, and the decoder at `:279` computes the expected banner and compares byte-wise. Route the path through `stringValue` inside `bannerFor` and both sides move together — the writer emits `==> "p\nq.proj/columns.txt" il=12 <==` and the reader expects exactly that, on one physical line. There is no decode-side rule to add, no validator, and no rejection branch. Reuse `stringValue`; a second hand-rolled escaper would recreate the asymmetry this fix exists to remove.
3. **Every banner's bytes change, and pre-release that is free.** Adding quotes changes the banner for *all* paths, not only exotic ones, so a manifest written by the new code is not readable by the old and vice versa. CAS has no persisted data in the field, so this needs no format generation, no migration, and no compatibility shim — and the symmetric fix is preferred over an LF-only escape precisely because a special case is one more surface for the next drift. The quotes also remove a latent ambiguity for paths containing spaces.
4. **Two goldens and one comment move with it.** `src/Disks/tests/gtest_cas_part_manifest_format.cpp:71` carries the literal banner `"==> c/small.txt il=12 <==\n"` inside a golden encoding; and the format's own documentation at `CasPartManifestFormat.h:25` states the banner shape as `==> <path> il=<n> <==\n`. The negative assertion at `:109` (`EXPECT_FALSE(encodePartManifest(m).contains("==>"))`) is about a manifest with no Inline entry and is unaffected. `grep -n '==>' ` over that file returns exactly those two lines; no other test or document in the tree spells the banner out.
5. **The sweep cannot delete an undecodable object, only walk past it.** Deciding an orphan manifest is safe to delete requires reading its body to derive the source edges — exactly what fails. So half two retains the object, records it, and advances the cursor: one visibly-leaked object instead of a pool-wide wedge. `SYSTEM CAS FSCK` counts such a body as **`unreachable`** — `CasFsck.cpp`'s manifest-debris scan increments `report.unreachable` for any `cas/manifests/` body no committed ref owns, with no eligibility precondition on the increment. It is *not* `unaccounted`: that counter is produced only inside blob-object classification and would stay `0` here.
6. **Part (3) of the spec's fix list becomes unnecessary rather than out of scope.** It asked for the projection directory name to be escaped in generic `MergeTree` code (`ProjectionsDescription::getDirectoryName`). Once the manifest carries any path faithfully, the CAS layer needs nothing from upstream. Do not touch that code, and do not open an upstream consult for it as part of this work.

## Global Constraints {#global-constraints}

- Branch `cas-gc-rebuild`. No rebase, no amend — add new commits.
- **The worktree is shared.** Other sessions hold uncommitted work in this checkout. Before every commit, re-run `git status --short <the exact paths>` and stage only paths whose diff is yours. Never `git add -A`, `git add .`, or `git commit -a`.
- **No `LOGICAL_ERROR` anywhere in this work.** Task 1 adds no failure path at all — it makes a previously unrepresentable path representable. Task 2's path is reachable from an object already in the pool, so it is `CORRUPTED_DATA` and is caught rather than thrown. Before writing any `EXPECT_THROW`, check the error code at the site; if a site you touch throws `LOGICAL_ERROR` on an input-reachable condition, stop and report it rather than testing it.
- Allman braces (opening brace on its own line) — enforced by the style check.
- Comments must not cite this plan, the BACKLOG, a task number, or an issue number. Keep the reason, drop the provenance.
- Every build and every test run goes to its own uniquely named log under `build/`, with an exit marker appended, and the status is read from the marker rather than from the shell.
- The test number `05026_cas_manifest_path_newline` is already reserved by `add-test`; both files exist as untracked stubs, the reference empty. Do not renumber, do not re-run `add-test`, do not `chmod`.

---

## Task 1: Carry the entry path through one escaper everywhere it is written {#task-1}

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp` — `bannerFor` only
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h:25` — the documented banner shape
- Modify: `src/Disks/tests/gtest_cas_part_manifest_format.cpp` — the golden banner at `:71`, plus new round-trip tests
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.sh`
- Modify: `tests/queries/0_stateless/05026_cas_manifest_path_newline.reference`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: no signature changes at all. `bannerFor` keeps `(std::string_view, uint64_t) -> String`; only the bytes it returns change.

**Ordering is load-bearing.** Both tests are written and run RED before the production change, so the
pre-fix evidence comes from the tree as it stands. Do not reach for `git stash` to manufacture a pre-fix
state: other sessions are writing in this worktree and a bare `pop` can take their entry, `ninja` only
builds a binary while `tests/clickhouse-test` talks to an already-running server, and two runs sharing one
pool directory let the red run's leftover object be counted by the green one.

- [ ] **Step 1: Write the failing gtest**

The entry must be **Inline**: a Blob entry has no payload-zone banner, so a Blob-only manifest round-trips
an LF path cleanly today and would prove nothing. `manifestWithSinglePath` builds a Blob entry, so it is the
wrong helper here — build the manifest the way `sample()` builds its Inline entry.

```cpp
/// The path is written twice: escaped into the entry-record line, and -- before this fix -- raw into the
/// payload-zone banner. Any byte that the escaper spells differently therefore has to survive BOTH, or
/// the writer produces an object it cannot read back one transaction later.
TEST(CASPartManifestFormat, InlineEntryPathSurvivesEveryEscapableByte)
{
    const std::vector<String> paths{
        String("p\nq.proj/columns.txt"),   /// the reported reproducer: LF splits the banner line
        String("a\rb.txt"),
        String("tab\there.txt"),
        String("quote\"and\\slash.txt"),
        String("nul\0byte.txt", 12),       /// length-explicit, or the NUL is lost to the terminator
    };
    for (const String & path : paths)
    {
        SCOPED_TRACE(path);
        PartManifest m;
        m.ref = ManifestRef{17, 66, 7};
        m.root_namespace_id = RootNamespace("00/ff@cas@");
        ManifestEntry e;
        e.path = path;
        e.placement = EntryPlacement::Inline;
        e.inline_bytes = "hello world!";
        m.entries = {e};
        m.payload_digest = computePayloadDigest(m);

        const PartManifest got = decodePartManifest(encodePartManifest(m));
        ASSERT_EQ(got.entries.size(), 1u);
        EXPECT_EQ(got.entries[0].path, path);
        EXPECT_EQ(got.entries[0].inline_bytes, "hello world!");
    }
}

/// The banner quotes and escapes the path with the SAME writer the entry-record line uses. Pin the byte
/// shape, so a future hand-rolled escaper here cannot silently diverge from the record line again.
TEST(CASPartManifestFormat, InlineBannerCarriesTheEscapedPath)
{
    PartManifest m;
    m.ref = ManifestRef{17, 66, 7};
    m.root_namespace_id = RootNamespace("00/ff@cas@");
    ManifestEntry e;
    e.path = "p\nq.proj/c.txt";
    e.placement = EntryPlacement::Inline;
    e.inline_bytes = "x";
    m.entries = {e};
    m.payload_digest = computePayloadDigest(m);

    EXPECT_NE(encodePartManifest(m).find("==> \"p\\nq.proj/c.txt\" il=1 <=="), String::npos);
}
```

Check `ManifestEntry`'s field names for an Inline entry against `sample()` before writing this — the entry
carries `inline_bytes` and no `ref`/`blob_size`, and getting that wrong makes the test fail for a reason
that has nothing to do with the banner.

- [ ] **Step 2: Write the stateless test, before touching any production file**

The gtests prove the format is symmetric. This proves the thing a user asked for: a projection whose name
contains a newline works on a content-addressed disk, exactly as it does on an ordinary one. That is a
stronger oracle than the previous revision's "the INSERT fails with the right message", and it is the
oracle that matches what this fix is for.

Two idioms are copied rather than invented, both from tests that already do this:

- the fsck read, from `tests/queries/0_stateless/04290_cas_no_leftovers.sh`. The statement is
  `SYSTEM CAS FSCK '<disk>'` — there is no `ON DISK` in the grammar, `ParserSystemQuery` parses the disk as
  a plain target — and the `awk` indexes columns **by name** out of the `TSVWithNames` header, so a column
  added to the summary later cannot silently move the one being read.
- the naming, from `tests/queries/0_stateless/05020_cas_fsck.sh`. Unique per-run disk and pool names are not
  cosmetic here: a fixed pool path would let Step 3's pre-fix run and Step 5's post-fix run share one pool,
  and the pre-fix run deliberately leaves an orphan manifest behind.

```bash
#!/usr/bin/env bash
# Tags: no-fasttest
# ^ cas is an object-storage metadata type; keep it off the minimal fasttest image.

# A projection name is used verbatim as a part-relative directory, so a projection named with a newline
# puts a newline in a part-file path. MergeTree allows that, and a content-addressed disk has to carry it:
# the manifest writes each path twice -- escaped in its record line, and in an Inline entry's payload-zone
# banner -- and both spellings have to agree or the writer cannot read back what it just wrote.

CUR_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CUR_DIR"/../shell_config.sh

DISK_NAME="ca_05026_${CLICKHOUSE_TEST_UNIQUE_NAME}_${RANDOM}"
POOL_DIR="${CLICKHOUSE_USER_FILES_UNIQUE}_05026_${RANDOM}"
TABLE="t_05026_${CLICKHOUSE_TEST_UNIQUE_NAME}"

${CLICKHOUSE_CLIENT} --query "DROP TABLE IF EXISTS ${TABLE};"

${CLICKHOUSE_CLIENT} --query "
CREATE TABLE ${TABLE} (k UInt32, v String, PROJECTION \`p
q\` (SELECT v, count() GROUP BY v))
ENGINE = MergeTree ORDER BY k
SETTINGS disk = disk(
    type = object_storage,
    object_storage_type = local,
    metadata_type = cas,
    server_root_id = '05026',
    name = '${DISK_NAME}',
    path = '${POOL_DIR}/');"

echo 'insert_ok'
${CLICKHOUSE_CLIENT} --query "INSERT INTO ${TABLE} SELECT number, 'v' || (number % 3) FROM numbers(30);" \
    && echo 1

# The part is read back through the same manifest that was just written -- on the broken tree the INSERT
# never got this far, because the manifest it wrote could not be decoded.
echo 'rows'
${CLICKHOUSE_CLIENT} --query "SELECT count() FROM ${TABLE};"

# And the projection itself is usable, which is what a newline in its name must not prevent.
echo 'projection_result'
${CLICKHOUSE_CLIENT} --query "SELECT v, count() FROM ${TABLE} GROUP BY v ORDER BY v;"

# No manifest body without a committed owner: the successful INSERT left nothing orphaned, and on the
# broken tree the failed one did -- that object wedged every later collection round.
${CLICKHOUSE_CLIENT} --query "SYSTEM CAS FSCK '${DISK_NAME}'" --format TSVWithNames \
    | awk -F'\t' 'NR==1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
                  { print "unreachable", $col["unreachable"]; print "dangling", $col["dangling"] }'

${CLICKHOUSE_CLIENT} --query "DROP TABLE ${TABLE};"
${CLICKHOUSE_CLIENT} --query "SELECT 'dropped_ok';"
```

Reference file:

```
insert_ok
1
rows
30
projection_result
v0	10
v1	10
v2	10
unreachable 0
dangling 0
dropped_ok
```

Generate the reference from a green run and then read it: check that `rows` is 30 and the three groups are
10 each, rather than pasting whatever the run produced. Confirm the projection is actually used for that
`GROUP BY` — `EXPLAIN indexes = 1` on it during development, not in the committed test, whose job is the
result rather than the plan.

- [ ] **Step 3: Run both tests RED, against the unmodified tree**

No production file has been touched yet, so the binary under test IS the pre-fix binary and no stashing is
involved. What matters is that the server serving the stateless test was started from this binary: a bare
`ninja` leaves whatever server is already running in place, and a red/green pair that shares one server
proves nothing about either. Use the isolated job, which starts its own server from the build:

```bash
ninja -C build clickhouse unit_tests_dbms > build/040_red_build.log 2>&1; echo "EXIT=$?" >> build/040_red_build.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_red.log 2>&1; echo "EXIT=$?" >> build/040_fmt_red.log
python3 -m ci.praktika run "Stateless tests" --test 05026_cas_manifest_path_newline > build/040_stateless_red.log 2>&1; echo "EXIT=$?" >> build/040_stateless_red.log
```

Expected, and all three parts are required:

- `InlineEntryPathSurvivesEveryEscapableByte` FAILS on the LF, CR and tab cases with a payload-zone banner
  mismatch. It should PASS on the quote/backslash case even now — those bytes are escaped in the record
  line and pass through the raw banner unchanged on both sides — so a failure there means the test builds
  its manifest wrongly, not that the format is worse than described.
- `InlineBannerCarriesTheEscapedPath` FAILS because the banner is unquoted today.
- the stateless test FAILS at `insert_ok`, and the fsck read reports a **non-zero `unreachable`**. That
  leftover object is what distinguishes fixed from broken here; an earlier revision of this plan asserted
  a counter (`unaccounted`) that stays `0` on both trees. If `unreachable` is `0`, the oracle does not
  discriminate: stop and report it, and do not proceed by weakening the assertion.

State in your report which binary served this run, and how you know.

- [ ] **Step 4: Route the banner's path through the record line's escaper**

One function. Do not add a validator, a rejection branch, or a second escaper.

```cpp
/// The path is written into the banner through the SAME escaper the entry-record line uses. It has to be
/// the same one: the decoder rebuilds this banner from the path it read out of the record line and
/// compares byte-wise, so any spelling difference between the two writers is an object the writer cannot
/// read back. Concatenating the path raw here is what made a part-file path containing a newline
/// undecodable -- the LF split this line, and no reader could match it again.
String bannerFor(std::string_view path, uint64_t n)
{
    CasJsonWriter w(path.size() + 32);
    w.append("==> ");
    w.stringValue(path);
    w.append(" il=");
    w.u64Number(n);
    w.append(" <==");
    return std::move(w).take();
}
```

`take` is `String take() &&`, so the `std::move` is required. Check that `CasJsonWriter`'s header is
already included in this translation unit — it is, since `encodePartManifest` builds one — and that
`u64Number` renders the same digits the old `std::to_string(n)` did for every `uint64_t`.

- [ ] **Step 5: Move the two goldens the byte change invalidates**

Both are named in spec-delta item 4, and both must change in this commit or the suite is red for a reason
unrelated to the next task:

- `gtest_cas_part_manifest_format.cpp:71` — the golden banner line inside a full expected encoding becomes
  `"==> \"c/small.txt\" il=12 <==\n"`. Do not regenerate the whole golden from output; change that one
  line, so the diff shows exactly what moved.
- `CasPartManifestFormat.h:25` — the documented shape becomes the quoted-and-escaped form, with a clause
  saying the path is escaped by the same writer as the record line and why.

- [ ] **Step 6: Green run, then the whole CAS gate**

```bash
ninja -C build clickhouse unit_tests_dbms > build/040_green_build.log 2>&1; echo "EXIT=$?" >> build/040_green_build.log
build/src/unit_tests_dbms --gtest_filter='CASPartManifestFormat*' > build/040_fmt_green.log 2>&1; echo "EXIT=$?" >> build/040_fmt_green.log
python3 -m ci.praktika run "Stateless tests" --test 05026_cas_manifest_path_newline > build/040_stateless_green.log 2>&1; echo "EXIT=$?" >> build/040_stateless_green.log
build/src/unit_tests_dbms --gtest_filter='CAS*:Cas*:CA*' > build/040_gate_1.log 2>&1; echo "EXIT=$?" >> build/040_gate_1.log
```

Expected: the new tests pass, the stateless test passes every assertion, and the gate is otherwise
unchanged. The pool directory differs from Step 3's run by construction, so a clean `unreachable` here is
about this run's pool — say in your report that you checked that.

**A `CAS*` failure here is a finding, not noise.** Every Inline entry's banner bytes just changed, so a
test carrying a manifest fixture as literal bytes, or a checksum over one, will notice. Report each one
with the fixture it came from; do not adapt an expectation you cannot explain.

- [ ] **Step 7: Commit**

```bash
git status --short src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.cpp \
                   src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/CasPartManifestFormat.h \
                   src/Disks/tests/gtest_cas_part_manifest_format.cpp \
                   tests/queries/0_stateless/05026_cas_manifest_path_newline.sh \
                   tests/queries/0_stateless/05026_cas_manifest_path_newline.reference
git add <only the paths whose diff is yours>
git commit -m 'Carry a manifest entry path through one escaper in the record line and the banner'
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
- Consumes: nothing from Task 1, and it is not made redundant by it. Task 1 removes ONE cause of an undecodable manifest — the format could not represent the path. This half is about an undecodable body whatever its cause: bit rot, a future format bug, a peer's manifest over the relink channel, or a pool that was already wedged before Task 1 landed. The encoder never produces such bytes, so the test plants them at the pool level rather than going through DDL.
- Produces: `ManifestSweepResult::undecodable`, a `uint64_t` beside `listed`/`deleted`/`skipped`.

**Why not a `retained_*` counter.** The `retained_*` family is documented in the header as "the §6
premise's share of `skipped`, by reason class", and each member has a `SweepRetainClass` enum entry that
`topRetainReason` ranks and `reportSweepRetention` narrates. An undecodable body was never judged by the
deletion premise at all — it failed before the premise could be evaluated — so filing it as a retain
reason would corrupt what "the top retain reason" means. It belongs with `skipped`, whose own contract
already reads "counts malformed, protected, ineligible, budget-exhausted, or race-spared keys". So:
increment **both** `skipped` (the contract requires it) and the new `undecodable` (the reason), add **no**
`SweepRetainClass` member, and leave `topRetainReason` and `reportSweepRetention` untouched.

Neither half substitutes for the other. A pool wedged today stays wedged after Task 1 alone, because the object is already in it and nothing re-reads a manifest to repair it.

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
    /// produced -- built by hand, because a correct encoder never emits a banner that disagrees with
    /// its own entry record.
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
    /// `putIfAbsent` over an existing key writes nothing and reports PreconditionFailed, so a
    /// silently-legal body would make every assertion below pass against the wrong object.
    ASSERT_EQ(put.outcome, PutOutcome::Done) << "the poison body was not the one planted";
```

`PutResult` is `WriteResultT<PutOutcome>` with a plain `outcome` field, and `EXPECT_EQ(....outcome, PutOutcome::Done)`
is the idiom `gtest_cas_backend.cpp` already uses. A bare `head(...).exists` would not do: the fixture's own
body satisfies it.

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
- **`ProjectionsDescription::getDirectoryName`** — the upstream half of the spec's fix list, and it is now moot rather than deferred. The manifest carries any path faithfully, so the CAS layer needs nothing from upstream and no consult should be opened. Say that in the Task 3 commit, so a later reader does not reopen it: the spec asked for upstream escaping only because the CAS format could not represent the path.

## Self-review {#self-review}

**Spec coverage.** The spec's fix (1) is deliberately NOT implemented: it asked for encode-side rejection, and rejecting a path `MergeTree` accepts would make the CAS disk refuse a working table. Task 1 fixes the cause the spec correctly diagnosed instead — one escaper for every place the format writes a path — which is what makes fix (3), upstream escaping of the projection directory name, unnecessary rather than deferred. Its fix (2), the non-wedging sweep, is Task 2. The test gap the spec names — that the format tests pin `\n` in inline BYTES but never in the entry PATH — is closed by Task 1 Step 1.

**Placeholders.** Three remain, all narrow and each flagged in place: the Inline entry's field names in Task 1 Step 1 and Task 2 Step 1 (check them against `sample()` / `blobEntryFor` rather than trusting this plan), and the reference file's exact whitespace in Task 1 Step 2, which is generated from a run and then read. Every other step carries its actual content.

**Type consistency.** `bannerFor` keeps its signature; only its bytes change. `undecodable` is a `uint64_t` on `ManifestSweepResult`, beside `skipped`, deliberately not a `SweepRetainClass`. The decode result becomes `std::optional<PartManifest>` at one call site, with three deref sites named.

**What earlier revisions got wrong, so none of it is reintroduced.** Six defects. The design one is first because it invalidated a whole task:

- the fix was encode-side **rejection** of a control character in a path. `MergeTree` accepts such a projection name, so that would have left the CAS disk permanently refusing a table that works on every other disk type — a functional divergence between metadata types, adopted deliberately, to avoid a two-call-site format fix. Task 1 is now the format fix.
- the stateless test asserted `unaccounted = 0`. That counter is produced only by blob classification; an orphan manifest lands in `unreachable`. The assertion would have passed on the broken tree.
- the pre-fix run was placed after the fix, reached for `git stash` in a worktree other sessions are writing to, rebuilt a binary without restarting the server that would serve the test, and reused one pool directory across both runs. It is now Step 3, before any production edit, on unique per-run names, through the isolated job that starts its own server.
- the sweep test asserted the cursor had moved via `EXPECT_NE(next_cursor, …)`. `InMemoryBackend` returns an empty `next_cursor` at the end of the keyspace, so on a single-object page that assertion fails *after* a correct fix. It is now `EXPECT_TRUE(wrapped)` plus a second object planted beyond the poison key.
- the poison plant went on top of the fixture's own legal body. `putIfAbsent` over an existing key writes nothing, so every assertion would have run against a manifest that decodes fine. The plant now replaces the fixture's write, and `put.outcome` is asserted.
- the reason given for keeping the decoder unchanged was that an embedded LF "cannot reach a decoded path". It can: the record line escapes the path and restores it, and only an Inline entry's raw banner is sensitive. Under the new design the decoder changes anyway, through the same function as the encoder.

Five of the six were caught by reading the code that decides — the parser, the counter's increment site, the backend's write contract, the escaper — and the sixth by a reviewer asking why we would reject input the database accepts. None was caught by re-reading the plan's own reasoning.

**The risk this plan cannot remove.** Every Inline entry's banner bytes change, so any test or fixture carrying manifest bytes as literals will move. Two are named from `grep -n '==>'`; a fixture that stores a *checksum* over manifest bytes rather than the bytes would not appear in that grep, and only the gate run finds it. Task 1 Step 6 says such a failure is a finding rather than noise, because the tempting response — regenerating a golden until the suite goes quiet — hides whatever else moved with it.

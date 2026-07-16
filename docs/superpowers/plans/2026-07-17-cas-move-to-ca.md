# MOVE PART/PARTITION to a content-addressed disk — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `ALTER TABLE … MOVE PART|PARTITION TO DISK/VOLUME 'ca'` (and the background TTL/policy mover, which shares the same code) work correctly TO a content-addressed (`CA`) disk: one part = one manifest = one ref, published atomically, with dedup applied.

**Architecture:** Two independent bugs, both required to fix: **L1 (identity)** — the part-path parser routes every file under `<table>/moving/<part>/…` to the literal ref `"moving"` instead of the part's own name, so concurrent/successive moves collide. **L2 (atomicity)** — `DataPartStorageOnDiskBase::clonePart` copies a part file-by-file, each in its own autocommit disk transaction, which a CA disk cannot support for more than one file. The fix threads the whole clone through one CA transaction (mirroring `freeze`'s `owned_transaction` shape) once L1 makes that transaction publish under the correct final ref.

**Tech Stack:** C++ (ClickHouse `MergeTree`/`Disks` subsystem), gtest, the `utils/ca-soak` Python scenario-soak framework.

## Global Constraints

- Branch `cas-gc-rebuild`. No `git push`.
- No `rebase`/`amend` — add new commits.
- Every commit is pathspec-exact (`git add <exact files>`, never `-A`/`.`); before committing run `git diff --cached --stat` and confirm it contains ONLY the files this task touched (foreign-file check — this is a shared worktree).
- Wrap every `git` command in `flock /tmp/cas_git.lock -c '...'`.
- Wrap every build in `flock /tmp/cas_build.lock -c '...'`. Never pass `-j` to `ninja`, never use `nproc` — let it auto-detect. Always redirect build output to a log file under the build directory (e.g. `build/build_<task>.log`); a subagent analyzes the log and reports a concise pass/fail summary back, not the raw log.
- Redirect every test run to a log file under the build directory too (unique name per test so parallel runs don't clobber each other); a subagent analyzes it.
- Every commit trailer ends with exactly:
  ```
  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  ```
- Say "exception", never "crash" (release build never crashes on a logical error). Say "ASan", never "ASAN".
- CA fold/GC code must never throw on a 404 — not touched by this plan, but do not introduce a new throw-on-404 path anywhere near the GC/fold surface.
- Never delete `ci/tmp/rustfs`.
- Wrap literal ClickHouse SQL/class/function names in inline code (`` `MOVE PART` ``, `` `clonePart` ``); write function names as `f`, not `f()`, in prose/comments/commit messages.

---

## Phase 1 — L1: publish a moved part under its final identity

### Task 1: `kMovingDirName` + `route()` — the Atomic-layout fix

The Atomic on-disk layout (`store/<u3>/<uuid>/moving/<part>/<file>`) already anchors `part_idx` on the component right after the `<uuid>` pair with **no code change needed** in the parser — `"moving"` naturally lands there, exactly the way `"detached"` does (see `PartPathParser::findPartDirComponent`, `PartPathParser.cpp:150-164`: there is no `kDetachedDirName` special case in that branch at all). The only thing missing is a `route()` branch that re-splits `"moving"` the way it already re-splits `"detached"` — except a moved part must resolve to its **final** live ref, with no prefix (unlike `detached/<part>`).

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h` (add `kMovingDirName` next to `kDetachedDirName`, currently line 23)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` (`route()`, currently lines 636-664)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp`

**Interfaces:**
- Consumes: `ContentAddressed::PartFilePath` (existing, has `table_uuid`/`part_name`/`file`), `ContentAddressedMetadataStorage::Route` (existing, has `ns`/`ref`/`file`), `splitFirstComponent` (existing free function in `ContentAddressedMetadataStorage.cpp:76-82`, `"<first>/<rest>" -> {first, rest}`).
- Produces: `ContentAddressed::kMovingDirName` (new constant, `= "moving"`), consumed by Task 2's non-Atomic parser fix.

- [ ] **Step 1: Write the failing gtest for `route()`**

  Add to `src/Disks/tests/gtest_ca_wiring.cpp`, right after `TEST(CaWiringRoute, DetachedFoldsIntoTableNamespaceWithPrefixedRef)` (currently ends at line 539):

  ```cpp
  TEST(CaWiringRoute, MovingFoldsOntoTheFinalLiveRefWithoutPrefix)
  {
      /// L1 (MOVE-to-CA fix): the mover clones a part under <table>/moving/<part>/ before the
      /// atomic rename into place. Unlike `detached`, a moved part must resolve DIRECTLY onto its
      /// final live ref (no prefix) -- the destination CA transaction publishes under that final
      /// identity, so the later moveDirectory(moving/<part> -> <part>) collapses to a same-key
      /// no-op instead of colliding with every other part on the shared ref "moving".
      auto storage = openWiringStorage();
      auto p = parsePartFilePath("store/uui/uuid-1/moving/all_1_1_0/data.bin");
      ASSERT_TRUE(p.has_value());
      EXPECT_EQ(p->part_name, std::string(kMovingDirName));
      EXPECT_EQ(p->file, "all_1_1_0/data.bin");

      auto r = storage->route(*p);
      ASSERT_TRUE(r.has_value());
      EXPECT_EQ(r->ns.string(), storage->liveNamespace("uuid-1").string());
      EXPECT_EQ(r->ref, "all_1_1_0");
      EXPECT_EQ(r->file, "data.bin");

      /// The bare moving CONTAINER dir <table>/moving routes to the table ns with an empty ref.
      auto pc = parsePartFilePath("store/uui/uuid-1/moving");
      ASSERT_TRUE(pc.has_value());
      auto rc = storage->route(*pc);
      ASSERT_TRUE(rc.has_value());
      EXPECT_EQ(rc->ns.string(), storage->liveNamespace("uuid-1").string());
      EXPECT_TRUE(rc->ref.empty());
      EXPECT_TRUE(rc->file.empty());
  }
  ```

  This will fail to compile (`kMovingDirName` does not exist yet) — that is the expected "RED" state.

- [ ] **Step 2: Confirm it fails to build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task1_red.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task1_red.log
  ```

  Expected: `NINJA_EXIT=` non-zero, error mentions `kMovingDirName` is not declared. Have a subagent read `build/build_task1_red.log` and confirm this is the ONLY error (report back a one-line summary, not the raw log).

- [ ] **Step 3: Add the `kMovingDirName` constant**

  In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h`, right after the existing `kDetachedDirName` block (line 23):

  ```cpp
  /// The MergeTree part-mover staging directory (`MergeTreeData::MOVING_DIR_NAME`,
  /// `MergeTreeData.h`). A part being relocated to another disk (explicit `ALTER … MOVE
  /// PART|PARTITION`, or a background TTL/policy move) is cloned under <table>/moving/<part>/
  /// before the atomic rename into its final place. `parsePartFilePath` reports such a path with
  /// part_name == kMovingDirName and the real part dir as the FIRST component of `file` -- the
  /// exact same shape `kDetachedDirName` already produces (the PoC contract, B36), for free, on
  /// the Atomic layout (no parser change needed there: "moving" already lands on `part_idx`
  /// because it is the component right after the table <uuid>, same as "detached"). Unlike
  /// detached, `route()` folds this DIRECTLY onto the part's FINAL live ref (no prefix): the
  /// destination CA transaction publishes under that final identity, so the later
  /// moveDirectory(moving/<part> -> <part>) collapses to a same-key no-op.
  inline constexpr std::string_view kMovingDirName = "moving";
  ```

- [ ] **Step 4: Add the `route()` branch**

  In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp`, inside `route()` (lines 636-664), insert a new branch between the `kDetachedDirName` branch (ending at line 658 `return r;`) and the generic fallback (lines 660-662):

  ```cpp
      if (p.part_name == ContentAddressed::kMovingDirName)
      {
          /// L1 (MOVE-to-CA fix): re-split exactly like detached, but fold onto the part's FINAL
          /// live ref directly -- no "moving/" prefix. An empty p.file (the bare <table>/moving
          /// container dir) yields an empty ref, same convention as the detached container.
          r.ns = liveNamespace(p.table_uuid);
          auto [part, file] = splitFirstComponent(p.file);
          r.ref = part;
          r.file = file;
          return r;
      }
  ```

  (Full resulting function body, for orientation — only the new block is added, nothing else changes:)

  ```cpp
  std::optional<ContentAddressedMetadataStorage::Route>
  ContentAddressedMetadataStorage::route(const ContentAddressed::PartFilePath & p) const
  {
      Route r;
      if (!p.backup_name.empty())
      {
          r.ns = shadowNamespace(p.shadow_table_dir);
          r.ref = p.part_name;
          r.file = p.file;
          return r;
      }
      if (p.part_name == ContentAddressed::kDetachedDirName)
      {
          r.ns = liveNamespace(p.table_uuid);
          auto [part, file] = splitFirstComponent(p.file);
          r.ref = part.empty() ? "" : std::string(ContentAddressed::kDetachedRefPrefix) + part;
          r.file = file;
          return r;
      }
      if (p.part_name == ContentAddressed::kMovingDirName)
      {
          r.ns = liveNamespace(p.table_uuid);
          auto [part, file] = splitFirstComponent(p.file);
          r.ref = part;
          r.file = file;
          return r;
      }
      r.ns = liveNamespace(p.table_uuid);
      r.ref = p.part_name;
      r.file = p.file;
      return r;
  }
  ```

- [ ] **Step 5: Also add the plain-parser gtest** (mirrors `DetachedPathsReportTheSharedDetachedComponent`)

  Add to `src/Disks/tests/gtest_ca_wiring.cpp`, right after `TEST(CaPartPathParser, DetachedPathsReportTheSharedDetachedComponent)` (currently ends at line 137):

  ```cpp
  TEST(CaPartPathParser, MovingPathsReportTheSharedMovingComponent)
  {
      // Atomic layout: "moving" lands on part_idx for free (it is the component right after the
      // table <uuid>, same mechanism as "detached" -- no parser change needed here, only route()).
      auto d = parsePartFilePath("uui/uuid-1/moving/all_1_1_0/data.bin");
      ASSERT_TRUE(d.has_value());
      EXPECT_EQ(d->table_uuid, "uuid-1");
      EXPECT_EQ(d->part_name, std::string(kMovingDirName));
      EXPECT_EQ(d->file, "all_1_1_0/data.bin");
  }
  ```

- [ ] **Step 6: Build and run, confirm GREEN**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task1_green.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task1_green.log
  ./build/src/unit_tests_dbms --gtest_filter='CaPartPathParser.MovingPathsReportTheSharedMovingComponent:CaWiringRoute.MovingFoldsOntoTheFinalLiveRefWithoutPrefix' > build/test_task1.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task1.log
  ```

  Expected: `NINJA_EXIT=0`, `TEST_EXIT=0`, both tests `[ PASSED ]`. Have a subagent read both logs and report a one-line pass/fail summary.

- [ ] **Step 7: Commit**

  ```bash
  flock /tmp/cas_git.lock -c "git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp src/Disks/tests/gtest_ca_wiring.cpp"
  flock /tmp/cas_git.lock -c 'git diff --cached --stat'
  ```

  Verify the stat output lists ONLY those 3 files, then:

  ```bash
  flock /tmp/cas_git.lock -c "git commit -m \"\$(cat <<'EOF'
  cas: MOVE-to-CA fix L1 part 1 -- route() folds \`moving\` onto the final live ref

  \`ALTER TABLE ... MOVE PART|PARTITION TO DISK 'ca'\` clones a part under
  \`<table>/moving/<part>/\` before the atomic rename into place. On the Atomic on-disk layout the
  parser already anchors \`part_idx\` on \`moving\` for free (same mechanism as \`detached\`), but
  \`route()\` had no case for it, so every file fell through to the generic branch and resolved to
  the literal ref \`moving\` -- colliding across every part ever moved. Add the \`kMovingDirName\`
  route() branch, folding directly onto the part's FINAL ref (unlike \`detached\`, no prefix), so
  the later moveDirectory(moving/<part> -> <part>) collapses to an existing same-key no-op.

  The non-Atomic layout fix (findPartDirComponent) is a separate follow-up task.

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )\""
  git -C /home/mfilimonov/workspace/ClickHouse/master log --oneline -1
  ```

### Task 2: `findPartDirComponent` — the non-Atomic-layout fix

For a non-Atomic (Ordinary/Memory/Lazy) table, `data/<db>/<table>/moving/<part>/<file>` has no `<uuid>` anchor, so `findPartDirComponent` falls back to the right-to-left `looksLikePartDir` scan (`PartPathParser.cpp:191-194`). Without an explicit anchor, that scan finds the REAL part dir (e.g. `all_1_1_0`) first and folds `moving` into the table id — `data/<db>/<table>/moving` — the exact same class of bug `kDetachedDirName`'s anchor (lines 172-174) was added to prevent for `detached` (U#6: a permanently-orphaned, `DROP TABLE`-uncleaned namespace). Mirror that fix for `moving`.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.cpp` (`findPartDirComponent`, lines 172-174)
- Test: `src/Disks/tests/gtest_ca_wiring.cpp`

**Interfaces:**
- Consumes: `ContentAddressed::kMovingDirName` (Task 1).
- Produces: nothing new — `parsePartFilePath` now reports `part_name == kMovingDirName` for non-Atomic `moving` paths too, feeding the SAME `route()` branch Task 1 added.

- [ ] **Step 1: Write the failing gtest**

  Add to `src/Disks/tests/gtest_ca_wiring.cpp`, right after `TEST(CaPartPathParser, MovingPathsReportTheSharedMovingComponent)` (added in Task 1):

  ```cpp
  TEST(CaPartPathParser, MovingPathsNonAtomicFoldIntoTheTableNamespace)
  {
      // Mirrors DetachedPathsNonAtomicFoldIntoTheTableNamespace (U#6): without an explicit anchor
      // the right-to-left part-dir scan would anchor on the INNER real part dir and fold "moving"
      // into a spurious table_uuid ("data/<db>/<table>/moving"), diverging from the table's real
      // namespace -- the identical bug class the detached anchor was added to prevent.
      auto d = parsePartFilePath("data/db/tbl/moving/all_1_1_0/data.bin");
      ASSERT_TRUE(d.has_value());
      EXPECT_EQ(d->table_uuid, "data/db/tbl");
      EXPECT_EQ(d->part_name, std::string(kMovingDirName));
      EXPECT_EQ(d->file, "all_1_1_0/data.bin");

      // The bare non-Atomic moving CONTAINER dir folds to part_name == "moving" with an empty
      // file, exactly like the Atomic container.
      auto c = parsePartFilePath("data/db/tbl/moving");
      ASSERT_TRUE(c.has_value());
      EXPECT_EQ(c->table_uuid, "data/db/tbl");
      EXPECT_EQ(c->part_name, std::string(kMovingDirName));
      EXPECT_TRUE(c->file.empty());
  }
  ```

- [ ] **Step 2: Confirm it fails**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task2_red.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task2_red.log
  ./build/src/unit_tests_dbms --gtest_filter='CaPartPathParser.MovingPathsNonAtomicFoldIntoTheTableNamespace' > build/test_task2_red.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task2_red.log
  ```

  Expected: build succeeds, but the test FAILS — `d->table_uuid` is `"data/db/tbl/moving"` instead of `"data/db/tbl"`, or `d->part_name` is `"all_1_1_0"` instead of `"moving"`. Have a subagent confirm the failure is exactly this mismatch (not a build error).

- [ ] **Step 3: Fix `findPartDirComponent`**

  In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.cpp`, change the existing detached-only anchor loop (lines 172-174):

  ```cpp
      // No uuid anchor: a non-Atomic table path. The MergeTree `detached` dir is a reserved table-level
      // subdir (data/<db>/<table>/detached/<part>/...), exactly like the Atomic layout where the uuid
      // anchor makes `detached` the part_name. Anchor on it FIRST (leftmost, index >= 1): the right-to-
      // left part-dir scan below would otherwise anchor on the INNER <part>-shaped component and fold
      // `detached` into a spurious table id (data/<db>/<table>/detached) that DROP TABLE never cleans,
      // orphaning a permanently-live ref (U#6). Mirrors route()'s part_name == kDetachedDirName folding.
      for (size_t i = 1; i < p.size(); ++i)
          if (p[i] == kDetachedDirName)
              return PartDirAnchor{0, i}; // table id = the whole path before `detached`
  ```

  to:

  ```cpp
      // No uuid anchor: a non-Atomic table path. `detached` (data/<db>/<table>/detached/<part>/...)
      // and `moving` (data/<db>/<table>/moving/<part>/...) are both reserved table-level subdirs,
      // exactly like the Atomic layout where the uuid anchor makes them the part_name for free.
      // Anchor on either FIRST (leftmost, index >= 1): the right-to-left part-dir scan below would
      // otherwise anchor on the INNER <part>-shaped component and fold the reserved dir into a
      // spurious table id (data/<db>/<table>/detached or .../moving) that DROP TABLE never cleans,
      // orphaning a permanently-live ref (U#6, extended to `moving` by the MOVE-to-CA fix). Mirrors
      // route()'s part_name == kDetachedDirName / kMovingDirName folding.
      for (size_t i = 1; i < p.size(); ++i)
          if (p[i] == kDetachedDirName || p[i] == kMovingDirName)
              return PartDirAnchor{0, i}; // table id = the whole path before the reserved dir
  ```

- [ ] **Step 4: Build and run, confirm GREEN**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build unit_tests_dbms > build/build_task2_green.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task2_green.log
  ./build/src/unit_tests_dbms --gtest_filter='CaPartPathParser.*:CaWiringRoute.*' > build/test_task2.log 2>&1; echo "TEST_EXIT=$?" >> build/test_task2.log
  ```

  Expected: `NINJA_EXIT=0`, `TEST_EXIT=0`, all `CaPartPathParser`/`CaWiringRoute` tests `[ PASSED ]` (this filter also re-runs every pre-existing detached/shadow/dedup-log test, confirming no regression). Have a subagent report a one-line summary.

- [ ] **Step 5: Commit**

  ```bash
  flock /tmp/cas_git.lock -c "git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Parts/PartPathParser.cpp src/Disks/tests/gtest_ca_wiring.cpp"
  flock /tmp/cas_git.lock -c 'git diff --cached --stat'
  ```

  Verify the stat lists ONLY those 2 files, then:

  ```bash
  flock /tmp/cas_git.lock -c "git commit -m \"\$(cat <<'EOF'
  cas: MOVE-to-CA fix L1 part 2 -- findPartDirComponent anchors \`moving\` on non-Atomic tables

  Completes L1 (see the previous commit's route() fix): on a non-Atomic (Ordinary/Memory/Lazy)
  table, \`data/<db>/<table>/moving/<part>/<file>\` had no anchor for the reserved \`moving\` subdir,
  so the right-to-left part-dir scan folded it into a spurious table id
  (\`data/<db>/<table>/moving\`) -- the same orphaning bug class (U#6) the \`detached\` anchor exists
  to prevent. Anchor \`moving\` the same way, in the same loop.

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )\""
  git -C /home/mfilimonov/workspace/ClickHouse/master log --oneline -1
  ```

---

## Phase 2 — L2: whole part in one CA transaction

### Task 3: CA-aware branch in `DataPartStorageOnDiskBase::clonePart`

L1 alone is not enough: the generic clone path (`copyDirectoryContent` → `IDisk::copyFile` → `DiskObjectStorage::writeFile`, `IDisk.cpp:76`) still writes each file through its OWN fresh autocommit transaction (`DiskObjectStorage::writeFile`, `DiskObjectStorage.cpp:936-946`, `createObjectStorageTransaction()` + `writeFileWithAutoCommit`) — a `.bin` column file throws `NOT_IMPLEMENTED` ("Autocommit writes are not supported for content part files") the moment a second file lands under the same ref. `freeze` solves this for the SAME-disk case by threading one `owned_transaction` through `Backup()` (`DataPartStorageOnDiskBase::freeze`, lines 509-597) — but `Backup()`'s transactional branch calls `transaction->copyFile` (`Backup.cpp:64`, same-disk only: `DiskObjectStorageTransaction::copyFile` → `copyFileImpl` → `generateObjectKeyForPath`, which throws `NOT_IMPLEMENTED` for CA cross-disk). `clonePart` is inherently cross-disk (read local, write CA), so a NEW helper is needed: recursively enumerate the source part's files and stream each one into ONE destination transaction via its **non-autocommit** `writeFile` (`IDiskTransaction::writeFile`, `IDiskTransaction.h:82-86` — the exact primitive `freeze` already uses for the `metadata_version.txt` write, `DataPartStorageOnDiskBase.cpp:570-576`).

**Files:**
- Modify: `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp` (new anonymous-namespace helper + `clonePart`, currently lines 655-690)
- No new test file — this is the L2 half of a fix whose end-to-end gate is the (already RED) scenario S36 in Phase 3. This task's own build + a manual smoke check (Step 5 below) are the fast feedback loop before the expensive scenario run.

**Interfaces:**
- Consumes: `ContentAddressed::kMovingDirName`/`route()` (Phase 1) — no direct call, but Phase 1 is what makes the destination transaction publish under one ref instead of colliding.
- Produces: nothing new — `clonePart`'s public signature (`IDataPartStorage.h:297-305`) is unchanged; the branch is entirely internal, gated on `dst_disk->isContentAddressed()` (`IDisk.h:477`, already `true` for a `content_addressed`-metadata `DiskObjectStorage`, `DiskObjectStorage.cpp:774-776`).

- [ ] **Step 1: Add the includes**

  In `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp`, add to the include block (currently lines 1-24):

  ```cpp
  #include <Core/Defines.h>
  #include <IO/copyData.h>
  ```

- [ ] **Step 2: Add the cross-disk copy-into-transaction helper**

  In `src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp`, add a new anonymous namespace right before `DataPartStorageOnDiskBase::clonePart` (currently line 655):

  ```cpp
  namespace
  {

  /// Recursively copy every file under `source_path` on `src_disk` into `destination_path` through
  /// `dst_transaction`'s NON-autocommit `writeFile` (IDiskTransaction::writeFile, NOT
  /// writeFileWithAutoCommit) -- the same primitive `freeze` already uses for a single file (the
  /// metadata_version.txt write, DataPartStorageOnDiskBase::freeze). Cross-disk, so it cannot reuse
  /// Backup()/BackupImpl: that helper's transactional branch calls transaction->copyFile, which is
  /// SAME-disk only (DiskObjectStorageTransaction::copyFile throws NOT_IMPLEMENTED across disks on
  /// CA), and its non-transactional branch always autocommits per file via IDisk::copyFile /
  /// copyDirectoryContent. Sequential, not the parallel copyThroughBuffers thread pool: a
  /// content-addressed transaction batches every file into ONE eventual manifest, and its staging
  /// map is not mutex-guarded; MOVE is a background, latency-insensitive operation, so
  /// parallelizing this is a deferred optimization, not a correctness requirement.
  void copyDirectoryContentIntoTransaction(
      IDisk & src_disk,
      const String & source_path,
      IDiskTransaction & dst_transaction,
      const String & destination_path,
      const ReadSettings & read_settings,
      const WriteSettings & write_settings,
      const std::function<void()> & cancellation_hook)
  {
      dst_transaction.createDirectories(destination_path);
      for (auto it = src_disk.iterateDirectory(source_path); it->isValid(); it->next())
      {
          auto source = it->path();
          auto destination = fs::path(destination_path) / it->name();

          if (src_disk.existsDirectory(source))
          {
              copyDirectoryContentIntoTransaction(
                  src_disk, source, dst_transaction, destination, read_settings, write_settings, cancellation_hook);
              continue;
          }

          auto in = src_disk.readFile(source, read_settings);
          auto out = dst_transaction.writeFile(destination, DBMS_DEFAULT_BUFFER_SIZE, WriteMode::Rewrite, write_settings);
          copyData(*in, *out, cancellation_hook);
          out->finalize();
      }
  }

  }
  ```

- [ ] **Step 3: Branch `clonePart` on `dst_disk->isContentAddressed()`**

  Replace the body of `DataPartStorageOnDiskBase::clonePart` (currently lines 655-690):

  ```cpp
  MutableDataPartStoragePtr DataPartStorageOnDiskBase::clonePart(
      const std::string & to,
      const std::string & dir_path,
      const DiskPtr & dst_disk,
      const ReadSettings & read_settings,
      const WriteSettings & write_settings,
      LoggerPtr log,
      const std::function<void()> & cancellation_hook) const
  {
      String path_to_clone = fs::path(to) / dir_path / "";
      auto src_disk = volume->getDisk();

      if (dst_disk->existsDirectory(path_to_clone))
      {
          throw Exception(ErrorCodes::DIRECTORY_ALREADY_EXISTS,
                          "Cannot clone part {} from '{}' to '{}': path '{}' already exists",
                          dir_path, getRelativePath(), path_to_clone, fullPath(dst_disk, path_to_clone));
      }

      if (dst_disk->isContentAddressed())
      {
          /// L2 (MOVE-to-CA fix): a content-addressed disk models a part as ONE atomic unit (N
          /// files -> one manifest -> one ref). The generic per-file autocommit path below would
          /// publish a separate one-file ref per file -- colliding on the shared "moving" ref
          /// before L1, and throwing NOT_IMPLEMENTED on a non-first content file even after L1
          /// ("Autocommit writes are not supported for content part files"). Run the whole clone
          /// through ONE self-created disk transaction instead, mirroring freeze's
          /// owned_transaction shape -- but streaming cross-disk bytes, since freeze's Backup() is
          /// same-disk hardlink/copyFile (throws NOT_IMPLEMENTED for CA cross-disk).
          auto clone_transaction = dst_disk->createTransaction();
          try
          {
              copyDirectoryContentIntoTransaction(
                  *src_disk, getRelativePath(), *clone_transaction, path_to_clone,
                  read_settings, write_settings, cancellation_hook);
              clone_transaction->commit();
          }
          catch (...)
          {
              LOG_WARNING(log, "Rolling back transaction after failed attempt to move a data part to {}", path_to_clone);
              clone_transaction->undo();
              throw;
          }
      }
      else
      {
          try
          {
              dst_disk->createDirectories(to);
              src_disk->copyDirectoryContent(getRelativePath(), dst_disk, path_to_clone, read_settings, write_settings, cancellation_hook);
          }
          catch (...)
          {
              /// It's safe to remove it recursively (even with zero-copy-replication)
              /// because we've just did full copy through copyDirectoryContent
              LOG_WARNING(log, "Removing directory {} after failed attempt to move a data part", path_to_clone);
              dst_disk->removeRecursive(path_to_clone);
              throw;
          }
      }

      auto single_disk_volume = std::make_shared<SingleDiskVolume>(dst_disk->getName(), dst_disk, 0);
      return create(single_disk_volume, to, dir_path, /*initialize=*/ true);
  }
  ```

- [ ] **Step 4: Build**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build clickhouse > build/build_task3.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task3.log
  ```

  Expected: `NINJA_EXIT=0`. Have a subagent read `build/build_task3.log` and report a one-line pass/fail summary (this rebuilds the full server binary, since `clonePart` is reached from the `MergeTreePartsMover` background thread, not just unit tests).

- [ ] **Step 5: Manual smoke check (fast feedback before the expensive Phase 3 scenario run)**

  This is a throwaway local check, NOT a committed test file. Start a local server with a two-disk (`default` local + inline `content_addressed`) policy and drive one `MOVE PART` by hand:

  ```bash
  mkdir -p /tmp/cas_move_smoke
  cat > /tmp/cas_move_smoke/storage.xml <<'EOF'
  <clickhouse>
      <storage_configuration>
          <disks>
              <ca_smoke>
                  <type>object_storage</type>
                  <object_storage_type>local</object_storage_type>
                  <metadata_type>content_addressed</metadata_type>
                  <server_root_id>cas-move-smoke</server_root_id>
                  <path>/tmp/cas_move_smoke/pool/</path>
                  <scratch_path>/tmp/cas_move_smoke/scratch/</scratch_path>
              </ca_smoke>
          </disks>
          <policies>
              <smoke_policy>
                  <volumes>
                      <hot><disk>default</disk></hot>
                      <cas><disk>ca_smoke</disk></cas>
                  </volumes>
              </smoke_policy>
          </policies>
      </storage_configuration>
  </clickhouse>
  EOF
  ./build/programs/clickhouse server --config-file=/etc/clickhouse-server/config.xml \
      -- --storage_configuration.disks.ca_smoke.path=/tmp/cas_move_smoke/pool/ \
      > /tmp/cas_move_smoke/server.log 2>&1 &
  # (or copy storage.xml into that server's config.d/ if the above override syntax is inconvenient
  # locally -- either way, the point is: one server, one policy with a local disk + a CA disk)
  sleep 3
  ./build/programs/clickhouse client --query "
      CREATE TABLE t_move_smoke (a UInt64, s String) ENGINE=MergeTree ORDER BY a
      SETTINGS storage_policy='smoke_policy';
      INSERT INTO t_move_smoke SELECT number, randomString(64) FROM numbers(1000);
      SELECT name, disk_name FROM system.parts WHERE table='t_move_smoke' AND active;
      ALTER TABLE t_move_smoke MOVE PARTITION ID 'all' TO DISK 'ca_smoke';
      SELECT name, disk_name FROM system.parts WHERE table='t_move_smoke' AND active;
      SELECT count(), sum(cityHash64(s)) FROM t_move_smoke;
      ALTER TABLE t_move_smoke MOVE PARTITION ID 'all' TO DISK 'default';
      SELECT name, disk_name FROM system.parts WHERE table='t_move_smoke' AND active;
      SELECT count(), sum(cityHash64(s)) FROM t_move_smoke;
  "
  ```

  Expected: no `Code: 236` / `NOT_IMPLEMENTED` exception; `disk_name` flips `default → ca_smoke → default`; the `count()`/`sum(cityHash64(s))` pair is identical before and after both moves. If this fails, fix before moving on to Phase 3 (which is far more expensive to iterate on). Tear down: `kill %1; rm -rf /tmp/cas_move_smoke`.

- [ ] **Step 6: Commit**

  ```bash
  flock /tmp/cas_git.lock -c "git add src/Storages/MergeTree/DataPartStorageOnDiskBase.cpp"
  flock /tmp/cas_git.lock -c 'git diff --cached --stat'
  ```

  Verify the stat lists ONLY that 1 file, then:

  ```bash
  flock /tmp/cas_git.lock -c "git commit -m \"\$(cat <<'EOF'
  cas: MOVE-to-CA fix L2 -- clonePart routes CA destinations through one transaction

  \`DataPartStorageOnDiskBase::clonePart\` copied a part file-by-file, each through its own
  autocommit disk transaction (\`copyDirectoryContent\` -> \`IDisk::copyFile\` ->
  \`DiskObjectStorage::writeFile\`). A content-addressed disk cannot support that: a part is ONE
  atomic unit (N files -> one manifest -> one ref), and a non-first content file throws
  NOT_IMPLEMENTED ("Autocommit writes are not supported for content part files"). Add a CA-aware
  branch, gated on \`dst_disk->isContentAddressed()\`, that creates ONE destination transaction and
  streams every file into it via the transaction's non-autocommit \`writeFile\` (the same primitive
  \`freeze\` already uses for its single metadata_version.txt write), committing once. \`freeze\`'s
  own owned_transaction path cannot be reused here: its \`Backup()\` helper is same-disk-only
  (\`transaction->copyFile\` throws NOT_IMPLEMENTED cross-disk on CA), while MOVE always streams
  bytes across disks.

  Together with the route()/findPartDirComponent identity fix (previous two commits), a moved
  part now publishes under its own final ref instead of colliding with every other in-flight move
  on the ref "moving".

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )\""
  git -C /home/mfilimonov/workspace/ClickHouse/master log --oneline -1
  ```

---

## Phase 3 — scenario gate (the authoritative RED→GREEN check)

The `S36`/`S37` scenario cards (`utils/ca-soak/scenarios/cards/s36_s37_disk_move.py`) already exist and are currently RED — `utils/ca-soak/scenarios/BACKLOG.md` (`S36-20260716T201906-1`) records the exact bug this plan fixes: `Code: 236. DB::Exception: promote: ref 'moving' already names a different committed manifest`. Both `S36`'s `compose_variant = "multidisk"` and the underlying `docker-compose-multidisk.yml` + `configs/storage_conf_multidisk_ch{1,2}.xml` are already wired into the framework (`utils/ca-soak/scenarios/framework/cluster_boot.py` already knows the `multidisk` variant) — running `S36` needs no new compose plumbing.

### Task 4: add the dedup-on-TO-CA assertion to `S36`

The spec requires a specific check that a MOVE of byte-identical content dedups instead of re-uploading. `S36`'s existing `blob_puts > 0` check (lines 226-234 of the scenario file) only proves the build path ran, not that a SECOND, duplicate move skips the upload. Add a dedicated leg using two tables with byte-identical (deterministic, non-`randomString`) content.

**Files:**
- Modify: `utils/ca-soak/scenarios/cards/s36_s37_disk_move.py`

**Interfaces:**
- Consumes: `_common.counters_window(ctx)` (existing, `utils/ca-soak/scenarios/cards/_common.py:62-71`), `sql.create_ca_table`/`sql.insert_values` (existing, already used throughout this file).
- Produces: nothing new — an added assertion inside `S36.run`.

- [ ] **Step 1: Add the dedup leg**

  In `utils/ca-soak/scenarios/cards/s36_s37_disk_move.py`, insert this block into `S36.run`, right after the GC-reclaim assertion for the OFF-CA leg (currently ends at line 296, `"content vacated by the OFF-CA move was not fully reclaimed"))`) and before the `# --- chaos leg` comment (currently line 298):

  ```python
          # --- dedup-on-TO-CA: moving a part whose content already exists in the pool must dedup,
          # not re-upload -------------------------------------------------------------------------
          dedup_table_a = "s36_dedup_a"
          dedup_table_b = "s36_dedup_b"
          dedup_rows = 200
          for t in (dedup_table_a, dedup_table_b):
              for n in cl.nodes():
                  sql.create_ca_table(n, t, columns="id UInt64, payload String", order_by="id",
                                      extra_settings={"storage_policy": "'ca_local'"})
          # A deterministic (non-random) payload so table B's part is BYTE-IDENTICAL to table A's
          # part: repeat() is the same on every call, unlike randomString() (which the rest of this
          # scenario relies on being unique per part, to keep unrelated dedup out of the other
          # assertions above).
          dedup_gen = (f"SELECT number AS id, repeat('cas-move-dedup-probe-', 100) AS payload "
                      f"FROM numbers({dedup_rows})")
          sql.insert_values(cl.node1, dedup_table_a, dedup_gen, timeout=300)
          sql.insert_values(cl.node1, dedup_table_b, dedup_gen, timeout=300)
          cl.node1.command(f"SYSTEM SYNC REPLICA {dedup_table_a}", timeout=120)
          cl.node1.command(f"SYSTEM SYNC REPLICA {dedup_table_b}", timeout=120)

          # Move table A's part to CA first -- pays the real upload cost. Table B's part is
          # byte-identical, so ITS move must dedup-resolve the blobs instead of re-uploading them.
          cl.node1.command(f"ALTER TABLE {dedup_table_a} MOVE PARTITION ID 'all' TO DISK 'ca'", timeout=300)

          counters_dedup = _common.counters_window(ctx)
          cl.node1.command(f"ALTER TABLE {dedup_table_b} MOVE PARTITION ID 'all' TO DISK 'ca'", timeout=300)
          dedup_delta = counters_dedup().get("_total", {})
          raw_puts = int(dedup_delta.get("CasBlobPut", 0))
          dedup_puts = int(dedup_delta.get("CasBlobPutDedup", 0))
          result.observations["dedup_on_to_ca_counters"] = {"CasBlobPut": raw_puts, "CasBlobPutDedup": dedup_puts}
          dedup_ok = dedup_puts > 0 and raw_puts == 0
          result.add(Verdict.check(
              "MOVE TO-CA of byte-identical content dedups instead of re-uploading",
              "CasBlobPutDedup > 0 and CasBlobPut == 0 for the second (duplicate) move",
              f"CasBlobPut={raw_puts} CasBlobPutDedup={dedup_puts}", dedup_ok,
              "" if dedup_ok else
              "the second MOVE of byte-identical content re-uploaded blobs instead of dedup-resolving them"))

          oracle_dedup_a = cl.node1.query(sql.table_checksum_query(dedup_table_a)).strip()
          oracle_dedup_b = cl.node1.query(sql.table_checksum_query(dedup_table_b)).strip()
          result.add(Verdict.check(
              "dedup-probe tables read back identical data after the TO-CA moves",
              oracle_dedup_a, oracle_dedup_b, oracle_dedup_a == oracle_dedup_b))

  ```

- [ ] **Step 2: Syntax-check the file**

  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak && python3 -m py_compile scenarios/cards/s36_s37_disk_move.py && echo COMPILE_OK
  ```

  Expected: `COMPILE_OK`, no output before it.

- [ ] **Step 3: Commit**

  ```bash
  flock /tmp/cas_git.lock -c "git add utils/ca-soak/scenarios/cards/s36_s37_disk_move.py"
  flock /tmp/cas_git.lock -c 'git diff --cached --stat'
  ```

  Verify the stat lists ONLY that 1 file, then:

  ```bash
  flock /tmp/cas_git.lock -c "git commit -m \"\$(cat <<'EOF'
  cas: S36 -- add the dedup-on-TO-CA assertion (byte-identical content must not re-upload)

  The existing blob_puts > 0 check only proves the TO-CA move published through the normal build
  path, not that a SECOND move of already-present content skips the upload. Add a dedicated leg:
  two tables get the exact same deterministic (repeat(), not randomString()) content, table A
  moves to 'ca' first (pays the real upload), then table B's byte-identical move is asserted to
  dedup (CasBlobPutDedup > 0, CasBlobPut == 0) instead of re-uploading.

  Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27
  EOF
  )\""
  git -C /home/mfilimonov/workspace/ClickHouse/master log --oneline -1
  ```

### Task 5: run `S36` to GREEN

**Files:** none (execution-only task; any fix required by a red finding goes back to Phase 1/2 as a new commit, not into this task).

- [ ] **Step 1: Build the server binary with all of Phase 1+2+Task 4 applied**

  ```bash
  flock /tmp/cas_build.lock -c "ninja -C build clickhouse > build/build_task5.log 2>&1"; echo "NINJA_EXIT=$?" >> build/build_task5.log
  ```

  Confirm `NINJA_EXIT=0` via a subagent reading `build/build_task5.log`.

- [ ] **Step 2: Remount the rebuilt binary into the ca-soak cluster and run S36**

  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
  # down -v for a clean pool + data dir; down/up remounts the freshly-built binary (bind-mounted,
  # not baked into the image) -- see reference_ca_soak_fresh_restart conventions.
  python3 -m scenarios.run --scenario S36 --scale dev --seed 1 > /home/mfilimonov/workspace/ClickHouse/master/build/s36_run.log 2>&1
  echo "RUN_EXIT=$?" >> /home/mfilimonov/workspace/ClickHouse/master/build/s36_run.log
  ```

  Expected: `RUN_EXIT=0`, and every `Verdict.check(...)` in the run's `runs/<ts>_S36_seed1/` report is `pass` — in particular: the `Code 236` promote collision is gone, both `MOVE PART`/`MOVE PARTITION` directions succeed, concurrent `SELECT`s see 0 errors, `fsck` `dangling==0` after each leg, GC reclaims the vacated side (residual 0), the chaos (kill mid-move) leg reports a consistent single copy, and the new dedup-on-TO-CA assertion from Task 4 passes.

  Have a subagent read `build/s36_run.log` and the run's own report/BACKLOG entry (if any FAIL was appended) and return a concise per-verdict pass/fail summary — not the raw log.

- [ ] **Step 3: If red, fix and re-run**

  Any failure here means Phase 1 or Phase 2's fix is incomplete or has a bug the unit gtests / smoke check did not catch (e.g. a projection sub-directory, a verbatim table-level file, or the chaos/restart interaction with an in-flight CA transaction). Diagnose via `superpowers:systematic-debugging`, land the fix as a NEW commit against the relevant Phase 1/2 file (never amend), rebuild, and re-run Step 2 until GREEN. Do not proceed to Task 6 while `S36` is red.

- [ ] **Step 4: Record the result**

  No code change in this step. If `S36` needed a fix, that fix's own commit (from Step 3) already closes the loop — this step is just the "confirmed GREEN" checkpoint before moving on. Report to the plan owner: `S36` run id (`runs/<ts>_S36_seed1/`), pass/fail count, and whether any Phase 1/2 fix commit was needed along the way.

### Task 6: run `S37` as a regression check

`S37` already passes 22/23 (per project memory) and exercises the SAME `clonePart` code path via policy/TTL-triggered moves and a mixed-disk merge. This task confirms Phase 1+2 did not regress it, and explicitly leaves the `CA↔CA`-move leg as backlog (§Deferred in the design spec) — no new assertion is added for it here.

**Files:** none (execution-only task).

- [ ] **Step 1: Run S37 against the same rebuilt binary**

  ```bash
  cd /home/mfilimonov/workspace/ClickHouse/master/utils/ca-soak
  python3 -m scenarios.run --scenario S37 --scale dev --seed 1 > /home/mfilimonov/workspace/ClickHouse/master/build/s37_run.log 2>&1
  echo "RUN_EXIT=$?" >> /home/mfilimonov/workspace/ClickHouse/master/build/s37_run.log
  ```

- [ ] **Step 2: Confirm no regression**

  Have a subagent read `build/s37_run.log` and the run's report, and compare the pass count against the pre-existing 22/23 baseline (project memory: the one known-red leg is the `CA↔CA`-move placement, which this plan explicitly defers — see §Deferred below). Report: pass/fail count, and whether the ONLY red leg (if any) is the already-known deferred one. Any OTHER new red leg is a regression — diagnose via `superpowers:systematic-debugging` and fix as a new commit against Phase 1/2 before considering this plan done.

---

## Deferred / non-goals (do not implement — see design spec `§Deferred`/`§Non-goals`)

- **`CA↔CA` same-pool move** (moving a part between two CA disks in the same pool): expected to likely work via the same L1+L2 code with no special-casing (the target publish dedup-resolves already-pooled content). The one open question — whether the target's final ref `<part>` collides benignly with the source's existing ref `<part>` — is a **backlog-verify** item, not implemented by this plan. If a future verification run finds a real collision, that becomes its own spec/plan.
- **Parallel cross-disk copy-into-transaction**: `copyDirectoryContentIntoTransaction` (Task 3) is deliberately sequential. Do not parallelize it as part of this plan; only revisit if a real large-part move latency problem is measured.
- **`CA→local`** and **insert-time policy routing**: both already work (destination is a plain `DiskLocal`, no ref/manifest, no collision) — untouched by this plan.

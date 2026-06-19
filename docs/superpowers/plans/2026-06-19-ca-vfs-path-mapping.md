# CAS Disk — Path-Mirroring Layout & Browsable Introspection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Recompose the content-addressed (CA) MergeTree S3 layout so the pool mirrors the ClickHouse disk tree (browsable with stock tools), with a self-describing `@cas@` content-addressing boundary, and consolidate per-server control objects under one `roots/<server>/` subtree — without touching the GC algorithm, the manifest/journal format, the fence/retire protocol, or any TLA+ model.

**Architecture:** The CA disk is a `DiskObjectStorage` whose metadata layer (`ContentAddressedMetadataStorage` + `ContentAddressedTransaction`, "the wiring") maps ClickHouse disk paths onto a content-addressed object pool. `RootNamespace` is *opaque to the core*: GC, the manifest/journal CAS protocol, and `tryParseRootShardKey` never interpret it. This refactor therefore changes (a) namespace-string composition in the wiring (`liveNamespace`/`detachedNamespace`/`route`), (b) a few `Cas::Layout` key constructors for by-key control objects (registry, watermark), and (c) one GC-code edit set (precommit relocation, separable). Everything else is wiring-only. The regression oracle is the green CA gtest suite + the CA stateless lane; every task is red → implement → green → CA-lane → commit.

**Tech Stack:** C++ (Allman braces), GoogleTest (`unit_tests_dbms`), `ninja`, the praktika test harness (`ci.praktika`), the `utils/ca-soak` chaos harness.

**Safety contract (spec §4 invariants N1–N7) — DO NOT VIOLATE:**
- N1 — `blobs/`/`trees/`/`packs/` key scheme + hashing: unchanged.
- N2 — GC algorithm (reachability, fence rounds, retire/epoch, in-degree snapshots, scheduler): unchanged.
- N3 — root-shard manifest + journal format and CAS publish protocol: unchanged.
- N4 — watermark + precommit/build-root protocol & ordering: *keys may move*, protocol may not.
- N5 — registry mechanism (CAS-append on `W-REGISTER`, GC fencing, authoritative discovery): *key may move*, mechanism may not.
- N6 — `RefPayload.mutable_files` overlay + ref/tree/blob data model: unchanged.
- N7 — TLA+ models and proven invariants: no re-floor, no re-model.

**Non-nesting invariant (critical, spec §5.7):** no registered namespace may be a path-prefix of another. The new scheme preserves this: `…/store/…@cas@` vs `…/detached/store/…@cas@` diverge at `store`/`detached`. Detached parts therefore stay a *sibling* namespace (not folded under the live table). Verify this property in tests, never break it.

---

## File Structure

Map of every file this plan creates or modifies, with its single responsibility.

| File | Responsibility | Touched in |
|---|---|---|
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h` | The `@cas@` suffix constant `kCasArchiveSuffix`; declarations unchanged otherwise. | Phase 1 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp` | Namespace composers (`liveNamespace`/`detachedNamespace`), `route`, the read-surface generic fallthroughs (`existsFile`/`getStorageObjects`/`tryGetInManifestBytes`/`listDirectory`), removal of `genericNamespace`. | Phases 1–4 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h` | Method decls: drop `genericNamespace`, thread the table disk-path into `liveNamespace`/`detachedNamespace`; comment block. | Phases 1, 2 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp` | `writeFile` non-table branch (plain mountpoint object), `moveFile`/`removeFile` generic fallthroughs. | Phase 2 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h` | `@cas@`-gated `tryParseRootShardKey`; `rootsRegistryKey` → `gc/registry`; `serverWatermarkKey` → `roots/<server>/_watermark`; `precommitNamespacePrefix`; `isBuildRootNamespace` (renamed concept "precommit"); `checkNamespace` reservation edits; plain-mountpoint-object key helper. | Phases 1, 3, 6 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` / `.cpp` | Plain mountpoint object put/get/remove (`putObject`-at-mirrored-path); shadow enumeration via scoped LIST. | Phases 2, 3 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp` | `buildRootNs` → `precommitNs` (returns `roots/<server>/_precommits`); noun rename `build`→`precommit`. | Phase 6 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp` | `watermarkOf` (by-key, no behavior change), `reclaimAbandonedPrecommit` owner derivation from the shared `<server>` token, precommit discovery via `gc/precommits`. | Phase 6 |
| `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h`, `Core/CasBuild.h`, `Core/CasEvent*.h`, `Core/CasRootShardCodec.*` | Comment-only: remove "part" vocabulary from the generic layer; keep `ref`. | Phase 5 |
| `src/Disks/tests/gtest_cas_layout.cpp` | Unit tests for namespace composition, `@cas@`-gated `tryParseRootShardKey`, relocated keys. | Phases 1, 3, 6 |
| `src/Disks/tests/gtest_cas_store.cpp` | Unit tests for plain-mountpoint objects and shadow scoped-LIST enumeration. | Phases 2, 3 |
| `docs/superpowers/specs/2026-06-19-ca-vfs-contract.md` | NEW. The VFS contract: entities, mutability invariant, path grammar, listing/merge semantics. | Phase 5 |

---

## Conventions used by every task

**Build (always redirect, always summarize via a subagent):**

```bash
mkdir -p tmp
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
```

After each `ninja`, dispatch a subagent to read the log and return only a concise pass/fail + first error. Do not paste the raw log.

**CA gtests (redirect to a per-test log in the build dir):**

```bash
./build/src/unit_tests_dbms --gtest_filter='CasLayout.*' > build/test_cas_layout.log 2>&1; echo "exit=$?"
```

Then a subagent reads `build/test_cas_layout.log` and returns the pass/fail summary.

**CA stateless lane (from repo root; binary symlinked at `ci/tmp/clickhouse`):**

```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test 04036_backup_partition_transaction_visibility > build/ca_lane_04036.log 2>&1; echo "exit=$?"
```

A subagent reads the lane log and returns the pass/fail summary.

**Style:** Allman braces. Never use `sleep` to fix a race. Wrap literal SQL/class/function names in inline code in comments.

---

## Phase 1 — Path-mirroring namespaces + the `@cas@` suffix (spec §5.1)

Recompose namespaces so the namespace string IS the table's canonical disk path with `@cas@` appended to the table-dir segment, and gate root-shard parsing on `@cas@`.

### Resolved design decisions for Phase 1 (read before starting)

- **Atomic vs non-Atomic path mirroring.** The spec says live = `<server>/store/<u3>/<uuid>@cas@`. But the parser supports two layouts: Atomic (`store/<u3>/<uuid>/…`) and non-Atomic (`data/<db>/<tbl>/…`). `parsePartFilePath` carries only `table_uuid` (the bare `<uuid>` for Atomic, the joined `data/db/table` string for non-Atomic — see `findTableUuidComponent` and the fallback in `PartPathParser.cpp`). For Atomic, `<table_uuid>` is the bare uuid → we must reconstruct `store/<u3>/<uuid>` where `<u3>` is the first 3 chars. For non-Atomic, `<table_uuid>` is already the full `data/db/table` joined path → it is used verbatim. The composer below branches on this. This mirrors `shadowNamespace`, which already passes the LITERAL shadow table dir through `canonicalDiskPath` (the path-based precedent). We add `@cas@` to the LAST segment in both cases.
- **`@cas@` is a suffix, not a segment.** It is appended to the table-dir name (`…/<uuid>@cas@`), never `…/@cas@/…`. `@` is S3-safe and never occurs in uuids, part names, detached prefixes, projection names, or column files.
- **`@cas@`-scoped shard parsing.** Today `tryParseRootShardKey` classifies *any* numeric tail under `roots/` as a shard, so a future plain object `roots/srv1/foo/7` would be mis-parsed. The fix: a key is a shard only when its namespace's **last segment ends in `@cas@`** OR it is the precommit namespace (handled in Phase 6; for Phase 1 we gate on `@cas@` only and keep the existing `_builds/` path until Phase 6). This makes the grammar correct for all consumers (GC, fsck, raw tooling), not just registry-driven GC.

### Task 1.1: Add the `@cas@` suffix constant

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h:24` (after `kDetachedDirName`)
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_layout.cpp`)

```cpp
TEST(CasLayout, CasArchiveSuffixConstant)
{
    EXPECT_EQ(DB::ContentAddressed::kCasArchiveSuffix, "@cas@");
}
```

Add the include at the top of `gtest_cas_layout.cpp` (after the existing `CasLayout.h` include):

```cpp
#include <Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h>
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.CasArchiveSuffixConstant' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — compile error, `kCasArchiveSuffix` is not a member of `DB::ContentAddressed`.

- [ ] **Step 3: Add the constant** (in `PartPathParser.h`, immediately after the `kDetachedDirName` declaration block at line 24)

```cpp
/// The content-addressing boundary marker: a SUFFIX on a table-dir segment (`…/<uuid>@cas@`), not a
/// path segment. It marks where the mirrored ClickHouse path ends and the content-addressed archive
/// begins — like a `.zip` extension (`foo.zip/inner/file`). `@` is S3-safe and never occurs in
/// ClickHouse uuids, part names, detached prefixes, projection names, or column files, so it cannot
/// collide with real path data. Root-shard parsing is gated on this suffix (see
/// `Cas::Layout::tryParseRootShardKey`).
inline constexpr std::string_view kCasArchiveSuffix = "@cas@";
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.CasArchiveSuffixConstant' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: PASS (1 test).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h src/Disks/tests/gtest_cas_layout.cpp
git commit -m "$(cat <<'EOF'
CA VFS: add @cas@ archive-suffix constant

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 1.2: `@cas@`-gate `tryParseRootShardKey`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h:151-178`
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_layout.cpp`)

```cpp
TEST(CasLayout, TryParseRootShardKeyCasGated)
{
    Layout l("p");
    /// Positive: a @cas@ archive directory with a numeric tail is a shard.
    auto a = l.tryParseRootShardKey("p/roots/srv1/store/3f2/3f2a-uuid@cas@/7");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->first, RootNamespace{"srv1/store/3f2/3f2a-uuid@cas@"});
    EXPECT_EQ(a->second, 7u);
    /// Negative: a plain mountpoint object with a numeric tail but NO @cas@ ancestor is opaque,
    /// never a shard (the bug the gating fixes).
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/foo/7").has_value());
    /// Negative: the @cas@ archive's verbatim-file area is still excluded.
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/store/3f2/3f2a-uuid@cas@/_files/123").has_value());
    /// Negative: a numeric tail directly under a non-@cas@ namespace segment.
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/store/3f2/3f2a-uuid/7").has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.TryParseRootShardKeyCasGated' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — `tryParseRootShardKey("p/roots/srv1/foo/7")` currently returns a value (mis-parsed).

- [ ] **Step 3: Gate the classifier** (replace the body of `tryParseRootShardKey` in `CasLayout.h:151-178`). The change adds the `@cas@` requirement on `last_ns_segment` while keeping the existing `_files`/empty rejections:

```cpp
    std::optional<std::pair<RootNamespace, uint64_t>> tryParseRootShardKey(const String & key) const
    {
        const String roots = rootsPrefix();
        if (!key.starts_with(roots))
            return std::nullopt;
        const std::string_view rest(key.data() + roots.size(), key.size() - roots.size());

        const size_t last_slash = rest.rfind('/');
        if (last_slash == std::string_view::npos || last_slash == 0 || last_slash + 1 == rest.size())
            return std::nullopt;                       /// need "<ns>/<tail>" with both parts non-empty

        const std::string_view tail = rest.substr(last_slash + 1);
        uint64_t shard = 0;
        const auto [end, ec] = std::from_chars(tail.data(), tail.data() + tail.size(), shard);
        if (ec != std::errc() || end != tail.data() + tail.size())
            return std::nullopt;                       /// non-numeric (or overflow) tail is not a shard

        const std::string_view ns = rest.substr(0, last_slash);
        const size_t prev_slash = ns.rfind('/');
        const std::string_view last_ns_segment = prev_slash == std::string_view::npos ? ns : ns.substr(prev_slash + 1);
        if (last_ns_segment == "_files")
            return std::nullopt;
        if (last_ns_segment.empty())
            return std::nullopt;                       /// empty ns segment ("a//7", "//7") — Layout never writes these

        /// @cas@-scoped shard parsing (design §5.1/§5.2): a key is a root-shard manifest ONLY when its
        /// namespace's last segment is a content-addressed archive directory (`…@cas@`). A plain
        /// mountpoint object with a numeric tail and no `@cas@` ancestor (`roots/srv1/foo/7`) is an
        /// opaque ordinary file and is NEVER classified as a shard. The precommit namespace
        /// (Phase 6) is admitted by the same rule once it gains its own predicate.
        if (!last_ns_segment.ends_with("@cas@") && !isPrecommitNamespaceSegment(last_ns_segment))
            return std::nullopt;

        return std::make_pair(RootNamespace{String(ns)}, shard);
    }
```

Add this private static helper just below `tryParseRootShardKey` (Phase 1 stub; Phase 6 fills it in — for now `_precommits` does not yet exist, so it returns false and the gate is purely `@cas@`):

```cpp
    /// True iff a namespace's last segment is the per-server precommit area (`_precommits`). Phase 1
    /// stub: returns false until Phase 6 relocates precommits under `roots/<server>/_precommits`.
    /// Until then build-root shards live at `_builds/<server_hex>/<N>`, classified by the
    /// `_builds`-prefix path below.
    static bool isPrecommitNamespaceSegment(std::string_view) { return false; }
```

> **NOTE — keep `_builds/` shards classifiable in Phase 1.** Build-root shards still live at
> `_builds/<server_hex>/<N>` until Phase 6. Their last namespace segment is `<server_hex>` (no
> `@cas@`), so the new gate would stop classifying them. To avoid breaking GC discovery between
> phases, the gate must also admit a `_builds/`-rooted namespace. Replace the final condition with:

```cpp
        const bool is_cas_archive = last_ns_segment.ends_with("@cas@");
        const bool is_legacy_build_root = ns.starts_with("_builds/");   /// removed in Phase 6
        if (!is_cas_archive && !is_legacy_build_root && !isPrecommitNamespaceSegment(last_ns_segment))
            return std::nullopt;
```

- [ ] **Step 4: Run tests to verify pass** (run the whole `CasLayout` suite to confirm no regression in the existing `TryParseRootShardKey` test — note: the existing test at line 66 uses `p/roots/srv1/3f2e-uuid/7` which will now FAIL the gate; it must be updated to use a `@cas@` namespace).

First update the existing positive case in `TEST(CasLayout, TryParseRootShardKey)` (line 66-69) to be `@cas@`-scoped:

```cpp
    auto a = l.tryParseRootShardKey("p/roots/srv1/3f2e-uuid@cas@/7");
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->first, RootNamespace{"srv1/3f2e-uuid@cas@"});
    EXPECT_EQ(a->second, 7u);
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/tbl@cas@/_files/format_version.txt").has_value());
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/tbl@cas@/_files/123").has_value());
    EXPECT_FALSE(l.tryParseRootShardKey("p/roots/srv1/tbl@cas@/notashard").has_value());
```

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.*' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: PASS (all `CasLayout.*` tests).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h src/Disks/tests/gtest_cas_layout.cpp
git commit -m "$(cat <<'EOF'
CA VFS: gate tryParseRootShardKey on the @cas@ archive boundary

Root-shard classification now requires the namespace's last segment to be a
content-addressed archive (`…@cas@`) so plain mountpoint objects with numeric
tails are never mis-parsed as shards. `_builds/` legacy build-roots stay
classifiable until Phase 6.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 1.3: Path-mirroring `liveNamespace` / `detachedNamespace`

The composers must mirror the canonical disk path with `@cas@` appended. They currently take only `table_uuid`; the parser already gives us the right form (bare uuid for Atomic, full `data/db/table` for non-Atomic), so the composer reconstructs `store/<u3>/<uuid>` for the Atomic case.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:134-137`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:389-415`
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_layout.cpp`). These are pure-string tests of a free helper we extract so the composition is unit-testable without a `ContentAddressedMetadataStorage` instance.

```cpp
TEST(CasVfsPaths, MirroredArchiveNamespace)
{
    using DB::ContentAddressed::mirroredArchiveNamespace;
    /// Atomic: bare uuid -> store/<u3>/<uuid>@cas@
    EXPECT_EQ(mirroredArchiveNamespace("3f2a0000-0000-0000-0000-000000000001"),
              "store/3f2/3f2a0000-0000-0000-0000-000000000001@cas@");
    /// Non-Atomic: a full data/db/tbl path is used verbatim, @cas@ appended to the last segment.
    EXPECT_EQ(mirroredArchiveNamespace("data/mydb/events"),
              "data/mydb/events@cas@");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasVfsPaths.MirroredArchiveNamespace' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — compile error, `mirroredArchiveNamespace` is undeclared.

- [ ] **Step 3: Add the free helper + rewire the composers.**

In `PartPathParser.h`, after `kCasArchiveSuffix`, declare:

```cpp
/// Compose the mirrored content-addressed archive path for a table identifier as the parser reports
/// it. Atomic tables report the bare `<uuid>` → reconstruct `store/<u3>/<uuid>@cas@` (u3 = first 3
/// chars, matching ClickHouse's store fanout). Non-Atomic tables report the full joined
/// `data/<db>/<tbl>` path → append `@cas@` to it verbatim. The `@cas@` suffix lands on the
/// table-dir (last) segment in both cases. Pure; no ClickHouse dependency.
std::string mirroredArchiveNamespace(const std::string & table_uuid);
```

In `PartPathParser.cpp`, add the definition (the layout discriminator: a bare Atomic uuid contains no `/`; a non-Atomic identifier is a `data/…` joined path):

```cpp
std::string mirroredArchiveNamespace(const std::string & table_uuid)
{
    if (table_uuid.find('/') == std::string::npos)
    {
        /// Atomic: a bare uuid; mirror ClickHouse's store/<u3>/<uuid> fanout.
        const std::string u3 = table_uuid.substr(0, 3);
        return "store/" + u3 + "/" + table_uuid + std::string(kCasArchiveSuffix);
    }
    /// Non-Atomic: a full data/<db>/<tbl> path already; append the suffix to the last segment.
    return table_uuid + std::string(kCasArchiveSuffix);
}
```

In `ContentAddressedMetadataStorage.cpp`, rewrite the composers (lines 391-415). Delete `genericNamespace` in this task only from the header/impl IF Phase 2 has not run yet — but Phase 2 owns its removal, so here we ONLY rewrite live/detached and leave `genericNamespace` untouched:

```cpp
Cas::RootNamespace ContentAddressedMetadataStorage::liveNamespace(const std::string & table_uuid) const
{
    /// Path-mirroring (design §5.1): the namespace IS the table's canonical disk path with the
    /// content-addressed boundary marked by `@cas@` on the table-dir segment, prefixed by the
    /// server id. e.g. `srv1/store/3f2/3f2a…@cas@`.
    return Cas::RootNamespace{server_id + "/" + ContentAddressed::mirroredArchiveNamespace(table_uuid)};
}

Cas::RootNamespace ContentAddressedMetadataStorage::detachedNamespace(const std::string & table_uuid) const
{
    /// A SIBLING archive under `detached/` — preserves the non-nesting invariant (design §5.7):
    /// `…/store/…@cas@` and `…/detached/store/…@cas@` diverge at `store`/`detached`, so no
    /// namespace is a path-prefix of another and GC's per-namespace prefix-LIST never crosses.
    return Cas::RootNamespace{server_id + "/detached/" + ContentAddressed::mirroredArchiveNamespace(table_uuid)};
}
```

> **Non-Atomic detached caveat (resolved).** For a non-Atomic `data/db/tbl` identifier, the detached
> form becomes `srv1/detached/data/db/tbl@cas@`. The live form is `srv1/data/db/tbl@cas@`. These
> diverge at the second segment (`detached` vs `data`), so the non-nesting invariant holds for both
> layouts. (For Atomic they diverge at `detached` vs `store`.)

- [ ] **Step 4: Run tests to verify pass**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasVfsPaths.*:CasLayout.*' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartPathParser.cpp \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
        src/Disks/tests/gtest_cas_layout.cpp
git commit -m "$(cat <<'EOF'
CA VFS: path-mirroring live/detached namespaces with @cas@ boundary

liveNamespace/detachedNamespace now mirror the table's canonical disk path
(Atomic store/<u3>/<uuid>@cas@, non-Atomic data/db/tbl@cas@), via the pure
mirroredArchiveNamespace helper. Detached stays a sibling archive (non-nesting
invariant preserved for both layouts).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 1.4: Build, full CA gtest suite, CA stateless lane (Phase 1 regression gate)

- [ ] **Step 1: Build the binary and unit tests**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
```
Dispatch a subagent per log; expect both `exit=0` and no errors.

- [ ] **Step 2: Run all CA gtests**

Run: `./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all.log 2>&1; echo "exit=$?"`
Subagent reads `build/test_cas_all.log`; expect all PASS.

- [ ] **Step 3: Run the CA stateless lane (backup-visibility + a broad smoke)**

Run:
```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test "04036_backup_partition_transaction_visibility" > build/ca_lane_phase1.log 2>&1; echo "exit=$?"
```
Subagent reads `build/ca_lane_phase1.log`; expect PASS.

- [ ] **Step 4: Commit a checkpoint (no code change; a tag commit is optional — skip if nothing changed).** If Steps 1-3 surfaced fixes, commit them with message `CA VFS: Phase 1 fixes from regression gate`.

---

## Phase 2 — Eliminate `_disk`/`genericNamespace`: loose files are plain mountpoint objects (spec §5.2)

Non-table, non-part files become plain S3 objects at their mirrored path `roots/<server>/<path>`, with no namespace and no `_files` wrapper. The `@cas@`-scoped parsing invariant (Phase 1) already makes such loose objects opaque to shard classification.

### Resolved design decision for Phase 2

The store currently has no "plain object at a key under roots" API — only `putNamespaceFile`/`getNamespaceFile`/`removeNamespaceFile` (which wrap into `…/_files/…`). We add a thin trio `putMountpointObject`/`getMountpointObject`/`removeMountpointObject` on `Cas::Store` that write/read/delete a plain object at `roots/<key>` via the backend directly (no manifest, no journal, no content-addressing). The wiring passes `server_id + "/" + path` as the key. GC never scans these (it deletes only `blobs/`/`trees/`/`packs/` and folds only registered namespaces), so they are owned by their path and removed only by `removeFile` — exactly the spec's GC-safety argument.

### Task 2.1: Add plain mountpoint-object API to `Cas::Store` and `Cas::Layout`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h` (new `mountpointObjectKey`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` / `.cpp`
- Test: `src/Disks/tests/gtest_cas_store.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_store.cpp`, using the existing store fixture pattern — inspect the top of `gtest_cas_store.cpp` and `cas_test_helpers.h` for the `makeStore`/open helper used by sibling tests and reuse it verbatim).

```cpp
TEST(CasStore, MountpointObjectRoundTrip)
{
    auto store = DB::Cas::tests::openFreshStoreForTest();   /// reuse the existing fixture helper
    const DB::String key = "srv1/clickhouse_access_check_abc";
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
    store->putMountpointObject(key, "probe-bytes");
    auto got = store->getMountpointObject(key);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "probe-bytes");
    store->removeMountpointObject(key);
    EXPECT_FALSE(store->getMountpointObject(key).has_value());
}
```

> If `openFreshStoreForTest` does not exist under that name, use whatever fresh-store fixture the
> neighbouring `CasStore` tests already call (grep `gtest_cas_store.cpp` for `Store::open` / the
> first `TEST(CasStore` body and copy its setup).

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasStore.MountpointObjectRoundTrip' > build/test_cas_store.log 2>&1; echo "exit=$?"`
Expected: FAIL — compile error, `putMountpointObject` undeclared.

- [ ] **Step 3: Implement.**

In `CasLayout.h`, after `namespaceFilesPrefix`, add:

```cpp
    /// A PLAIN mountpoint object (design §5.2): a loose, non-content-addressed file mirrored at its
    /// ClickHouse path under `roots/`, with NO namespace and NO `_files` wrapper. `key` is the
    /// server-prefixed mirrored path (e.g. `srv1/clickhouse_access_check_abc`). It must NOT end in a
    /// reserved area and must not look like a shard — the `@cas@`-gated `tryParseRootShardKey`
    /// guarantees a numeric tail here is never mis-classified. The `_files`/`_pool_meta`/`_registry`
    /// reservations still apply to its segments via the path itself (these never appear in a real
    /// ClickHouse loose-file path).
    String mountpointObjectKey(const String & key) const
    {
        if (key.empty() || key.front() == '/' || key.back() == '/' || key.find("//") != String::npos)
            throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                "CasLayout: mountpoint object key must be a clean relative path, got '{}'", key);
        return prefix + "/roots/" + key;
    }
```

In `CasStore.h`, after the verbatim-namespace-file block (near line 122):

```cpp
    /// ---- plain mountpoint objects (loose, non-content-addressed disk files; design §5.2) ----
    /// A loose disk file (the startup write probe; anything written outside a `@cas@` archive) is a
    /// plain object at its mirrored path `roots/<key>`. No manifest, no journal, no dedup. GC never
    /// scans these (it deletes only content and folds only registered namespaces); they are owned by
    /// their path and removed only by `removeMountpointObject`.
    void putMountpointObject(const String & key, const String & bytes);
    std::optional<String> getMountpointObject(const String & key);
    void removeMountpointObject(const String & key);
```

In `CasStore.cpp`, implement (mirror the existing `putNamespaceFile`/`getNamespaceFile`/`removeNamespaceFile` backend-call shape — grep those three for the exact `pool_backend->put/get/remove` call signature and copy it; the only difference is the key constructor):

```cpp
void Store::putMountpointObject(const String & key, const String & bytes)
{
    pool_backend->put(pool_layout.mountpointObjectKey(key), bytes);
}

std::optional<String> Store::getMountpointObject(const String & key)
{
    if (const std::optional<GetResult> got = pool_backend->get(pool_layout.mountpointObjectKey(key)))
        return got->bytes;
    return std::nullopt;
}

void Store::removeMountpointObject(const String & key)
{
    pool_backend->remove(pool_layout.mountpointObjectKey(key));
}
```

> Verify the exact `Backend` method names/signatures against the existing `putNamespaceFile`
> definition in `CasStore.cpp` (around the verbatim-file section) and match them — do not invent
> `put`/`get`/`remove` if the backend uses different names.

- [ ] **Step 4: Run test to verify it passes**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasStore.MountpointObjectRoundTrip' > build/test_cas_store.log 2>&1; echo "exit=$?"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h \
        src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.cpp \
        src/Disks/tests/gtest_cas_store.cpp
git commit -m "$(cat <<'EOF'
CA VFS: plain mountpoint-object store API (loose disk files)

Adds put/get/removeMountpointObject — a loose disk file is a plain object at its
mirrored path roots/<key>, no namespace/_files wrapper, never scanned by GC.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 2.2: Route loose files to mountpoint objects in the wiring; remove `genericNamespace`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedTransaction.cpp:387-417` (writeFile non-table branch), `:790-918` (moveFile/removeFile generic fallthroughs)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:410-415` (delete `genericNamespace`), `:461`, `:800` (generic fallthroughs)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h:137` (delete decl)
- Test: `src/Disks/tests/gtest_cas_store.cpp` (covered by 2.1) + the CA stateless lane

- [ ] **Step 1: Write the failing test** — the behavioral oracle is the CA stateless lane (the startup write-probe path), but add a focused store-level test that mimics the wiring's loose-file flow to fail first:

```cpp
TEST(CasStore, LooseFileNotAShardCandidate)
{
    auto store = DB::Cas::tests::openFreshStoreForTest();
    /// A loose probe object whose tail is numeric must NOT be classified as a shard manifest.
    store->putMountpointObject("srv1/probe/7", "x");
    EXPECT_FALSE(store->layout().tryParseRootShardKey(store->layout().mountpointObjectKey("srv1/probe/7")).has_value());
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasStore.LooseFileNotAShardCandidate' > build/test_cas_store.log 2>&1; echo "exit=$?"`
Expected: PASS already (the `@cas@` gate from Phase 1 makes it pass — this is a guard test, not a red test). If it FAILS, the Phase 1 gate regressed; fix before continuing.

> This is a guard/characterization test, not strict TDD-red, because the real behavior change is in
> the wiring write path whose oracle is the stateless lane. Keep it as a regression guard.

- [ ] **Step 3: Implement the wiring change.**

In `ContentAddressedMetadataStorage.h`, delete the `genericNamespace` declaration (line 137):

```cpp
    static Cas::RootNamespace shadowNamespace(const std::string & shadow_table_dir);
    // (genericNamespace removed — loose files are plain mountpoint objects, design §5.2)
```

In `ContentAddressedMetadataStorage.cpp`, delete the `genericNamespace` definition (lines 410-415) entirely. Replace its three use sites:

`existsFile` (line 461):
```cpp
        return store()->getMountpointObject(server_id + "/" + path).has_value();
```

`tryGetInManifestBytes` (line 800): loose files are NOT in-manifest bytes — they are real objects. Remove the generic fallthrough so it returns nullopt for non-table non-part paths (the caller then reads the real object via `getStorageObjects`):
```cpp
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            return store()->getNamespaceFile(liveNamespace(tf->table_uuid), tf->tail);
        return std::nullopt;   /// loose files are plain objects, not in-manifest bytes (design §5.2)
    }
```

In `getStorageObjects` (line 765), add a loose-file branch BEFORE the `isPartFilePath` throw so a real mountpoint object resolves to a `StoredObject` at its key:
```cpp
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
                "ContentAddressed: table-level verbatim file is in-manifest, not a storage object: {}", path);
        /// A loose mountpoint object: a real plain object at roots/<server>/<path>.
        const std::string key = store()->layout().mountpointObjectKey(server_id + "/" + path);
        if (store()->getMountpointObject(server_id + "/" + path))
            return {StoredObject(key, path, getFileSize(path))};
        throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
    }
```

> NOTE: `getFileSize` for a loose object needs a size. Add a loose-file branch in `getFileSize`
> (line 557, before the `isPartFilePath` throw): read the object and return its byte length.
> ```cpp
>     if (!ContentAddressed::isPartFilePath(path))
>     {
>         if (auto tf = ContentAddressed::parseTableFilePath(path))
>             throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
>         if (auto bytes = store()->getMountpointObject(server_id + "/" + path))
>             return bytes->size();
>         throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: no object for {}", path);
>     }
> ```

In `ContentAddressedTransaction.cpp` `writeFile` non-table branch (lines 394-417), replace the `else { genericNamespace }` arm with a plain mountpoint-object write:
```cpp
    if (!ContentAddressed::isPartFilePath(path))
    {
        if (auto tf = ContentAddressed::parseTableFilePath(path))
        {
            const Cas::RootNamespace ns = metadata_storage.liveNamespace(tf->table_uuid);
            const std::string name = tf->tail;
            std::string prefix_bytes;
            if (mode == WriteMode::Append)
                if (auto existing = metadata_storage.store()->getNamespaceFile(ns, name))
                    prefix_bytes = std::move(*existing);
            return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
                [this, ns, name, carried = std::move(prefix_bytes)](std::string bytes)
                {
                    metadata_storage.store()->putNamespaceFile(ns, name, carried + bytes);
                });
        }
        /// A loose disk file (the startup write probe): a plain mountpoint object (design §5.2).
        const std::string key = metadata_storage.serverId() + "/" + path;
        std::string prefix_bytes;
        if (mode == WriteMode::Append)
            if (auto existing = metadata_storage.store()->getMountpointObject(key))
                prefix_bytes = std::move(*existing);
        return std::make_unique<ContentAddressed::CaInlineWriteBuffer>(
            [this, key, carried = std::move(prefix_bytes)](std::string bytes)
            {
                metadata_storage.store()->putMountpointObject(key, carried + bytes);
            });
    }
```

In `ContentAddressedTransaction.cpp` `moveFile` generic fallthrough (lines 797-811), replace the `genericNamespace` arm of `locate_verbatim` with a mountpoint move (read+put+remove):
```cpp
    if (!ContentAddressed::isPartFilePath(path_from) && !ContentAddressed::isPartFilePath(path_to))
    {
        auto move_table_verbatim = [&](const ContentAddressed::TableFilePath & src_tf,
                                       const ContentAddressed::TableFilePath & dst_tf)
        {
            const Cas::RootNamespace src_ns = metadata_storage.liveNamespace(src_tf.table_uuid);
            const Cas::RootNamespace dst_ns = metadata_storage.liveNamespace(dst_tf.table_uuid);
            if (src_ns.string() == dst_ns.string() && src_tf.tail == dst_tf.tail)
                return;
            auto bytes = metadata_storage.store()->getNamespaceFile(src_ns, src_tf.tail);
            if (!bytes)
                throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
            metadata_storage.store()->putNamespaceFile(dst_ns, dst_tf.tail, *bytes);
            metadata_storage.store()->removeNamespaceFile(src_ns, src_tf.tail);
        };
        auto src_tf = ContentAddressed::parseTableFilePath(path_from);
        auto dst_tf = ContentAddressed::parseTableFilePath(path_to);
        if (src_tf && dst_tf)
        {
            move_table_verbatim(*src_tf, *dst_tf);
            return;
        }
        /// Loose mountpoint files (rare): read + put + remove plain objects.
        const std::string src_key = metadata_storage.serverId() + "/" + path_from;
        const std::string dst_key = metadata_storage.serverId() + "/" + path_to;
        if (src_key == dst_key)
            return;
        auto bytes = metadata_storage.store()->getMountpointObject(src_key);
        if (!bytes)
            throw Exception(ErrorCodes::FILE_DOESNT_EXIST, "ContentAddressed: moveFile source missing: {}", path_from);
        metadata_storage.store()->putMountpointObject(dst_key, *bytes);
        metadata_storage.store()->removeMountpointObject(src_key);
        return;
    }
```

In `ContentAddressedTransaction.cpp` `removeFile` generic fallthrough (lines 912-917), replace the `genericNamespace` arm:
```cpp
    if (auto tf = ContentAddressed::parseTableFilePath(path))
    {
        metadata_storage.store()->removeNamespaceFile(metadata_storage.liveNamespace(tf->table_uuid), tf->tail);
        return;
    }
    /// Loose mountpoint file: exact-token delete of the plain object (design §5.2).
    metadata_storage.store()->removeMountpointObject(metadata_storage.serverId() + "/" + path);
```

- [ ] **Step 4: Build + run CA gtests + CA stateless lane**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all.log 2>&1; echo "exit=$?"
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test "04036_backup_partition_transaction_visibility" > build/ca_lane_phase2.log 2>&1; echo "exit=$?"
```
Subagent per log. Expect all PASS. The startup write-probe (a loose file) is exercised on every server start in the lane, so a broken loose-file path fails the whole lane loudly.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: loose disk files become plain mountpoint objects; drop _disk/genericNamespace

writeFile/moveFile/removeFile non-table branches and the read-surface generic
fallthroughs now use put/get/removeMountpointObject at roots/<server>/<path>.
genericNamespace removed. GC-safe: plain objects are never scanned, owned by path.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 3 — `roots/` is data; `gc/` is discovery (spec §5.3)

Move the registry key `roots/_registry` → `gc/registry`; drop the now-unneeded `_registry`-under-`roots/` reservation; replace the `listNamespaces`-based shadow enumeration in `listDirectory`/`existsDirectory` with a scoped S3 LIST of the mirrored subtree.

### Task 3.1: Relocate the registry key to `gc/registry`

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h:68-71` (`rootsRegistryKey`), `:256-260` (`checkNamespace` `_registry` reservation)
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Update the failing test** — change the existing `TEST(CasLayout, RegistryKeyAndReservedSegment)` (lines 88-98) to assert the new key and that the `_registry` segment is no longer reserved under `roots/`:

```cpp
TEST(CasLayout, RegistryKeyMovedToGc)
{
    Layout l("p");
    EXPECT_EQ(l.rootsRegistryKey(), "p/gc/registry");
    /// The registry no longer lives under roots/, so a `_registry` namespace segment is no longer
    /// reserved (design §5.3 bonus cleanup) — but it also never occurs in a real CH path.
    EXPECT_NO_THROW(l.rootShardKey(RootNamespace{"a/_registry@cas@"}, 0));
    /// `_files` and `_pool_meta`-style reservations are unaffected.
    EXPECT_THROW(l.rootShardKey(RootNamespace{"a/_files"}, 0), DB::Exception);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.RegistryKeyMovedToGc' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — `rootsRegistryKey()` still returns `p/roots/_registry`; the `_registry` reservation still throws.

- [ ] **Step 3: Implement.**

In `CasLayout.h`, change `rootsRegistryKey` (lines 68-71):
```cpp
    /// The namespace registry (design §5.3): authoritative namespace universe, CAS-appended on
    /// W-REGISTER, fenced by GC, the source of GC discovery (never LIST). Relocated from
    /// `roots/_registry` to `gc/registry` — `roots/` is now data only; discovery is infrastructure.
    /// The CAS-append + fence MECHANISM is unchanged (N5): only the key moves, and it is read/written
    /// strictly by this computed key.
    String rootsRegistryKey() const
    {
        return prefix + "/gc/registry";
    }
```

In `checkNamespace`, delete the `_registry` reservation block (lines 256-260) entirely — it is no longer needed because nothing lives at `roots/_registry`.

- [ ] **Step 4: Run tests to verify pass**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.*' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: PASS (delete or update any old test that asserted the `roots/_registry` key or the `_registry` reservation; grep `gtest_cas_layout.cpp` and any other gtest for `roots/_registry` and `_registry` and fix).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h src/Disks/tests/gtest_cas_layout.cpp
git commit -m "$(cat <<'EOF'
CA VFS: move the namespace registry key roots/_registry -> gc/registry

roots/ is data only; discovery is infrastructure (design §5.3). By-key move,
CAS-append+fence mechanism unchanged (N5). Drops the now-unneeded _registry
reservation in checkNamespace.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 3.2: Shadow enumeration via scoped LIST instead of `listNamespaces`

The shadow branches in `listDirectory` (lines 644-652) and `existsDirectory` (lines 500-504) enumerate shadow children via `store()->listNamespaces("shadow/")` (a registry read) and prefix-match. With path-mirroring, the natural enumeration is a scoped S3 LIST of the mirrored subtree, re-checking `listRefs` per candidate (the loose-LIST-then-recheck contract from the spec). We add a `Store::listMirroredChildren(prefix)` that does a bounded LIST over `roots/<prefix>` and returns the distinct next-segment names.

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasStore.h` / `.cpp` (new `listMirroredChildren`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp:500-504`, `:644-652`
- Test: `src/Disks/tests/gtest_cas_store.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_store.cpp`)

```cpp
TEST(CasStore, ListMirroredChildren)
{
    auto store = DB::Cas::tests::openFreshStoreForTest();
    /// Seed two shadow archives by writing a verbatim file into each (creates the prefix in S3).
    store->putNamespaceFile(DB::Cas::RootNamespace{"shadow/bk1/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    store->putNamespaceFile(DB::Cas::RootNamespace{"shadow/bk2/store/3f2/3f2a-uuid@cas@"}, "x", "1");
    auto children = store->listMirroredChildren("shadow/");
    std::sort(children.begin(), children.end());
    ASSERT_EQ(children.size(), 2u);
    EXPECT_EQ(children[0], "bk1");
    EXPECT_EQ(children[1], "bk2");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasStore.ListMirroredChildren' > build/test_cas_store.log 2>&1; echo "exit=$?"`
Expected: FAIL — `listMirroredChildren` undeclared.

- [ ] **Step 3: Implement.**

In `CasStore.h`, after `listNamespaces`:
```cpp
    /// Scoped LIST of the mirrored subtree (design §5.3): the distinct next-path-segment names under
    /// `roots/<prefix>` (a loose LIST used by browse only; callers re-check `listRefs`/`getFileSize`
    /// before showing an entry). NOT authoritative — GC still uses the compact registry. `prefix`
    /// is a server-relative or shadow-relative path ending in '/'.
    std::vector<String> listMirroredChildren(const String & prefix);
```

In `CasStore.cpp`, implement using the backend's LIST over `roots/<prefix>` (grep `CasStore.cpp`/`CasFsck.cpp` for the existing `backend().list(...)` or `listObjects` call used for the `roots/`/`blobs/` prefixes and copy its shape):
```cpp
std::vector<String> Store::listMirroredChildren(const String & prefix)
{
    const String full = pool_layout.rootsPrefix() + prefix;   /// e.g. <pool>/roots/shadow/
    std::unordered_set<String> children;
    for (const auto & object : pool_backend->list(full))       /// match the existing list() signature
    {
        const String & key = object.key;                       /// adapt to the real field name
        if (!key.starts_with(full))
            continue;
        const std::string_view rest(key.data() + full.size(), key.size() - full.size());
        const size_t slash = rest.find('/');
        const std::string_view seg = slash == std::string_view::npos ? rest : rest.substr(0, slash);
        if (!seg.empty())
            children.emplace(seg);
    }
    return {children.begin(), children.end()};
}
```

> Verify `pool_backend->list(...)`'s real method name and the element field (`.key`/`.path`) against
> the existing LIST use in `CasFsck.cpp` (it lists `blobsPrefix()` etc.) and match it exactly.

In `ContentAddressedMetadataStorage.cpp`, replace the shadow enumeration in `existsDirectory` (lines 500-504):
```cpp
        const std::string canonical = canonicalDiskPath(path);
        const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
        for (const auto & child : store()->listMirroredChildren(scope))
        {
            const Cas::RootNamespace child_ns{scope + child};
            if (!store()->listRefs(child_ns).empty())
                return true;
        }
        return false;
```

And in `listDirectory` (lines 644-652):
```cpp
        const std::string canonical = canonicalDiskPath(path);
        const std::string scope = canonical.empty() ? "shadow/" : canonical + "/";
        std::unordered_set<std::string> result;
        for (const auto & child : store()->listMirroredChildren(scope))
            result.emplace(child);
        return toVector(std::move(result));
```

> NOTE: a mirrored LIST naturally surfaces intermediate path segments AND `@cas@`-suffixed table
> dirs. For the logical (`@cas@`-stripped) view, strip a trailing `@cas@` from each child before
> returning. Add a small lambda in `listDirectory`'s shadow branch:
> ```cpp
> auto strip_cas = [](std::string s) {
>     constexpr std::string_view suffix = "@cas@";
>     if (s.size() >= suffix.size() && std::string_view(s).ends_with(suffix))
>         s.resize(s.size() - suffix.size());
>     return s;
> };
> ```
> and `result.emplace(strip_cas(child));`.

- [ ] **Step 4: Build + CA gtests + CA stateless lane**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all.log 2>&1; echo "exit=$?"
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test "04036_backup_partition_transaction_visibility" > build/ca_lane_phase3.log 2>&1; echo "exit=$?"
```
Subagent per log. Expect all PASS (the backup test exercises the shadow tree).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: enumerate shadow tree via scoped mirrored LIST, not the registry

listDirectory/existsDirectory shadow branches use Store::listMirroredChildren
(a bounded loose LIST over roots/shadow/...) + a listRefs recheck, decoupling
browse from the GC-only registry (design §5.3).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 4 — Navigation via stock `clickhouse-disks` verbs (spec §5.5)

The `IMetadataStorage` traversal methods already back stock `cd`/`ls`/`read`. After Phases 1-3 they already navigate the new layout. This phase is a verification phase: confirm the stock verbs render the logical view, add NO new `ca-*` verbs, NO new facade.

### Task 4.1: Manual verification of stock navigation (read-only)

**Files:** none modified. This task produces a verification transcript only.

- [ ] **Step 1: Bring up a CA disk with one table and a freeze.** Using the CA stateless config (the same disk config the lane uses; locate it via the lane's config under `tests/config` referenced by the praktika job), run a short SQL session:

```bash
./build/programs/clickhouse local --path build/ca_nav_check --query "
  CREATE TABLE t (a UInt64) ENGINE=MergeTree ORDER BY a SETTINGS disk='<ca_disk_name>';
  INSERT INTO t VALUES (1)(2)(3);
  ALTER TABLE t FREEZE;
" > build/ca_nav_setup.log 2>&1; echo "exit=$?"
```
> Substitute `<ca_disk_name>` with the content-addressed disk name from the lane config. If
> `clickhouse local` cannot mount the CA disk standalone, run the same SQL via a server started with
> the lane config and `clickhouse client` instead. A subagent reads `build/ca_nav_setup.log`.

- [ ] **Step 2: Navigate with stock `clickhouse-disks` verbs (read-only).**

```bash
./build/programs/clickhouse disks --disk <ca_disk_name> --query "list store" > build/ca_nav_ls.log 2>&1; echo "exit=$?"
./build/programs/clickhouse disks --disk <ca_disk_name> --query "list store/<u3>/<uuid>" >> build/ca_nav_ls.log 2>&1
./build/programs/clickhouse disks --disk <ca_disk_name> --query "read store/<u3>/<uuid>/all_1_1_0/count.txt" > build/ca_nav_read.log 2>&1; echo "exit=$?"
```
Expected (`build/ca_nav_ls.log`): the LOGICAL view — `store/<u3>/<uuid>/all_1_1_0/…` with `@cas@`
stripped, no `<N>` manifest objects, no `_files`. A subagent confirms the listing has the part dir
and the part's logical file names, not the physical archive internals.

- [ ] **Step 3: Confirm the physical archive layout via raw object listing** (correspondence, not identity):

```bash
ls -R build/ca_nav_check 2>/dev/null | grep -E '@cas@|/_files/|roots/' > build/ca_nav_raw.log 2>&1; echo "exit=$?"
```
> For a Local-backend pool the keys are on-disk paths; for an S3 backend use `aws s3 ls --recursive`
> against the bucket prefix. Expected (`build/ca_nav_raw.log`): the PHYSICAL layout —
> `roots/<server>/store/<u3>/<uuid>@cas@/<N>` and `…@cas@/_files/format_version.txt`, plus
> `gc/registry`. A subagent confirms the raw keys carry `@cas@` and correspond segment-for-segment
> to the logical paths from Step 2.

- [ ] **Step 4: Record findings.** Capture the two transcripts (logical vs physical) in the commit
message body as evidence that stock verbs render the logical view while raw listing shows the
`@cas@` archive — the two correspond segment-for-segment.

- [ ] **Step 5: Commit (docs/evidence only — no code).** If no code changed, skip the commit; otherwise:

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: verify stock clickhouse-disks cd/ls/read navigate the mirrored layout

No new ca-* verbs, no facade. Logical view (stock verbs) vs physical @cas@ archive
(raw listing) correspond segment-for-segment.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 5 — Terminology + VFS contract doc (spec §5.6)

Remove "part" vocabulary from the generic CA layer comments/local names; keep `ref`. Write the VFS contract doc.

### Task 5.1: De-"part" the generic-layer comments

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h`
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h`
- Modify: the `CasEvent` `ref_name` comment (grep `ref_name` in `Core/CasEvent*.h`)
- Modify: `CasRootShardCodec` "per-part" comment (grep `per-part` in `Core/CasRootShardCodec.*`)
- Test: none (comment-only; the build is the oracle).

- [ ] **Step 1: Find every "part" occurrence in the generic layer**

Run:
```bash
grep -rni "part" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasEvent*.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasRootShardCodec.* > build/cas_part_grep.log 2>&1; cat build/cas_part_grep.log
```
A subagent classifies each hit: replace "part" with "ref"/"directory entry"/"tree entry" where it is generic-layer vocabulary; LEAVE any occurrence that is genuinely about MergeTree parts in the wiring (none should be in these Core files). Do NOT touch `partition`, `apart`, etc. (substring false positives).

- [ ] **Step 2: Apply the comment edits.** For each generic-layer hit, rewrite the comment to use `ref`/`tree entry`/`directory`. Example (illustrative — apply to the actual lines the grep finds): a comment "the part's tree" becomes "the ref's tree"; "per-part files" becomes "per-ref overlay files"; the `CasEvent` `ref_name` comment "the part name" becomes "the ref name (a mutable directory handle, git-style)".

- [ ] **Step 3: Build to confirm comments compile**

Run: `ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"`
Subagent confirms `exit=0`.

- [ ] **Step 4: Re-grep to confirm no stray generic-layer "part" vocabulary remains**

Run: `grep -rni "\bpart\b" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasTreeCodec.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h > build/cas_part_grep2.log 2>&1; cat build/cas_part_grep2.log`
Expected: empty (or only justified comments a subagent confirms are intentional).

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: remove "part" vocabulary from the generic CA layer comments

The generic store knows refs, trees, blobs — not parts (part-awareness is a
wiring property). Comment-only; `ref` kept and documented as a mutable directory
handle, git-style.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 5.2: Write the VFS contract doc

**Files:**
- Create: `docs/superpowers/specs/2026-06-19-ca-vfs-contract.md`
- Test: none (prose).

- [ ] **Step 1: Write the document.** Create `docs/superpowers/specs/2026-06-19-ca-vfs-contract.md` with the following content (note: this is a `docs/superpowers/specs` file, not a `docs/en` doc, so the `docs/`-frontmatter/anchor rule does not apply — match the existing spec file's plain-header style):

```markdown
# CAS Disk — VFS Contract {#title}

- **Status:** Companion to `2026-06-19-ca-vfs-path-mapping-design.md`
- **Date:** 2026-06-19
- **Branch:** `cas-mergetree-poc`

## Entities

- **Pool** — an S3 prefix holding all content + control objects.
- **Content object** — an immutable, content-addressed, globally deduplicated object: a `blob`
  (file bytes), a `tree` (directory listing), or a `pack` (packed small files). Addressed by a
  128-bit content hash.
- **Namespace** — an *opaque string* the core never interprets; the wiring composes it to mirror the
  ClickHouse disk path, marking the content-addressed boundary with the `@cas@` suffix on the
  table-dir segment.
- **Ref** — a mutable named pointer to an immutable tree (a mutable directory handle, git-style),
  plus a small inline overlay of per-ref mutable files (`mutable_files`).
- **Verbatim file** — a plain, name-keyed mutable object. Two locations: loose in the mountpoint
  (`roots/<server>/<path>`) or inside a `@cas@` archive (`…@cas@/_files/<name>`).

## Mutability invariant

A node is **immutable if and only if it is content-addressed** (has a hash). Trees, subtrees, blobs,
and pack-slices have a hash → immutable. Namespaces, refs, and overlay/verbatim files have no hash →
mutable. `@cas@` is exactly the line between deduplicated immutable content and ordinary files.

## Path grammar

```
POOL/
  blobs/ trees/ packs/                              content (immutable, deduplicated)
  roots/                                            DATA ONLY (server mountpoints; CH paths mirrored)
    <server>/store/<u3>/<uuid>@cas@/<N>             a table archive: root-shard manifests
    <server>/store/<u3>/<uuid>@cas@/_files/<name>   that table's verbatim files
    <server>/detached/store/<u3>/<uuid>@cas@/<N>    detached parts (sibling archive)
    <server>/_precommits/<N>                        this server's in-flight precommits (Phase 6)
    <server>/_watermark                             this server's watermark (Phase 6)
    <server>/<plain path>                           loose non-CAS files (e.g. the write probe)
    shadow/<backup>/store/<u3>/<uuid>@cas@/<N>       FREEZE snapshots
  gc/
    registry                                        authoritative namespace list (GC discovery)
    precommits                                      precommit discovery index (Phase 6)
    state snap/<gen>/<shard> hb                      GC's own state
  _pool_meta
```

Reserved folders among data carry a leading underscore (`_files`, `_precommits`, `_watermark`,
`_registry`-style names) so they never collide with a real ClickHouse path segment. `@cas@` is a
suffix on a directory name, never its own folder.

## Listing / merge semantics

- Stock `clickhouse-disks` `cd`/`ls`/`read` present the **logical** ClickHouse view: `@cas@`
  stripped, files reconstructed from the manifest/trees — a normal-feeling MergeTree disk.
- Raw `aws s3 ls` shows the **physical** archive: the same paths with `@cas@` on table dirs and the
  manifest/protobuf objects (`<N>`, `_files/…`) inside. The two correspond segment-for-segment but
  are deliberately different renderings (logical vs physical), not byte-identical listings.
- Browse uses a bounded, occasional scoped LIST (loose) and re-checks `listRefs` before showing an
  entry. GC uses the compact authoritative registry (never a full LIST).

## What is NOT guaranteed

- The logical and physical listings are not byte-identical.
- A loose mountpoint object is not content-addressed (no dedup, no hardlink/rename).
- Raw subtree deletion (`rm roots/<server>/`) is destructive offline maintenance — NOT equivalent to
  `dropNamespace`; it bypasses the journal and the fenced index prune and may leave index/GC
  leftovers that a repair/prune step reconciles.

## Parts, merges, projections live in the wiring

The content-addressed store knows refs, trees, blobs, packs, and namespaces — nothing about
ClickHouse parts, merges, or projections. Part naming, the detached-part sibling-archive split, the
projection `<proj>.proj/` name prefix, and mutable-per-part file classification are all **wiring
policy** in `ContentAddressedMetadataStorage` / `ContentAddressedTransaction` / `PartPathParser`,
never in the `Cas::` core.
```

- [ ] **Step 2: Sanity-check the doc renders** (it is Markdown; just confirm it exists and is non-empty).

Run: `wc -l docs/superpowers/specs/2026-06-19-ca-vfs-contract.md`
Expected: a non-zero line count.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-06-19-ca-vfs-contract.md
git commit -m "$(cat <<'EOF'
CA VFS: add the VFS contract doc

Entities, the mutability invariant (immutable iff content-addressed), the path
grammar, logical-vs-physical listing semantics, and the explicit
"parts/merges/projections live in the wiring" statement.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase 6 (SEPARABLE / OPTIONAL — spec §5.4, the §8 open decision) — Server-scoped data consolidation

> **THIS PHASE IS SEPARABLE AND MAY BE CUT.** It is the ONLY phase that edits GC code, and therefore
> the only one requiring a re-soak (no TLA+ re-model — protocol/fence/reclaim ordering unchanged).
> If deferred, Phases 1-5 + 7 still complete the refactor with ZERO GC-code edits (the watermark
> sub-task 6.1 is by-key and trivially safe; you may keep it and defer only the precommit sub-task
> 6.2). Decide per spec §8 before starting.

> **SPEC GAP RESOLVED (precommit discovery).** Spec §5.4 specifies a NEW `gc/precommits` discovery
> index "the same pattern as the registry". The CURRENT code does NOT have a separate precommit
> index — the build-root namespace (`_builds/<server_hex>`) is CAS-registered in the SAME
> `roots/_registry`/`gc/registry` like any namespace, and GC discovers it through that registry +
> `isBuildRootNamespace`. Building a *separate* `gc/precommits` index is therefore net-new
> machinery beyond a relocation and risks touching the discovery/fence path (N5). **Resolution:**
> keep using the single relocated `gc/registry` for precommit discovery (the precommit namespace
> `roots/<server>/_precommits` is registered there like every other namespace, recognized by an
> `isPrecommitNamespace` predicate). Do NOT build a second index. This honors "mechanism unchanged"
> (N5) more faithfully than adding a parallel index. Flag this divergence from §5.4's literal text
> for the reviewer.

### Task 6.1: Watermark key → `roots/<server>/_watermark` (by-key, trivial)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h:213-216` (`serverWatermarkKey`)
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_layout.cpp`). The `<server>` token in the key must be the SAME canonical token the mountpoint uses. The watermark key takes a server hex today; keep that arg but place it under `roots/<server_hex>/_watermark`:

```cpp
TEST(CasLayout, WatermarkUnderMountpoint)
{
    Layout l("p");
    EXPECT_EQ(l.serverWatermarkKey("deadbeefdeadbeefdeadbeefdeadbeef"),
              "p/roots/deadbeefdeadbeefdeadbeefdeadbeef/_watermark");
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.WatermarkUnderMountpoint' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — current key is `p/servers/de/deadbeef…`.

- [ ] **Step 3: Implement.** In `CasLayout.h` (lines 213-216):

```cpp
    /// Per-server watermark (design §5.4): relocated from `servers/<2hex>/<server_hex>` to
    /// `roots/<server>/_watermark`, bringing the server's whole footprint under one subtree. Read by
    /// GC strictly by this computed key (watermarkOf); W-ANCHOR ordering is location-independent (N4).
    /// `<server>` is the SAME canonical token the mountpoint uses (the 32-lower-hex server id).
    String serverWatermarkKey(const String & server_id_hex) const
    {
        return prefix + "/roots/" + server_id_hex + "/_watermark";
    }
```

> NOTE: the `<server>` mountpoint token in the wiring is `server_id` (a string), while GC computes
> the watermark key from `u128ToHex(server_id)`. The CONTRACT (spec §5.4) is that these resolve the
> SAME token. Confirm the wiring's `server_id` member equals `u128ToHex(poolConfig().server_id)`; if
> the wiring uses a non-hex display name, that is a PRE-EXISTING mismatch — flag it. For the PoC the
> mountpoint already uses `server_id` consistently in `liveNamespace`, and GC uses the hex; since
> the watermark is read only by GC by-key, the watermark relocation is self-consistent as long as
> the mountpoint `<server>` directory and the watermark `<server_hex>` directory are the SAME
> string. **If they differ, align the wiring's `server_id` to the 32-hex form in this task** (see
> the §5.4 "one canonical `<server>` token" contract) and re-run Phase 1 lane to confirm.

- [ ] **Step 4: Run tests + build + CA gtests + CA lane**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all.log 2>&1; echo "exit=$?"
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test "04036_backup_partition_transaction_visibility" > build/ca_lane_phase6_1.log 2>&1; echo "exit=$?"
```
Subagent per log. Expect all PASS.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: relocate the per-server watermark to roots/<server>/_watermark

By-key move (watermarkOf reads by computed key); W-ANCHOR ordering unchanged (N4).
Brings the watermark under the server's own subtree (G4).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 6.2: Precommit namespace → `roots/<server>/_precommits`; rename `build`→`precommit`; owner from shared `<server>` token

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasLayout.h` (`isBuildRootNamespace` → `isPrecommitNamespace`, `isPrecommitNamespaceSegment`, `checkNamespace` `_builds` → `_precommits`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp:552-556` (`buildRootNs` → `precommitNs`)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp:1780-1811` (owner derivation) + the `isBuildRootNamespace` use sites (lines 1250, 1590)
- Test: `src/Disks/tests/gtest_cas_layout.cpp`

- [ ] **Step 1: Write the failing test** (append to `gtest_cas_layout.cpp`)

```cpp
TEST(CasLayout, PrecommitNamespaceUnderMountpoint)
{
    Layout l("p");
    const String server = "deadbeefdeadbeefdeadbeefdeadbeef";
    const RootNamespace ns{server + "/_precommits"};
    /// Recognized as the precommit namespace:
    EXPECT_TRUE(Layout::isPrecommitNamespace(ns));
    /// Its shards are classifiable (gated through the precommit-segment rule, not @cas@):
    auto parsed = l.tryParseRootShardKey("p/roots/" + server + "/_precommits/3");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->first, ns);
    EXPECT_EQ(parsed->second, 3u);
    /// A user namespace may not use the `_precommits` segment:
    EXPECT_THROW(l.rootShardKey(RootNamespace{"a/_precommits"}, 0), DB::Exception);
    /// The owning server is the leading segment (the shared <server> token, NOT a 32-hex _builds parse):
    EXPECT_EQ(Layout::precommitOwnerToken(ns), server);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build/src/unit_tests_dbms --gtest_filter='CasLayout.PrecommitNamespaceUnderMountpoint' > build/test_cas_layout.log 2>&1; echo "exit=$?"`
Expected: FAIL — `isPrecommitNamespace`/`precommitOwnerToken` undeclared.

- [ ] **Step 3: Implement.**

In `CasLayout.h`, replace `isBuildRootNamespace` (lines 224-232) with:
```cpp
    /// The precommit namespace (design §5.4, was the build-root `_builds/<server_hex>`): now
    /// `roots/<server>/_precommits` — one shard per in-flight precommit keyed by `build_seq`. It is
    /// an ordinary namespace key-wise (`rootShardKey` works unchanged); only behavioral branches
    /// (fold pending-tolerance, abandoned-precommit reclaim) key off `isPrecommitNamespace`.
    static bool isPrecommitNamespace(const RootNamespace & ns)
    {
        return ns.string().ends_with("/_precommits");
    }

    /// The owning-server token of a precommit namespace: the leading `<server>` segment of
    /// `<server>/_precommits` — the SAME canonical token the mountpoint and watermark use (design
    /// §5.4 contract). Reclaim derives the server from THIS, never from a 32-hex `_builds/<hex>` parse.
    static String precommitOwnerToken(const RootNamespace & ns)
    {
        const String & s = ns.string();
        static constexpr std::string_view suffix = "/_precommits";
        if (!s.ends_with(suffix))
            return {};
        return s.substr(0, s.size() - suffix.size());
    }
```

Update `isPrecommitNamespaceSegment` (the Phase 1 stub) to recognize `_precommits`:
```cpp
    static bool isPrecommitNamespaceSegment(std::string_view seg) { return seg == "_precommits"; }
```

In `tryParseRootShardKey`, remove the Phase 1 legacy `_builds/` admission (the `is_legacy_build_root` term) — precommit shards are now admitted via `isPrecommitNamespaceSegment(last_ns_segment)`:
```cpp
        if (!last_ns_segment.ends_with("@cas@") && !isPrecommitNamespaceSegment(last_ns_segment))
            return std::nullopt;
```

In `checkNamespace`, replace the `_builds` reservation (lines 261-266) with a `_precommits` reservation that still permits the precommit namespace itself:
```cpp
            /// Reserved for the precommit namespace (design §5.4: `<server>/_precommits`). A user
            /// namespace must not use the `_precommits` segment, but the precommit namespace itself
            /// is legal — recognized by `isPrecommitNamespace`.
            if (segment == "_precommits" && !isPrecommitNamespace(ns))
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_precommits'", s);
            /// Reserved for the per-server watermark object (`<server>/_watermark`).
            if (segment == "_watermark")
                throw DB::Exception(DB::ErrorCodes::BAD_ARGUMENTS,
                    "CasLayout: namespace '{}' uses the reserved segment '_watermark'", s);
```

In `CasBuild.cpp`, rename `buildRootNs` → `precommitNs` (lines 552-556) and relocate the namespace string. The `<server>` token MUST equal the mountpoint token (`u128ToHex(server_id)`):
```cpp
RootNamespace Build::precommitNs() const
{
    /// `<server>/_precommits` — this server's precommit namespace (design §5.4, was `_builds/<hex>`).
    /// `<server>` is the canonical server token shared with the mountpoint and watermark.
    return RootNamespace{u128ToHex(store->poolConfig().server_id) + "/_precommits"};
}
```
Rename the `buildRootNs()` declaration in `CasBuild.h` to `precommitNs()` and update its three call sites in `CasBuild.cpp` (lines 593, 977, 994, 1013 — grep `buildRootNs` and replace each). Also update the comment at `CasBuild.cpp:554`/`579`/`592` from "build-root" to "precommit".

In `CasGc.cpp`, replace the two `isBuildRootNamespace` use sites (lines 1250, 1590) with `isPrecommitNamespace`. Then rewrite the owner derivation in `reclaimAbandonedPrecommit` (lines 1796-1811) to read the shared `<server>` token instead of the 32-hex `_builds/` parse:
```cpp
    /// Derive the owning server from the SHARED `<server>` token (design §5.4 contract): the leading
    /// segment of `<server>/_precommits`, identical to the mountpoint and watermark token — NOT a
    /// 32-hex `_builds/<hex>` parse.
    const String server_token = Layout::precommitOwnerToken(ns);
    if (server_token.size() != 32)
        return;   /// not a well-formed precommit namespace — leave it untouched (never reclaim blindly)
    UInt128 server;
    try
    {
        server = hexToU128(server_token);
    }
    catch (...)
    {
        return;
    }
```

Update the `reclaimAbandonedPrecommit` doc comment (lines 1783-1789) replacing "`_builds/<server_hex>`" with "`<server>/_precommits`".

> The `gc/precommits` discovery index from §5.4 is intentionally NOT built — see the SPEC GAP note
> at the head of this phase. The precommit namespace is discovered via the existing relocated
> `gc/registry`, exactly as today (N5 mechanism unchanged).

- [ ] **Step 4: Run tests + build + CA gtests + CA lane**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all.log 2>&1; echo "exit=$?"
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  --test "04036_backup_partition_transaction_visibility" > build/ca_lane_phase6_2.log 2>&1; echo "exit=$?"
```
Subagent per log. Expect all PASS. Pay special attention to `gtest_cas_build_root_dangle.cpp` and `gtest_cas_b140_dangle.cpp` — the precommit/dangle tests; if they reference `buildRootNs`/`_builds`, update them to the new names in this task.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: relocate precommits to roots/<server>/_precommits; rename build->precommit

Build-root namespace `_builds/<server_hex>` -> `<server>/_precommits` under the
server's own subtree (G4). Owner derived from the shared <server> token, not a
32-hex parse. checkNamespace reserves _precommits/_watermark. Discovery stays on
the single relocated gc/registry (no separate gc/precommits index; N5 unchanged).
Protocol/fence/reclaim ordering unchanged — validated by re-soak, not a re-model.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 6.3: Re-soak (required because Phase 6 edits GC code)

**Files:** none modified.

- [ ] **Step 1: Run a chaos soak via the harness.**

Run:
```bash
utils/ca-soak > build/ca_soak_phase6.log 2>&1; echo "exit=$?"
```
> Use the harness's documented duration/chaos flags (inspect `utils/ca-soak --help` and match the
> established soak invocation from prior runs). A subagent monitors the run and reads
> `build/ca_soak_phase6.log`, returning: rounds completed, any dangle/leak assertion, any GC
> over-reclaim, and the final clean-exit status.

- [ ] **Step 2: Confirm zero invariant violations.** Expected: the soak completes with no INV-NO-DANGLE, no over-reclaim, no precommit-reclaim regression. If any assertion fires, STOP — Phase 6 has a real GC bug; do not merge.

- [ ] **Step 3: Commit the soak evidence in the message** (no code unless a fix was needed). If a fix was required, commit it as `CA VFS: Phase 6 soak fix — <symptom>`.

---

## Phase 7 — Full validation

### Task 7.1: Full CA stateless lane + integration + chaos soak

**Files:** none modified (a green-gate phase; any failure produces a fix commit).

- [ ] **Step 1: Build clean.**

Run:
```bash
ninja -C build clickhouse > build/ninja_clickhouse.log 2>&1; echo "exit=$?"
ninja -C build unit_tests_dbms > build/ninja_unit_tests.log 2>&1; echo "exit=$?"
```
Subagent per log; expect `exit=0`.

- [ ] **Step 2: Full CA gtest suite.**

Run: `./build/src/unit_tests_dbms --gtest_filter='*Cas*' > build/test_cas_all_final.log 2>&1; echo "exit=$?"`
Subagent; expect all PASS.

- [ ] **Step 3: Full CA stateless lane (not just one test).**

Run:
```bash
ln -sf "$(pwd)/build/programs/clickhouse" ci/tmp/clickhouse
python3 -m ci.praktika run "Stateless tests (arm_binary, content_addressed storage, parallel)" \
  > build/ca_lane_full.log 2>&1; echo "exit=$?"
```
Subagent reads `build/ca_lane_full.log`; expect PASS, with `04036_backup_partition_transaction_visibility` explicitly green. Triage any failure with the `cas-test-triage` skill (recoverable / real CA bug / genuinely-unsupported).

- [ ] **Step 4: Relevant integration tests** (if the CA disk has integration coverage; locate via `grep -rl content_addressed tests/integration/`):

Run (substitute the discovered selector):
```bash
python3 -m ci.praktika run "integration" --test <ca_integration_selector> > build/ca_integration_full.log 2>&1; echo "exit=$?"
```
Subagent; expect PASS. If there is no CA-specific integration test, note that and skip.

- [ ] **Step 5: Final chaos soak.**

Run: `utils/ca-soak > build/ca_soak_final.log 2>&1; echo "exit=$?"`
Subagent monitors; expect clean completion, zero invariant violations.

- [ ] **Step 6: Final commit (evidence / any straggler fix).**

```bash
git add -A
git commit -m "$(cat <<'EOF'
CA VFS: full validation — CA gtests, stateless lane, integration, chaos soak green

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)" || echo "nothing to commit"
```

---

## Self-review

**Spec coverage:** §5.1 path-mirroring + `@cas@` + `@cas@`-gated parsing → Tasks 1.1-1.3, 1.2. §5.2 eliminate `_disk`/`genericNamespace` + `@cas@`-scoped opacity → Tasks 2.1-2.2. §5.3 registry → `gc/registry`, shadow scoped-LIST, drop `_registry` reservation → Tasks 3.1-3.2. §5.5 stock-verb navigation, no `ca-*` → Task 4.1. §5.6 de-"part" comments + VFS contract doc → Tasks 5.1-5.2. §5.4 watermark + precommit relocation + `build`→`precommit` rename + owner-from-token (separable) → Tasks 6.1-6.3. §7 full validation → Tasks 7.1. Non-nesting invariant (§5.7) checked in Task 1.3. N1-N7 honored throughout (no GC algorithm/manifest/journal/fence edits except the bounded Phase 6 recognizer/parse, re-soaked).

**Two divergences from the literal spec, flagged for the reviewer:**
1. **Precommit discovery index.** §5.4 calls for a NEW `gc/precommits` index. The current code has no separate precommit index — the build-root namespace is registered in the single registry. To honor "mechanism unchanged" (N5), Task 6.2 keeps discovery on the relocated `gc/registry` and does NOT add a second index. (Resolved in the Phase 6 header gap note.)
2. **Atomic vs non-Atomic mirroring.** The parser's `table_uuid` is a bare uuid for Atomic and a joined `data/db/tbl` path for non-Atomic. `mirroredArchiveNamespace` (Task 1.3) reconstructs `store/<u3>/<uuid>@cas@` for Atomic and appends `@cas@` to the verbatim joined path for non-Atomic; the non-nesting invariant holds for both. (Resolved in Task 1.3.)

---

## Execution handoff

**Plan complete and saved to `docs/superpowers/plans/2026-06-19-ca-vfs-path-mapping.md`. Two execution options:**

**1. Subagent-Driven (recommended)** — dispatch a fresh subagent per task, review between tasks, fast iteration.

**2. Inline Execution** — execute tasks in this session using executing-plans, batch execution with checkpoints.

**Which approach?**

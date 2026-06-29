# CA Layout Phase 1 — re-rooted relocation (cas/refs + cas/manifests) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Move the hot ref shards to `cas/refs/<ns>/<shard>` and the cold part-manifests to `cas/manifests/<ns>/…` out of the shared `roots/` tree, so GC discovery is a `LIST cas/refs/` (only ref shards) instead of a recursive walk of `roots/` (which interleaves the manifest backlog + verbatim files). Namespaces become `server_root_id`-qualified.

**Architecture:** Identity-preserving relocation. The namespace string already carries a server prefix (`serverPrefix()` = server-UUID-hex) — Phase 1 swaps it to the configured `server_root_id` (one line in `liveNamespace`). `CasLayout` key methods change their base prefix (`/roots/` → `/cas/refs/` for shards, `/cas/manifests/` for manifests; verbatim `_files` stay under `roots/`). Discovery LISTs `cas/refs/` (pool-wide, all servers — GC is one pool-global leader folding all namespaces, blob in-degree spans the shared pool). The registry-driven `discoverUniverse` is unchanged. Manifest identity (`writer_instance_id`, `manifest_instance_id`, `<aa>` fan-out) is UNTOUCHED — Phase 3 reshapes it. So **no TLA+ identity change**.

**Tech Stack:** C++ (`DB::Cas`), GoogleTest (`unit_tests_dbms`).

**Scope:** Phase 1 only. Spec: `docs/superpowers/specs/2026-06-28-cas-layout-hot-cold-split-design.md` (rev5). Builds on Phase 0 (committed: `e843bc1`..`a14870`; `server_root_id`, `casRefsServerPrefix`/`casManifestsServerPrefix`/`serverRootDataPrefix` helpers exist).

## Global Constraints
- Branch `cas-layout-hot-cold-split` (current). New commits only; never rebase/amend.
- Allman braces. Build from `/home/mfilimonov/workspace/ClickHouse/master/build` via `ninja unit_tests_dbms` (NO `-j`/`nproc`), redirect to a log, analyze via subagent.
- CA pre-release, no persisted data ⟹ no migration / no compat. Fail-closed.
- Commit trailers (exact): `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` / `Claude-Session: https://claude.ai/code/session_01MXfxaevd1iF9R8uaj7MPFk`.
- Stage ONLY each task's files; **never `git add -A`** (uncommitted `CasBuild.*`/`gtest_cas_build.cpp` must stay untouched).

## Ground truth (verbatim, from the surface map)
- `Layout` ctor `explicit Layout(String prefix_)` (`CasLayout.h:43`) — holds `prefix` only; the namespace string carries the server token, so the relocation needs NO srid plumbing into key methods.
- `rootShardKey(ns,shard)` (`CasLayout.h:67`) = `prefix + "/roots/" + ns.string() + "/" + shard`. Callers: `CasStore.cpp:386,734,916,924,932`, `CasOrphanManifestSweep.cpp:76`.
- `manifestKey(id)` (`CasLayout.h:110`) = `prefix + "/roots/" + ns + "/_manifests/" + writer + "/" + build + "/" + manifestAa(ref) + "/" + inst_hex + ".proto"`. Callers: `CasBuild.cpp:566,645,808`, `CasStore.cpp:572`, `CasGc.cpp:152,928,1110`, `CasOrphanManifestSweep.cpp:86,113`, `CasFsck.cpp:106`.
- `rootNamespacePrefix(ns)` (`CasLayout.h:75`) = `prefix + "/roots/" + ns + "/"` — live callers use it ONLY to build `…+ "_manifests/"`: `CasOrphanManifestSweep.cpp:150,188`, `CasFsck.cpp:227`.
- `rootsPrefix()` (`CasLayout.h:207`) = `prefix + "/roots/"`. Callers: `CasGc.cpp:1282` (discovery LIST), `:1336` (strip-to-cursor-key), `CasStore.cpp:1006` (`listMirroredChildren` browse — verbatim tree, KEEP).
- `namespaceFileKey`/`namespaceFilesPrefix` (`CasLayout.h:85,98`) → `roots/<ns>/_files/…` — verbatim, STAY under roots/. Callers `CasStore.cpp:303,308,313,847,897`.
- `liveNamespace` (`ContentAddressedMetadataStorage.cpp:447`) = `RootNamespace{serverPrefix() + "/" + mirroredArchiveNamespace(table_uuid)}`; `serverPrefix()` (`:418`) = `u128ToHex(serverIdToU128(server_id))`. `server_root_id` member exists (`.h:181`).
- `discoverUniverse` (`CasGc.cpp:1250`) reads `gc/registry` (pool-global, one key) — iterates namespace strings; unaffected by relocation. `listRootShardTokens` (`CasGc.cpp:1276`) LISTs `rootsPrefix()`. `computeDiscoverDecisions` (`CasGc.cpp:1336-1347`) strips `rootsPrefix()` from each listed key to form the cursor key, matching `rootShardKey`'s base.
- `cursorKey`/`parseCursorKey` (`CasGcCursorKey.h:22,39`) split on the LAST `/` — a srid-qualified ns parses fine.
- `CasFsck.cpp:227` manifests LIST: `manifests_prefix = layout.rootNamespacePrefix(ns) + "_manifests/"`; blobs LIST `:166` (unchanged); refs are registry-driven (not physically listed).
- `CasObjectStorageBackend::list` (`CasObjectStorageBackend.cpp:564-609`) calls `object_storage->listObjects(physical_prefix, children, /*max_keys=*/0)` → full materialization + in-memory sort/slice. Resumable primitive: `IObjectStorage::iterate(path_prefix, max_keys, with_tags, start_after)` (`IObjectStorage.h:209`), `ObjectStorageIterator` `next()/isValid()/current()` (`ObjectStorageIterator.h:13`).

---

### Task 1: `CasLayout` key relocation + all callers (atomic) {#task-1}

**Files:** `CasLayout.h`; callers in `CasGc.cpp`, `CasOrphanManifestSweep.cpp`, `CasFsck.cpp` (manifest-prefix retarget); tests `gtest_cas_layout.cpp` + any gtest asserting on the old `/roots/<ns>/<shard>` or `/roots/<ns>/_manifests/` key strings.

**Why atomic:** moving `rootShardKey`/`manifestKey` without simultaneously moving discovery's LIST+strip and the sweep/fsck manifest enumeration would break GC. All move in one task.

**Interfaces (produces):**
- `rootShardKey(ns,shard)` → `prefix + "/cas/refs/" + ns.string() + "/" + shard`.
- `manifestKey(id)` → `prefix + "/cas/manifests/" + ns + "/" + writer + "/" + build + "/" + manifestAa(ref) + "/" + inst_hex + ".proto"` (drop the `/_manifests/` infix — the `cas/manifests/` prefix conveys it; KEEP writer/build/aa/inst — identity unchanged).
- NEW `casRefsPrefix()` → `prefix + "/cas/refs/"` (pool-wide, for the discovery LIST + strip).
- NEW `manifestNamespacePrefix(ns)` → `prefix + "/cas/manifests/" + ns.string() + "/"` (all manifests of a namespace, for the sweep/fsck enumeration — replaces `rootNamespacePrefix(ns) + "_manifests/"`).
- `rootNamespacePrefix(ns)` — KEEP (browse/`_files` still resolve under `roots/`); but its `_manifests/` callers move to `manifestNamespacePrefix`. `rootsPrefix()` KEEP (browse). `namespaceFileKey`/`namespaceFilesPrefix` UNCHANGED.

- [ ] **Step 1: Write the failing key-string test** — update/extend `gtest_cas_layout.cpp`:
```cpp
TEST(CasLayout, RelocatedRefAndManifestKeys)
{
    Layout l("p");
    const RootNamespace ns{"srid/store/ab/uuid@cas@"};
    EXPECT_EQ(l.rootShardKey(ns, 3), "p/cas/refs/srid/store/ab/uuid@cas@/3");
    EXPECT_EQ(l.casRefsPrefix(), "p/cas/refs/");
    EXPECT_EQ(l.manifestNamespacePrefix(ns), "p/cas/manifests/srid/store/ab/uuid@cas@/");
    // manifestKey: build a ManifestId with the file's existing helper/literal ref and assert it begins with
    // "p/cas/manifests/srid/store/ab/uuid@cas@/" and ends with ".proto" (no "/_manifests/" infix).
}
```
(Also fix any EXISTING `gtest_cas_layout.cpp` assertion that hardcodes `/roots/<ns>/<shard>` or `/roots/<ns>/_manifests/`.)
- [ ] **Step 2: Run → compile/assert FAIL.** `cd build && ninja unit_tests_dbms > build_p1t1.log 2>&1` then run `--gtest_filter='CasLayout.*'`.
- [ ] **Step 3: Implement** the `CasLayout` changes above; retarget `CasGc.cpp:1282`/`:1336` (discovery LIST + strip) from `rootsPrefix()` to `casRefsPrefix()`; retarget `CasOrphanManifestSweep.cpp:150,188` and `CasFsck.cpp:227` from `rootNamespacePrefix(ns) + "_manifests/"` to `manifestNamespacePrefix(ns)`. `rootShardKey`/`manifestKey` callers need NO change (they call the method, which now returns the new prefix). Verify `computeDiscoverDecisions`'s strip uses `casRefsPrefix()` so listed keys → cursor keys (`<ns>/<shard>`) still line up with `per_ns_shard`.
- [ ] **Step 4: Build + test** — `ninja unit_tests_dbms > build_p1t1b.log 2>&1` then `./src/unit_tests_dbms --gtest_filter='CasLayout.*:CasGc*:CasReuse*:CasOrphan*:CasFsck*:CasBuild.*:CasServerRoot*:CasMount*' > build/test_p1t1.log 2>&1`. ALL pass (esp. a full GC round still drains, fsck still classifies, sweep still reclaims — discovery now LISTs cas/refs/). Then `--gtest_filter='Cas*:Ca*'` and confirm only the known pre-existing `CaWiringOps.FreezeViaHardLinksIntoShadow` fails.
- [ ] **Step 5: Commit** (stage CasLayout.h + the 3 caller .cpp + the gtest(s) you changed): `git commit -m "CA Phase1: relocate ref shards→cas/refs, manifests→cas/manifests; discovery LISTs cas/refs (identity unchanged)" + trailers`.

---

### Task 2: `liveNamespace` uses `server_root_id` (not server-UUID-hex) {#task-2}

**Files:** `ContentAddressedMetadataStorage.cpp:447` (`liveNamespace`); a test (integration-style or a unit assertion on the produced namespace).

**Interfaces:** `liveNamespace(table_uuid)` → `RootNamespace{server_root_id + "/" + mirroredArchiveNamespace(table_uuid)}` (swap `serverPrefix()` → `server_root_id`). `shadowNamespace` STAYS pool-global (no server prefix — backups are read by any replica). The owner/identity is now `server_root_id` end-to-end (Phase 0 + this).

- [ ] **Step 1: Failing test** — if a unit test can construct a `ContentAddressedMetadataStorage` with a known `server_root_id`, assert `liveNamespace("uuid")` begins with `<server_root_id>/store/`. If that's impractical at unit level, add a stateless `.sql` assertion or rely on Task-1's relocation tests + an integration check; document the choice. (At minimum: grep that no other code path still composes a namespace from `serverPrefix()` for the live tree.)
- [ ] **Step 2: Run → FAIL** (namespace still uses server-hex).
- [ ] **Step 3: Implement** the one-line swap. Confirm `validateServerRootId(server_root_id)` already ran (Phase 0) so the prefix is a clean path. Confirm `checkNamespace` accepts `<srid>/store/<u3>/<uuid>@cas@`.
- [ ] **Step 4: Build + test** — `ninja unit_tests_dbms`; run the relevant filter + `Cas*:Ca*` (only known failure remains). If a stateless `.sql` content_addressed test now produces objects under `cas/refs/<srid>/...`, that's expected.
- [ ] **Step 5: Commit** `git commit -m "CA Phase1: live namespace prefixed by server_root_id (identity end-to-end)" + trailers`.

---

### Task 3 (companion): server-side paginated `CasObjectStorageBackend::list` {#task-3}

**Files:** `CasObjectStorageBackend.cpp:564-609` (`list`); test in `gtest_cas_backend*.cpp`.

**Why:** the discovery LIST (now `cas/refs/`) is the hot path; the current `list` materializes the WHOLE prefix (`max_keys=0`) + sorts + slices in memory → O(N²/page) across pages. Use the resumable `iterate` primitive.

**Interfaces:** `list(prefix, cursor, limit)` → call `object_storage->iterate(prefix, limit, /*with_tags=*/false, cursor.empty() ? std::nullopt : std::optional<String>(cursor))`, pull up to `limit` entries via `next()/current()`, set `next_cursor` to the last returned key (empty when `!isValid()` after the page), preserving the `ListedKey{key,size,token}` shape (token from the iterate metadata if `supportsListTokens()`). Keep behavior identical for callers (same ordering contract — confirm `iterate` returns lexicographic order like the old sort; if not, document + keep the in-memory sort within the page only).

- [ ] **Step 1: Failing test** — a backend test that LISTs a prefix with >limit keys across multiple `list(prefix, cursor, limit)` calls and asserts: every key returned exactly once, correct `next_cursor` chaining, terminates. (If `InMemoryBackend::list` is the one tested, this task targets `CasObjectStorageBackend` — the real one; test via the object-storage-backed path or assert the RPC count is bounded. If hard to unit-test the real backend, assert the `iterate`-based contract with a fake `IObjectStorage` or document that the integration soak validates it.)
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement** the `iterate`-based pagination; remove the `max_keys=0` full-list + in-memory slice. Tolerate backends where `iterate` is unavailable by falling back ONLY if a capability flag says so (else use iterate). Keep `supportsListTokens` honest (token surfaced iff the iterate metadata carries an etag).
- [ ] **Step 4: Build + test** — `ninja unit_tests_dbms`; backend tests + `CasGc*` (discovery still works) green; `Cas*:Ca*` only-known-failure.
- [ ] **Step 5: Commit** `git commit -m "CA Phase1: server-side paginated backend list via iterate(start_after) (kills max_keys=0 full-materialization)" + trailers`.

---

## Self-Review
**Spec coverage:** refs→cas/refs + manifests→cas/manifests (Task 1); discovery LISTs cas/refs (Task 1); namespace srid-qualified (Task 2 — note: the spec writes `<server_root_id>/store/…`; the impl already had `<server-hex>/store/…`, Task 2 swaps to srid); fsck/sweep follow the relocation (Task 1); manifest identity untouched (Phase 3 reshapes) → no TLA+ change; `max_keys` companion (Task 3). `_files` stay under roots/ ✓. `blobs/`/`trees/` untouched ✓.
**Placeholder scan:** Task-3's test has an "if hard to unit-test the real backend" branch bounded by an objective contract (every key once, chained cursor, terminates) — the implementer picks the concrete harness; not a TODO. Key-string tests are concrete.
**Type consistency:** `casRefsPrefix()`, `manifestNamespacePrefix(ns)`, `rootShardKey`/`manifestKey` (changed base, same signatures) consistent across tasks. `liveNamespace` swap is isolated.
**Risk:** Task 1 is the atomic relocation — the regression gate (full GC round drains + fsck + sweep under cas/refs) is the proof. If discovery's strip prefix and `rootShardKey`'s base get out of sync, cursor keys won't match `per_ns_shard` → token-diff breaks → tests catch it.

## Execution
Subagent-driven: fresh implementer per task, controller self-review between (codex unusable here — times out). Task 1 first (atomic relocation + regression gate), then Task 2 (namespace swap), then Task 3 (pagination companion).

# CAS Optimization Levers §§1–5 (Round B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** land the five Round-B structural levers — GC fold read-buffer right-sizing (§1), dedup-cache sizing (§2, measured), configurable cache validation (§3), manifest-trust relink adoption (§4), and absence-means-Clean blob meta (§5) — each behind current-behavior-preserving defaults where semantics allow, each with its effect measured by a short comparative soak, sequenced strictly §1→§2→§3→§4→§5 on the post-rev.6 baseline.

**Architecture:** §1 reuses the existing `ReadSettings::adjustBufferSize` cap through a new pure header helper. §2 adds no CAS code — it is a measurement run over `dedup_cache_bytes` using counters that already exist (`CasBlobHead`, `CasBlobBodyPutAvoided`, `CasDedupCacheHits/Misses`). §3 introduces one string setting `part_folder_validate` applied only to the `ForceFresh` body re-proof in the part-folder cache. §4 trusts the durable manifest edge on the relink/adopt path and deletes the now-dead promote-time copy-forward. §5 turns the `.meta` object into a pure tombstone (absence = Clean) across five transitions, gated by TLA+ before any code and landing as a single revertible commit. A shared soak-config plumbing task (Task 2) lands early so §2 and §3 can run their matrices.

**Tech Stack:** ClickHouse ProfileEvents (`M(...)` macro), gtest with counter-delta and instrumented-backend assertions (`CountingBackend` / `InMemoryBackend` in `src/Disks/tests/`), TLA+/TLC (`docs/superpowers/models/`, `run_meta.sh` pattern), the `utils/ca-soak/scenarios` phase-3 soak harness.

## Global Constraints

- Spec: `docs/superpowers/specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md` §§1–5 + Sequencing + Testing + Decision log. §0 is DONE (its counters exist); do not re-plan it.
- This round runs AFTER the rev.6 lease-exclusivity work (landed through `9a30f7d99f4`); the code-collision surface is `ProfileEvents.cpp` appends only — never touch `CasStore` publish/lease code.
- No compat scaffolding (pre-release, no persisted data): change transitions in place; never add migration/back-compat branches.
- Allman braces (opening brace on its own line); ProfileEvents descriptions follow the `Cas*` house style (`"CA <subsystem>: <what> (Round-B §...)"`).
- No dead counters (S13 INTROSPECTION-1): every ProfileEvent has ≥1 emit site and ≥1 delta-asserting test; when a lever removes the last emit site of a counter, remove the counter in the same commit.
- Build ONLY via `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/<task>_build.log 2>&1` (a prod-scale scenario campaign may be running — NEVER build the `clickhouse` binary target; the controller rebuilds it after the plan completes).
- After each code task: the filtered sweep `timeout 900 build/src/unit_tests_dbms --gtest_filter='*Cas*:RefWriter*:*RefTableCache*:CaWiring*:CaPartPathParser*:*DedupCache*'` — 0 failed, baseline 977 (post-rev.6) + the task's new tests. (The `*DedupCache*` term is required: the `CaDedupCache` suite is not matched by `*Cas*`.)
- Tests use injectable clocks (no wall-clock reads); any test that must wait crosses a threshold with an injected time source, never a real sleep. A `>= 500ms`-style real delay in a test is a review-blocking discipline break (grep the diff for `sleep`).
- §5 rule: the TLA+ gate (Task 6) MUST be GREEN before ANY §5 code (Task 7); §5 code lands as ONE `git revert`-able commit.
- New commits only; stage files by explicit path (shared worktree); verify `HEAD` after each commit. Commit messages end with:
  `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01EfRKU1Tt1CnaiFYh3kue27`

## File Structure

Source (all under `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/` unless noted):

- `Core/CasObjectStorageBackend.{h,cpp}` — §1: new `casSizedReadSettings` helper + its use in `readObjectRanged` / `openObjectRangedStream`.
- `Core/CasBuild.{h,cpp}` — §4: `DepEntry.adopted` discriminator, promote trust block, removal of `copyForwardFromCondemned` / `isCopyForwardableTokenless`; §5: create/adopt/resurrect meta transitions.
- `Core/CasGc.cpp` — §5: spare tombstone-clear transition (delete stays body-then-meta, already conditional).
- `PartFolderView.{h,cpp}` — §3: immutable `validated_at_ms` timestamp on the view.
- `CachedPartFolderAccess.{h,cpp}` — §3: `PartFolderValidate` type, `CacheParams.validate`, `ForceFresh` age/never branch, `CasPartFolderValidateSkipped`.
- `ContentAddressedMetadataStorage.{h,cpp}` — §3: new ctor param threaded to `CacheParams`; §4: `adoptPartFromManifest` comment update.
- `src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp` — §3: read the `part_folder_validate` config key.
- `src/Common/ProfileEvents.cpp` — §3: `+CasPartFolderValidateSkipped`; §4: `+CasBlobAdoptTrusted`, `-CasBlobCopyForward`; §5: `-CasMetaCreateClean/-CasMetaAdoptBackfill/-CasMetaResurrectClean`, `+CasMetaResurrectClear/+CasMetaSpareClear`.

Tests:

- `src/Disks/tests/gtest_cas_backend.cpp` — §1 helper test.
- `src/Disks/tests/gtest_cas_part_folder_access.cpp` — §3 mode-matrix tests (extends the existing `CountingBackend` fixture).
- `src/Disks/tests/gtest_cas_build.cpp` — §4 trusted-adopt test + migration of 3 copy-forward tests; §5 create/resurrect meta tests.
- `src/Disks/tests/gtest_cas_gc_round.cpp` (or `gtest_cas_gc_leak.cpp`) — §5 spare-clear + condemn-on-absent-meta tests.

Soak / models:

- `utils/ca-soak/configs/storage_conf_tuned_ch{1,2}.xml` (generated), `utils/ca-soak/docker-compose-tuned.yml`, `utils/ca-soak/scenarios/framework/cluster_boot.py` — Task 2 plumbing.
- `docs/superpowers/models/CaMetaAbsenceClean.tla` + `*.cfg` + `run_metaabsence.sh` — §5 TLA+ gate.

---

### Task 1: §1 — GC fold read-buffer right-sizing

Fold body GETs open a ~1 MiB `ReadBufferFromS3` per object while the average fold body is ~3.7 KB. Size the buffer from the known body size (the Native `get` already HEADs it) + a small slack, capped at the current default — reusing `ReadSettings::adjustBufferSize` (`IO/ReadSettings.cpp:40`, already caps at the base `buffer_size`). Mechanical, no setting, no behavior change.

**Files:**
- Modify: `Core/CasObjectStorageBackend.h` (add the helper declaration + a slack constant near the other free helpers)
- Modify: `Core/CasObjectStorageBackend.cpp` (`readObjectRanged` `:426-461`, `openObjectRangedStream` `:468-492`)
- Test: `src/Disks/tests/gtest_cas_backend.cpp`

**Interfaces:**
- Produces: `DB::Cas::casSizedReadSettings(const ReadSettings & base, uint64_t known_size) -> ReadSettings` — returns `base` unchanged when `known_size == 0`, else `base.adjustBufferSize(known_size + CAS_FOLD_READ_SLACK_BYTES)`.

- [ ] **Step 1: Write the failing test**

In `gtest_cas_backend.cpp`, near the other backend unit tests, add (include `<IO/ReadSettings.h>` and the backend header if not already present):

```cpp
TEST(CasSizedReadSettings, CapsToKnownSizePlusSlackButNeverAboveBase)
{
    DB::ReadSettings base;
    base.remote_fs_settings.buffer_size = 1ULL << 20;   /// 1 MiB default
    base.local_fs_settings.buffer_size = 1ULL << 20;

    /// A ~3.7 KB fold body: buffer shrinks to size + slack, far below the 1 MiB default.
    const auto small = DB::Cas::casSizedReadSettings(base, 3700);
    EXPECT_EQ(small.remote_fs_settings.buffer_size, 3700 + DB::Cas::CAS_FOLD_READ_SLACK_BYTES);
    EXPECT_EQ(small.local_fs_settings.buffer_size, 3700 + DB::Cas::CAS_FOLD_READ_SLACK_BYTES);

    /// A body larger than the default is capped AT the default (never grown).
    const auto big = DB::Cas::casSizedReadSettings(base, 8ULL << 20);
    EXPECT_EQ(big.remote_fs_settings.buffer_size, 1ULL << 20);

    /// Unknown size (0) = leave the base untouched (the metadata-fetch fallback path).
    const auto unknown = DB::Cas::casSizedReadSettings(base, 0);
    EXPECT_EQ(unknown.remote_fs_settings.buffer_size, 1ULL << 20);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/s1_build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasSizedReadSettings.*'`
Expected: COMPILE ERROR — `casSizedReadSettings` / `CAS_FOLD_READ_SLACK_BYTES` not members of `DB::Cas`.

- [ ] **Step 3: Implement**

In `Core/CasObjectStorageBackend.h`, inside `namespace DB::Cas`, add (near the top-level free declarations):

```cpp
/// §1 (Round-B): the fold/point GETs read tiny bodies (~3.7 KB avg) but a default ReadBufferFromS3
/// preallocates ~1 MiB. When the body size is already known (the Native `get` HEADs it first), shrink
/// the read buffer to `size + slack`, capped at the caller's default — a pure reuse of
/// `ReadSettings::adjustBufferSize`. `known_size == 0` means "unknown", leave the settings untouched.
constexpr uint64_t CAS_FOLD_READ_SLACK_BYTES = 4096;
ReadSettings casSizedReadSettings(const ReadSettings & base, uint64_t known_size);
```

In `Core/CasObjectStorageBackend.cpp`, add the definition next to the other file-scope helpers (after `openObjectRangedStream`), inside `namespace DB` where `Cas::` symbols are defined — or qualify:

```cpp
ReadSettings Cas::casSizedReadSettings(const ReadSettings & base, uint64_t known_size)
{
    if (known_size == 0)
        return base;
    return base.adjustBufferSize(known_size + CAS_FOLD_READ_SLACK_BYTES);
}
```

Then change the two `readObject` call sites to size down when `known_size` is known.

`readObjectRanged` (`:429`):
```cpp
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
```

`openObjectRangedStream` (`:471`):
```cpp
    auto buf = object_storage.readObject(
        StoredObject(path), casSizedReadSettings(getReadSettings(), known_size), /*read_hint=*/std::nullopt);
```

(Both functions already receive `known_size`; the Native `get` at `:560` threads `hr->size` into `readObjectRanged`. No other change — the whole-body and ranged branches are correctness-independent of buffer size.)

- [ ] **Step 4: Run to verify it passes**

Run: same build + `--gtest_filter='CasSizedReadSettings.*'`
Expected: 1 PASSED.

- [ ] **Step 5: Filtered sweep + commit**

Run the Global-Constraints sweep (0 failed). Then:
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasObjectStorageBackend.cpp \
  src/Disks/tests/gtest_cas_backend.cpp
git commit -m "cas: §1 right-size fold/point read buffers to the known body size"
```

- [ ] **Step 6: Soak verify (acceptance)**

Rebuild the server binary is the controller's job; the acceptance metric is fold buffer churn. After the controller rebuilds, a 10-minute phase-3 soak (Task 2 plumbing not required — no config varies) must show GC fold allocation churn collapse (the audit's 1.96 GB/round → near-zero) with no change in `CasGc*` outcome counters. Record the delta in the spec §1 note.

---

### Task 2: Soak-matrix variant-config plumbing (EARLY — enables §2 and §3 matrices)

Today a config knob is varied by hand-writing a whole `storage_conf_<variant>.xml` + a `docker-compose-<variant>.yml` and registering it in `COMPOSE_VARIANTS` (`utils/ca-soak/scenarios/framework/cluster_boot.py:24-38`; the S24 `smalldedupcache` variant does exactly this — `configs/storage_conf_small_dedup_cache_ch1.xml:26` sets `<dedup_cache_bytes>1048576</dedup_cache_bytes>` inside the `<ca>` disk block). The §2 matrix (dedup default/×4/×16) and §3 matrix (validate always/age 5/age 60/never) would need seven such hand-authored pairs. This task adds ONE template-render path so a run passes an overrides dict instead.

**Files:**
- Create: `utils/ca-soak/docker-compose-tuned.yml` (a copy of `docker-compose.yml` that mounts `configs/storage_conf_tuned_ch1.xml` / `_ch2.xml` in place of `storage_conf_ch1/2.xml`; identical otherwise)
- Modify: `utils/ca-soak/scenarios/framework/cluster_boot.py` (`COMPOSE_VARIANTS` `:24`, `NODE_COUNTS` `:42`, `reset_cluster` `:133`)
- Test: `utils/ca-soak/scenarios/tests/` (a small pytest that renders and asserts the XML)

**Interfaces:**
- Produces: `render_tuned_config(overrides: dict[str, str]) -> None` in `cluster_boot.py` — reads `configs/storage_conf_ch{1,2}.xml`, inserts one child element per `overrides` item inside the `<ca>` element (replacing any existing same-named child), and writes `configs/storage_conf_tuned_ch{1,2}.xml`. Consumed by `reset_cluster(variant="tuned", overrides={...})`.
- Override keys used downstream: `dedup_cache_bytes` (§2), `part_folder_validate` (§3).

- [ ] **Step 1: Write the failing test**

`utils/ca-soak/scenarios/tests/test_render_tuned_config.py`:
```python
from framework import cluster_boot

def test_render_injects_overrides_into_ca_block(tmp_path, monkeypatch):
    monkeypatch.chdir(cluster_boot.CA_SOAK_ROOT)  # the dir holding configs/
    cluster_boot.render_tuned_config({"dedup_cache_bytes": "268435456",
                                      "part_folder_validate": "age 5"})
    for node in ("ch1", "ch2"):
        xml = (cluster_boot.CA_SOAK_ROOT / "configs" / f"storage_conf_tuned_{node}.xml").read_text()
        assert "<dedup_cache_bytes>268435456</dedup_cache_bytes>" in xml
        assert "<part_folder_validate>age 5</part_folder_validate>" in xml
        assert "<metadata_type>content_addressed</metadata_type>" in xml  # base block preserved
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd utils/ca-soak && python -m pytest scenarios/tests/test_render_tuned_config.py -q`
Expected: FAIL — `render_tuned_config` / `CA_SOAK_ROOT` not defined.

- [ ] **Step 3: Implement**

In `cluster_boot.py`, add a module constant and the render function (use `xml.etree.ElementTree`, which preserves the existing structure and only touches the `<ca>` child list):
```python
from pathlib import Path
import xml.etree.ElementTree as ET

CA_SOAK_ROOT = Path(__file__).resolve().parents[2]   # .../utils/ca-soak

def render_tuned_config(overrides: dict) -> None:
    """Render configs/storage_conf_tuned_ch{1,2}.xml from the base storage_conf, injecting one child
    element per override inside the <ca> disk block (replacing a same-named child). §2/§3 soak matrices
    feed one variable per run through here instead of hand-authoring a compose+XML pair per value."""
    for node in ("ch1", "ch2"):
        base = CA_SOAK_ROOT / "configs" / f"storage_conf_{node}.xml"
        tree = ET.parse(base)
        ca = tree.getroot().find("./storage_configuration/disks/ca")
        if ca is None:
            raise RuntimeError(f"no <ca> disk block in {base}")
        for key, value in overrides.items():
            existing = ca.find(key)
            if existing is not None:
                ca.remove(existing)
            child = ET.SubElement(ca, key)
            child.text = str(value)
        tree.write(CA_SOAK_ROOT / "configs" / f"storage_conf_tuned_{node}.xml",
                   encoding="unicode", xml_declaration=False)
```

Register the variant: add `"tuned": "docker-compose-tuned.yml"` to `COMPOSE_VARIANTS` (`:24`) and `"tuned": 2` to `NODE_COUNTS` (`:42`). In `reset_cluster` (`:133`), accept an optional `overrides=None` and call `render_tuned_config(overrides)` before the `up` when `variant == "tuned"` and `overrides`. Create `docker-compose-tuned.yml` by copying `docker-compose.yml` and replacing the two `storage_conf_ch{1,2}.xml` mount lines (`:75`, `:116`) with `storage_conf_tuned_ch{1,2}.xml`.

- [ ] **Step 4: Run to verify it passes**

Run: `cd utils/ca-soak && python -m pytest scenarios/tests/test_render_tuned_config.py -q`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add utils/ca-soak/scenarios/framework/cluster_boot.py \
  utils/ca-soak/docker-compose-tuned.yml \
  utils/ca-soak/scenarios/tests/test_render_tuned_config.py
git commit -m "cas soak: tuned compose variant with per-run <ca> config overrides"
```

(The generated `configs/storage_conf_tuned_ch{1,2}.xml` are run artifacts — do not commit them.)

---

### Task 3: §2 — Dedup-cache sizing (measurement, no CAS code)

`dedup_cache_bytes` already exists (`MetadataStorageFactory.cpp:267`, default 64 MiB; pool field `CasStore.h:126`; cache built `CasStore.cpp:115`) and the judging counters already exist (`CasBlobHead:744`, `CasBlobBodyPutAvoided:748`, `CasDedupCacheHits/Misses:861-862`). Per the spec's 2026-07-14 mechanics correction, a dedup-cache HIT does NOT skip the occupancy HEAD — it selects `putBlob`'s `head_first` branch, so the HEAD still runs on hits and what a hit avoids is the body PUT (`CasBlobBodyPutAvoided`). Judge the matrix by BOTH `CasBlobHead` AND `CasBlobBodyPutAvoided`/PUT-class deltas, with `CasDedupCacheHits/Misses` as the per-lookup denominators. This task has NO code; its deliverable is a recorded tuning decision.

**Files:**
- Modify (documentation only, Step 4): `docs/superpowers/specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md` §2 (append the measured decision)

- [ ] **Step 1: Baseline + matrix runs**

Using Task 2's plumbing, run three 10-minute phase-3 soaks, same seed, one variable per run (see `reference_ca_soak_duration_phase3`: phase 3 `--duration 10m` is time-driven):
- default: `reset_cluster(variant="tuned", overrides={"dedup_cache_bytes": "67108864"})`
- ×4: `overrides={"dedup_cache_bytes": "268435456"}`
- ×16: `overrides={"dedup_cache_bytes": "1073741824"}`

- [ ] **Step 2: Collect deltas from `metric_log`**

For each run read the per-family sums from `system.metric_log` (NOT `system.events` — it resets on chaos restarts): `CasBlobHead`, `CasBlobBodyPutAvoided`, `CasBlobPut`, `CasBlobPutDedup`, `CasDedupCacheHits`, `CasDedupCacheMisses`. Compute hit-rate = `Hits/(Hits+Misses)` and the PUT-class total = `CasBlobPut + CasBlobPutDedup`.

- [ ] **Step 3: Decide**

Pick the smallest `dedup_cache_bytes` past which `CasBlobBodyPutAvoided` and the PUT-class total flatten (diminishing returns), noting the `CasBlobHead` cost stays flat-to-up (a bigger cache does not remove HEADs — it trades body PUTs for HEADs).

- [ ] **Step 4: Record the decision (the deliverable)**

Append to spec §2 the table (bytes → CasBlobHead, CasBlobBodyPutAvoided, PUT-class total, hit-rate) and the chosen default. If the chosen default differs from 64 MiB, note that the factory default (`MetadataStorageFactory.cpp:267`) change is a follow-up one-liner (out of scope for this measurement task — a default flip ships only with a green matrix). Commit the spec edit:
```bash
git add docs/superpowers/specs/2026-07-13-cas-memory-s3-budget-optimizations-design.md
git commit -m "cas: §2 dedup-cache sizing — measured decision recorded"
```

---

### Task 4: §3 — Configurable cache validation (`part_folder_validate`)

The mass `HEAD`s come from `ForceFresh` `getView` call sites (`CachedPartFolderAccess::getView` `:90` → `buildView` `:128` → `readManifestShared`'s mandatory HEAD), where the part is locally pinned and the body cannot legally vanish — a fail-closed `INV-NO-DANGLE` net, not healthy-protocol correctness. Add one setting applied to that re-proof; `CachedForLoad` (already skips the HEAD via the retained-view hit at `:62`) and `StrictValidate` (bypasses retention entirely, `:100`) are untouched.

Design (resolved ambiguity D): the spec's `always | age <X> | never` is one string setting `part_folder_validate` parsed to `{Always, Age(seconds), Never}`. Under `Age`/`Never`, `ForceFresh` may serve a retained view whose last body validation is recent enough, skipping the HEAD. Freshness of the ref itself still comes from `resolve` (`:52`, `allow_stale=false` for `ForceFresh`) — only the body-existence HEAD is skipped.

**Files:**
- Modify: `PartFolderView.h` (`:25-27` ctor, `:58-65` members) + `PartFolderView.cpp` (`make` / ctor) — add immutable `validated_at_ms`
- Modify: `CachedPartFolderAccess.h` (`CacheParams` `:33-43`, add `PartFolderValidate`), `CachedPartFolderAccess.cpp` (`getView` `:47-121`)
- Modify: `ContentAddressedMetadataStorage.{h,cpp}` (ctor param `:140-142` region → `CacheParams` `:446-449`)
- Modify: `MetadataStorageFactory.cpp` (read the key near `:297`, thread into the ctor call `:313-320`)
- Modify: `src/Common/ProfileEvents.cpp` (append `CasPartFolderValidateSkipped`)
- Test: `src/Disks/tests/gtest_cas_part_folder_access.cpp`

**Interfaces:**
- Produces: `struct PartFolderValidate { enum class Mode { Always, Age, Never }; Mode mode = Mode::Always; uint64_t age_seconds = 0; };` (in `CachedPartFolderAccess.h`), `CacheParams.validate`, `PartFolderView::validatedAtMs()`, ProfileEvent `CasPartFolderValidateSkipped`.
- Consumes: `PartFolderView` gains `uint64_t validated_at_ms` set at construction (a body-HEAD-proven moment).

- [ ] **Step 1: Write the failing tests**

In `gtest_cas_part_folder_access.cpp` (reuse `CountingBackend`, `openStoreForTest`, `publishPart`, `deleteManifestBody` — the existing fixture; use an injected clock — the test constructs the view timeline with `CacheParams.validate.age_seconds` and drives `getView` twice, physically deleting the body between calls to prove the HEAD was or was not paid):

```cpp
TEST(CasPartFolderAccess, ValidateNeverServesRetainedViewWithoutBodyHead)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    auto params = cacheOn();
    params.validate = {ContentAddressed::PartFolderValidate::Mode::Never, 0};
    ContentAddressed::CachedPartFolderAccess access(store, params);
    const ContentAddressed::PartRefKey key{ns, "part_1"};

    /// Prime the retained view (pays the HEAD once).
    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::ForceFresh), nullptr);
    /// Body vanishes (a protocol violation the net would normally catch)...
    deleteManifestBody(*backend, layout, id);
    const auto skips_before = ProfileEvents::global_counters[ProfileEvents::CasPartFolderValidateSkipped].load();
    /// ...but `never` serves the retained view, no HEAD, no throw.
    EXPECT_NO_THROW(access.getView(key, ContentAddressed::Freshness::ForceFresh));
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasPartFolderValidateSkipped].load() - skips_before, 1);
}

TEST(CasPartFolderAccess, ValidateAlwaysStillHeadsEveryForceFresh)
{
    auto backend = std::make_shared<CountingBackend>();
    auto store = openStoreForTest(backend);
    const Cas::Layout layout("p");
    const Cas::RootNamespace ns{"srv/t1"};
    const auto id = publishPart(store, ns, "part_1", {inlineEntry("checksums.txt", "cs")});

    ContentAddressed::CachedPartFolderAccess access(store, cacheOn());   /// default = Always
    const ContentAddressed::PartRefKey key{ns, "part_1"};
    ASSERT_NE(access.getView(key, ContentAddressed::Freshness::ForceFresh), nullptr);
    deleteManifestBody(*backend, layout, id);
    /// `always` re-proves the body every ForceFresh — the deleted body surfaces as FILE_DOESNT_EXIST.
    expectThrowsCode(ErrorCodes::FILE_DOESNT_EXIST,
        [&] { access.getView(key, ContentAddressed::Freshness::ForceFresh); });
}
```

Also update the existing `TEST(CasPartFolderAccess, ...)` at `:184-200` ("delete live manifest body, every mode throws") so its loop asserts the **default** (`Always`) behavior — it already constructs `access(store)` with retention off, so it is unaffected, but add a one-line comment that `always` is the mode under test (the `never`/`age` skip is proven by the two tests above).

- [ ] **Step 2: Run to verify it fails**

Run: `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/s3_build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasPartFolderAccess.Validate*'`
Expected: COMPILE ERROR — `PartFolderValidate` / `CacheParams::validate` / `CasPartFolderValidateSkipped` undefined.

- [ ] **Step 3: Implement — view timestamp**

`PartFolderView.h`: add a member `uint64_t validated_at_ms = 0;` and accessor `uint64_t validatedAtMs() const { return validated_at_ms; }`; add a `uint64_t validated_at_ms_` ctor parameter. `PartFolderView.cpp`: `make` and the ctor stamp it (`make` is only reached after `readManifestShared`'s HEAD, so construction time IS a body-proven moment). Use the repo's `nowMs()` helper (same one `promote` uses at `CasBuild.cpp:1163`); the mutable-refresh clone at `CachedPartFolderAccess.cpp:76` carries the cached view's timestamp forward (`cached->validatedAtMs()`) — a mutable drift did not re-prove the body.

- [ ] **Step 4: Implement — setting type + counter**

`CachedPartFolderAccess.h`, above `CacheParams`:
```cpp
    struct PartFolderValidate
    {
        enum class Mode : uint8_t { Always, Age, Never };
        Mode mode = Mode::Always;
        uint64_t age_seconds = 0;    /// only meaningful for Mode::Age
    };
```
Add `PartFolderValidate validate;` to `CacheParams`.

`src/Common/ProfileEvents.cpp`, after the last `Cas*` entry (`CasRefRecoverySealPublished`, `:863`):
```cpp
    M(CasPartFolderValidateSkipped, "CA part-folder cache: ForceFresh body re-proof HEADs skipped because part_folder_validate is 'never' or the retained view's last validation is younger than the 'age' window (Round-B §3)", ValueType::Number) \
```

- [ ] **Step 5: Implement — `getView` ForceFresh age/never branch**

In `CachedPartFolderAccess.cpp::getView`, after the `CachedForLoad` block (`:88`) and before `buildView` (`:90`), add the `ForceFresh` skip path. It reuses the retained view only when the fresh `resolve` still matches it (manifest id + mutable files) and the age policy allows it:
```cpp
    /// §3: ForceFresh may serve a retained view WITHOUT the mandatory body HEAD when part_folder_validate
    /// permits — the ref currency is proven by `resolve` above; only the INV-NO-DANGLE body re-proof is
    /// skipped. StrictValidate never enters here (it bypasses retention). CachedForLoad is handled above.
    if (freshness == Freshness::ForceFresh && view_cache
        && params.validate.mode != PartFolderValidate::Mode::Always)
    {
        if (auto cached = view_cache->get(cache_key);
            cached && cached->manifestId() == resolved->manifest_id
            && cached->mutableFiles() == resolved->mutable_files)
        {
            const bool fresh_enough = params.validate.mode == PartFolderValidate::Mode::Never
                || (nowMs() - cached->validatedAtMs()) < params.validate.age_seconds * 1000ULL;
            if (fresh_enough)
            {
                ProfileEvents::increment(ProfileEvents::CasPartFolderViewHits);
                ProfileEvents::increment(ProfileEvents::CasPartFolderValidateSkipped);
                recordDecision(cache_key, LastDecision::Hit, cached.get(), /*retained=*/true);
                return cached;
            }
        }
    }
```
(Include the header that declares `nowMs()` — the same one `CasBuild.cpp` uses.)

- [ ] **Step 6: Implement — config plumbing**

`MetadataStorageFactory.cpp`, near `:297`:
```cpp
        const std::string part_folder_validate_raw = config.getString(config_prefix + ".part_folder_validate", "always");
```
Parse it with a small local helper (accept `always`, `never`, `age <N>`; anything else throws `BAD_ARGUMENTS`) into a `ContentAddressed::CachedPartFolderAccess::PartFolderValidate`, and pass it as a new trailing ctor argument. Thread a matching `PartFolderValidate` parameter through the `ContentAddressedMetadataStorage` ctor (`.h`/`.cpp` `:140-142` region) into the `CacheParams{ ... .validate = ... }` at `:446-449`.

- [ ] **Step 7: Run to verify it passes**

Run: same build + `--gtest_filter='CasPartFolderAccess.*'`
Expected: all PASS (the two new + the unchanged existing ones).

- [ ] **Step 8: Sweep + commit**

Run the sweep (0 failed). Then:
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/PartFolderView.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/CachedPartFolderAccess.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/MetadataStorageFactory.cpp \
  src/Common/ProfileEvents.cpp \
  src/Disks/tests/gtest_cas_part_folder_access.cpp
git commit -m "cas: §3 part_folder_validate setting (always[default]/age/never) on the ForceFresh re-proof"
```

- [ ] **Step 9: Soak matrix (acceptance)**

After the controller rebuilds, run four 10-minute soaks via Task 2 plumbing: `part_folder_validate` = `always`, `age 5`, `age 60`, `never`. Judge by `CasBlobHead` / `CasPartFolderManifestGets` deltas (the HEAD reduction) and confirm "genuine divergence still fails" (a real manifest change under a retained view still rebuilds via the mismatch at `:85`). Record the chosen default recommendation in spec §3.

---

### Task 5: §4 — Manifest-trust relink adoption

A relink/adopt promote re-observes every tokenless (adoptEvidence) blob leaf: `store->backend().head(blob_key)` (`CasBuild.cpp:1106`) + `loadMeta(...)` (`:1113`), ~68 HEAD + ~36 GET per part, the largest single read-class consumer. Replace the per-file observation on this path with manifest trust: the SOURCE pins the part for the whole fetch (in-degree ≥ 1 ⇒ not condemnable) and the FETCHER's precommit edge is durable before adoption (EDGE-BEFORE-OBSERVE holds literally — edge, then no observe at all). This matches the D4 relink trust model (ordinary ReplicatedMergeTree interserver trust).

Resolved ambiguity B: both `adoptEvidence` (`CasBuild.cpp:823`) and `recordPendingBlobDep` (`:830`) record an indistinguishable tokenless `DepEntry{Blob, nullopt, size}`; a pending-upload dep that reached promote un-tokened is a staging BUG that must still fail closed. So add an `adopted` discriminator (set only by `adoptEvidence`) and trust ONLY adopted tokenless leaves; a tokenless non-adopted leaf at promote fails closed. `copyForwardFromCondemned` / `isCopyForwardableTokenless` are then dead (their only caller is the promote loop) and are removed, along with the now-dead `CasBlobCopyForward` counter (only emit site `CasBuild.cpp:756`).

**Files:**
- Modify: `Core/CasBuild.h` (`DepEntry` `:158-163`, remove `copyForwardFromCondemned` `:196` / `isCopyForwardableTokenless` `:215`)
- Modify: `Core/CasBuild.cpp` (`adoptEvidence` `:811`, promote loop `:1095-1130`, delete `copyForwardFromCondemned` `:677-808` / `isCopyForwardableTokenless` `:229-233`, extern block `:18-26`)
- Modify: `src/Common/ProfileEvents.cpp` (`+CasBlobAdoptTrusted`, `-CasBlobCopyForward:749`)
- Modify: `ContentAddressedMetadataStorage.cpp` (`adoptPartFromManifest` comments `:1263-1269`, `:1291-1308`)
- Test: `src/Disks/tests/gtest_cas_build.cpp` (new trusted-adopt test; migrate `PromoteCondemnedTokenlessBlobCopiesForward`, `PromoteAbsentTokenlessBlobAbortsRetryable` `:1006`, `PromoteCondemnedLeafWithoutDepAbortsFailClosed` `:1036`)

**Interfaces:**
- Produces: `DepEntry.adopted` (bool), `Build::isTrustedAdopt(const BlobRef &) const`, ProfileEvent `CasBlobAdoptTrusted`, audit event `reason="manifest-trust"` (empty token).

- [ ] **Step 1: PLAN GATE — grep-proof the adopted-dep consumer set**

Before writing code, prove the spec's claim that the only consumers of adopted-dep observation/tokens are the displacement branch, rollback, and the B170 token-join — all on the write/dedup path (`observeAndAdmit`/`putBlob`), NOT the relink promote loop. Run and record:
```bash
grep -rn "copyForwardFromCondemned\|isCopyForwardableTokenless" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
grep -rn "adoptEvidence\|recordPendingBlobDep" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/
grep -rn "depIsTokened\|\.token\b" src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp
```
Expected: `copyForwardFromCondemned` + `isCopyForwardableTokenless` are called ONLY at the promote loop (`:1117`, `:1120`); `adoptEvidence` callers are `publishEntries` + `ContentAddressedTransaction.cpp:218/935` (all committed-source adopts); `depIsTokened` gates the edge-protected branch. If any OTHER consumer of an adopted-dep token surfaces, STOP — §4 returns to design (spec §4 plan gate).

- [ ] **Step 2: Write the failing test**

In `gtest_cas_build.cpp`, add (use `CountingBackend` so we can assert ZERO probes on the blob/meta keys during promote; model the adopt+precommit+promote shape on the existing `Promote*Tokenless*` tests):
```cpp
TEST(CasBuild, PromoteTrustsAdoptedLeafNoProbeManifestTrust)
{
    auto b = std::make_shared<CountingBackend>();
    auto s = openStore(b);
    const RootNamespace ns{"srv1/tbl"};
    /// A committed-source blob lives in the shared pool.
    { auto seed = s->startBuild({}); seed->putBlob(streamRefOf("payload-TR"), BlobSource::fromString("payload-TR")); }
    const String blob_key = s->layout().blobKey(streamRefOf("payload-TR"));
    const String meta_key = s->layout().blobMetaKey(streamRefOf("payload-TR"));

    auto build = startBuildFor(s, ns, "part_1");
    const ManifestEntry entry = blobManifestEntryStreaming("data.bin", "payload-TR");
    build->adoptEvidence(entry);
    const ManifestId id = build->stageManifest({entry});
    build->precommitAdd(ns, "part_1", id);

    const auto trusted_before = ProfileEvents::global_counters[ProfileEvents::CasBlobAdoptTrusted].load();
    const auto head_before = b->headCountFor(blob_key);       // CountingBackend accessor
    const auto meta_get_before = b->getCountFor(meta_key);

    build->promote(ns, "part_1", build->buildId(), id);

    EXPECT_TRUE(s->resolveRef(ns, "part_1").has_value());
    EXPECT_EQ(ProfileEvents::global_counters[ProfileEvents::CasBlobAdoptTrusted].load() - trusted_before, 1);
    EXPECT_EQ(b->headCountFor(blob_key) - head_before, 0) << "trust must not HEAD the adopted blob";
    EXPECT_EQ(b->getCountFor(meta_key) - meta_get_before, 0) << "trust must not loadMeta the adopted blob";
}
```
(If `CountingBackend` lacks per-key `headCountFor` / `getCountFor`, add them to the fixture in `gtest_cas_build.cpp` — a simple per-key counter map incremented in its overridden `head` / `get`.)

- [ ] **Step 3: Run to verify it fails**

Run: `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/s4_build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasBuild.PromoteTrusts*'`
Expected: COMPILE ERROR — `CasBlobAdoptTrusted` undefined (RED).

- [ ] **Step 4: Implement — discriminator + trust block**

`CasBuild.h`: add `bool adopted = false;` to `DepEntry` (comment: "true only for adoptEvidence — a committed-source W-EVIDENCE dep, trusted at promote (§4)"). Replace the `isCopyForwardableTokenless` declaration (`:215`) with `bool isTrustedAdopt(const BlobRef & ref) const;` and delete the `copyForwardFromCondemned` declaration (`:196`).

`CasBuild.cpp`:
- `adoptEvidence` (`:823`): `deps[entry.ref] = DepEntry{ObjectKind::Blob, std::nullopt, entry.blob_size, /*adopted=*/true};`
- Replace `isCopyForwardableTokenless` (`:229-233`) with:
```cpp
bool Build::isTrustedAdopt(const BlobRef & ref) const
{
    /// §4: a leaf trusted at promote iff this build holds a TOKENLESS dep recorded by adoptEvidence
    /// (a committed-source W-EVIDENCE adopt: the source pins it, in-degree >= 1, not condemnable). A
    /// tokenless PENDING-upload dep (recordPendingBlobDep, adopted=false) is NOT trusted — it must be
    /// tokened by putBlob before promote; reaching promote un-tokened is a staging bug (fail closed).
    auto it = deps.find(ref);
    return it != deps.end() && !it->second.token.has_value() && it->second.adopted;
}
```
- Delete `copyForwardFromCondemned` (`:677-808`) entirely; drop `extern const Event CasBlobCopyForward;` (`:20`).
- Promote loop (`:1095-1130`): replace the per-non-tokened-leaf observe/copy-forward body with the trust block:
```cpp
            for (const ManifestEntry & e : body.entries)
            {
                if (e.placement != EntryPlacement::Blob)
                    continue;
                if (depIsTokened(e.ref))
                    continue;   /// edge-protected (EDGE-BEFORE-OBSERVE); putBlob validated under the durable edge
                /// §4 manifest-trust: a tokenless adoptEvidence leaf is trusted — the committed source pins
                /// it (in-degree >= 1, not condemnable) and this build's precommit edge is durable. No HEAD,
                /// no loadMeta, no copy-forward; the durable manifest edge is the liveness evidence.
                if (!isTrustedAdopt(e.ref))
                    throw Exception(ErrorCodes::LOGICAL_ERROR,
                        "promote: blob leaf {} has no tokened and no adopted dep at commit — a staging bug "
                        "(a pending upload never completed); failing closed",
                        store->layout().blobKey(e.ref));
                ProfileEvents::increment(ProfileEvents::CasBlobAdoptTrusted);
                EventEmitter{*store}.emit([&](CasEvent & ev)
                {
                    ev.type = CasEventType::BlobReuseAdopt;
                    ev.object_kind = CasEventObjectKind::Blob;
                    ev.object_hash = blobIdOf(e.ref);
                    ev.outcome = "adopt";
                    ev.reason = "manifest-trust";   /// distinguishable trusted-adopt class (empty token)
                });
            }
```

`ProfileEvents.cpp`: delete the `CasBlobCopyForward` line (`:749`); append after Task 4's §3 line:
```cpp
    M(CasBlobAdoptTrusted, "CA blob: relink/adopt promote leaves trusted via the durable manifest edge — no per-file HEAD/loadMeta probe (Round-B §4; reason=manifest-trust)", ValueType::Number) \
```

- [ ] **Step 5: Migrate the affected copy-forward tests**

- `PromoteTrustsAdoptedLeafNoProbeManifestTrust` — the new positive (Step 2).
- `PromoteCondemnedTokenlessBlobCopiesForward` (`:~970-1004`) — DELETE (copy-forward-at-promote no longer exists; its behavior is replaced by trust, covered by the new test). Add a one-line note in the commit body.
- `PromoteAbsentTokenlessBlobAbortsRetryable` (`:1006-1034`) — DELETE and add a replacement `TEST(CasBuild, PromoteTrustsAdoptedLeafEvenIfBackendRaced)` documenting the trust trade-off: an adopted leaf is published without a presence probe (absence detection moves to fsck / actual body GETs, per the D4 interserver-trust model). Keep it minimal — assert the ref publishes.
- `PromoteCondemnedLeafWithoutDepAbortsFailClosed` (`:1036+`) — KEEP but update: a leaf with NO dep is neither tokened nor adopted, so `isTrustedAdopt` is false → the new `LOGICAL_ERROR` fail-closed fires. Change the expected code from `ABORTED` to `LOGICAL_ERROR` and the comment to reference `isTrustedAdopt`.

- [ ] **Step 6: Update `adoptPartFromManifest` comments**

`ContentAddressedMetadataStorage.cpp:1263-1269` and `:1291-1308`: the byte-fetch fallback no longer triggers on a condemned/absent adopted blob (trust removes the promote probe). Update the comments to say the fallback now fires only on manifest decode failure or a non-blob error, and that a genuinely-missing adopted blob (an invariant violation) is caught by fsck, not at promote — matching the D4 relink trust model. Do NOT change the control flow (the `catch (ABORTED)` still handles the pending-bug fail-closed and precommit-not-live cases).

- [ ] **Step 7: Run to verify it passes**

Run: same build + `--gtest_filter='CasBuild.*'`
Expected: all PASS (new + migrated).

- [ ] **Step 8: Sweep + commit**

Run the sweep (0 failed). Then:
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.h \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/ContentAddressedMetadataStorage.cpp \
  src/Common/ProfileEvents.cpp \
  src/Disks/tests/gtest_cas_build.cpp
git commit -m "cas: §4 manifest-trust relink adoption — trust the durable edge, drop per-file promote probes"
```

- [ ] **Step 9: Soak verify (acceptance)**

After the controller rebuilds, a 10-minute soak that drives relink/attach (S-scenarios that fetch parts) must show the read-class GET+HEAD drop (~30% target) with `CasBlobAdoptTrusted` accumulating and no new dangling-ref / fsck findings. Record in spec §4.

---

### Task 6: §5 — TLA+ gate (absence-means-Clean), GREEN before any code

The spec's §5 makes `.meta` a pure tombstone (absence = Clean) across five transitions with an unsafe direction (reverse resurrect order) excluded by construction. This is a SEMANTIC change to condemn/resurrect invariants: model it and prove it before touching code. The existing `CaMetaDescriptor.tla` models the PRE-§5 envelope design (create writes Clean, resurrect CAS-to-Clean) and `CaMetaDescriptorRaw.tla` is the REJECTED raw-body variant — neither is the §5 model. Write a new model on their vocabulary.

**Files:**
- Create: `docs/superpowers/models/CaMetaAbsenceClean.tla`
- Create: `docs/superpowers/models/CaMetaAbsenceClean_reduced.cfg` (safe, all sabotages off) + one cfg per sabotage
- Create: `docs/superpowers/models/run_metaabsence.sh` (copy of `run_meta.sh` pointing at `CaMetaAbsenceClean.tla`)

**Interfaces:**
- Produces: a GREEN safe run and a violating run per sabotage — the gate the §5 code (Task 7) depends on.

- [ ] **Step 1: Author the model**

Model the ENVELOPE (fresh-incarnation-token) world with `metaState ∈ {absent, condemned}` (there is no `clean` object — absence IS clean) and `body ∈ {absent, tok}`. Encode exactly the five transitions from spec §5:
1. `Create` = body PUT only (fresh `tok`); NO meta write, NO meta read.
2. `GcCondemn` = `metaState := condemned` (CAS, round-stamped); body unchanged.
3. `GcDelete` = delete body at the exact condemn-time token THEN delete meta at the exact etag (both conditional, ordered body-before-meta).
4. `Resurrect` = fresh body first (new `tok`) THEN delete the tombstone (If-Match observed etag).
5. `Spare/Heal` = a recheck meeting a tombstone with in-degree ≥ 1 deletes the tombstone (If-Match).
Add crash points between each ordered pair. Define the load-bearing invariant:
```
INV_ABSENCE_NO_QUEUED_DELETE ==
  (metaState = "absent") => ~(\E d \in queuedDeletes : d.tok = body.tok)
```
plus the analogues `INV_NO_DANGLE` (a live ref's body is never queued for exact-token delete), `INV_NO_LOSS`, and `INV_META_BODY`-analogue as in `CaMetaDescriptor.tla`. Add sabotage flags for the unsafe directions: `SabResurrectMetaFirst` (delete tombstone BEFORE the fresh body — the excluded direction), `SabGcDeleteMetaFirst` (delete meta before body), `SabAdoptOverTombstone`, `SabCreateReadsMeta` (create born under a stale tombstone). Each MUST break `INV_ABSENCE_NO_QUEUED_DELETE` or `INV_NO_DANGLE`.

- [ ] **Step 2: Write the runner + cfgs**

`run_metaabsence.sh` = `run_meta.sh` with the final `CaMetaDescriptor.tla` replaced by `CaMetaAbsenceClean.tla`. `CaMetaAbsenceClean_reduced.cfg` (safe): all `Sab* = FALSE`, `INVARIANT` lines for TypeOK + the four invariants. One cfg per sabotage flipping exactly that flag to `TRUE`.

- [ ] **Step 3: Run the gate**

Run and record (log paths land in `../../../tmp/`):
```bash
cd docs/superpowers/models
./run_metaabsence.sh CaMetaAbsenceClean_reduced.cfg          # MUST print "Model checking completed" (GREEN)
for f in CaMetaAbsenceClean_sab_*.cfg; do ./run_metaabsence.sh "$f"; done   # each MUST print a violation
```
Expected: safe = no violation; every sabotage = an invariant violation. If the safe cfg violates, the §5 protocol as modeled is wrong — STOP and reconcile with the spec before any code.

- [ ] **Step 4: Commit the gate**

```bash
git add docs/superpowers/models/CaMetaAbsenceClean.tla docs/superpowers/models/CaMetaAbsenceClean_*.cfg docs/superpowers/models/run_metaabsence.sh
git commit -m "cas: §5 TLA+ gate — absence-means-Clean tombstone model (safe GREEN, sabotages RED)"
```

---

### Task 7: §5 — Absence means Clean (blob meta as tombstone) — LAST commit, revertible

GATED on Task 6 GREEN. Turn `.meta` into a pure tombstone: create writes no meta, adopt-backfill is removed, resurrect deletes the tombstone after a fresh body, spare clears the tombstone, GC delete stays body-then-meta (already conditional). Data reads never consult `.meta` — after §4 the only `loadMeta` callers are the dedup/adopt point-read (`observeAndAdmit`) and GC, which is the read-side contract. Land as ONE commit.

**Files:**
- Modify: `Core/CasBuild.cpp` (create path: remove `writeFreshMetaClean` `:449-454` + calls `:561,:581,:663`; remove adopt-backfill `:336-341`; resurrect: replace `writeResurrectMetaClean` `:463-478` + calls `:622,:648` with tombstone-delete; extern block `:21-23`)
- Modify: `Core/CasGc.cpp` (spare loop `:439-470`: add conditional tombstone-clear)
- Modify: `src/Common/ProfileEvents.cpp` (`-CasMetaCreateClean/-CasMetaAdoptBackfill/-CasMetaResurrectClean`; `+CasMetaResurrectClear/+CasMetaSpareClear`)
- Test: `src/Disks/tests/gtest_cas_build.cpp` (create writes no meta; resurrect deletes tombstone), `src/Disks/tests/gtest_cas_gc_round.cpp` (condemn on absent-meta blob; spare clears tombstone)

**Interfaces:**
- Consumes: `deleteMetaExact` (`CasBlobMeta.h:56`, conditional), `loadMeta`, `MetaState::Condemned`.
- Produces: ProfileEvents `CasMetaResurrectClear`, `CasMetaSpareClear` (live emit sites); removes the three now-dead Clean-reason counters.

- [ ] **Step 1: Write the failing tests**

In `gtest_cas_build.cpp`:
```cpp
TEST(CasBuild, FreshCreateWritesNoMeta)
{
    auto b = std::make_shared<InMemoryBackend>();
    auto s = openStore(b);
    auto build = s->startBuild({});
    build->putBlob(streamRefOf("payload-NM"), BlobSource::fromString("payload-NM"));
    /// §5: a fresh body writes NO meta object — absence is Clean by definition.
    EXPECT_FALSE(b->get(s->layout().blobMetaKey(streamRefOf("payload-NM"))).has_value());
}
```
In `gtest_cas_gc_round.cpp` (extend the smallest full-round test):
```cpp
    /// §5: condemn works on a blob that has NO .meta (absent = Clean); the round writes the tombstone.
    /// After a delete round, the tombstone is removed body-then-meta.
    const auto resurrect_clear_before = ProfileEvents::global_counters[ProfileEvents::CasMetaSpareClear].load();
    /* <existing spare-driving body: an in-degree recovers before graduation> */
    EXPECT_GE(ProfileEvents::global_counters[ProfileEvents::CasMetaSpareClear].load() - resurrect_clear_before, 1);
```

- [ ] **Step 2: Run to verify it fails**

Run: `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/s5_build.log 2>&1 && build/src/unit_tests_dbms --gtest_filter='CasBuild.FreshCreateWritesNoMeta:CasGcRound.*'`
Expected: `FreshCreateWritesNoMeta` FAILS (a meta is still written); `CasMetaSpareClear` undefined (compile RED).

- [ ] **Step 3: Implement — create path (no meta)**

`CasBuild.cpp`: delete the `writeFreshMetaClean` lambda (`:449-454`) and its three call sites (`:561`, `:581`, `:663`). Delete the adopt-backfill block in `observeAndAdmit` (`:336-341`) — under §5 an absent meta already means Clean, so no backfill is written (the dedup point-read at `:292` stays). Drop `extern const Event CasMetaCreateClean;` and `CasMetaAdoptBackfill;` (`:21-22`).

- [ ] **Step 4: Implement — resurrect deletes the tombstone (after the fresh body)**

`CasBuild.cpp`: replace the `writeResurrectMetaClean` lambda (`:463-478`) with `deleteResurrectTombstone(std::optional<LoadedMeta> lm_before)` that, AFTER the fresh body upload, conditionally deletes the tombstone at the observed etag:
```cpp
    auto deleteResurrectTombstone = [&](std::optional<LoadedMeta> lm_before)
    {
        /// §5: a fresh incarnation (new token) has displaced the condemned body — the tombstone is now
        /// stale. Delete it (If-Match the observed etag) so absence restores the Clean steady state.
        /// Ordering (fresh body FIRST, then delete tombstone) is load-bearing: the reverse would open
        /// "absence while the old body is still dying" (the excluded unsafe direction). Conditional, so a
        /// racing re-condemn cannot be stomped; an absent/mismatched meta = someone already reconciled it.
        if (!lm_before)
            return;
        ProfileEvents::increment(ProfileEvents::CasMetaResurrectClear);
        deleteMetaExact(store->backend(), store->layout(), ref, lm_before->etag);
    };
```
Update the two call sites (`:622`, `:648`) to `deleteResurrectTombstone(lm)`. These already run after the fresh-body displacement (they follow `uploadFromSource`/`putOverwrite`), preserving body-first ordering. Drop `extern const Event CasMetaResurrectClean;` (`:23`).

- [ ] **Step 5: Implement — spare clears the tombstone**

`CasGc.cpp` spare loop (`:439-470`): where a spare's in-degree recovered, schedule a conditional tombstone-clear (mirrors `deleteConfirmedMeta` but only when the meta is still `Condemned`):
```cpp
            {
                const BlobRef ref = entry.ref;
                scheduleMetaJob([this, ref]()
                {
                    const auto lm = loadMeta(store->backend(), store->layout(), ref);
                    if (lm && lm->meta.state == MetaState::Condemned)
                    {
                        ProfileEvents::increment(ProfileEvents::CasMetaSpareClear);
                        deleteMetaExact(store->backend(), store->layout(), ref, lm->etag);   /// If-Match: a racing re-condemn is not stomped
                    }
                });
            }
```
Replace the current add-only "spare does NOT touch the meta" comment (`:461-469`) with the §5 healing rule (rule 5): a tombstone met with nonzero in-degree is cleared; conditional so a deposed leader that lost its round CAS cannot stomp a live re-condemn.

- [ ] **Step 6: Implement — counters**

`ProfileEvents.cpp`: delete `CasMetaCreateClean` (`:843`), `CasMetaAdoptBackfill` (`:844`), `CasMetaResurrectClean` (`:845`). Append:
```cpp
    M(CasMetaResurrectClear, "CA blob meta: tombstone deleted on the resurrect path after a fresh incarnation displaced the condemned body (Round-B §5; absence = Clean)", ValueType::Number) \
    M(CasMetaSpareClear, "CA blob meta: tombstone cleared by GC when a spared candidate's in-degree recovered (Round-B §5 healing rule 5; conditional on the observed etag)", ValueType::Number) \
```

- [ ] **Step 7: Run to verify it passes**

Run: same build + `--gtest_filter='CasBuild.FreshCreateWritesNoMeta:CasBuild.*Resurrect*:CasGcRound.*'`
Expected: all PASS.

- [ ] **Step 8: Sweep + commit (single revertible commit)**

Run the sweep (0 failed). Then ONE commit (the whole §5 lever):
```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasBuild.cpp \
  src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Core/CasGc.cpp \
  src/Common/ProfileEvents.cpp \
  src/Disks/tests/gtest_cas_build.cpp \
  src/Disks/tests/gtest_cas_gc_round.cpp
git commit -m "cas: §5 blob meta absence means Clean — .meta becomes a pure tombstone (TLA-gated, revertible)"
```

- [ ] **Step 9: Soak verify (acceptance)**

After a baseline soak with the §0 counters proves the class decomposition, run a 10-minute soak and confirm the PUT class drops (~23% create-Clean removal + the resurrect-refresh share) with `CasMetaPut` collapsing toward zero, `CasMetaResurrectClear`/`CasMetaSpareClear` accumulating, and NO new dangling-ref / fsck findings. Record in spec §5. This is the LAST commit of the round and is one-command revertible (`git revert <sha>`).

---

### Task 8: §6 — `content_addressed_log` emit-path allocation trim

Not a resize bug (the flush path reserves correctly: `SystemLog.cpp` `column->reserve(to_flush.size())`, `ContentAddressedLog.cpp` `map.reserve(detail.size())`, queue `reserved_size_rows`). Two real per-event wastes, both on the HOT emitter thread (folded into the insert/merge memory totals, not the 2.2 GiB saving-thread figure): (a) `makeCasEventSink` deep-copies every field incl. a full `std::map<String,String>` per event because the sink takes `const CasEvent &` (no move possible); (b) the `reason` column is a full `String` per row though it is templated rationale (`event_type`/`outcome`/`object_kind` are already `LowCardinality`). The log is opt-in (off by default; soak/CI only), so this is a soak-observability cost — cheap to fix, correct to fix.

**Files:**
- Modify: `Core/CasEvent.h` (`CasEventSink` typedef `:72` — take the event by value or `CasEvent &&`).
- Modify: `Core/CasStore.h` (`emitEvent` `:583` — forward an rvalue: `if (event_sink_) event_sink_(std::move(e));`), and the `.cpp` emit call sites (`grep -n "emitEvent(" Core/CasStore.cpp` — ~8 sites; pass rvalues, `std::move` any named local whose event is dead after emit).
- Modify: `Core/CasGc.cpp` if it invokes the sink directly (the 2nd direct sink site — thread the same rvalue).
- Modify: `ContentAddressedMetadataStorage.cpp` (`makeCasEventSink` `:299-318` — take `CasEvent ev` by value / `&&` and `std::move` each field into the element: `e.detail = std::move(ev.detail); e.reason = std::move(ev.reason); e.namespace_ = std::move(ev.namespace_);` etc.).
- Modify: `src/Interpreters/ContentAddressedLog.cpp` (`getColumnsDescription` `:35` — `reason` → `lc_string`).
- Test: `src/Disks/tests/gtest_cas_build.cpp` (or wherever event-sink tests live) + a schema test near the log element.

**Interfaces:**
- Changes: `CasEventSink` = `std::function<void(CasEvent)>` (by value) — every caller passes an rvalue; the by-value param is move-constructed from a temporary at the emit site (a move, not the old deep copy). `namespace`/`ref_name` stay `String` (per-row varied); `object_hash`/`token` stay `String` (high cardinality); only `reason` flips to `LowCardinality`.

- [ ] **Step 1: RED — schema test.** Assert `ContentAddressedLogElement::getColumnsDescription()` gives `reason` type `LowCardinality(String)` (today it is `String`, so this FAILS). A cheap way: find the `reason` column in the description and `EXPECT_TRUE(typeid_cast<const DataTypeLowCardinality *>(col.type.get()))`.
- [ ] **Step 2: RED — move test.** Build a sink like `makeCasEventSink` (or call it via a stubbed context) with an rvalue `CasEvent` carrying a `detail` map with a sentinel entry; after the call, assert the produced element carries the detail AND the source event's `detail`/`reason` are moved-from (empty). Today the sink takes `const &`, so this test won't even compile against the new signature → drives the typedef change. (If a stubbed context is heavy, assert the move at the makeCasEventSink seam via a small test double that captures the element.)
- [ ] **Step 3: Run RED.** `flock /tmp/cas_build.lock ninja -C build unit_tests_dbms > build/opt_t8_build.log 2>&1` then the two tests — expect FAIL (schema is String; move test won't compile / source not moved-from).
- [ ] **Step 4: Implement.** Change the `CasEventSink` typedef to by-value; `emitEvent` forwards `std::move(e)`; fix the ~8 `emitEvent` call sites + the 2nd direct sink site to pass rvalues; `makeCasEventSink` takes the event by value and `std::move`s each field; `reason` → `lc_string` in `getColumnsDescription`. Allman braces.
- [ ] **Step 5: Run GREEN.** Both tests pass.
- [ ] **Step 6: Sweep + commit.** Global-Constraints sweep (0 failed, 977 + new tests). Commit `cas: opt §6 — content_addressed_log emit-path move + reason LowCardinality` (explicit paths).
- [ ] **Step 7: Soak verify (optional, cheap).** In any §2/§3 matrix soak, confirm `content_addressed_log` still records every event correctly (no rows dropped/garbled by the move) and note the emitter-thread allocation delta if the metric is available. Behavior-preserving; the acceptance is "no lost/garbled rows".

---

## Self-Review

**Spec coverage:**
- §0 introspection — DONE (not re-planned; its counters `CasMetaPut/Cas/Delete`, `CasMeta{CreateClean,AdoptBackfill,ResurrectClean}`, `CasGcMetaOps`, `CasGcEnumerationPages`, `CasDedupCacheHits/Misses` verified present).
- §1 fold buffer → Task 1. §2 dedup sizing → Task 3 (+ Task 2 plumbing). §3 validate setting → Task 4. §4 manifest-trust → Task 5. §5 absence-Clean → Tasks 6 (gate) + 7 (code). Soak-config plumbing → Task 2. §6 `content_addressed_log` emit-path trim → Task 8 (independent of §1-5 ordering; can land any time). Sequencing §1→§2→§3→§4→§5 honored; plumbing lands early (Task 2) so §2/§3 matrices run.
- Testing (spec §Testing): per-lever gtests RED-first (Tasks 1/4/5/7); §5 TLA gate before code (Task 6); 10-minute soak matrix as acceptance (Steps "Soak verify" in Tasks 1/3/4/5/7); name-set sweep after every code commit (Global Constraints + each task's sweep step).

**Placeholder scan:** the `/* <fixture: ...> */` markers in Tasks 5/7 are explicit copy-from-named-sibling instructions (the sibling tests are named); the §5 TLA `.tla` body is specified by its state, five transitions, invariant, and sabotage set (the modeling is the task's creative deliverable, run to a GREEN/RED gate). No TBD/TODO.

**Type consistency:** `casSizedReadSettings`/`CAS_FOLD_READ_SLACK_BYTES` (Task 1) used verbatim. `PartFolderValidate{Mode,age_seconds}` + `CacheParams.validate` + `validatedAtMs` + `CasPartFolderValidateSkipped` (Task 4) consistent across files. `DepEntry.adopted` + `isTrustedAdopt` + `CasBlobAdoptTrusted` (Task 5). `deleteResurrectTombstone` + `CasMetaResurrectClear`/`CasMetaSpareClear` and the three removed Clean counters (Task 7) consistent. `CasBlobCopyForward` removed in Task 5 (its only emit site is deleted there).

**Resolved ambiguities** (also relayed to the controller): (A) §2 needs no new counter — `CasBlobHead`/`CasBlobBodyPutAvoided` already exist, so §2 is measurement-only. (B) §4 trusts only adoptEvidence tokenless leaves via a new `DepEntry.adopted` bit; a pending-upload tokenless leaf still fails closed, and a genuinely-absent adopted blob is no longer caught at promote (moves to fsck) — the accepted D4 trust trade-off; three copy-forward tests migrated. (C) §5 gate is a NEW model (`CaMetaAbsenceClean.tla`), not the pre-§5 `CaMetaDescriptor.tla` nor the rejected `CaMetaDescriptorRaw.tla`. (D) `part_folder_validate` is one string setting parsed to `{Always,Age(s),Never}`. (E) §1 reuses `ReadSettings::adjustBufferSize` (caps at base) with a 4 KiB slack. (F) §4 removes the now-dead `CasBlobCopyForward` counter with its function.

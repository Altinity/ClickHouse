# Task T3 (draft): Task 7 closure — decommission evidence

Status: **UNVERIFIED-DRAFT**. Written in `/home/mfilimonov/workspace/ClickHouse/draft-t3`
(branch `draft/t3`), which is under a no-build/no-test-run constraint for this session. Every
claim below that would normally come from compiling or running the suite is marked
`UNVERIFIED-DRAFT`. A finisher in a build-capable lane must run RED/GREEN, the mutation demos, the
targeted suite, and the `test_content_addressed_drop_pool_member` integration lane before this can
close.

File touched: `src/Disks/tests/gtest_cas_decommission_catalog_duties.cpp`.

## Step 1 — the fence test split

Two changes, both applied:

1. Renamed `FinalCatalogFenceKeepsSlotWhenVictimEntryAppearsDuringDrain` to
   `VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot` (name now matches what it proves — the late
   entry is admitted during the roots-prefix LIST drain, strictly before `retirement_catalog_cut`
   is read, so it is caught by arm (a), not arm (b)). Strengthened its assertion from
   "warnings non-empty" to check the specific arm-(a) string:
   `report.warnings.front().find("catalog still owns victim namespaces")`.

2. Added `CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`, a new fixture backend
   (`MutateCatalogBetweenRetirementReadsBackend`) and test that exercises arm (b) — the
   `fresh_retirement_catalog` token/catalog comparison in
   `Tools/CasDecommission.cpp`'s `decommissionPoolMember`.

### The injection seam

`decommissionPoolMember`'s retirement tail (`Tools/CasDecommission.cpp`) reads the whole-pool
catalog exactly twice in sequence, with nothing but two local-variable copies between them:

```cpp
std::optional<CasRefCatalog::Snapshot> retirement_catalog_cut;
if (report.warnings.empty())
{
    retirement_catalog_cut = CasRefCatalog::read(admin->backend(), admin->layout());   // read #1
    ...
}
const Layout layout = admin->layout();
const BackendPtr pool_backend = admin->poolBackendPtr();
if (report.warnings.empty())
{
    const CasRefCatalog::Snapshot fresh_retirement_catalog
        = CasRefCatalog::read(admin->backend(), admin->layout());                      // read #2
    if (!retirement_catalog_cut
        || fresh_retirement_catalog.token != retirement_catalog_cut->token
        || fresh_retirement_catalog.catalog != retirement_catalog_cut->catalog)
    {
        report.warnings.push_back(
            "catalog changed after the victim ownership check; refusing slot retirement against a stale cut");
    }
}
```

Both reads go through `CasRefCatalog::read`, which does exactly one
`backend.get(layout.refCatalogKey())` (`Pool/CasRefCatalog.cpp`); `refCatalogKey()` resolves to
`"<prefix>/cas/ref_catalog"`, i.e. `"p/cas/ref_catalog"` for this fixture's pool prefix. So the
seam to rewire onto is `Backend::get`, not `Backend::list` (which is what the old fixture
intercepted).

The complication: `get("p/cas/ref_catalog")` is also called earlier in the same function —
once before `admin` is even opened (`catalog_cut`, line ~129), once inside
`Pool::openForDecommission`'s `mountWritable` (the unconditional `observe_catalog()` call), and
once per owned life inside the per-namespace loop (none fire for an empty victim pool with no
namespaces, which is what both fence tests use as their fixture). Rather than counting raw
`get` calls from the top of the function — which is fragile, since it silently depends on how many
internal catalog reads `Pool::open`/`Pool::openForDecommission` perform on their own mount path —
I ordered the injection off a second seam that is unambiguous by construction: the mountpoint
drain's `list("p/roots/victim/", cursor, limit)` (`deleteListedPrefix(admin->backend(),
admin->layout().serverRootDataPrefix(victim_srid), ...)`, immediately before the retirement tail)
is the **last** `list` call anywhere before either catalog read. Grepping
`Tools/CasDecommission.cpp` confirms `CasRefCatalog::read` (hence `get("p/cas/ref_catalog")`) is
called at exactly four sites in the file: line ~129 (`catalog_cut`), line ~167 (per owned life,
inert for an empty pool), and the two retirement-tail reads — none of which sit between the
mountpoint-drain `list` and the retirement tail. So: once the fixture backend observes that
specific `list` call, the next two (and only the next two) `get("p/cas/ref_catalog")` calls it
sees are, in order, `retirement_catalog_cut` then `fresh_retirement_catalog`. The fixture arms on
the first of those (records it, mutates nothing) and mutates on the second (admits the late entry
via `CasRefCatalog::casAdmitEntry` before returning), so `fresh_retirement_catalog` observes a
catalog `retirement_catalog_cut` did not.

This also explains the existing test's actual bug precisely: its
`AddVictimEntryDuringRootDrainBackend` hooks the *same* `list("p/roots/victim/", ...)` prefix, but
fires the mutation immediately, inside that first LIST call — before either catalog read, not
between them. That is why it was always exercising arm (a).

`MutateCatalogBetweenRetirementReadsBackend::get` re-enters itself when it calls
`CasRefCatalog::casAdmitEntry(*this, ...)` (which itself calls `get`/`casPut` against the same
backend); the `added` guard, set before the recursive call, prevents that reentry from being
mistaken for a third real caller.

### UNVERIFIED-DRAFT items for this step

- Whether `Pool::openForDecommission`'s mount path really performs exactly one
  `get("p/cas/ref_catalog")` (the unconditional `observe_catalog()` in `mountWritable`) with no
  extra reads for this fixture's specific starting state (already-owned root, no conflict) is
  read from a comment in `CasPool.cpp` ("Existing matching owner and epoch objects take fast paths
  that do not need an emptiness observation"), not from an instrumented run. It does not matter
  for correctness of the new fixture (which only counts calls *after* the mountpoint-drain LIST),
  but if that comment is stale, `MutateCatalogBetweenRetirementReadsBackend`'s pre-drain call count
  would be different from what I assumed while designing it — which is irrelevant to the mutation
  point but worth the finisher's eye.
- The mutation timing itself (does the fresh read genuinely observe the admitted entry, given
  in-memory backend semantics) is UNVERIFIED-DRAFT — no test run happened in this worktree.

## Step 2 — red-first (not run; instructions for the finisher)

Not executed here (no builds allowed in this worktree). The finisher must:

1. Build and run `CasDecommissionCatalogDuties.CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`
   against the **current, unmodified** production code first, and expect it to pass immediately —
   the fence (arm b) already exists in `224aacd8eb96f6da6e0b2457dad1b09ac563d3d5`; this test is
   backfilling missing coverage for an already-implemented duty, not driving new production code.
   A failure here is a **CODE finding**: it would mean the rewired injection still lands before
   `retirement_catalog_cut` (i.e. arm (b) is unreachable from this fixture, same defect class as
   the original test) — stop and escalate rather than patch the test to pass.
2. The actual falsifiability proof for this test is the **mutation demonstration**: comment out
   the token/catalog comparison in arm (b) (see Step 3, patch (iii) analog below — arm (b) itself),
   rebuild, rerun, and confirm this specific test goes red. That is what proves the test is
   sensitive to the fence rather than accidentally green for an unrelated reason.
3. Log to `build/t3_fence_*.log` per the plan.

## Step 3 — mutation-demonstration patches (drafted, not applied/run)

All three target `Tools/CasDecommission.cpp`. Each is a minimal skip of one load-bearing duty. The
finisher applies each in turn, rebuilds, runs the full
`CasDecommissionCatalogDuties.*` suite, captures the failing output, then reverts before moving to
the next.

### (i) Skip the `_ckpt`-absence `CORRUPTED_DATA` throw

```patch
--- a/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
+++ b/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
@@
         if (selected_entry.state == NsState::Removing)
         {
-            if (!admin->backend().head(admin->layout().refCkptKey(life)).exists)
+            if (false && !admin->backend().head(admin->layout().refCkptKey(life)).exists)
                 throw Exception(ErrorCodes::CORRUPTED_DATA,
                     "ca-decommission: namespace '{}' is Removing but its exact checkpoint is absent; "
                     "the catalog row remains owned and the victim slot cannot be retired",
                     ns_str);
```

Predicted catcher: `CasDecommissionCatalogDuties.RemovingWithoutCheckpointIsCorruptionAndKeepsSlot`
(expects `CORRUPTED_DATA`; with the throw skipped, `dropNamespace` is called on a life whose
`_ckpt` is absent from a `Removing` row it never itself created — the fixture only ever wrote the
catalog row without ever completing a birth/checkpoint write, so `dropNamespace`'s own resumption
path should fail differently or the test's post-conditions on catalog state diverge). Secondary
candidate: `PartialRemovalProgressStillWakesGcWhenLaterNamespaceFails`, which relies on the same
throw firing for its `broken_ns` fixture.

### (ii) Drop the `request_gc_round` `SCOPE_EXIT`

```patch
--- a/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
+++ b/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
@@
     bool gc_round_needed = false;
-    /// A namespace may have reached `Removing` before a later namespace fails closed. Preserve the
-    /// already-earned liveness signal on every exit: the callback only wakes the existing serialized
-    /// GC worker and cannot perform catalog work itself.
-    SCOPE_EXIT({
-        if (gc_round_needed && request_gc_round)
-            request_gc_round();
-    });
+    (void)gc_round_needed;
```

Predicted catchers: `RemovingWithCheckpointResumesTerminalAndKeepsSlotForGc` (asserts
`wake_requests.load() == 1u`), `PartialRemovalProgressStillWakesGcWhenLaterNamespaceFails` (asserts
the wake happens even though the call as a whole throws — this is the one that specifically pins
the `SCOPE_EXIT` firing on a fail-closed exit path, not just the normal-return path), and
`FoldedTerminalRemainsGcOwnedAndOnlyRequestsAnotherRound` (also asserts `wake_requests.load() ==
1u`). All three should go red simultaneously since they all assert on the same callback.

### (iii) Skip `victim_still_owned` (arm a)

```patch
--- a/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
+++ b/src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Tools/CasDecommission.cpp
@@
         retirement_catalog_cut = CasRefCatalog::read(admin->backend(), admin->layout());
-        const bool victim_still_owned = std::any_of(
+        const bool victim_still_owned = false && std::any_of(
             retirement_catalog_cut->catalog.entries.begin(), retirement_catalog_cut->catalog.entries.end(),
             [&](const CatalogEntry & entry)
             {
                 return entry.ns.string() == victim_srid
                     || entry.ns.string().starts_with(victim_namespace_prefix);
             });
```

Predicted catcher: `VictimEntryAppearingBeforeTheOwnershipCutKeepsSlot` (this draft's renamed
test — the late entry is admitted before `retirement_catalog_cut` is read, so only arm (a) can
catch it; with arm (a) disabled, `report.slot_removed` should flip true and the "catalog still
owns victim namespaces" warning should disappear). This is also, incidentally, the mutation the
plan's Step 5 note anticipates when it says the new arm-(b) test is expected green immediately —
the corresponding demonstration for arm (b) itself is: comment out the `fresh_retirement_catalog`
token/catalog `if` in the second block (not drafted as a separate patch here since it is
structurally identical to (iii) one block down); its predicted catcher is this draft's new
`CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot`, and finding that it is NOT caught
would mean the rewired injection still lands too early — see Step 2 above.

## Step 4 — ownership inventory

Enumeration of every "how does `decommissionPoolMember` decide which catalog rows belong to the
victim" site in `Tools/CasDecommission.cpp`, confirming exact catalog-name ownership and no
life-key prefix fallback:

1. **Catalog cut** (line ~129): `catalog_cut = CasRefCatalog::read(*backend, catalog_layout)` —
   one whole-catalog snapshot, taken before `admin` is even opened. This, not a later or partial
   view, is the ownership universe for namespace selection.

2. **Namespace selection** (lines ~146–157):
   ```cpp
   const String victim_namespace_prefix = victim_srid + "/";
   std::vector<std::pair<CatalogEntry, NamespaceLifeId>> owned_lives;
   for (const CatalogEntry & entry : catalog_cut.catalog.entries)
   {
       if (entry.ns.string() != victim_srid && !entry.ns.string().starts_with(victim_namespace_prefix))
           continue;
       const auto life = catalog_cut.life_index.resolve(entry.incarnation);
       ...
       owned_lives.emplace_back(entry, *life);
   }
   ```
   This iterates `catalog_cut.catalog.entries` — the exact rows of the one cut above, not a LIST of
   any object-storage prefix. The match is `entry.ns.string() == victim_srid` (the root itself,
   were it ever admitted as its own namespace) or `entry.ns.string().starts_with(victim_srid +
   "/")` — the trailing slash makes `victim_srid` one full canonical path component, so
   `victim_srid = "victim"` cannot match a sibling root such as `"victim2"` (only
   `"victim2/..."` would need `victim2/` as its own prefix, which it does not share with
   `"victim/"`). There is no code path here that derives ownership from a *physical life key*
   (`NamespaceLifeId`/`refCkptKey`/`namespaceStreamPrefix`) — the `life` value used per selected
   entry is a **result** of resolving the already-catalog-selected `entry.incarnation`, never an
   independent selection input.

3. **Per-namespace incarnation re-check** (lines ~167–175): before acting on a selected entry, the
   loop re-reads the catalog (`CasRefCatalog::read(admin->backend(), admin->layout())`) and
   requires `*current_entry == selected_entry` (exact structural equality, not just a name match),
   throwing `CORRUPTED_DATA` otherwise. This tightens ownership further — a same-name row that
   changed incarnation after the cut is refused, not silently re-selected — but it is still driven
   by the same catalog-entry identity, not a physical key.

4. **`deleteListedPrefix` calls** (the only `list`-based deletion in the file): the manifest debris
   sweep (`admin->layout().casManifestsServerPrefix(victim_srid)`, lines ~220–229, actually swept
   per-namespace via `sweepNamespace` rather than deleted outright), the staging drain
   (`admin->poolConfig().pool_prefix + "/staging/" + victim_srid + "/"`, lines ~235–236), and the
   mountpoint drain (`admin->layout().serverRootDataPrefix(victim_srid)`, lines ~241–242). All
   three prefixes are rooted at `<pool_prefix>/{cas/manifests,staging,roots}/<victim_srid>/` —
   physically loose, non-catalog debris scoped to the victim's own server-root subtree, never a
   raw namespace-name prefix over the ref/log/manifest object space. None of them is used to
   *decide* which namespaces are owned; they run unconditionally over the victim's own root
   regardless of which entries the catalog selected in step 2.

5. **Retirement-tail re-checks** (lines ~249–282, both arms — see Step 1's seam analysis above):
   `victim_still_owned` re-scans `retirement_catalog_cut.catalog.entries` with the identical
   `entry.ns.string() == victim_srid || entry.ns.string().starts_with(victim_namespace_prefix)`
   predicate as step 2 — same ownership rule, re-applied after the drains, still over catalog
   entries. `fresh_retirement_catalog`'s token/catalog equality check does not re-derive ownership
   at all; it only detects that the whole catalog moved since the first re-scan, refusing to trust
   a stale ownership verdict rather than re-deriving a new one.

**Conclusion**: every ownership decision in `decommissionPoolMember` is driven exclusively by
exact `CatalogEntry.ns` matching (self or canonical-path-component prefix) against one immutable
per-call catalog cut, re-validated per-entry and again in the retirement tail. `deleteListedPrefix`
is confined to loose, non-namespace physical debris under the victim's own root and never
participates in namespace ownership selection. There is no code path that derives ownership from a
physical life key or an object-storage LIST over the ref/namespace address space.

## Step 5 — integration lane

Not run in this worktree (no praktika/docker here). Per the plan, MAIN runs
`python3 -m ci.praktika run "integration" --test "test_content_addressed_drop_pool_member" >
build/t3_drop_pool_member.log 2>&1` on lane-g's behalf, expected 2/2 passed. **UNVERIFIED-DRAFT.**
Note (unchanged from the audit): this lane's `STAGE-A CONTRACT` banner asserts suppression-era
behavior and is edited by T6, not this task — a green result here today is expected and is not
itself T6's closure evidence.

## Step 6 — gate, review, commit

Not run here. `build/t3_gate.log`, review, and the closure commit are all finisher work.

**Naming collision, already known and to be handled by the finisher, not by this draft**: the
dispatch's own note says the plan-mandated closure subject `ca: decommission — catalog-exact
duties; retirement fenced on owned entries` may already exist in history. Checked in this
worktree:

```
$ git log --oneline --all --grep="decommission — catalog-exact duties" -i
(no output)
```

**UNVERIFIED-DRAFT** as a global claim (only this worktree's visible history was checked, and this
worktree is a snapshot at `8e86c58a0f5`; MAIN's `cas-gc-rebuild` may have moved since). The
finisher must re-check `git log --oneline | head` on the actual integration target before using
this subject, and use a follow-up-shaped subject if it collides, per the dispatch.

## Open questions for the finisher

1. Confirm (by an actual run) that
   `CatalogTokenMovedBetweenOwnershipCutAndRetirementKeepsSlot` is green against unmodified
   `224aacd8eb9`-derived code, and separately red when arm (b)'s comparison is commented out. If it
   is unexpectedly red against unmodified code, that is a CODE finding (the injection still lands
   too early) — escalate rather than adjust the test's assertions to match.
2. Verify the `mountWritable`/`observe_catalog` call-count assumption noted in Step 1's
   UNVERIFIED-DRAFT callout does not change which `list`/`get` sequence the fixture backend
   observes for this specific victim (already-owned, no namespaces) — if `openForDecommission`
   turns out to touch `p/roots/victim/` via `list` for some other reason before the mountpoint
   drain, the "past_mountpoint_drain" flag could latch early and the fixture would misfire.
3. Run the three mutation demonstrations in Step 3 and confirm the predicted catchers; update the
   report with actual pass/fail output per test, using the mandatory apply/capture/revert wording.
4. Run Step 5's integration lane and Step 6's full CA gate; commit with the collision-checked
   subject.

## Commit

`ca: draft — T3 decommission closure tests (UNVERIFIED-DRAFT, no runs)` on `draft/t3`.

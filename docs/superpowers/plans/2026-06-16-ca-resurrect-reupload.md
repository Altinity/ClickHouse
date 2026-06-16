# CA resurrect = re-upload a fresh incarnation — Implementation Plan (B167)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans.

**Goal:** Stop the bodyless publish-gate `resurrect`-vs-GC livelock (B167) by making blobs **re-creatable at the gate, exactly like trees** — retain the blob body and re-upload a fresh incarnation instead of GET-from-existing.

**Architecture:** Mirror the existing tree machinery. Trees retain their encoded payload (`retained_trees`) and `recreateTree` re-uploads a fresh incarnation at the gate. Add `retained_blobs` (capped, to bound memory — B165) + `recreateBlob`, and wire it into the three gate branch sites that currently do `retained_trees.contains(hash) ? recreateTree : resurrect/throw`. No GC change (C rejected). Spec: `docs/superpowers/specs/2026-06-16-ca-resurrect-reupload-design.md`. Model: `CaResurrectLiveness.tla` (TLC-green).

**Build/test:** `cd build && ninja unit_tests_dbms > build_b167.log 2>&1`; `build/src/unit_tests_dbms --gtest_filter='Cas*:CaWiring*'`.

---

### Task 1: `retained_blobs` + capped retention in `putBlob` + `recreateBlob`

**Files:** `Core/CasBuild.h`, `Core/CasBuild.cpp`.

- [ ] **Step 1 (CasBuild.h):** after `std::map<UInt128, String> retained_trees;` (line 133):
```cpp
    /// B167: retained blob bodies for gate re-create (mirrors retained_trees). Capped by
    /// RECREATABLE_BLOB_CAP so large blobs do not bloat RAM (B165); an over-cap blob is NOT retained
    /// and falls back to resurrect (the rare large-blob residual). Small column blobs (the common
    /// dedup case) are retained -> recreateBlob wins against GC, closing the B167 livelock.
    std::map<UInt128, String> retained_blobs;
```
and declare `recreateBlob` next to `recreateTree` (after line 119):
```cpp
    void recreateBlob(const UInt128 & hash);
```

- [ ] **Step 2 (CasBuild.cpp):** a retention cap constant near the top of the anon/namespace (e.g. by `max_attempts`):
```cpp
/// B167: retain blob bodies up to this size for gate re-create; above it, fall back to resurrect.
constexpr uint64_t RECREATABLE_BLOB_CAP = 1ul << 20;   /// 1 MiB
```

- [ ] **Step 3 (CasBuild.cpp `putBlob`):** materialize+retain small bodies. Replace the streaming write (`source.write_payload(sink->buffer()); ... written = ...`) so that for `source.size <= RECREATABLE_BLOB_CAP` the body is materialized once before the loop, reused each attempt, and retained on success. Concretely, before the `for` loop:
```cpp
    std::optional<String> retained_body;
    if (source.size <= RECREATABLE_BLOB_CAP)
    {
        retained_body.emplace();
        WriteBufferFromString wb(*retained_body);
        source.write_payload(wb);
        wb.finalize();
        if (retained_body->size() != source.size)
            throw Exception(ErrorCodes::LOGICAL_ERROR,
                "putBlob: source wrote {} bytes, declared {}", retained_body->size(), source.size);
    }
```
Inside the loop, write the body from either the materialized copy or the stream:
```cpp
        const size_t before = sink->buffer().count();
        if (retained_body)
            writeString(*retained_body, sink->buffer());
        else
            source.write_payload(sink->buffer());
        const size_t written = sink->buffer().count() - before;
```
(the existing `written != source.size` check stays for the streaming branch). On the `PutOutcome::Done` branch, after recording `deps[...]`, retain:
```cpp
            if (retained_body)
                retained_blobs[logical_hash] = *retained_body;
```

- [ ] **Step 4 (CasBuild.cpp):** add `recreateBlob`, mirroring `recreateTree` (place right after `recreateTree`):
```cpp
void Build::recreateBlob(const UInt128 & hash)
{
    /// B167 W-REVALIDATE re-create branch for blobs (mirrors recreateTree). The body was RETAINED at
    /// putBlob (<= RECREATABLE_BLOB_CAP), so re-upload it as a FRESH incarnation rather than GET-ing
    /// the existing (racy) object. A fresh incarnation is fold-invisible until referenced, so GC
    /// cannot delete it in the span -> converges (no HEAD->GET race).
    const auto it = retained_blobs.find(hash);
    if (it == retained_blobs.end())
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "Build::recreateBlob: no retained body for blob {} (only retained blobs are re-creatable)",
            u128ToHex(hash));

    const String & body = it->second;
    const String key = store->layout().blobKey(BlobId(u128ToHex(hash)));
    const PoolMeta & meta = store->poolMeta();
    const PoolConfig & cfg = store->poolConfig();

    EnvelopeHeader header;
    header.kind = ObjectKind::Blob;
    header.hash_algo = 1;
    header.logical_size = body.size();
    header.logical_hash = hash;
    header.domain_id = meta.pool_id;
    header.incarnation_tag = mintU128();
    header.build_id = build_id;
    header.provenance = Provenance{nowMs(), cfg.server_id, /*ch_version*/ 0, info.op};
    header.pad_to_header_len = static_cast<uint32_t>(meta.blob_header_len);
    const String head_bytes = encodeEnvelopeHeader(header);

    WriteSinkPtr sink = store->backend().putIfAbsentStream(key);
    writeString(head_bytes, sink->buffer());
    writeString(body, sink->buffer());
    Token tok;
    const PutOutcome outcome = sink->finalize(&tok);
    if (outcome == PutOutcome::Done)
    {
        deps[{static_cast<uint8_t>(ObjectKind::Blob), hash}] =
            DepEntry{ObjectKind::Blob, tok, store->retireView().round(), body.size()};
        return;
    }
    /// PreconditionFailed: a concurrent writer re-created it first; resolve against the current
    /// incarnation (adopt, or resurrect if condemned).
    observeAndAdmit(ObjectKind::Blob, hash, key);
}
```
(Verify the exact key helper: `recreateTree` uses `store->layout().treeKey(id)`; use the blob analog `blobKey` — grep `layout().blobKey` / `keyFor(ObjectKind::Blob,...)` and match the existing call.)

- [ ] **Step 5: build dbms** `ninja dbms > build_b167_core.log 2>&1`; commit.

---

### Task 2: wire `recreateBlob` into the three gate sites

**Files:** `Core/CasBuild.cpp` (`gateCheckDeps`, `revalidateDeps`).

- [ ] **Step 1 — `gateCheckDeps` (the token-bearing view-hit branch, ~line 489):**
```cpp
                if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                    recreateTree(hash);
                else if (kind == ObjectKind::Blob && retained_blobs.contains(hash))
                    recreateBlob(hash);
                else
                    resurrect(kind, hash, keyFor(kind, hash));
```

- [ ] **Step 2 — `revalidateDeps` tokenless-evidence HEAD-absent branch (~line 585):** today blobs THROW "lost and not re-creatable" here — add the blob re-create:
```cpp
                if (kind == ObjectKind::Tree && retained_trees.contains(hash))
                    recreateTree(hash);
                else if (kind == ObjectKind::Blob && retained_blobs.contains(hash))
                    recreateBlob(hash);
                else
                    throw Exception(ErrorCodes::ABORTED,
                        "publish evidence dependency {} lost and not re-creatable; retry the operation",
                        u128ToHex(hash));
```

- [ ] **Step 3 — `revalidateDeps` token-bearing view-hit branch (~line 610):** mirror Step 1 (`else if Blob && retained_blobs.contains → recreateBlob; else resurrect`). Read the exact lines 610-615 and apply the same three-way branch.

- [ ] **Step 4: build dbms** `ninja dbms`; commit `"CA B167: gate re-creates retained blobs (mirrors trees) instead of bodyless resurrect"`.

---

### Task 3: unit test — gate re-creates a condemned putBlob'd blob

**Files:** `Disks/tests/gtest_cas_build.cpp` (or `gtest_cas_protocol_scenarios.cpp` — match where the tree-recreate / condemned-dep tests live; grep `recreateTree`/`findCondemned`/`ret
ained_trees` in the tests).

- [ ] **Step 1:** add a test (mirror the existing tree gate-recreate test): build a blob via `putBlob` (small, ≤ cap); arrange the retire view to report it condemned (the test backend / retireView stub used by the existing condemned-dep tests); delete the blob object (simulate GC's exact-token delete); run the publish gate (`gateCheckDeps`/publish); assert the publish SUCCEEDS (the gate `recreateBlob`s a fresh incarnation) rather than throwing ABORTED. Add the contrast: a blob NOT retained (> cap, or adopted) still takes the resurrect path. (Use the same fixtures as the existing `recreateTree` test — copy its setup, swap tree→blob.)

- [ ] **Step 2: build + run** `ninja unit_tests_dbms`; `--gtest_filter='*CasBuild*:*recreate*:*Resurrect*:*Condemn*'`.

- [ ] **Step 3: full CA suite** `--gtest_filter='Cas*:CaWiring*'` → only the pre-existing B140 leak red.

- [ ] **Step 4: commit** `"CA B167: gate-recreate-blob test"`.

---

### Task 4 (after merge): soak validation (B160 + B167)
Rebuild `clickhouse`, run the two-replica soak with chaos off; confirm **0 broken detached parts** and a clean `fsck` (`dangling=0`) under productive GC — B160 (contention=0) + B167 (no broken parts) together.

## Notes
- The cap bounds RAM: total retained ≤ `RECREATABLE_BLOB_CAP` × distinct small blobs per build, freed when the build ends. Large blobs (> cap) and pure-adopted deps remain on the `resurrect` (GET-from-existing) path — the rare residual (live sources aren't condemned). If a genuine "no bytes anywhere" case arises it must fail closed (loud), never silently drop.
- `recreateBlob` is the blob twin of `recreateTree`; keep them structurally identical for reviewability.

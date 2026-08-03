# RCA — `WinnerShape/CasGcCompletedRemovalFenceRace.../Replacement`

**Verdict: TEST defect. The fence guarantee is intact; no data-loss-class hole.**

The `/Replacement` variant forges a catalog state that the production creation protocol cannot
produce (a `Live` namespace row with no `_ckpt`), and a refusal added on 2026-08-02 now — correctly
— rejects that state. Every fence assertion in the test passes; the exception is thrown in the
test's epilogue, after the fence has already been verified.

## 1. Reproduction

`build/src/unit_tests_dbms` (release, mtime 2026-08-03 01:20), log `build/rca_fence_repro.log`:

```
[ RUN  ] .../FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Absent
[  OK  ] ... (0 ms)
[ RUN  ] .../FencedLeaderStopsAfterWinnerRemovesOrReplacesLife/Replacement
unknown file: Failure
C++ exception with description "CAS recovery grounding: a Live or Removing namespace requires a
readable _ckpt with life_epoch" thrown in the test body.
Code: 246. DB::Exception: ... (CORRUPTED_DATA)
```

Stack, innermost frames:

```
Cas::chooseRecoveryGrounding      Pool/CasRefCkpt.cpp:152
Cas::CasRefLedger::runRecoveryWalkOnce   Pool/CasRefLedger.cpp:760
Cas::CasRefLedger::ensureRefTableRecovered  Pool/CasRefLedger.cpp:1424
Cas::CasRefLedger::namespaceLife  Pool/CasRefLedger.cpp:4732
Cas::Pool::namespaceLife          Pool/CasPool.cpp:1683
...TestBody()                     src/Disks/tests/gtest_cas_gc_frontier_gate.cpp:2781
```

`build_asan/src/unit_tests_dbms` (mtime 2026-08-03 00:59 — post-migration, so its result is
directly comparable), log `build/rca_fence_asan.log`: identical outcome, `/Absent` OK,
`/Replacement` failing on the same exception. No sanitizer report.

**Load-bearing detail:** gtest lists *no* `EXPECT_*` failures. Line 2781 is
`(void)store->namespaceLife(fixture.ns);`, which sits *after* every assertion about the fence.
All of these passed under `/Replacement`:

- `ASSERT_TRUE(leader_a_failure)` — the deposed leader did abort;
- zero `CasGcRefWalkPlansBuilt` — it built no walk plan;
- no `list <casRefsPrefix>` in the journal — it never reached the hot LIST;
- no `cas_begin <gcStateKey>` and no `put_begin .../fold_seal` — it published no successor
  generation;
- it still performed its mandatory erase-resolution `get <refCatalogKey>`.

I additionally ran `/Replacement` alone and confirmed the deposed leader fails for the *right*
reason, printing `CAS write could not be committed (CAS GC pre-fold drain lost authority before
the catalog settled); retrying later`. So the fenced leader stops, and stops on fence loss, under
both winner shapes.

## 2. What `/Replacement` does differently

`gtest_cas_gc_frontier_gate.cpp:2762-2772`. Both params take the same fixture — a namespace
`00/drain-race@cas@` at incarnation `177`, driven to `Removing` — block leader A on its catalog
CAS, and transfer the GC lease to leader B. Then:

- `/Absent`: the winner commits an **empty** catalog. Nothing claims the namespace name.
- `/Replacement`: the winner commits a catalog containing one row —
  `{ns, state = NsState::Live, incarnation = UInt128{178}}` — and **nothing else**.

The epilogue then calls `store->namespaceLife(fixture.ns)` to check that the next name-based
resolution does not retain the retired predecessor runtime. Under `/Absent` there is no row, so
grounding is never attempted. Under `/Replacement` the new `Live` row sends the recovery walk into
`chooseRecoveryGrounding`, which needs the life's `_ckpt`.

The successor life `178` has no `_ckpt`. The fixture writes one for the predecessor —
`seedCompletedRemoving` at line 296-304 does
`layout.refCkptKey(NamespaceLifeId::fromCatalogEntry(ns, 177))` with `life_epoch = 1` — but the
winner-catalog branch was never given the equivalent. The `_ckpt` key is scoped by
`NamespaceLifeId`, i.e. per *life*, so the predecessor's object does not cover incarnation `178`.

## 3. Is the refusal right? (the production question)

Yes. Two independent seams keep `Live`/`Removing` ⇒ `_ckpt` present, so the state the test forges
is not reachable through either of them.

**Creation.** `CasRefCatalog::createNamespace` (`Pool/CasRefCatalog.cpp:514-516`) admits the row as
`NsState::Creating` with a creator fence, then calls `completeCreation`. `completeCreation`
(line ~447 onward) publishes the `_ckpt` carrying `life_epoch = creator->writer_epoch` as its
step 2, *before* the step-3 flip to `Live`. `chooseRecoveryGrounding` correspondingly exempts
`Creating` (`CasRefCkpt.cpp:147`) — the exemption exists precisely because that is the only window
in which the row outruns its `_ckpt`. So no `Live` row is ever published without one.

**Removal.** There is no `refCkptKey` delete anywhere in the non-test tree; the object is reclaimed
by the namespace janitor scanning object keys. `Gc/CasNamespaceJanitor.cpp:83` gates every delete
on `catalog_cut.life_index.resolve(*life_id)` being false — a life still resolvable from the
catalog cut is skipped. A `_ckpt` is therefore deleted strictly *after* its catalog row is gone,
never while the row reads `Removing`.

## 4. Timeline — the red is one day old, not ancient

The suite was invisible to the old `Cas*:CA*` filter, so the dispatch was right not to assume
recency — but the logs settle it. Counting `[ OK ]` vs `[ FAILED ]` for this exact param across
`build*/*.log`:

| when | result |
|---|---|
| 2026-08-01 18:08 → 19:40 | **15 recorded passes** (`task5_r2_*`, `task5_r3_*`, `task5_checkpoint75a/b_*`) |
| 2026-08-03 01:11 onward | fails (`t1c2_gate_before.log`, `t1c2_gate.log`, this RCA's runs) |

Test born `f0416452507` (2026-08-01, *ca: preserve fence outcome across removal resolution*) —
which introduced both `CompetingCatalogOutcome` and the test name.

Regressing commit: **`e947949c2c7` (2026-08-02, *ca: add exact checkpoint recovery frontier*)**.
`git show` confirms `chooseRecoveryGrounding`, including the `if (!ckpt || !ckpt->life_epoch)`
refusal, is **entirely new** in that commit (all `+` lines), and that it added the 3-line call site
in `CasRefLedger.cpp`. Before it, `runRecoveryWalkOnce` tolerated a `Live` row with no `_ckpt`,
which is why the test's shortcut used to work. This confirms the dispatch's `t1c2_gate_before.log`
evidence exonerating the test-tree migration: the break predates the migration by a day and is
orthogonal to it.

So the test does **not** encode a contract retired by the removal-lifecycle rework or the ack-floor
fence. It encodes a *fixture shortcut* that a newly-added, correct production refusal now catches.

## 5. Recommendation (not implemented)

**Fix the test.** In the `/Replacement` branch, publish a well-formed `_ckpt` for the successor
life *before* the winner's catalog CAS — mirroring production's step-2-then-step-3 order and
reusing `seedCompletedRemoving`'s own construction:

```cpp
const NamespaceLifeId successor = NamespaceLifeId::fromCatalogEntry(fixture.ns, UInt128{178});
backend->putIfAbsent(layout.refCkptKey(successor), encodeRefCkpt(RefCkpt{.life_epoch = 1}));
```

The epilogue's intent — that the next name-based resolution installs a fresh runtime and drops the
retired predecessor life — is sound and worth keeping; it is only unreachable today because the
successor is malformed. Those two `EXPECT`s have never actually executed under `/Replacement`.

**Secondary, worth folding into the same edit:** `ASSERT_TRUE(leader_a_failure)` accepts *any*
exception. A fence test whose central assertion is "something threw" can pass for the wrong reason
— for instance if the deposed leader began failing on this very grounding refusal instead of on
fence loss. I verified by running that the exception today is the fence-loss one, but the assertion
should pin the error code / message rather than leave that to a manual check.

**No production change is warranted, and no other test should have caught this** — the state is
unreachable, so there is nothing for another test to catch.

## 6. Limits of this conclusion

- I established that the two `_ckpt` lifecycle seams I could find — creation step 2, and the
  janitor's `life_index.resolve` delete gate — preserve `Live`/`Removing` ⇒ `_ckpt` present. I did
  **not** exhaustively enumerate every writer of the catalog object, so I am not claiming "no
  production path whatsoever can produce a `Live` row without a `_ckpt`". `CasDecommission.cpp:179`
  HEADs the `_ckpt` and `Tools/CasFsck.cpp` reads it, but neither writes the catalog row ahead of
  it; a full audit of catalog writers was out of scope for this read-only pass.
- The verdict covers this test and this exception. I did not assess the other 26 failures visible
  in `build/t1c2_codex.log`.
- **Loose end, not a defect:** `Gc/CasGc.cpp:1385-1387` states that "a namespace has no `_ckpt`
  until its first snapshot publication commits". That is inconsistent with `completeCreation`
  publishing one at creation. It is prose drift, not a code fault — GC tolerating an absent witness
  is fail-safe in either reading — but the comment should be corrected so nobody derives the wrong
  invariant from it.

## Artefacts

- `/home/mfilimonov/workspace/ClickHouse/master/build/rca_fence_repro.log` — release repro, full stack
- `/home/mfilimonov/workspace/ClickHouse/master/build/rca_fence_asan.log` — ASan repro
- `/home/mfilimonov/workspace/ClickHouse/master/build/t1c2_gate_before.log`, `build/t1c2_gate.log` — pre/post migration, identical failure
- historical passes: `build/task5_r2_green_test.log`, `build/task5_r3_full_ca_test.log`,
  `build/task5_checkpoint75b_expanded_ca_tests_final.log`

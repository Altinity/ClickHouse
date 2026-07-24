# CAS mount-lease fence-not-rescue fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the ASan-fatal `LOGICAL_ERROR` abort on a mount-lease renewal mismatch under our own
(uuid, epoch) by classifying it as fail-closed state uncertainty (fence + self-remount), plus two
small independent hardenings (pre-I/O fence-deadline anchoring; probe-gated epoch re-mint guard).

**Architecture:** Spec `docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md`
(rev.4). Three phases: A = exhaustive non-aborting classifier in `MountLeaseKeeper::onRenewMismatch`
(the crash fix); B = anchor every fence-arm at the pre-I/O attempt instant (renewal hooks + the
startup arm after the materialization grace); C = `allocateWriterEpoch` refuses to re-mint epoch 1
while a mount object exists (authoritative probe), with a hardened decommission policy. No rescue of
ambiguous renewals — a lease holder that cannot confirm its renewal fences and remounts.

**Tech Stack:** C++ (ClickHouse `src/Disks/.../ContentAddressed/`), googletest (`unit_tests_dbms`),
TLA+/TLC (`docs/superpowers/models/CaCasMountCore.tla`).

## Global Constraints

- Branch: `cas-gc-rebuild`. Never rebase or amend — new commits only. Never push.
- C++ style: Allman braces (style check enforces). Em-dash `—` in operator-visible strings. Write
  function names as `f`, not `f()`, in comments/messages.
- Never use sleep to fix a race in C++ code. Tests use bounded polls or injected fake clocks.
- Build: `ninja -C build > build/ninja_<task>.log 2>&1` — no `-j`, always redirect to a log, always
  have a subagent summarize the log if it needs analysis.
- Unit tests binary: `build/src/unit_tests_dbms`. Always redirect test output to
  `build/test_<name>.log` (unique per test run).
- The CA gtest gate filter convention is `--gtest_filter='Cas*:CA*'` (project memory: this filter
  is the release gate for CAS work).
- Log messages / exception texts: say "exception" not "crash" for logical errors; `ASan` not `ASAN`.
- Commits end with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`
- TLC runs: `java -XX:+UseParallelGC -jar tmp/tla2tools.jar -config <cfg> CaCasMountCore.tla` from
  `docs/superpowers/models/` (tla2tools.jar already at `tmp/tla2tools.jar` repo-root — use an
  absolute path or copy; existing RESULTS files in that directory show the expected report format).

---

### Task 1: TLA+ gate — model the epoch-wipe twin and the re-mint guard

**Files:**
- Modify: `docs/superpowers/models/CaCasMountCore.tla`
- Create: `docs/superpowers/models/CaCasMountCore_sab_epochwipelive.cfg`
- Modify: `docs/superpowers/models/CaCasMountCore_RESULTS.md` (append a dated section; create if absent)

**Interfaces:**
- Consumes: the existing model — variables `owner, epoch, mount, localEpoch, localLost, wrote,
  lostThenWrote, ...` (`CaCasMountCore.tla:147-175`), honest action `AllocEpoch(a)`
  (`CaCasMountCore.tla:249`), the `Renew(a)` superseded branch that sets `localLost`
  (`CaCasMountCore.tla:356`), existing sabotage constants (`SabEpochReset` etc.,
  `CaCasMountCore.tla:140-146`) and the config battery
  (`CaCasMountCore_stage1.cfg`, `CaCasMountCore_sab_*.cfg`).
- Produces: a GREEN/RED table in `CaCasMountCore_RESULTS.md` that Tasks 2-6 cite as their phase-0
  gate. No code interfaces.

- [ ] **Step 1: Baseline — run the existing battery, confirm all-green before touching the model**

Run from `docs/superpowers/models/`:
```bash
for cfg in CaCasMountCore_stage1 CaCasMountCore_sab_epochreset CaCasMountCore_sab_foreigntakeover \
           CaCasMountCore_sab_adoptwedge CaCasMountCore_sab_fenceresurrect CaCasMountCore_sab_wallclockreclaim; do
  java -XX:+UseParallelGC -jar ../../../tmp/tla2tools.jar -config ${cfg}.cfg CaCasMountCore.tla \
    > /tmp/tlc_${cfg}.log 2>&1; echo "${cfg}: $?"
done
```
Expected: `stage1` exits 0 (invariants hold); every `sab_*` config exits non-zero (its target
invariant breaks — that is what a sabotage config is for). Record the baseline in the RESULTS file.
If the baseline itself is broken, STOP and report — do not proceed on a red baseline.

- [ ] **Step 2: Confirm Phase A needs no model change (alignment note)**

Read the `Renew(a)` action's superseded branch (`CaCasMountCore.tla:356` area): a confirmed
mismatch under a newer epoch already transitions to `localLost` knowledge — never a wedge/abort
state. Phase A maps the implementation's new `ABORTED` (and the downgraded `superseded`) onto
exactly this modeled transition. Append to `CaCasMountCore_RESULTS.md`:

```markdown
## 2026-07-24 — fence-not-rescue gate (spec rev.4)
Phase A (non-aborting classification) is model-ALIGNMENT, not a model change: the
implementation's LOGICAL_ERROR abort on a same-uuid confirmed mismatch had no model
counterpart — the model already routes every confirmed mismatch to `localLost` (Renew's
superseded branch) and proves `SupersededWriterMakesNoMutation` over it.
```

- [ ] **Step 3: Add the epoch-wipe-under-live-mount sabotage + the guard**

In `CaCasMountCore.tla`:
1. Add constant `SabEpochGuardOff` (comment: `\* FALSE = honest (epoch re-mint from 0 requires
   mount = None); TRUE drops that guard — models the pre-fix allocateWriterEpoch`).
2. Add action `WipeEpoch(a)` — models the environmental loss of the epoch object while a mount may
   be live (the round-2 finding-1 hole):
```tla
\* Environmental sabotage-adjacent action (always enabled — the environment can lose the epoch
\* object at any time; the QUESTION is whether re-minting over it is guarded):
WipeEpoch ==
    /\ epoch > 0
    /\ epoch' = 0
    /\ UNCHANGED << owner, mount, mtoken, clock, localEpoch, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>
```
3. Add the guarded re-mint action `RemintEpoch(a)` (the code's absent-epoch branch — distinct from
   the normal `AllocEpoch`, which models the CAS bump over a PRESENT epoch object):
```tla
RemintEpoch(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch = 0
    /\ (SabEpochGuardOff \/ mount = None)   \* the Phase C guard: authoritative mount absence
    /\ epoch' = 1
    /\ localEpoch' = [localEpoch EXCEPT ![a] = 1]
    /\ UNCHANGED << owner, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>
```
4. Add the decommission-branch action `RemintEpochDecom(a)` (Phase C's `DecommissionRecovery`:
   terminal-mount requirement + distinct-epoch mint), plus a constant `SabDecomBlindBypass`
   (TRUE = the rejected blind bypass — mint 1 regardless of mount liveness):
```tla
RemintEpochDecom(a) ==
    /\ ~rejected[a] /\ ~wedged[a]
    /\ owner = a
    /\ epoch = 0
    /\ mount # None
    /\ IF SabDecomBlindBypass
       THEN epoch' = 1                                  \* the round-3 finding-1 bug
       ELSE /\ mount.fenced \/ mount.deadline < clock   \* TERMINAL only (live refuses)
            /\ epoch' = mount.epoch + 1                 \* distinct by construction
    /\ localEpoch' = [localEpoch EXCEPT ![a] = epoch']
    /\ UNCHANGED << owner, mount, mtoken, clock, localLost, crashed, rejected,
                    adoptObs, fencedEpochs, wedged, wrote, rootEmpty, firstOwner,
                    lostThenWrote, reclaimed, remountedAfterFence,
                    fenceUntil, obsToken, obsSince, observedReclaimEver, supersededThenWrote >>
```
5. Wire all three into `Next`. Match the exact `UNCHANGED` tuple spelling used by the neighboring
   actions in the file (copy it from `AllocEpoch` — the variable list above is from
   `CaCasMountCore.tla:249-260` and must be kept in sync with the file's actual tuple).

- [ ] **Step 4: Create the sabotage configs and run all colors**

`CaCasMountCore_sab_epochwipelive.cfg`: copy `CaCasMountCore_stage1.cfg`, set
`SabEpochGuardOff = TRUE` (stage1 keeps/gets `SabEpochGuardOff = FALSE`,
`SabDecomBlindBypass = FALSE`). Also create `CaCasMountCore_sab_decomblindbypass.cfg`
(`SabDecomBlindBypass = TRUE`) — its counterexample is the round-3 finding-1 trace: a live
epoch-1 mount + `WipeEpoch` + the blind decommission mint recreates the same pair and the
two-writer witness breaks. Expected: RED (non-zero exit). The invariant that must
break under sabotage is the existing two-writer/superseded-mutation witness
(`SupersededWriterMakesNoMutation` / `lostThenWrote`-based — use whichever invariant name the
stage1 cfg lists; the counterexample trace must show: A holds (uuid,1) live → `WipeEpoch` →
A' (same actor family via the model's same-uuid twin path, or the double-claim path) re-mints 1 →
both write). Run:
```bash
java -XX:+UseParallelGC -jar ../../../tmp/tla2tools.jar -config CaCasMountCore_stage1.cfg CaCasMountCore.tla
java -XX:+UseParallelGC -jar ../../../tmp/tla2tools.jar -config CaCasMountCore_sab_epochwipelive.cfg CaCasMountCore.tla
```
Expected: stage1 (guard ON) exit 0 — the guard preserves the invariant even with `WipeEpoch`
enabled; sab cfg (guard OFF) non-zero with a counterexample trace. If the model's actor structure
cannot express the same-uuid twin without deeper surgery (round-3 №5 warned actor ≡ uuid), the
acceptable fallback — record it explicitly in RESULTS — is the weaker but still decisive witness:
guard OFF admits `epoch` re-mint to a value in `fencedEpochs`/already-`wrote` epochs (an
epoch-reuse witness invariant `EpochNeverReminted == \A a: (a,1) \in wrote => epoch # 0` shaped to
the model's actual bookkeeping). Do not silently downgrade — the RESULTS section must state which
witness was used and why.

- [ ] **Step 5: Re-run the FULL battery (old configs must keep their colors), append results, commit**

```bash
git add docs/superpowers/models/CaCasMountCore.tla docs/superpowers/models/CaCasMountCore_sab_epochwipelive.cfg docs/superpowers/models/CaCasMountCore_RESULTS.md
git commit -m "ca: TLA gate for fence-not-rescue (spec rev.4) — epoch-wipe twin + re-mint guard

Phase A is model-alignment (confirmed mismatch already maps to localLost).
New: WipeEpoch environmental action + guarded RemintEpoch; SabEpochGuardOff
sabotage config RED, honest config GREEN, full existing battery unchanged.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 2: Phase A — exhaustive non-aborting classification (the crash fix)

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
  (ErrorCodes block at :28-32; `MountLeaseKeeper::onRenewMismatch` at :822-880)
- Modify: `src/Common/ProfileEvents.cpp:886` (the `CasMountLeaseLost` description)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (repartition `ForeignTouchMakesRenewThrow` at :120)

**Interfaces:**
- Consumes: `emitMountEvent(sink, type, srid, branch, observed, reason)` (`CasServerRoot.cpp:228`),
  `describeMountHolder(const MountLease &)` (`CasServerRoot.cpp:216`),
  `ProfileEvents::CasMountLeaseLost`, `decodeMountLease`, base members `seq`/`last_token`
  (readable here — `onRenewMismatch` is invoked by `renewOnce` under `state_mutex`).
- Produces: `onRenewMismatch` throws `ErrorCodes::ABORTED` (never `LOGICAL_ERROR`) for every
  same-uuid body-present case; event `outcome` strings `same_epoch_state_uncertain` and
  `superseded` (unchanged name); message fragments `"state uncertain"` and
  `"superseded by a newer incarnation"` that Task 3's end-to-end test greps.

- [ ] **Step 1: Write the three failing/repartitioned tests**

In `src/Disks/tests/gtest_cas_heartbeat.cpp`, REPLACE `TEST(CasHeartbeat, ForeignTouchMakesRenewThrow)`
(lines 120-149) with the following three tests (keep the file's includes; add
`#include <Common/Exception.h>` only if not already present):

```cpp
/// Phase A (spec rev.4 2026-07-24): a confirmed renewal mismatch whose re-read shows OUR OWN
/// (uuid, epoch), unfenced, is state UNCERTAINTY (an ambiguous landed renewal of ours, or a
/// same-pair twin after epoch-state loss) — fail closed via fence + self-remount, never an
/// exception that aborts debug/ASan builds at construction.
TEST(CasHeartbeat, SameEpochUnfencedTouchIsUncertainNotFatal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; });
    keeper.start();

    /// The slot advances past our held token under our own pair (the ambiguous-landed-renewal shape).
    const HeadResult h = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(h.exists);
    MountLease advanced;
    advanced.server_uuid = uuid;
    advanced.writer_epoch = 9;
    advanced.seq = 99;
    backend->putOverwrite(layout.mountKey(srid), encodeMountLease(advanced), h.token);

    try
    {
        keeper.renewOnce();
        FAIL() << "renewOnce must throw on a confirmed mismatch";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED) << e.message();
        EXPECT_NE(e.message().find("state uncertain"), String::npos) << e.message();
        /// Forensics must ride in the message: the observed seq and our local seq.
        EXPECT_NE(e.message().find("seq=99"), String::npos) << e.message();
    }
}

/// A body under our own uuid but a NEWER writer_epoch is proven supersession — a normal fencing
/// outcome (the TLA model's localLost), fail closed but never an abort.
TEST(CasHeartbeat, SupersededTouchIsFailClosedNotFatal)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; });
    keeper.start();

    const HeadResult h = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(h.exists);
    MountLease successor;
    successor.server_uuid = uuid;
    successor.writer_epoch = 10;
    successor.seq = 1;
    backend->putOverwrite(layout.mountKey(srid), encodeMountLease(successor), h.token);

    try
    {
        keeper.renewOnce();
        FAIL() << "renewOnce must throw on a confirmed mismatch";
    }
    catch (const DB::Exception & e)
    {
        EXPECT_EQ(e.code(), DB::ErrorCodes::ABORTED) << e.message();
        EXPECT_NE(e.message().find("superseded by a newer incarnation"), String::npos) << e.message();
    }
}

/// A FOREIGN uuid on our mount slot cannot arise from any protocol interleaving (the owner anchor
/// refuses foreign claims at open; decommission impersonates the victim uuid). It stays a genuine
/// invariant violation: loud, aborting in debug/ASan builds.
TEST(CasHeartbeat, ForeignUuidTouchStillDies)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/100);

    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(100),
                            [&] { return now_ms; }, [] { return uint64_t{5}; });
    keeper.start();

    const HeadResult h = backend->head(layout.mountKey(srid));
    ASSERT_TRUE(h.exists);
    MountLease foreign;
    foreign.server_uuid = UInt128(0x9999);
    foreign.writer_epoch = 1;
    foreign.seq = 1;
    backend->putOverwrite(layout.mountKey(srid), encodeMountLease(foreign), h.token);
    EXPECT_DEATH(
        {
            DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
            keeper.renewOnce();
        },
        "held by a foreign server");
}
```

Note for the implementer: `DB::ErrorCodes::ABORTED` needs an `extern const int ABORTED;`
declaration in the test file's ErrorCodes block if the file declares its own (check the top of
`gtest_cas_heartbeat.cpp`; other CAS test files use `expectThrowsCode`-style helpers — match
whatever this file already does for code checks; a plain `e.code()` comparison as above works
everywhere).

- [ ] **Step 2: Build the test target and run — the first two tests must FAIL (die instead of throw)**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_taskA_tests.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.SameEpochUnfencedTouchIsUncertainNotFatal:CasHeartbeat.SupersededTouchIsFailClosedNotFatal:CasHeartbeat.ForeignUuidTouchStillDies' > build/test_taskA_failing.log 2>&1; echo "exit=$?"
```
Expected: non-zero exit. `SameEpochUnfencedTouchIsUncertainNotFatal` fails (current code throws
`LOGICAL_ERROR` from the base fallthrough — in a non-ASan build the exception constructs without
aborting only if `abort_on_logical_error` is false, so the test sees code `LOGICAL_ERROR` ≠
`ABORTED`); `SupersededTouchIsFailClosedNotFatal` fails the same way. `ForeignUuidTouchStillDies`
already passes (the foreign branch exists today).

- [ ] **Step 3: Implement the classifier change**

In `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`:

3a. Extend the ErrorCodes block (lines 28-32):
```cpp
namespace ErrorCodes
{
    extern const int ABORTED;
    extern const int CORRUPTED_DATA;
    extern const int FILE_DOESNT_EXIST;
    extern const int LOGICAL_ERROR;
}
```

3b. In `MountLeaseKeeper::onRenewMismatch` (starts :822), replace the function body's classified
section with (the `fenced_by_gc` branch and the `else`/vanished branch stay byte-identical; shown
in full for placement):

```cpp
void MountLeaseKeeper::onRenewMismatch(const String & mismatched_key)
{
    /// The base contract's PreconditionFailed just means "our token didn't match" — re-read the
    /// CURRENT body and classify. All four body-present cases and the absent case are covered
    /// below, each fail-closed and none (except the protocol-unreachable foreign_writer)
    /// constructing a LOGICAL_ERROR, which aborts debug/ASan builds at exception construction
    /// (STID 3982-3b48; parts 1a/1b covered vanished/absent-at-release, this covers the rest).
    const auto got = backend->get(mismatched_key);
    if (got)
    {
        const MountLease current = decodeMountLease(got->bytes);

        if (current.server_uuid == server_uuid && current.gc_fenced)
        {
            emitMountEvent(event_sink, CasEventType::MountConflict, srid, "fenced_by_gc", &current,
                "own mount slot fenced by GC after lease expiry (late renewal) — recoverable with a "
                "fresh writer_epoch");
            throw MountFencedException(fmt::format(
                "CAS mount-lease: key '{}' was fenced by GC after lease expiry (late renewal) ({}) — "
                "recoverable: re-open with a fresh writer_epoch", mismatched_key, describeMountHolder(current)));
        }

        if (current.server_uuid == server_uuid && current.writer_epoch == writer_epoch && !current.gc_fenced)
        {
            /// The slot advanced past our held token under our OWN (uuid, epoch), unfenced. This is
            /// state UNCERTAINTY, not proof of anything (spec rev.4): the common cause is our own
            /// earlier renewal PUT that landed while its ack was lost to a client-side timeout; the
            /// pathological one is a same-pair twin after durable epoch-state loss (narrowed by the
            /// allocateWriterEpoch re-mint guard). Both recover identically and fail closed: stop
            /// renewing, latch the write fence, self-remount under a fresh writer_epoch. Never a
            /// LOGICAL_ERROR — this shape is reachable by an ordinary network timeout.
            ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
            emitMountEvent(event_sink, CasEventType::MountConflict, srid, "same_epoch_state_uncertain", &current,
                "own mount slot advanced past our held token under our own (uuid, epoch) — state "
                "uncertain (ambiguous prior renewal or epoch-state loss); fencing and self-remounting");
            throw Exception(ErrorCodes::ABORTED,
                "CAS mount-lease: key '{}' advanced past our held token under our own (uuid, epoch) — "
                "state uncertain; fencing and recovering via self-remount (observed {} vs our seq={})",
                mismatched_key, describeMountHolder(current), seq);
        }

        if (current.server_uuid == server_uuid && current.writer_epoch != writer_epoch)
        {
            ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
            emitMountEvent(event_sink, CasEventType::MountConflict, srid, "superseded", &current,
                "own mount slot is held by a different writer_epoch — superseded by a newer incarnation");
            /// A normal fencing outcome (the model's localLost), not a programming assertion:
            /// a suspended predecessor legitimately resumes into this after a successor reclaimed.
            throw Exception(ErrorCodes::ABORTED,
                "CAS mount-lease: key '{}' was superseded by a newer incarnation ({}) — fencing "
                "(this incarnation is deposed; recovery is a fresh-epoch self-remount)",
                mismatched_key, describeMountHolder(current));
        }

        /// current.server_uuid != server_uuid — the one genuinely protocol-unreachable case
        /// (the owner anchor refuses foreign claims at open; decommission impersonates the victim
        /// uuid, it never manufactures a foreign one). Deliberately still LOGICAL_ERROR-loud.
        ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
        emitMountEvent(event_sink, CasEventType::MountConflict, srid, "foreign_writer", &current,
            "mount slot is held by a foreign server — failing closed, never taking over");
        throw Exception(ErrorCodes::LOGICAL_ERROR,
            "CAS mount-lease: key '{}' is held by a foreign server ({}) — failing closed, never taking over",
            mismatched_key, describeMountHolder(current));
    }

    /// The mount slot object VANISHED (backing store deleted under a live mount -- e.g. an
    /// operator or test rm -rf'd the pool dir). This is an ENVIRONMENTAL condition, not a logic
    /// error: there is no foreign writer to fail closed against. Stop renewing (fail-closed: the
    /// write fence latches to lost, we never re-mint) WITHOUT aborting the server --
    /// LOGICAL_ERROR here aborts debug/ASan builds at exception construction.
    ProfileEvents::increment(ProfileEvents::CasMountLeaseLost);
    emitMountEvent(event_sink, CasEventType::MountConflict, srid, "vanished", nullptr,
        "mount slot object vanished (backing store deleted under a live mount) — stopping renewal, fail-closed");
    throw Exception(ErrorCodes::FILE_DOESNT_EXIST,
        "CAS mount-lease: key '{}' vanished (backing store deleted under a live mount) — "
        "stopping renewal, fail-closed (never re-minting)", mismatched_key);

    /// NOTE: the pre-rev.4 trailing `SingleWriterSlot::onRenewMismatch(mismatched_key)` call is
    /// GONE — the five cases above are exhaustive for this keeper (body-present × {fenced,
    /// same-pair-unfenced, superseded, foreign} + absent), so the base class's generic
    /// LOGICAL_ERROR is unreachable here. The base implementation stays for other slot subclasses.
}
```
(The `else` keyword around the vanished branch disappears because every `if` above throws —
restructure exactly as shown: body-present cases inside `if (got)`, the vanished tail after it.)

3c. In `src/Common/ProfileEvents.cpp:886`, replace the `CasMountLeaseLost` description with:
```
M(CasMountLeaseLost, "Counts CAS mount-lease terminal losses: the mount slot object vanished (backing store deleted under a live mount), was superseded by a newer incarnation, advanced past the held token under the keeper's own (uuid, epoch) (state uncertain: an ambiguous landed renewal or epoch-state loss), or was taken by a foreign server. The keeper stopped renewing and latched its write fence to lost; non-zero values mean a mount was lost — investigate via system.content_addressed_log MountConflict rows.", ValueType::Number) \
```

- [ ] **Step 4: Build and run the three tests — all must pass**

```bash
ninja -C build > build/ninja_taskA_impl.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.*' > build/test_taskA_pass.log 2>&1; echo "exit=$?"
```
Expected: exit 0, all `CasHeartbeat.*` pass (including the untouched loop/deadline tests).

- [ ] **Step 5: Run the wider CAS gate filter**

```bash
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_taskA_gate.log 2>&1; echo "exit=$?"
```
Expected: exit 0. If `BackgroundLoopFencesImmediatelyOnConfirmedMismatch` (heartbeat :486)
constructed a same-uuid body expecting a death or a LOGICAL_ERROR, adjust it the same way as
Step 1's repartition (it must now observe the fence via `on_lost`, which is what its name says it
does — read it before touching it).

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Common/ProfileEvents.cpp src/Disks/tests/gtest_cas_heartbeat.cpp
git commit -m "ca: mount-lease renewal mismatch under own (uuid, epoch) fences, never aborts (STID 3982-3b48 part 2, Phase A)

New exhaustive classification in MountLeaseKeeper::onRenewMismatch: a
same-uuid/same-epoch/unfenced body is state uncertainty (ambiguous landed
renewal, or a same-pair twin after epoch-state loss) -> ABORTED, fence,
self-remount; superseded (same uuid, newer epoch) downgraded from
LOGICAL_ERROR to ABORTED (a normal fencing outcome, the TLA model's
localLost); foreign uuid deliberately stays LOGICAL_ERROR-loud (protocol-
unreachable past the owner anchor). The base-class fallthrough is now
unreachable for this keeper and its call is removed.

Root cause (CI, Altinity PR#2073 asan CAS-s3): a renewal PUT timed out
client-side but landed server-side; the next beat's confirmed mismatch
re-read our own live-looking body, matched no branch, and the base
LOGICAL_ERROR aborted the server at exception construction.

Spec: docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md (rev.4)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 3: Phase A end-to-end — the CI crash scenario through the real background loop

**Files:**
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (new fault backend + one test, appended after
  `TransientPutOverwriteFaultBackend` at :419-440)

**Interfaces:**
- Consumes: Task 2's `same_epoch_state_uncertain` branch (`ABORTED`); the existing
  `TransientPutOverwriteFaultBackend` pattern (:419) and the bounded-poll loop-test pattern of
  `BackgroundLoopRetriesTransientFailureWithoutFencingOrStopping` (:442);
  `keeper.setFenceCallbacks(on_renew_ok, on_lost)` (both `std::function<void()>` until Task 4).
- Produces: `ApplyThenThrowPutOverwriteFaultBackend` (used again by Task 4's delayed-ack test).

- [ ] **Step 1: Write the fault backend and the failing-shaped test**

Append to `src/Disks/tests/gtest_cas_heartbeat.cpp` (inside the same anonymous namespace as the
existing fault backend, or a new one next to it):

```cpp
namespace
{
/// The landed-but-unacked case: the putOverwrite APPLIES to the in-memory state, THEN throws a
/// transient exception — the exact CI shape (a client-side timeout whose PUT landed server-side).
/// The existing TransientPutOverwriteFaultBackend throws BEFORE applying and cannot model this.
class ApplyThenThrowPutOverwriteFaultBackend final : public InMemoryBackend
{
public:
    int fault_count = 0;

    PutResult putOverwrite(const String & k, const String & b, const Token & e, const ObjectMeta & m) override
    {
        if (fault_count > 0)
        {
            --fault_count;
            InMemoryBackend::putOverwrite(k, b, e, m);   /// the write LANDS...
            throw DB::Exception(DB::ErrorCodes::NETWORK_ERROR,
                "injected ambiguous fault: applied, ack lost");   /// ...the ack does not.
        }
        return InMemoryBackend::putOverwrite(k, b, e, m);
    }
};
}

/// End-to-end reproduction of the CI crash (Altinity PR#2073, asan CAS-s3 stateless): beat 1's
/// renewal lands but its ack is lost (transient -> the loop retries, deadline permitting); beat 2
/// renews with the now-stale token, gets a CONFIRMED mismatch, re-reads our own advanced body ->
/// the Phase A uncertain branch -> the loop stops, on_lost fires (fence latches; in production the
/// Pool self-remounts from there). No process death anywhere.
TEST(CasHeartbeat, BackgroundLoopSurvivesAmbiguousLandedRenewal)
{
    auto backend = std::make_shared<ApplyThenThrowPutOverwriteFaultBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/30000);

    std::atomic<bool> lost{false};
    MountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9, std::chrono::milliseconds(30000),
                            [&] { return now_ms; }, [] { return uint64_t{0}; }, CasEventSink{},
                            std::chrono::milliseconds(2000));
    keeper.setFenceCallbacks([] {}, [&] { lost = true; });
    keeper.start();   /// the adopt-path putOverwrite must land unfaulted.

    backend->fault_count = 1;   /// beat 1: lands + throws (ambiguous); beat 2: confirmed mismatch.
    keeper.startBackground(std::chrono::milliseconds(20));

    /// Bounded poll for on_lost (never a blind sleep): the deadline is generous; the loop needs
    /// two ~20ms beats. abort_on_logical_error stays ON to prove no branch constructs one.
    DB::abort_on_logical_error.store(true, std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!lost.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(lost.load()) << "the confirmed mismatch after an ambiguous landed renewal must "
                                "latch the fence via on_lost (and must not abort the process)";
    keeper.stopBackground();
}
```
(`abort_on_logical_error` may already default appropriately in this test binary — setting it true
makes the no-abort claim load-bearing. The 5ms poll sleep is a bounded wait on a real background
thread, matching the file's existing loop-test convention at :466-470 — not a race "fix".)

- [ ] **Step 2: Build, run the new test, confirm it passes (it is green-on-arrival if Task 2 landed; its value is pinning the end-to-end path)**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_taskA_e2e.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.BackgroundLoopSurvivesAmbiguousLandedRenewal' > build/test_taskA_e2e.log 2>&1; echo "exit=$?"
```
Expected: exit 0. Sanity: `git stash` the Task 2 classifier change and re-run — the test must then
FAIL (die) — `git stash pop` after. This proves the test actually covers the crash.

- [ ] **Step 3: Commit**

```bash
git add src/Disks/tests/gtest_cas_heartbeat.cpp
git commit -m "ca: gtest — end-to-end ambiguous-landed-renewal survives via fence (Phase A e2e)

New ApplyThenThrowPutOverwriteFaultBackend (applies, then throws — the
landed-but-unacked case the existing transient mock cannot model) + a real
backgroundLoop test reproducing the CI crash shape: beat 1 lands ambiguous,
beat 2's confirmed mismatch takes the same_epoch_state_uncertain branch,
on_lost latches the fence, no process death.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 4: Phase B (keeper) — anchor both fence deadlines at the pre-I/O attempt instant

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
  (`MountLeaseKeeper` decl: ctor, `setFenceCallbacks`, `refreshConfirmedDeadline`, new members)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
  (`prepareRenew` :676, `claim` refresh sites :717/:796, `onRenewSucceeded` :800,
  `refreshConfirmedDeadline` :661)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp`
  (`installKeeper` :226-248)
- Test: `src/Disks/tests/gtest_cas_heartbeat.cpp` (two `setFenceCallbacks` lambdas at :455/:508 +
  one new test)

**Interfaces:**
- Consumes: `CasMountRuntime::bootMsNow()` (`CasMountRuntime.cpp:68`), `setMountDeadline`,
  Task 3's `ApplyThenThrowPutOverwriteFaultBackend`.
- Produces: `MountLeaseKeeper` ctor gains a trailing defaulted
  `std::function<uint64_t()> boot_ms_fn_ = {}` (empty ⇒ real `CLOCK_BOOTTIME`);
  `setFenceCallbacks(std::function<void(uint64_t attempt_boot_ms)> on_renew_ok_, std::function<void()> on_lost_)`
  — **on_renew_ok now takes the boot-domain attempt anchor**;
  `void refreshConfirmedDeadline(uint64_t anchor_wall_ms)` (parameterized). Task 5 consumes the
  anchored-arm idea but no new symbol from this task.

- [ ] **Step 1: Write the failing test (anchored deadline, not response-time deadline)**

Append to `gtest_cas_heartbeat.cpp`:

```cpp
/// Phase B: the confirmed-lease deadline anchors at the ATTEMPT-START instant, not the response
/// instant — a slow ack must not extend the local fence past what the durable body authorizes.
TEST(CasHeartbeat, RenewDeadlineAnchorsAtAttemptStartNotResponseTime)
{
    auto backend = std::make_shared<InMemoryBackend>();
    Layout layout("pool");
    const String srid = "test";
    const UInt128 uuid(0x1234);
    uint64_t now_ms = 1000;
    seedOwnClaim(*backend, layout, srid, uuid, /*epoch=*/9, now_ms, /*ttl_ms=*/1000);

    TestableMountLeaseKeeper keeper(backend, layout, srid, uuid, /*writer_epoch=*/9,
                                    std::chrono::milliseconds(1000),
                                    [&] { return now_ms; }, [] { return uint64_t{0}; }, CasEventSink{},
                                    std::chrono::milliseconds(100));
    keeper.start();   /// claim at now=1000 -> anchored confirmed deadline 2000

    /// Beat at now=1500; the "ack" (onRenewSucceeded) arrives late, at now=2400 — after the
    /// durable expiry stamped by THIS beat's payload (1500+1000=2500 durable; anchor 1500).
    now_ms = 1500;
    keeper.renewOnce();
    now_ms = 2400;
    keeper.onRenewSucceeded();

    /// Anchored: deadline = 1500 + 1000 = 2500. At now=2401 with margin 100 the boundary check is
    /// 2401 + 100 >= 2500 -> must fence. (Response-time behavior — the bug — would give
    /// 2400 + 1000 = 3400 and NOT fence.)
    now_ms = 2401;
    EXPECT_TRUE(keeper.shouldFenceOnTransientRenewFailure())
        << "a late ack must not extend the confirmed deadline past attempt-start + TTL";
}
```

- [ ] **Step 2: Build + run — must FAIL on current code**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_taskB_tests.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.RenewDeadlineAnchorsAtAttemptStartNotResponseTime' > build/test_taskB_failing.log 2>&1; echo "exit=$?"
```
Expected: FAIL — current `refreshConfirmedDeadline` computes `now_ms_fn() + ttl` = 2400+1000=3400,
`shouldFence` at 2401 returns false.

- [ ] **Step 3: Implement the keeper anchoring**

3a. `CasServerRoot.h`, `MountLeaseKeeper`:
- ctor: append trailing param `std::function<uint64_t()> boot_ms_fn_ = {}` (after
  `lease_safety_margin_`); doc: "boot-domain clock for the on_renew_ok anchor; empty = real
  CLOCK_BOOTTIME. Injectable for tests and wired by CasMountRuntime::installKeeper."
- `setFenceCallbacks` signature: `void setFenceCallbacks(std::function<void(uint64_t)> on_renew_ok_, std::function<void()> on_lost_)`
  — doc gains: "on_renew_ok receives the ATTEMPT-START boot-domain instant of the renewal it
  acknowledges — the fence deadline must anchor there, never at response time (spec rev.4
  Phase B)."
- members: `std::function<uint64_t()> boot_ms_fn;` and
```cpp
    /// Pre-I/O anchors of the CURRENT attempt, stashed by prepareRenew (which runs at the start of
    /// every doStart/renewOnce attempt, off the state lock) and consumed by the success hooks.
    /// `mutable` + no synchronization is safe: prepareRenew, claim, and the hooks all run on the
    /// single renewal driver thread (see renewOnce's single-driver invariant).
    mutable uint64_t last_attempt_wall_ms = 0;
    mutable uint64_t last_attempt_boot_ms = 0;
```
- `refreshConfirmedDeadline()` becomes `refreshConfirmedDeadline(uint64_t anchor_wall_ms)`; doc:
  "anchor = the pre-I/O wall instant of the confirming attempt".
- on_renew_ok member type: `std::function<void(uint64_t)> on_renew_ok;`

3b. `CasServerRoot.cpp`:
- ctor init: `boot_ms_fn(boot_ms_fn_ ? std::move(boot_ms_fn_) : defaultBootMs)` where
```cpp
namespace
{
uint64_t defaultBootMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + static_cast<uint64_t>(ts.tv_nsec) / 1000000;
}
}
```
- `prepareRenew` (:676): stash the anchors FIRST (before sampling the payload's wall stamp is fine
  — same instant; the load-bearing property is BEFORE the PUT I/O):
```cpp
SingleWriterSlot::RenewPayload MountLeaseKeeper::prepareRenew() const
{
    /// Pre-I/O anchors (spec rev.4 Phase B): both fence deadlines anchor at this instant — the
    /// wall stamp doubles as the payload's now_ms, so anchor <= the durable stamp trivially.
    last_attempt_wall_ms = now_ms_fn();
    last_attempt_boot_ms = boot_ms_fn();
    return {.value = last_attempt_wall_ms, .value2 = min_active_fn()};
}
```
- `refreshConfirmedDeadline(uint64_t anchor_wall_ms)` (:661):
  `confirmed_deadline_ms = anchor_wall_ms + static_cast<uint64_t>(ttl.count());`
- both `claim` refresh call sites (:717, :796): `refreshConfirmedDeadline(last_attempt_wall_ms);`
  (claim runs inside `doStart`, which calls `prepareRenew` first — the stash is fresh).
- `onRenewSucceeded` (:800):
```cpp
void MountLeaseKeeper::onRenewSucceeded()
{
    /// Anchor at the attempt start (stashed by prepareRenew), never at this ack instant — a slow
    /// ack must not extend either fence past what the durable body authorizes (spec rev.4 Phase B).
    refreshConfirmedDeadline(last_attempt_wall_ms);
    if (on_renew_ok)
        on_renew_ok(last_attempt_boot_ms);
}
```

3c. `CasMountRuntime.cpp` `installKeeper` (:226-248): pass the boot fn and take the anchor:
```cpp
    mount_keeper = std::make_unique<MountLeaseKeeper>(
        backend_ptr, layout, server_root_id, our_uuid, writer_epoch,
        config.mount_lease_ttl_ms, now_ms,
        [this] { return minActive(); },
        [this](CasEvent e) { emitEvent(std::move(e)); },
        std::chrono::milliseconds(cas_request_budget.lease_safety_margin_ms),
        [this] { return bootMsNow(); });
    mount_keeper->setFenceCallbacks(
        [this, ttl_ms](uint64_t attempt_boot_ms) { setMountDeadline(attempt_boot_ms + ttl_ms); },
        ...unchanged on_lost...);
```

3d. Test-file lambda updates: `gtest_cas_heartbeat.cpp:455` and `:508` (and Task 3's new test):
`keeper.setFenceCallbacks([] {}, ...)` → `keeper.setFenceCallbacks([](uint64_t) {}, ...)`.
`TestableMountLeaseKeeper` (:40) already re-exports `onRenewSucceeded` — no change.

- [ ] **Step 4: Build everything, run the heartbeat + gate filters**

```bash
ninja -C build > build/ninja_taskB_impl.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasHeartbeat.*:CasMountAudit.*' > build/test_taskB_pass.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_taskB_gate.log 2>&1; echo "exit=$?"
```
Expected: both exit 0. `SuccessfulRenewExtendsTransientRetryDeadline` (:391) asserts deadline
arithmetic — if it assumed response-time refresh, re-derive its constants for anchored semantics
(the test drives `now_ms` explicitly; the anchored value is the `now_ms` at its `renewOnce` call,
not at its `onRenewSucceeded` call).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_heartbeat.cpp
git commit -m "ca: anchor mount-lease fence deadlines at the pre-I/O attempt instant (Phase B, keeper)

Both deadline sites (the keeper's wall-domain confirmed_deadline_ms and the
runtime's boot-domain mayMutate fence) previously refreshed from RESPONSE
time, exceeding the durable authorization by the request latency — bounded
only by the S3 request timeout (30 s default), not by any protocol constant.
prepareRenew now stashes one pre-I/O anchor per clock domain; the success
hooks and on_renew_ok(anchor) consume them. A slow ack can no longer extend
a local fence past what the durable body it acknowledges authorizes.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 5: Phase B (startup) — re-anchor the arm after the materialization grace

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
  (`mountWritable`, the grace-wait + arm region :648-685)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h/.cpp`
  (thin forwarder `keeperRenewOnce`)
- Test: `src/Disks/tests/gtest_cas_pool.cpp`

**Interfaces:**
- Consumes: `SingleWriterSlot::renewOnce` (public, `CasServerRoot.h:74`); `Pool::bootMsNow()`;
  `Pool::armMountFence(uuid, epoch, deadline_boot_ms)` (`CasPool.cpp:303`);
  `store->mount_runtime.waitSleep`; `PoolConfig::materialization_grace_ms`,
  `PoolConfig::boot_ms_fn`, `PoolConfig::wait_sleep_fn` (test fakes — same knobs
  `CasMountRuntime::bootMsNow`/`waitSleep` already honor).
- Produces: `void CasMountRuntime::keeperRenewOnce()` (forwarder, like `keeperStart`).

- [ ] **Step 1: Add the forwarder**

`CasMountRuntime.h` (next to `keeperStart`): `void keeperRenewOnce();`
`CasMountRuntime.cpp`:
```cpp
void CasMountRuntime::keeperRenewOnce()
{
    mount_keeper->renewOnce();
}
```

- [ ] **Step 2: Write the failing test**

In `src/Disks/tests/gtest_cas_pool.cpp` (the file's writable-open idiom is a bare
`DB::Cas::PoolConfig cfg; cfg.pool_prefix = "pool"; cfg.server_id = ...; cfg.server_root_id = ...;
DB::Cas::Pool::open(backend, cfg);` — see :142-147; wrapper-backend idiom: `ProbeWatchingBackend`
at :162). Add:

```cpp
namespace
{
/// Counts putOverwrite calls on the MOUNT key that happen at-or-after the materialization grace
/// (the flag is flipped by the wait_sleep_fn hook below) — observable proof of the Phase B redo.
class MountWriteAfterGraceCountingBackend final : public DB::Cas::InMemoryBackend
{
public:
    String mount_key;
    std::atomic<bool> grace_ran{false};
    std::atomic<int> mount_writes_after_grace{0};

    DB::Cas::PutResult putOverwrite(const String & k, const String & b, const DB::Cas::Token & e,
                                    const DB::Cas::ObjectMeta & m) override
    {
        if (grace_ran.load() && k == mount_key)
            ++mount_writes_after_grace;
        return InMemoryBackend::putOverwrite(k, b, e, m);
    }
};
}

/// Phase B startup-arm (spec rev.4, codex round-3 finding 2): an unclean predecessor triggers the
/// materialization grace; a grace that consumes the lease TTL must force ONE fresh conditional
/// lease write before arming — the fence must never arm from a pre-grace anchor that has already
/// expired (a successor could have legally reclaimed during the wait).
TEST(CasPool, StartupArmRedoesLeaseWriteWhenGraceConsumesTtl)
{
    auto backend = std::make_shared<MountWriteAfterGraceCountingBackend>();
    DB::Cas::Layout layout("pool");
    const String srid = "s";
    const DB::UInt128 uuid(0x42);
    backend->mount_key = layout.mountKey(srid);

    /// Seed a FENCED, expired predecessor body: the claim reclaims it with
    /// MountPriorState::Fenced -> unclean_reclaim = true -> the grace wait runs.
    {
        DB::Cas::MountLease prior;
        prior.server_uuid = uuid;
        prior.writer_epoch = 1;
        prior.seq = 7;
        prior.expires_at_ms = 1;      /// long expired
        prior.gc_fenced = true;
        backend->putIfAbsent(layout.mountKey(srid), DB::Cas::encodeMountLease(prior));
    }

    uint64_t fake_boot_ms = 10'000;
    DB::Cas::PoolConfig cfg;
    cfg.pool_prefix = "pool";
    cfg.server_id = uuid;
    cfg.server_root_id = srid;
    cfg.mount_lease_ttl_ms = std::chrono::milliseconds(30'000);
    cfg.materialization_grace_ms = 40'000;   /// grace > TTL -> the redo must trigger
    cfg.boot_ms_fn = [&] { return fake_boot_ms; };
    cfg.wait_sleep_fn = [&](uint64_t ms)
    {
        fake_boot_ms += ms;               /// the grace advances the fake boot clock past the TTL
        backend->grace_ran.store(true);
    };

    auto store = DB::Cas::Pool::open(backend, cfg);
    ASSERT_NE(store, nullptr);

    EXPECT_EQ(backend->mount_writes_after_grace.load(), 1)
        << "a TTL-consuming grace must be followed by exactly ONE fresh conditional lease write "
           "(the re-anchoring redo) before the write fence arms";
}
```
Notes: if `Pool::open`'s writable path insists on a pre-existing/probe-able root beyond what the
bare config provides, mirror whatever seeding the nearest writable-open test in this file performs
(`:142-147` opens writable with nothing pre-seeded — the pool bootstraps `_pool_meta` itself; the
fenced-mount seed above is the only extra object this scenario needs, and the owner/epoch objects
are created by the open itself). The `wait_sleep_fn` hook is also used for other open/remount
waits (`CasPool.h:158`) — flipping `grace_ran` on its FIRST call is correct here because the fenced
predecessor makes the materialization grace the first wait this open performs; if a fence-recovery
dance in `keeperStart` (the adopt-refuses-fenced-self path) inserts an earlier wait, gate the flag
on `ms == cfg.materialization_grace_ms` instead — the assertion stays the same.

- [ ] **Step 3: Run — must FAIL (seq stays 8: no redo on current code)**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_taskB2_tests.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasPool.StartupArmRedoesLeaseWriteWhenGraceConsumesTtl' > build/test_taskB2_failing.log 2>&1; echo "exit=$?"
```

- [ ] **Step 4: Implement the anchored arm in `mountWritable`**

In `CasPool.cpp`, the region :648-685. Capture the claim anchor BEFORE `keeperStart()` (inside the
claim loop, next to `installKeeper`):
```cpp
        store->mount_runtime.installKeeper(our_uuid, writer_epoch, now_ms);
        uint64_t claim_anchor_boot_ms = store->bootMsNow();   /// pre-I/O anchor of the claim attempt
        try
        {
            store->mount_runtime.keeperStart();
        }
        ...
```
(declare `uint64_t claim_anchor_boot_ms = 0;` before the loop so it survives the `break`). After
the grace wait, before the arm:
```cpp
    const uint64_t ttl_ms_u = static_cast<uint64_t>(store->config.mount_lease_ttl_ms.count());
    if (store->bootMsNow() >= claim_anchor_boot_ms + ttl_ms_u)
    {
        /// The materialization grace consumed the lease TTL: the claim's anchor can no longer
        /// authorize an armed fence (a successor may have legally started reclaiming). Re-anchor
        /// with ONE fresh conditional lease write — it fails closed (Phase A classification) if
        /// anything took the slot meanwhile — and arm from the new attempt's anchor (rev.4
        /// Phase B, round-3 finding 2).
        LOG_WARNING(getLogger("CasPool"),
            "Content-addressed mount {}: materialization grace ({} ms) consumed the lease TTL "
            "({} ms); re-writing the lease before arming the write fence", srid,
            store->config.materialization_grace_ms, ttl_ms_u);
        claim_anchor_boot_ms = store->bootMsNow();
        store->mount_runtime.keeperRenewOnce();
    }
    store->armMountFence(our_uuid, writer_epoch, claim_anchor_boot_ms + ttl_ms_u);
```
(replacing the current `store->armMountFence(our_uuid, writer_epoch, store->bootMsNow() + ...)`).

- [ ] **Step 5: Build, run the new test + pool suite + gate**

```bash
ninja -C build > build/ninja_taskB2_impl.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasPool.*' > build/test_taskB2_pool.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_taskB2_gate.log 2>&1; echo "exit=$?"
```
Expected: both exit 0.

- [ ] **Step 6: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasMountRuntime.cpp src/Disks/tests/gtest_cas_pool.cpp
git commit -m "ca: startup write-fence arms from the claim-attempt anchor; TTL-consuming grace forces a lease re-write (Phase B, startup)

mountWritable armed bootMsNow()+TTL AFTER the (unbounded, operator-configured)
materialization grace — a grace longer than the stale-token observation
threshold let a successor legally reclaim during the wait while the
predecessor then armed an already-superseded claim without revalidation.
The arm now anchors at the claim attempt's pre-I/O instant; if the grace
consumed the TTL, one fresh conditional lease write (keeperRenewOnce, fails
closed via the Phase A classification if the slot changed hands) re-anchors
before arming. Zero cost for sane configs (grace << TTL).

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 6: Phase C — probe-gated epoch re-mint guard, decommission-aware

**Files:**
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h`
  (declare `EpochMintPolicy` + the new `allocateWriterEpoch` signature, near :253)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp`
  (`allocateWriterEpoch` :151-190)
- Modify: `src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp`
  (`mountWritable` — hoist `now_ms` above the first alloc; pass the policy at the three sites
  :495, :592, :632)
- Test: `src/Disks/tests/gtest_cas_mount.cpp` (new tests next to the existing
  `allocateWriterEpoch` tests at :120-140), `src/Disks/tests/gtest_cas_decommission.cpp`

**Interfaces:**
- Consumes: `Backend::probeSentinelRaw(key) -> SentinelProbeResult{outcome, body}` with
  `ProbeOutcome{Present, KeyAbsent, ContainerAbsent, AccessDenied, Indeterminate}`
  (`CasBackend.h:299`; implemented natively at `CasObjectStorageBackend.cpp:735`,
  `InMemoryBackend` inherits the head-derived default); `Layout::mountKey(srid)`;
  `decodeMountLease`; `MountClaimPolicy` (`WaitForExpiry` = normal open, `NoWait` = decommission).
- Produces:
```cpp
enum class EpochMintPolicy : uint8_t
{
    NormalMount,            /// absent-epoch re-mint requires authoritative mount absence
    DecommissionRecovery,   /// absent-epoch re-mint requires a TERMINAL mount; mints a distinct epoch
};
uint64_t allocateWriterEpoch(Backend & b, const Layout & l, const String & srid,
                             EpochMintPolicy policy = EpochMintPolicy::NormalMount,
                             uint64_t now_ms = 0);   /// now_ms: required (nonzero) for DecommissionRecovery
```
(defaulted params keep the existing callers in `gtest_cas_mount.cpp:127` /
`gtest_cas_ref_writer.cpp:2236` compiling unchanged).

- [ ] **Step 1: Write the failing tests**

Append to `src/Disks/tests/gtest_cas_mount.cpp` (mirror the file's existing
`allocateWriterEpoch` test style at :120-140):

```cpp
/// Phase C (spec rev.4): an ABSENT epoch object over a PRESENT mount object means durable epoch
/// state was lost while a mount is live/recent — re-minting epoch 1 there is how a same-(uuid,
/// epoch) twin is born. Refuse.
TEST(CasMount, EpochRemintOverExistingMountRefuses)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), /*epoch=*/1, /*now_ms=*/1000, /*ttl_ms=*/30000).kind,
              MountClaimResult::Claimed);
    /// The epoch object is ABSENT (never created in this sequence) while the mount exists:
    EXPECT_THROW(allocateWriterEpoch(*b, l, "r"), DB::Exception);   /// CORRUPTED_DATA
}

TEST(CasMount, EpochRemintAuthoritativeAbsenceMints)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    EXPECT_EQ(allocateWriterEpoch(*b, l, "r"), 1u);   /// fresh root: both control objects absent
    EXPECT_EQ(allocateWriterEpoch(*b, l, "r"), 2u);   /// epoch present now: normal CAS bump, no probe
}

/// The probe outcome gates the mint: anything short of authoritative KeyAbsent fails closed.
TEST(CasMount, EpochRemintIndeterminateProbeFailsClosed)
{
    class IndeterminateProbeBackend final : public InMemoryBackend
    {
    public:
        SentinelProbeResult probeSentinelRaw(const String &) override
        {
            return {.outcome = ProbeOutcome::Indeterminate, .body = std::nullopt};
        }
    };
    auto b = std::make_shared<IndeterminateProbeBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    EXPECT_THROW(allocateWriterEpoch(*b, l, "r"), DB::Exception);
}

/// Decommission over a TERMINAL (expired/fenced) mount with a lost epoch object proceeds and mints
/// an epoch DISTINCT from the surviving mount's — the same-pair state is unrepresentable.
TEST(CasMount, DecommissionRemintOverTerminalMountMintsDistinctEpoch)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), /*epoch=*/3, /*now_ms=*/1000, /*ttl_ms=*/100).kind,
              MountClaimResult::Claimed);
    /// now_ms=5000: the ttl_ms=100 lease above is long expired -> terminal.
    EXPECT_EQ(allocateWriterEpoch(*b, l, "r", EpochMintPolicy::DecommissionRecovery, /*now_ms=*/5000), 4u);
}

/// Decommission over a LIVE mount with a lost epoch refuses — the blind bypass would recreate the
/// forbidden pair (codex round-3 finding 1) and defeat CasDecommission.RefusesLiveMember.
TEST(CasMount, DecommissionRemintOverLiveMountRefuses)
{
    auto b = std::make_shared<InMemoryBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    ASSERT_EQ(claimMount(*b, l, "r", UInt128(1), /*epoch=*/1, /*now_ms=*/1000, /*ttl_ms=*/30000).kind,
              MountClaimResult::Claimed);
    EXPECT_THROW(allocateWriterEpoch(*b, l, "r", EpochMintPolicy::DecommissionRecovery, /*now_ms=*/2000),
                 DB::Exception);   /// ABORTED: live member
}

/// The steady-state path (epoch object PRESENT) must never pay the probe — pins the zero
/// normal-path cost the spec claims.
TEST(CasMount, EpochBumpWithPresentEpochIssuesNoProbe)
{
    class ProbeCountingBackend final : public InMemoryBackend
    {
    public:
        int probes = 0;
        SentinelProbeResult probeSentinelRaw(const String & k) override
        {
            ++probes;
            return InMemoryBackend::probeSentinelRaw(k);
        }
    };
    auto b = std::make_shared<ProbeCountingBackend>();
    Layout l("p");
    claimOwnerOrThrow(*b, l, "r", UInt128(1));
    EXPECT_EQ(allocateWriterEpoch(*b, l, "r"), 1u);   /// bootstrap: ONE probe (absent-epoch branch)
    const int probes_after_bootstrap = b->probes;
    EXPECT_EQ(allocateWriterEpoch(*b, l, "r"), 2u);   /// epoch present: normal CAS bump...
    EXPECT_EQ(b->probes, probes_after_bootstrap) << "...must not probe the mount key";
}
```

- [ ] **Step 2: Build + run — the four new guard tests must FAIL (mints happen today), `EpochRemintAuthoritativeAbsenceMints` passes**

```bash
ninja -C build src/unit_tests_dbms > build/ninja_taskC_tests.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMount.EpochRemint*:CasMount.DecommissionRemint*' > build/test_taskC_failing.log 2>&1; echo "exit=$?"
```

- [ ] **Step 3: Implement**

3a. `CasServerRoot.h` near :253: the enum + new signature (shown in Interfaces above), with the doc
comment: "The absent-epoch branch is a LIFECYCLE decision, so it uses `probeSentinelRaw`'s
authoritative outcomes, never plain `get`-absence (which flattens transport faults into
'not found')."

3b. `CasServerRoot.cpp` `allocateWriterEpoch` — replace the absent-`got` branch (:167-180):
```cpp
        else
        {
            /// A missing `epoch` over a non-empty subtree is a reset hazard (durable monotone
            /// counter cannot be reconstructed) — fail closed.
            if (!serverRootSubtreeEmpty(b, l, srid))
                throw Exception(ErrorCodes::CORRUPTED_DATA,
                    "CAS server-root '{}' has no durable epoch object but its data subtree is "
                    "non-empty (writer_epoch reset hazard) — refusing to proceed",
                    srid);

            /// Same hazard through the CONTROL objects (spec rev.4 Phase C): an absent epoch while
            /// a mount object exists means epoch state was lost under a live/recent mount —
            /// re-minting epoch 1 there is how a same-(uuid, epoch) twin is born. This is a
            /// lifecycle decision, so it uses the authoritative probe, never get-absence.
            const SentinelProbeResult mount_probe = b.probeSentinelRaw(l.mountKey(srid));
            switch (mount_probe.outcome)
            {
                case ProbeOutcome::KeyAbsent:
                    break;   /// authoritative absence — fresh-root bootstrap proceeds below
                case ProbeOutcome::Present:
                {
                    if (policy == EpochMintPolicy::DecommissionRecovery)
                    {
                        chassert(now_ms != 0);   /// the decommission caller must pass its clock
                        const MountLease surviving = decodeMountLease(*mount_probe.body);
                        const bool live = !surviving.gc_fenced && surviving.expires_at_ms > now_ms;
                        if (live)
                            throw Exception(ErrorCodes::ABORTED,
                                "CAS decommission '{}': epoch object missing but a LIVE mount lease "
                                "exists ({}) — refusing to re-mint an epoch under a live member "
                                "(stop the server or wait for its lease to lapse)",
                                srid, describeMountHolder(surviving));
                        /// Terminal mount: proceed, but mint an epoch DISTINCT from the survivor's
                        /// by construction — the same-pair state is unrepresentable on this path.
                        current.next_writer_epoch = std::max<uint64_t>(1, surviving.writer_epoch + 1);
                        break;
                    }
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS server-root '{}' has no durable epoch object but a mount lease exists — "
                        "durable epoch state was lost while a mount is live or recently live; "
                        "refusing to re-mint epoch 1. If no server is live on this root, "
                        "decommission it or manually remove the stale mount object '{}'.",
                        srid, l.mountKey(srid));
                }
                case ProbeOutcome::ContainerAbsent:
                case ProbeOutcome::AccessDenied:
                case ProbeOutcome::Indeterminate:
                    throw Exception(ErrorCodes::CORRUPTED_DATA,
                        "CAS server-root '{}': cannot verify mount-lease absence before re-minting "
                        "the writer epoch (probe outcome: {}) — absence was never proven; failing closed",
                        srid, magic_enum::enum_name(mount_probe.outcome));
            }

            if (current.next_writer_epoch == 0)
                current.next_writer_epoch = 1;
        }
```
(keep the surrounding CAS loop intact; `current` is the zero-initialized `ServerEpoch` of the
absent branch — the decommission case seeds it with the distinct value, the bootstrap case falls
through to 1. Note the function signature gains `EpochMintPolicy policy` and `uint64_t now_ms`;
`ErrorCodes::ABORTED` is already declared by Task 2. Add `#include <magic_enum.hpp>` if the file
lacks it — check first; several CAS files use `magic_enum::enum_name`.)

3c. `CasPool.cpp` `mountWritable`: hoist the `now_ms` lambda definition (currently :499-503) ABOVE
step 3's first `allocateWriterEpoch` call (:495), derive the policy once, and update all three
sites:
```cpp
    const EpochMintPolicy epoch_policy = (policy == MountClaimPolicy::NoWait)
        ? EpochMintPolicy::DecommissionRecovery
        : EpochMintPolicy::NormalMount;
    ...
    uint64_t writer_epoch = allocateWriterEpoch(*store->pool_backend, store->pool_layout, srid,
                                                epoch_policy, now_ms());
```
(same argument tail at :592 and :632; the fence-recovery re-allocations run with the epoch object
present, so the policy is inert there — passed uniformly for honesty).

- [ ] **Step 4: Build, run the new tests + mount/decommission/gate suites**

```bash
ninja -C build > build/ninja_taskC_impl.log 2>&1
build/src/unit_tests_dbms --gtest_filter='CasMount.*' > build/test_taskC_mount.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='CasDecommission.*' > build/test_taskC_decom.log 2>&1; echo "exit=$?"
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_taskC_gate.log 2>&1; echo "exit=$?"
```
Expected: all exit 0 — in particular `CasDecommission.RefusesLiveMember` (:374) still green, and
any decommission test that exercises the partial-hand-cleanup path (owner absent, mount surviving)
still green (`openForDecommission` reaches the new `DecommissionRecovery` branch only when the
epoch object is ALSO missing).

- [ ] **Step 5: Commit**

```bash
git add src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.h src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasServerRoot.cpp src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Pool/CasPool.cpp src/Disks/tests/gtest_cas_mount.cpp src/Disks/tests/gtest_cas_decommission.cpp
git commit -m "ca: refuse to re-mint writer_epoch 1 while a mount object exists (Phase C)

allocateWriterEpoch's absent-epoch branch minted epoch 1 whenever the data
subtrees were empty — ignoring the mount/epoch CONTROL objects, so an epoch
object lost under a live mount handed a second same-uuid process the same
(uuid, epoch) pair (codex round-2 finding 1). The branch now gates on
probeSentinelRaw's AUTHORITATIVE outcomes (get-absence flattens transport
faults into not-found and must not gate a lifecycle decision): KeyAbsent
mints, Present refuses (CORRUPTED_DATA with recovery guidance), everything
else fails closed naming the probe outcome.

Decommission (EpochMintPolicy::DecommissionRecovery, derived from
MountClaimPolicy::NoWait) is NOT a blind bypass (codex round-3 finding 1):
it requires the surviving mount to be TERMINAL (a live member refuses,
preserving CasDecommission.RefusesLiveMember semantics) and mints
surviving.writer_epoch + 1 — distinct by construction.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

### Task 7: Final validation — full gate, spec status, memory

**Files:**
- Modify: `docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md` (Status line)
- Modify: `docs/superpowers/cas/BACKLOG.md` (the `[STID-3982-3b48 part 2]` §8 entry → implemented note)

**Interfaces:**
- Consumes: everything above.
- Produces: the done-state.

- [ ] **Step 1: Full rebuild + full CA gate**

```bash
ninja -C build > build/ninja_final.log 2>&1
build/src/unit_tests_dbms --gtest_filter='Cas*:CA*' > build/test_final_gate.log 2>&1; echo "exit=$?"
```
Expected: exit 0. Have a subagent summarize both logs; any red = stop and RCA (project rule: no
known reds).

- [ ] **Step 2: The CI lane check (the lane that caught the crash)**

Run the CAS-s3 stateless smoke locally if feasible (`python3 -m ci.praktika run` with the
`content_addressed s3 storage` job name per `reference_praktika_local_runs`), or explicitly record
in the PR/backlog that the lane validation rides the next CI push of `cas-gc-rebuild`. Do not
claim lane-green without a run.

- [ ] **Step 3: Update the spec Status line to `IMPLEMENTED (Phases A-C + TLA gate)` with the commit
hashes; update the BACKLOG §8 `[STID-3982-3b48 part 2]` entry to point at the commits; commit both
docs (docs-only commit).**

```bash
git add docs/superpowers/specs/2026-07-24-cas-mount-lease-self-race-fix-v2-design.md docs/superpowers/cas/BACKLOG.md
git commit -m "ca: mount-lease fence-not-rescue — spec/backlog status after implementation

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

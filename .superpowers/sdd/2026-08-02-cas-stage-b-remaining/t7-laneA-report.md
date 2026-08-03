# T7 lane A report

## Setup

- Worktree: `/home/mfilimonov/workspace/ClickHouse/lane-g`, branch `laneg/t7` off `f8df7d9a5e8`.
- Model-tool preflight: `tmp/tla2tools-official.jar` SHA-256
  `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3` confirmed; `tmp/tla2tools.jar`
  symlinked and re-verified to the same digest.

## Blocker found before Step A2

The dispatch stated `docs/superpowers/models/run_gc_partmanifest.sh` carries an unstaged +189/-12
rewrite in this worktree, to be used as Step A2's input. That file does not exist in `lane-g`: its
committed content there is byte-identical between `f8df7d9a5e8` and the prior `laneg/t6a` tip
(`477fe702a7a`), 13 lines, no local diff. The +189/-12 rewrite instead sits uncommitted in the
**`/home/mfilimonov/workspace/ClickHouse/master`** worktree — confirmed there via `git diff --stat`
(exactly "1 file changed, 189 insertions(+), 12 deletions(-)") and by reading the file: it is a
complete, working runner (43 `CONFIGS` rows + `SLOW_CONFIGS`, `tlc_temporal_gate.sh` sourcing, the
`UnchangedCompositeVars` model-error classifier, `PROPERTY`-based temporal checks). Since git
worktrees don't share uncommitted working-tree state, this could not have propagated into `lane-g`
on its own — the dispatch's premise about which worktree holds the edit is wrong.

Sent a question to `team-lead` (msg id `78cc445d-5fe0-4958-a789-6ab9b508d120`) asking whether
that master-worktree content is mine to copy into `lane-g` and commit there, or whether another
session is actively iterating on it — before touching another session's uncommitted work. Proceeded
with Step A1 in parallel since it does not depend on this file.

`team-lead` confirmed the master-worktree content is a preserved leftover from a previous session
(predates this campaign), nobody is iterating on it, and directed copying it into `lane-g`, running
the battery there, and leaving master's uncommitted copy for later reconciliation at integration
time. Copied `master`'s working copy of `run_gc_partmanifest.sh` into `lane-g` verbatim (confirmed
`git diff --stat` still reports exactly "189 insertions(+), 12 deletions(-)" after the copy), then
edited it to drop the four `CONFIGS` rows for the configs Step A1 retired
(`sab_skipchangedshard`, `sab_skipparksdeadprecommit`, `fix_skipparksdeadprecommit`,
`stage5_tokendiff`) — the rest of Step A2 below.

## Step A1 — the 10a verdict: RETIRED

Audited `listedTok`/`CanSkipShard`/`GDiscoverSkip` in `CaGcRootLocalPartManifestCore.tla`. Searched
`src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Gc/` and the CAS backend for any
production seam that skips a manifest/owner-transition fold read on a LIST-derived token match.
Found none:

- `supportsListTokens()`/`tokenForList` (`CasObjectStorageBackend.h/.cpp`) surface a LIST-derived
  incarnation token only for the blob-condemn exact-delete-vs-`HEAD`-first decision on an
  already-condemned object — unrelated to skipping a fold read.
- The closest structural analog, the landed ref-log intake fold in `CasGc.cpp` (the frozen-tail
  walk classifying `tail > cursor` / `tail == cursor` / `tail < cursor`), explicitly keeps the
  exact-key `GET` at `cursor + 1` **unconditional** even when `tail == cursor` (this model's
  `listedTok[n] = foldedTok[n]`), because that read is the only thing that ever sets
  `frontier_proven` — the comment there states in-code that skipping it "is not a cheaper version
  of this design; it is a GC that permanently reclaims nothing."

Verdict: the premise DIED. It's not fixable by tightening the guard — the landed design's answer to
an apparently-unchanged shard is "read it anyway, because the read is the proof," a different shape
of solution, not a stricter version of the listing-gated skip.

Action taken (retire, not rewrite):
- Removed the four configs that ever set `EnableTokenDiff = TRUE`: `stage5_tokendiff`,
  `sab_skipchangedshard`, `sab_skipparksdeadprecommit`, `fix_skipparksdeadprecommit`. No other
  config in the model sets that flag, so this is the complete retirement; the other 42 configs'
  state spaces are untouched.
- Added two comments in `CaGcRootLocalPartManifestCore.tla` (at the `CONSTANTS` block and at the
  Phase 2 action block) marking the arm retired/historical.
- Wrote the verdict into new `docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md`
  (this model had no `_RESULTS.md` in the current tree — a prior copy existed before the July 2
  `docs/superpowers` consolidation commit `3a054b9ffe6` deleted it; that older copy recorded
  model-internal soundness only, not a verdict against the landed C++ architecture, so it did not
  pre-empt this task).
- Sanity check: `stage0.cfg` re-run after the comment-only edits reproduces the exact
  `71184`/`19846` generated/distinct state counts, confirming the retirement touched nothing
  semantic.

Committed as `a19066a7893` on `laneg/t7`: "ca: tla — listedTok skip premise verdict for the
root-local part-manifest model" (6 files changed: `.tla` +11/-0 comment lines net, new `_RESULTS.md`,
4 deleted `.cfg` files).

## Step A2 — the ninth family: DONE

Ran the whole-suite battery (`SLOW=1`, `TLC_TIMEOUT` default 3600s) via nohup + end-marker against
the post-A1 config set: 39 fast rows + 5 slow rows = 44 total (down from the pre-verdict 43 fast +
5 slow = 48, after Step A1 retired the four `EnableTokenDiff = TRUE` configs). Verified before
running: all 44 referenced `.cfg` files exist, and the three temporal configs
(`sab_deletebodybeforedecrements`, `sab_noorphansweep`, `live`) all use singular `PROPERTY`, not
`PROPERTIES`.

First pass resolved 42 of 44 rows to their expectation immediately (`stage3`'s `incomplete`
expectation is itself a KNOWN timeout by design). Two `green`-expected rows hit the 3600s/3601s
default bound without a verdict: `stage5_lazytrim` and `live`. Per team-lead guidance, a TLC
timeout is a resource bound, not a model verdict, so neither was recorded as pass/fail/downgraded;
both were re-run standalone with `TLC_TIMEOUT=14400` (4h), `-workers 1` kept for determinism
comparability with the rest of the table:

- `live`: GREEN at 7536s (2h05m) — `74,147,107` generated / `17,845,340` distinct, reproducing the
  exact pre-consolidation historical count.
- `stage5_lazytrim`: still times out at the 4h bound (`14401`s) — `1,333,723,653` generated /
  `233,198,128` distinct, `24,395,446` still queued and draining only slowly. Recorded as
  UNPROVEN-BY-TIMEOUT, a second named model debt, not downgraded to green.

Wrote the full per-row table, checker identity (`TLC2 Version 2026.07.18.145032`, rev `30cc360`,
jar SHA-256 `cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3`), the extended-bound
rerun section, and both named model debts (Phase-4 sharding UNPROVEN + `stage5_lazytrim`
UNPROVEN-BY-TIMEOUT) into `CaGcRootLocalPartManifestCore_RESULTS.md`.

Committed as `<see final message>` on `laneg/t7`: "ca: tla — gc_partmanifest whole-suite runner,
ninth family asserted".

## Files touched (both steps)

- `/home/mfilimonov/workspace/ClickHouse/lane-g/docs/superpowers/models/CaGcRootLocalPartManifestCore.tla`
- `/home/mfilimonov/workspace/ClickHouse/lane-g/docs/superpowers/models/CaGcRootLocalPartManifestCore_RESULTS.md` (new)
- `/home/mfilimonov/workspace/ClickHouse/lane-g/docs/superpowers/models/run_gc_partmanifest.sh`
  (copied from the master worktree's uncommitted rewrite, then edited to drop the four retired
  configs' rows)
- Deleted: `CaGcRootLocalPartManifestCore_{stage5_tokendiff,sab_skipchangedshard,sab_skipparksdeadprecommit,fix_skipparksdeadprecommit}.cfg`
- `master`'s uncommitted copy of `run_gc_partmanifest.sh` was left untouched, per team-lead
  instruction — they will reconcile it at integration time once this commit merges.

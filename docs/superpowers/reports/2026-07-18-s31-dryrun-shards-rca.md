# S31 `ca-gc-dryrun` "shard coverage" FAIL — RCA (2026-07-18)

Run: `utils/ca-soak/scenarios/runs/20260718T001753_S31_seed1/` — check
"ca-gc-dryrun completeness under gc_shards>1" FAIL: "dryrun previewed 72; GC reclaimed ~406".

## Known or new
KNOWN and RECURRING (S31 failed identically 2026-07-02 previewed 0/reclaimed 40; 2026-07-13
23/78; 2026-07-18 72/406 — RUN_HISTORY.md rows). BUT the diagnosis carried in the card, the
`note_anomaly` text, and the BACKLOG narrative ("previewDeletes previews only target shard 0")
is STALE/WRONG: `utils/ca-soak/scenarios/BACKLOG.md:6` and `:1141` already record it as fixed.

## Exact cause
NOT a tool bug. `Gc::previewDeletes` (`src/Disks/.../ContentAddressed/Gc/CasGc.cpp:2173`) already
iterates every shard — `for (uint64_t shard = 0; shard < state.gc_shards; ++shard)` at
`CasGc.cpp:2196`, with the comment at `:2194` explicitly rejecting a shard-0-only preview. Proof
it covers both shards: `dryrun_preview_count == 72` exactly equals post-drop
`fsck.pending_gc == 72` (report.json), which is the total condemned set across BOTH shards.

The real cause is the CARD ORACLE (`utils/ca-soak/scenarios/cards/s28_s33_corner.py:590-616`):
it asserts `dry_count >= gc_deleted_observable`, where `dry_count` (72) is a SINGLE-ROUND,
point-in-time preview (zero-in-degree + `kCondemned` rows in the currently-adopted fold seal),
but `gc_deleted_observable` (`deleted_total`, `s28_s33_corner.py:578`) is the CUMULATIVE total
deleted across ALL forced-GC rounds to fixpoint (~406). Right after DROP most blobs are still
`unreachable=445`/`awaiting_gc=127` and not yet condemned in the adopted seal, so a single preview
cannot see them; they are condemned in later folds and deleted over ~2-3 rounds. `72 < 406` is
therefore EXPECTED and CORRECT — apples (one-round preview) vs oranges (multi-round reclaim).
Same class as the RESOLVED `F3-single-leader-dryrun-overproposal` (BACKLOG.md:1122): over-strict
oracle, not a CA/tool defect.

## Fix shape
- Tool: NONE. `previewDeletes` already covers all shards (CasGc.cpp:2196).
- Card: replace the unsound `dry_count >= deleted_total` contract. previewDeletes is a next-round
  preview, so the sound completeness check compares it to the SAME-INSTANT fsck classes
  (`pending_gc` [+ delete-pending]) captured right after the dryrun — NOT to the cumulative
  multi-round `deleted_total`. The subset contract already run in `standard_end`
  (`dryrun ⊆ unreachable ∪ pending-gc ∪ awaiting-gc`) passes and is the correct guard; the
  bespoke count comparison should be dropped or rebased onto the instantaneous fsck snapshot.
  Delete the stale "target shard 0 / checklist #9" narrative from the card, the anomaly text,
  and BACKLOG entries `[D3 / S31]` (`docs/superpowers/cas/BACKLOG.md:63`).

## Small-patch-safe verdict
SAFE small patch. Test-only (soak card oracle); no product code changes. Does NOT need the
product fix-wave pipeline. (Note: BACKLOG `[F3]` at cas/BACKLOG.md:62 — dryrun reachability
under-count — is a *separate* item and stays; it is not what fired here.)

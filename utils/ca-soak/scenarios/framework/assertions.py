"""Common hard assertions (README §"Common hard assertions").

Each helper appends one or more `Verdict`s to the `ScenarioResult` and returns them. Missing
observability data yields an explicit `inconclusive` verdict with a reason — never a silent pass.

These apply to every positive scenario unless it declares a stricter or negative rule:
  - Storage correctness: fsck `dangling == 0`.
  - GC safety: dry-run delete candidates ⊆ fsck unreachable set at quiescence.
  - Event audit: no `read_missing`/`dangling_access`/`corrupt_dangle`/`corrupt_decode`/
    `snap_journal_incoherent`/`exception` rows (unless a negative test expects the exception).
  - GC rounds: no `Failed` finish rows (`NotALeader` is expected on non-leaders).
  - No unbounded leftovers: after forced GC, `unreachable == 0` for non-abandoning scenarios; a
    nonzero residual must be classified.
"""

from . import observe
from .report import Verdict


def assert_fsck_clean(result, fsck: dict):
    """fsck dangling == 0. A missing/timed-out fsck is inconclusive."""
    if not fsck or "dangling" not in fsck:
        return [result.add(Verdict.inconclusive(
            "fsck dangling", "0", "fsck summary unavailable (timeout or parse failure)"))]
    dangling = fsck.get("dangling")
    # A PARTIAL scan (deadline hit, `--partial`) is a lower bound: dangling>0 is a real finding,
    # but dangling==0 proves nothing about the unwalked remainder — never let a partial clean pass.
    if fsck.get("partial") and dangling == 0:
        return [result.add(Verdict.inconclusive(
            "fsck dangling", "0",
            f"fsck partial (deadline): walked subset clean, remainder unproven ({fsck.get('reason', '')})"))]
    v = result.add(Verdict.check("fsck dangling", "0", dangling, dangling == 0))
    result.observations["fsck_final"] = {
        k: fsck.get(k) for k in ("reachable", "unreachable", "dangling", "physical_bytes",
                                 "referenced_logical_bytes", "distinct_blobs", "total_blob_refs",
                                 "dedup_ratio") if k in fsck}
    return [v]


# fsck object classes that a `ca-gc-dryrun` candidate may legitimately fall into. `ca-gc-dryrun`
# previews the NEXT GC round's deletes, which is the union of:
#   - `unreachable`  — orphan objects GC has not condemned yet;
#   - `pending-gc` / `awaiting-gc` — objects already CONDEMNED, sitting in the two-phase graduation
#     pipeline (zero in-degree, awaiting the min-ack rounds before physical delete).
# fsck splits "not reachable" into those sub-classes; the preview correctly targets all of them. The
# earlier oracle accepted only `unreachable`, so a run left with a bounded condemned residual (any
# create/insert/DROP scenario at the fixpoint — the last drop's blobs are condemned but not yet
# graduated) FALSELY failed with "dryrun ⊄ unreachable" while every candidate was actually `pending-gc`
# (verified 2026-07-07: minimal DROP repro → 7 candidates, all classified `pending-gc`, 0 reachable).
# A candidate classified `reachable` (or `dangling`/`unaccounted`) IS a real over-proposal and still
# fails — that is the genuine defect this oracle exists to catch.
_DRYRUN_DELETABLE_CLASSES = {"unreachable", "pending-gc", "awaiting-gc"}


def assert_dryrun_subset(result, fsck_detail_res: dict, dryrun_res: dict):
    """Dry-run delete candidates ⊆ fsck DELETABLE key set (unreachable ∪ pending-gc ∪ awaiting-gc).
    Requires a detailed fsck; if detail is unavailable the assertion is inconclusive."""
    if not fsck_detail_res or "detail" not in fsck_detail_res:
        return [result.add(Verdict.inconclusive(
            "dryrun ⊆ deletable (unreachable ∪ pending-gc)", "subset", "detailed fsck unavailable"))]
    if not dryrun_res or "entries" not in dryrun_res:
        return [result.add(Verdict.inconclusive(
            "dryrun ⊆ deletable (unreachable ∪ pending-gc)", "subset", "dry-run output unavailable"))]
    deletable_keys = {r["key"] for r in fsck_detail_res["detail"]
                      if r.get("class") in _DRYRUN_DELETABLE_CLASSES}
    candidate_keys = {e["key"] for e in dryrun_res["entries"]}
    leaked = sorted(candidate_keys - deletable_keys)
    ok = not leaked
    # Classify the leaked candidates so the note distinguishes a real over-proposal (a candidate fsck
    # calls `reachable`) from a harness/fsck-detail gap (candidate absent from the detail listing).
    by_key = {r["key"]: r.get("class") for r in fsck_detail_res["detail"]}
    leaked_classes = {k: by_key.get(k, "NOT-IN-FSCK") for k in leaked}
    note = "" if ok else f"{len(leaked)} candidate(s) not deletable, classes={sorted(set(leaked_classes.values()))}, e.g. {leaked[:3]}"
    v = result.add(Verdict.check("dryrun ⊆ deletable (unreachable ∪ pending-gc)", "subset",
                                 f"{len(candidate_keys)} candidates / {len(deletable_keys)} deletable",
                                 ok, note))
    if leaked:
        result.note_anomaly(f"GC dry-run proposed deleting {len(leaked)} key(s) NOT in the deletable "
                            f"set (classes={leaked_classes}): {leaked[:10]}")
    return [v]


def assert_event_audit(result, ca_events: dict, expect_exception: bool = False):
    """No bad CA-log event types. For a negative scenario (`expect_exception=True`) a single
    `exception` row is allowed and the assertion instead requires the OTHER bad types stay zero."""
    bad = dict(ca_events.get("bad_total", {}))
    if expect_exception:
        bad.pop("exception", None)
    ok = not bad
    note = "" if ok else f"bad events: {bad}"
    v = result.add(Verdict.check(
        "event audit (no bad rows)", "0 bad-type rows" + (" (exception allowed)" if expect_exception else ""),
        bad if bad else 0, ok, note))
    if bad:
        result.note_anomaly(f"CA event log contains bad-type rows: {bad}")
    return [v]


def assert_gc_no_failed(result, gc_summary: dict):
    """No REAL Error GC finish rows. Benign concurrency-retry aborts (a deposed fold/fence loser that
    cleanly retries — classified by `observe._gc_error_is_benign`) are EXPECTED under more than one GC
    leader and are NOT failures. Only real errors fail here — notably the in-degree `merged ... < 0`
    undercount CORRUPTED_DATA, which is still counted in `failed`. The concurrent-leader RECLAIM
    property (does the residual actually drain?) is asserted separately by the residual-drain checks,
    not by counting Error rows."""
    if not gc_summary:
        return [result.add(Verdict.inconclusive("GC no Failed rounds", "0", "GC log unavailable"))]
    failed = gc_summary.get("failed", 0)
    benign = gc_summary.get("failed_benign", 0)
    observed = str(failed) + (f" (+{benign} benign concurrency-retry)" if benign else "")
    v = result.add(Verdict.check("GC no Failed rounds", "0", observed, failed == 0))
    result.observations["gc_summary"] = gc_summary
    if failed:
        result.note_anomaly(f"GC log has {failed} real (non-benign) Error finish row(s)")
    return [v]


# Object prefixes that GC is responsible for reclaiming. If any of these remain UNREACHABLE after a
# forced GC fixpoint, that is a genuine content/manifest leftover (a real finding). Everything else
# (GC state under gc/, root shard objects + _watermark, _pool_meta, verbatim _files) is bookkeeping
# that legitimately persists — notably because namespace registration is monotone (a dropped table
# clears its refs/files but leaves its namespace's root objects registered; README §"surprise
# checklist"), so a small bounded residual of root bookkeeping after dropping tables is expected.
RECLAIMABLE_UNREACHABLE_PREFIXES = ("blobs", "_manifests")


def _classify_key(key: str) -> str:
    """Bucket a pool key by prefix — delegates to the shared layout-aware classifier
    (`observe.classify_pool_path`); see its docstring for the 2026-07 relocation rationale."""
    return observe.classify_pool_path(key)


def classify_unreachable(fsck_detail_res: dict) -> dict:
    """Bucket the fsck `unreachable` detail rows by object prefix."""
    buckets: dict = {}
    for r in (fsck_detail_res or {}).get("detail", []):
        if r.get("class") == "unreachable":
            b = _classify_key(r.get("key", ""))
            buckets[b] = buckets.get(b, 0) + 1
    return buckets


def assert_no_leftovers(result, fsck: dict, abandons: bool = False, residual_after_gc=None,
                        fsck_detail_res: dict = None):
    """After forced GC, no reclaimable content (`blobs/`, `_manifests/`) remains unreachable.

    The raw `unreachable` count is NOT required to be 0: GC state (`gc/`), root shard objects +
    `_watermark`, `_pool_meta`, and verbatim `_files` legitimately persist (and a dropped table leaves
    its namespace registered — monotone registry). So we CLASSIFY the residual by prefix: a residual
    composed only of bookkeeping is `pass` (bounded+classified); any unreachable `blobs/`/`_manifests/`
    object is a `fail` (a real content/manifest leak). For an abandoning scenario even the content
    check is relaxed to a recorded+classified residual (the scenario's own bound assertion governs)."""
    val = residual_after_gc if residual_after_gc is not None else (fsck or {}).get("unreachable")
    if val is None:
        return [result.add(Verdict.inconclusive("no unbounded leftovers", "unreachable==0",
                                                "unreachable count unavailable"))]
    buckets = classify_unreachable(fsck_detail_res) if fsck_detail_res else {}
    result.observations["unreachable_classification"] = {"residual_count": val, "by_prefix": buckets}
    reclaimable = sum(buckets.get(p, 0) for p in RECLAIMABLE_UNREACHABLE_PREFIXES)
    bookkeeping = {k: v for k, v in buckets.items() if k not in RECLAIMABLE_UNREACHABLE_PREFIXES}

    if val == 0:
        return [result.add(Verdict.check("no unbounded leftovers", "no unreachable content", 0, True))]

    if not buckets:
        # We have a nonzero count but no detail to classify it — cannot prove it is bookkeeping.
        return [result.add(Verdict.inconclusive(
            "no unbounded leftovers", "no unreachable content/manifests",
            f"residual={val} but fsck detail unavailable to classify by prefix"))]

    if abandons:
        v = result.add(Verdict("leftovers (abandoning scenario)", "bounded+classified",
                               f"residual={val} by_prefix={buckets}", "pass",
                               "abandoning scenario — see scenario-specific bound assertion"))
        return [v]

    if reclaimable > 0:
        v = result.add(Verdict.check(
            "no unbounded leftovers", "no unreachable blobs/manifests after forced GC",
            f"{reclaimable} reclaimable (by_prefix={buckets})", False,
            "unreachable content/manifest objects remain — possible GC leak"))
        result.note_anomaly(
            f"forced GC left {reclaimable} unreachable RECLAIMABLE object(s) (blobs/_manifests) — "
            f"possible leak; full residual by prefix: {buckets}. If explicit GC was driven concurrently "
            f"with background GC (or on both replicas), this is likely the known GC-CONCURRENT-LEADER-LEAK "
            f"(see BACKLOG): a divergent-fold abort orphans owner-removal events permanently.")
        return [v]

    # Residual is entirely bookkeeping — expected and bounded.
    v = result.add(Verdict(
        "no unbounded leftovers", "residual is GC/namespace bookkeeping only",
        f"residual={val} (bookkeeping: {bookkeeping})", "pass",
        "no reclaimable content unreachable; residual is GC state / monotone-registry root objects"))
    return [v]


def assert_reclaimable_drained(result, verdict_name, residual, fsck_detail_res: dict = None):
    """Assert that RECLAIMABLE content (blobs/, _manifests/) drained to 0 after forced GC.

    This is the B1/B2-correct drain-to-zero check:
      - B1 (convergence): callers MUST pass `residual` from the CONVERGED end-checkpoint
        (`gc_residual_unreachable` from `checkpoint.end_checkpoint`), NOT a mid-run
        `forced_gc_to_fixpoint` snapshot. Mid-run snapshots can be transiently >0 under concurrent
        GC leaders while the pool is still converging.
      - B2 (prefix-aware): only RECLAIMABLE prefixes (`blobs`, `_manifests`) are required to be 0.
        The "other" class (namespace registry / root-shard / GC state objects) legitimately persists
        after `dropNamespace` (monotone registry — checklist #6, BACKLOG S30) and is NOT asserted
        to be 0; it is recorded as an observation only.

    Returns the list of `Verdict`s added (one). On a bookkeeping-only residual the verdict is `pass`
    with a note. On a nonzero reclaimable residual the verdict is `fail` with the classified counts.
    """
    if residual is None:
        return [result.add(Verdict.inconclusive(
            verdict_name, "reclaimable unreachable == 0 (blobs/_manifests)",
            "residual unavailable (end-checkpoint forced-GC did not return a count)"))]

    buckets = classify_unreachable(fsck_detail_res) if fsck_detail_res else {}
    reclaimable = sum(buckets.get(p, 0) for p in RECLAIMABLE_UNREACHABLE_PREFIXES)
    bookkeeping = {k: v for k, v in buckets.items() if k not in RECLAIMABLE_UNREACHABLE_PREFIXES}

    # Always record the full breakdown as an observation so the report carries the context.
    result.observations.setdefault("reclaimable_drain_check", {})[verdict_name] = {
        "residual_total": residual, "reclaimable": reclaimable,
        "by_prefix": buckets, "bookkeeping": bookkeeping}

    if residual == 0 or reclaimable == 0:
        note = ""
        if residual > 0 and bookkeeping:
            note = (f"total residual={residual} but all {residual} are bookkeeping "
                    f"(monotone namespace registry / GC state — expected); by_prefix={buckets}")
        return [result.add(Verdict.check(
            verdict_name, "reclaimable unreachable == 0 (blobs/_manifests)",
            reclaimable, True, note))]

    # reclaimable > 0: real content/manifest leak.
    if not buckets:
        # Nonzero residual but no fsck detail to classify — cannot prove it is bookkeeping.
        return [result.add(Verdict.inconclusive(
            verdict_name, "reclaimable unreachable == 0 (blobs/_manifests)",
            f"residual={residual} but no fsck detail available to classify by prefix — "
            "rerun with a detailed fsck to determine whether this is bookkeeping or a real leak"))]

    v = result.add(Verdict.check(
        verdict_name, "reclaimable unreachable == 0 (blobs/_manifests)",
        f"{reclaimable} reclaimable (by_prefix={buckets})", False,
        f"unreachable blobs/_manifests remain after converged forced GC — possible GC leak; "
        f"bookkeeping (other) residual={sum(bookkeeping.values())} is expected/bounded"))
    result.note_anomaly(
        f"forced GC left {reclaimable} unreachable RECLAIMABLE object(s) (blobs/_manifests) — "
        f"possible GC leak; full residual by prefix: {buckets}. "
        f"bookkeeping-only residual (other={sum(bookkeeping.values())}) is expected and bounded.")
    return [v]


def run_common_assertions(result, *, fsck_final, fsck_detail_res, dryrun_res, ca_events,
                          gc_summary, abandons=False, expect_exception=False,
                          residual_after_gc=None):
    """Run all common positive-scenario assertions in one call. Returns the list of verdicts added."""
    out = []
    out += assert_fsck_clean(result, fsck_final)
    out += assert_dryrun_subset(result, fsck_detail_res, dryrun_res)
    out += assert_event_audit(result, ca_events, expect_exception=expect_exception)
    out += assert_gc_no_failed(result, gc_summary)
    out += assert_no_leftovers(result, fsck_final, abandons=abandons,
                               residual_after_gc=residual_after_gc, fsck_detail_res=fsck_detail_res)
    return out

"""Task 15: replay tooling + reproducibility.

A soak failure is only useful if it can be replayed deterministically. `dump_failure` produces a
STABLE, fully-specified reproducer dict — every key is always present and the value is a pure
function of the inputs — and writes it to `failure.json`. Given the recorded `seed`, `base_time`,
`op_id` (and `chaos_seed` for the chaos phases) a developer can re-drive the run with
`--until-op <op_id>` to land just before the failing op and inspect the divergence.

The dict is deliberately self-contained: it carries the model's expected aggregates, BOTH replicas'
observed aggregates, the last op, the last fault (phases 2/3), and the fsck verdict. It is pure (no
I/O) except for the final `json.dump`, so it is unit-testable without docker and its determinism can
be asserted directly.
"""

import json

# Replay schema version. Bump when the failure.json shape changes so a stale dump is recognizable.
REPLAY_SCHEMA_VERSION = 1

# Every key that MUST be present in a dump, so a reproducer is fully specified and a missing field is
# a bug rather than a silently-absent default. Asserted by the unit test.
REQUIRED_KEYS = (
    "schema_version",
    "seed",
    "chaos_seed",
    "base_time",
    "op_id",
    "phase",
    "until_op",
    "error",
    "last_fault",
    "last_op",
    "model_expected",
    "nodes",
    "fsck",
    "fsck_status",
)


def _normalize_node_aggs(node_aggs):
    """Accept either a mapping {label: aggs} or a (node1, node2) pair and return a deterministically
    ordered dict {label: aggs-or-None}. A pair is labelled node1/node2 (the soak has exactly two
    replicas); a mapping is copied with its keys sorted so the dump byte-order is stable."""
    if node_aggs is None:
        return {"node1": None, "node2": None}
    if isinstance(node_aggs, dict):
        return {k: node_aggs[k] for k in sorted(node_aggs)}
    # A sequence of per-node aggregate dicts (or Nones) -> label by replica index.
    seq = list(node_aggs)
    return {f"node{i + 1}": seq[i] for i in range(len(seq))}


def _strip_fsck(fsck):
    """Drop the bulky raw `stdout`/`detail` blobs from an fsck result so the dump stays compact and
    its key set is stable. Returns an empty dict for a missing fsck (the key is still present)."""
    if not fsck:
        return {}
    return {k: v for k, v in fsck.items() if k not in ("stdout",)}


def dump_failure(seed, base_time, op_id, phase, model_aggs, node_aggs, last_fault=None, *,
                 path=None, chaos_seed=None, until_op=None, error=None, last_op=None,
                 fsck=None, fsck_status="not-run"):
    """Build a STABLE, fully-specified reproducer dict and (if `path` is given) write it to
    `failure.json`. Pure given its inputs: the returned dict is identical for identical arguments
    (the only side effect is the optional file write).

    Required positional inputs are the minimum needed to replay: `seed` + `base_time` + `op_id`
    reproduce the deterministic ledger and time base; `phase` selects the driver. `model_aggs` is the
    model's expected aggregates at failure; `node_aggs` is the observed per-replica aggregates (a
    {label: aggs} mapping or a (node1, node2) pair). `last_fault` is the most recent injected fault
    (phases 2/3; None in phase 1).

    Keyword inputs enrich the report without changing replay: `chaos_seed` (chaos schedule),
    `until_op` (the cap to re-drive to), `error` (the failure kind+message), `last_op` (the op dict),
    `fsck` (the fsck verdict) and `fsck_status` (how that verdict was obtained — see
    `dump_failure`'s callers; e.g. "settled", "transient/unconfirmed", "skipped", "not-run").

    Every key in `REQUIRED_KEYS` is always present so a dump is never partially specified."""
    payload = {
        "schema_version": REPLAY_SCHEMA_VERSION,
        "seed": seed,
        "chaos_seed": chaos_seed,
        "base_time": base_time,
        "op_id": op_id,
        "phase": phase,
        # The op_id to re-drive to: explicit `until_op` if given, else the failing op_id (so a replay
        # stops just AT the failing op). None only when there is no op context at all.
        "until_op": until_op if until_op is not None else op_id,
        "error": error,
        "last_fault": last_fault,
        "last_op": last_op,
        "model_expected": model_aggs,
        "nodes": _normalize_node_aggs(node_aggs),
        "fsck": _strip_fsck(fsck),
        "fsck_status": fsck_status,
    }
    if path is not None:
        with open(path, "w") as fh:
            json.dump(payload, fh, indent=2, default=str, sort_keys=False)
    return payload

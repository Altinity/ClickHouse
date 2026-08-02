"""The P0 cards' fail-close probes and opaque-life injection seams.

Codex phase-A finding F4: both cards caught every exception from `events_snapshot` and skipped that
node, so a probe that failed on every node returned `{counter: 0}` and the "no always-zero counter
moved" verdict passed while nothing had been read. That is the same laundering the soak driver's own
`SignalTracker` exists to prevent, reproduced one layer up — and it is worse in a card, because a card's
verdict is the thing a human reads.

These tests pin the three cases apart: total probe failure, PARTIAL failure (one node answers, one
does not — the sneaky one, because the answering node supplies plausible numbers), and a node that
answers but omits a requested counter, which would otherwise read as zero.
"""

import inspect

import pytest

from scenarios.cards.s38_late_put_injection import (
    _VIOLATION_EVENTS,
    _discover_single_life_id,
    _injected_log_observation,
    _violation_counters,
)
from scenarios.cards.s43_same_uuid_recreation import S43


class _Node:
    """Answers `query` the way a server does: TSV, and — because the probe asks with
    `system_events_show_zero_values = 1` — including the counters that are still zero."""

    def __init__(self, name, values=None, raises=None):
        self.name = name
        self.values = values
        self.raises = raises

    def query(self, sql, **kw):
        if self.raises is not None:
            raise self.raises
        return "".join(f"{k}\t{v}\n" for k, v in self.values.items())

    def __repr__(self):
        return self.name


class _Cluster:
    def __init__(self, *nodes):
        self._nodes = nodes

    def nodes(self):
        return self._nodes


def _all_zero():
    return {e: 0 for e in _VIOLATION_EVENTS}


def test_a_clean_read_still_works():
    cl = _Cluster(_Node("n1", _all_zero()), _Node("n2", _all_zero()))
    assert _violation_counters(cl, _VIOLATION_EVENTS) == _all_zero()


def test_it_takes_the_peak_across_nodes():
    hot = dict(_all_zero(), CasRefApplyPoisoned=3)
    cl = _Cluster(_Node("n1", _all_zero()), _Node("n2", hot))
    assert _violation_counters(cl, _VIOLATION_EVENTS)["CasRefApplyPoisoned"] == 3


def test_total_probe_failure_raises_instead_of_reporting_zeros():
    cl = _Cluster(_Node("n1", raises=RuntimeError("boom")), _Node("n2", raises=RuntimeError("boom")))
    with pytest.raises(RuntimeError, match="counter probe FAILED"):
        _violation_counters(cl, _VIOLATION_EVENTS)


def test_partial_probe_failure_raises_too():
    """The dangerous one: n1 answers with plausible zeros, so a skip-on-error probe would return a
    complete-looking, entirely wrong result."""
    cl = _Cluster(_Node("n1", _all_zero()), _Node("n2", raises=RuntimeError("node down")))
    with pytest.raises(RuntimeError, match="counter probe FAILED"):
        _violation_counters(cl, _VIOLATION_EVENTS)


def test_a_missing_counter_is_not_a_zero():
    """With `system_events_show_zero_values = 1` the binary enumerates its whole registry, so a name
    still absent really is absent — a counter this build does not have, which must not read as zero.
    (Without that setting the check would be wrong in the other direction and would fail on any fresh
    cluster whose counters have not moved yet; that is how the first run of this card broke.)"""
    partial = _all_zero()
    partial.pop(_VIOLATION_EVENTS[0])
    cl = _Cluster(_Node("n1", partial))
    with pytest.raises(RuntimeError, match="did not return"):
        _violation_counters(cl, _VIOLATION_EVENTS)


class _S3Prefixes:
    def __init__(self, prefixes):
        self.prefixes = prefixes

    def list_objects_v2(self, **kwargs):
        assert kwargs == {
            "Bucket": "test",
            "Prefix": "soak_pool/cas/ns/stream/",
            "Delimiter": "/",
        }
        return {"CommonPrefixes": [{"Prefix": prefix} for prefix in self.prefixes]}


def test_single_table_life_discovery_accepts_one_direct_opaque_id():
    life_id = "0123456789abcdef0123456789abcdef"
    assert _discover_single_life_id(
        _S3Prefixes([f"soak_pool/cas/ns/stream/{life_id}/"])) == life_id


@pytest.mark.parametrize(
    "prefixes",
    [
        [],
        [
            "soak_pool/cas/ns/stream/0123456789abcdef0123456789abcdef/",
            "soak_pool/cas/ns/stream/fedcba9876543210fedcba9876543210/",
        ],
        ["soak_pool/cas/ns/stream/not-a-life-id/"],
        ["soak_pool/cas/ns/stream/0123456789ABCDEF0123456789ABCDEF/"],
        ["soak_pool/cas/ns/stream/0123456789abcdef0123456789abcdef/_log/"],
    ],
)
def test_single_table_life_discovery_refuses_ambiguous_or_noncanonical_children(prefixes):
    assert _discover_single_life_id(_S3Prefixes(prefixes)) is None


def test_injected_log_observation_records_opaque_life_without_deleted_namespace_local():
    observation = _injected_log_observation("0123456789abcdef0123456789abcdef", "key", "txn", b"body")
    assert observation == {
        "life_id": "0123456789abcdef0123456789abcdef",
        "key": "key",
        "txn_id": "txn",
        "body": "body",
    }


def test_s43_freezes_absence_then_forces_a_write_before_discovering_life2():
    source = inspect.getsource(S43.run)
    absence_verdict = source.index('"the recreated table does not absorb the previous life\'s state"')
    control_insert = source.index(f"INSERT INTO {{_TABLE}} VALUES")
    life2_discovery = source.index("life2_life_id = _discover_single_life_id(s3)")
    assert absence_verdict < control_insert < life2_discovery

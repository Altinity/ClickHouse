"""The P0 cards' counter probe must FAIL rather than report a clean zero it never read.

Codex phase-A finding F4: both cards caught every exception from `events_snapshot` and skipped that
node, so a probe that failed on every node returned `{counter: 0}` and the "no always-zero counter
moved" verdict passed while nothing had been read. That is the same laundering the soak driver's own
`SignalTracker` exists to prevent, reproduced one layer up — and it is worse in a card, because a card's
verdict is the thing a human reads.

These tests pin the three cases apart: total probe failure, PARTIAL failure (one node answers, one
does not — the sneaky one, because the answering node supplies plausible numbers), and a node that
answers but omits a requested counter, which would otherwise read as zero.
"""

import pytest

from scenarios.cards.s38_late_put_injection import _VIOLATION_EVENTS, _violation_counters


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

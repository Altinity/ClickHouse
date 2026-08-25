"""Unit tests for the CAS mount-fence reroute handling (FREEZE_LONG robustness).

A replica frozen past the mount-lease TTL is GC-fenced and returns an ABORTED (code 236) with a
"CAS mount lost / lease expired — refusing to mutate ref shard" body for the whole outage. The old
code retried this on the SAME node within the tiny `retry_on_aborted` budget (<1s), gave up, and the
outer transport-retry did not reroute an ABORTED — so an 83s FREEZE_LONG aborted the soak with a
WORKLOAD FAILURE. The fix: classify it (`is_mount_fenced`), skip same-node ABORTED retry for it, and
reroute it to the healthy peer via `retry_on_transport` (shared pool, own live lease).

Pure tests, no docker.
"""

import pytest

from soak.cluster import (
    QueryError,
    classify_retry_error,
    is_mount_fenced,
    is_node_down,
    retry_on_transport,
    retry_on_aborted,
)


def mount_fenced_error(node="Node(localhost:8123)"):
    body = ("Code: 236. DB::Exception: CAS mount lost / lease expired — refusing to mutate ref shard "
            "for server_root 'ca_soak_ch1'. (ABORTED) (version 26.6.1.1)")
    return QueryError(node, 500, body, "INSERT INTO ca_stress VALUES")


def lease_not_held_error(node="Node(localhost:8123)"):
    # The OTHER shape of the same condition: the disk-lifecycle gate refuses a read/write outright
    # instead of aborting a ref mutation. It carries NETWORK_ERROR (210) -- CAS mints its transient
    # refusals in the class upstream already treats as retry-later, so a lease blip is never read as
    # damage. The harness must recognise it here or a routine chaos-window fence becomes a WORKLOAD
    # FAILURE (it did once, and killed a 2.5h soak).
    body = ("Code: 210. DB::Exception: content-addressed disk 'ca' -- mount lease not held; backing may "
            "be temporarily unreachable; the operation is admitted again once the disk recovers to Live; "
            "TRANSIENT unavailability, not damage. (NETWORK_ERROR) (version 26.6.1.1)")
    return QueryError(node, 500, body, "INSERT INTO ca_stress VALUES")


def b137_aborted_error():
    # The B137 transient (resurrect-vs-GC race): an ABORTED WITHOUT the fence message — clears on the
    # same node in ms, so it keeps the same-node retry path.
    body = "Code: 236. DB::Exception: ... retry the operation. (ABORTED)"
    return QueryError("Node(x:1)", 500, body, "INSERT INTO t VALUES")


# --- classification ---------------------------------------------------------------------------

def test_mount_fence_is_aborted_and_fenced():
    e = mount_fenced_error()
    assert e.is_aborted is True
    assert e.is_mount_fenced is True
    assert is_mount_fenced(e) is True


def test_mount_fence_is_not_node_down():
    # It is an ABORTED, so it must NOT be classified as node-down (distinct recovery bookkeeping).
    e = mount_fenced_error()
    assert e.is_node_down is False
    assert is_node_down(e) is False


def test_lease_not_held_is_fenced_and_labelled_mount_fenced():
    e = lease_not_held_error()
    assert e.is_aborted is False
    assert e.is_mount_fenced is True
    assert is_mount_fenced(e) is True
    # NETWORK_ERROR is also a NODE_DOWN code, so the two classifiers overlap on this body. Harmless:
    # both prescribe reroute-to-the-peer, and the availability label is decided most-specific-first.
    assert classify_retry_error(e) == "mount_fenced"


def test_plain_network_error_is_not_mount_fenced():
    # Sharing the code is the price of the retryable class -- so the MESSAGE decides. A real socket
    # failure must not be booked as a mount fence.
    e = QueryError("Node(x:1)", 500, "Code: 210. DB::Exception: I/O error: Broken pipe. (NETWORK_ERROR)",
                   "INSERT INTO t VALUES")
    assert e.is_mount_fenced is False
    assert is_mount_fenced(e) is False


def test_b137_aborted_is_not_mount_fenced():
    # A plain retryable ABORTED (no fence message) must keep the same-node retry path.
    e = b137_aborted_error()
    assert e.is_aborted is True
    assert e.is_mount_fenced is False
    assert is_mount_fenced(e) is False


# --- retry_on_aborted: mount-fence is NOT retried on the same node ----------------------------

def test_retry_on_aborted_reraises_mount_fence_immediately():
    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        raise mount_fenced_error()

    with pytest.raises(QueryError) as ei:
        retry_on_aborted(fn, attempts=6, backoff_s=0.0)
    assert ei.value.is_mount_fenced
    assert calls["n"] == 1   # raised on the first try — no futile same-node hammering


def test_retry_on_aborted_still_retries_b137():
    calls = {"n": 0}

    def fn():
        calls["n"] += 1
        if calls["n"] < 3:
            raise b137_aborted_error()
        return "ok"

    assert retry_on_aborted(fn, attempts=6, backoff_s=0.0) == "ok"
    assert calls["n"] == 3   # the plain ABORTED still retries on the same node


# --- retry_on_transport: mount-fence REROUTES to the healthy peer -----------------------------

def test_mount_fence_reroutes_and_recovers():
    # Frozen ch1 keeps returning the fence ABORTED; the reroute alternates to ch2, which succeeds.
    seen_replicas = []

    def attempt(idx_box=[0]):
        i = idx_box[0]
        idx_box[0] += 1
        replica = i % 2
        seen_replicas.append(replica)
        if replica == 0:            # ch1 is frozen/fenced
            raise mount_fenced_error("Node(localhost:8123)")
        return "ok"                 # ch2 (peer) services the write

    out = retry_on_transport(attempt, attempts=10, sleep_fn=lambda s: None)
    assert out == "ok"
    assert seen_replicas == [0, 1]  # tried fenced ch1, rerouted to ch2, done


def test_mount_fence_exhaustion_raises():
    # If BOTH replicas are fenced (e.g. both paused), it still fails loudly after the budget.
    def attempt():
        raise mount_fenced_error()

    with pytest.raises(QueryError):
        retry_on_transport(attempt, attempts=5, sleep_fn=lambda s: None)

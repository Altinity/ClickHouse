from soak.ledger import OpType
from soak.rowgen import row_for_rid, insert_rids, NBUCKETS, TS_WINDOW, BASE_TIME

MASK64 = (1 << 64) - 1

class Model:
    """Authoritative per-rid model. Mirrors the SQL workload op-for-op so a quiesced checkpoint can be
    asserted exactly. row_fp is immutable identity; v/version are mutable counters bumped by update."""
    def __init__(self, seed: int, base_time: int = BASE_TIME, ttl_seconds: int = 90 * 60, insert_block: int = 200):
        self.seed = seed
        self.base_time = base_time
        self.ttl_seconds = ttl_seconds
        self.insert_block = insert_block
        self.rows: dict[int, dict] = {}     # rid -> row dict (mutable v/version)

    def _pred_bucket(self, param: int) -> int:
        return param % NBUCKETS

    def apply(self, op):
        if op.type == OpType.INSERT:
            n = 1 + (op.param % self.insert_block)
            for rid in insert_rids(op.op_id, n):
                r = row_for_rid(self.seed, rid, self.base_time)
                # The model never reads `payload` (256 B/row); only the INSERT SQL emitter does, and
                # it recomputes it from row_for_rid independently. Drop it so the oracle holds only
                # the fields it actually uses — payload was the bulk of the per-row footprint.
                del r["payload"]
                self.rows[rid] = r
        elif op.type == OpType.UPDATE:
            b = self._pred_bucket(op.param)
            for r in self.rows.values():
                if r["bucket"] == b:
                    r["v"] += 1
                    r["version"] += 1
        elif op.type == OpType.DELETE:
            b = self._pred_bucket(op.param)
            self.rows = {rid: r for rid, r in self.rows.items() if r["bucket"] != b}
        elif op.type == OpType.TRUNCATE:
            self.rows.clear()
        elif op.type == OpType.DROP_PARTITION:
            self.rows.clear()   # single BASE_TIME day -> one partition -> drop == full clear (documented)
        elif op.type == OpType.OPTIMIZE:
            pass

    def _expired(self, r, now: int) -> bool:
        return r["ts"] + self.ttl_seconds <= now

    def prune_expired(self, now: int) -> int:
        """Evict rows the table has already TTL-deleted. `now` is monotonic wall-clock and a row's ts
        is fixed, so once `_expired` is true it stays true forever — pruning these rids can never
        change `live_rows`/`aggregates` at this or any later `now`. Without this the dict retained
        every inserted rid for the whole run and the driver OOM'd on a multi-hour soak. Returns the
        number of rids reclaimed. Call only when the workload is drained (e.g. at a checkpoint)."""
        expired = [rid for rid, r in self.rows.items() if self._expired(r, now)]
        for rid in expired:
            del self.rows[rid]
        return len(expired)

    def live_rows(self, now: int):
        return [r for r in self.rows.values() if not self._expired(r, now)]

    def ambiguous_band_nonempty(self, now: int, eps: int) -> bool:
        return any(abs((r["ts"] + self.ttl_seconds) - now) <= eps for r in self.rows.values())

    def aggregates(self, now: int) -> dict:
        live = self.live_rows(now)
        if not live:
            return {"count": 0, "sum_fp": 0, "uniq_keys": 0, "sum_v": 0, "sum_version": 0,
                    "min_op": None, "max_op": None}
        return {
            "count": len(live),
            "sum_fp": sum(r["row_fp"] for r in live) & MASK64,
            "uniq_keys": len({(r["bucket"], r["k"]) for r in live}),
            "sum_v": sum(r["v"] for r in live),
            "sum_version": sum(r["version"] for r in live),
            "min_op": min(r["op_id"] for r in live),
            "max_op": max(r["op_id"] for r in live),
        }

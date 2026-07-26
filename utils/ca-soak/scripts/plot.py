#!/usr/bin/env python3
"""Render the CA soak metrics curve from the Phase-3 sqlite sink (`soak.db`).

The KEY curve the 24h soak proves is REFERENCED (logical) vs PHYSICAL pool size over time, with the
fsck `unreachable` (M-F debris, B140) overlaid:

  * referenced/logical bytes  -- what the table LOGICALLY holds, summed across BOTH replicas'
    `system.parts` (bytes_on_disk). Over a shared CA pool this is the referenced footprint; the gap
    below `pool_bytes` is the dedup + not-yet-GC'd surface.
  * physical pool bytes        -- `pool_bytes`, the true on-RustFS object footprint (S3 LIST). May be
    None if the LIST probe was unavailable (curve still meaningful from the rest).
  * fsck unreachable           -- orphan blobs the incremental GC structurally cannot reclaim
    (B140 M-F debris); should stay BOUNDED, not grow without limit.
  * fsck dangling              -- referenced-but-missing objects; must be 0 (INV-NO-LOSS).

Renders a PNG with matplotlib if importable; otherwise DEGRADES to a TSV the user can plot with any
tool, and SAYS SO on stdout (no hard dependency on matplotlib).

Usage: plot.py <soak.db> [out.png]
"""

import sqlite3
import sys

# Import the canonical column list from the metrics module so the schema stays single-sourced.
sys.path.insert(0, __file__.rsplit("/scripts/", 1)[0])
from soak.metrics import _COLS  # noqa: E402


def load_series(db_path: str):
    """Read rows, aggregate per-timestamp ACROSS nodes (sum the per-node bytes/rows; pool_* are
    pool-global so take the max non-null). Returns a list of dicts ordered by ts, with a `t_rel`
    seconds-from-start column for plotting."""
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    # Select only the columns this particular db actually has: `_COLS` grows over time (the CAS signal
    # counters were added 2026-07-26) and the repository is full of older soak dbs that must stay
    # plottable. Every consumer below already treats a missing value as None.
    have = {r[1] for r in conn.execute("PRAGMA table_info(metrics)")}
    selected = [c for c in _COLS if c in have]
    cur = conn.execute(f"SELECT {', '.join(selected)} FROM metrics ORDER BY ts")
    raw = [dict(r) for r in cur.fetchall()]
    conn.close()

    by_ts = {}
    for r in raw:
        ts = r["ts"]
        agg = by_ts.setdefault(ts, {
            "ts": ts, "logical_bytes": 0, "table_rows": 0, "pool_objects": None,
            "pool_bytes": None, "fsck_reachable": None, "fsck_unreachable": None,
            "fsck_dangling": None, "parts_active": 0,
        })
        agg["logical_bytes"] += r.get("bytes_on_disk") or 0
        agg["table_rows"] += r.get("table_rows") or 0
        agg["parts_active"] += r.get("parts_active") or 0
        for k in ("pool_objects", "pool_bytes"):
            v = r.get(k)
            if v is not None:
                agg[k] = max(agg[k] or 0, v)        # pool-global: take the populated reading
        for k in ("fsck_reachable", "fsck_unreachable", "fsck_dangling"):
            v = r.get(k)
            if v is not None:
                agg[k] = v                          # carry the latest fsck reading at this ts
    series = sorted(by_ts.values(), key=lambda x: x["ts"])
    if series:
        t0 = series[0]["ts"]
        for s in series:
            s["t_rel"] = s["ts"] - t0
    return series


def write_tsv(series, out_path: str):
    cols = ["t_rel", "ts", "table_rows", "parts_active", "logical_bytes", "pool_objects",
            "pool_bytes", "fsck_reachable", "fsck_unreachable", "fsck_dangling"]
    with open(out_path, "w") as fh:
        fh.write("\t".join(cols) + "\n")
        for s in series:
            fh.write("\t".join("" if s.get(c) is None else str(s.get(c)) for c in cols) + "\n")


def render_png(series, out_path: str) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return False

    t = [s["t_rel"] for s in series]
    GB = 1024 ** 3
    logical_gb = [(s["logical_bytes"] or 0) / GB for s in series]
    pool_gb = [None if s["pool_bytes"] is None else s["pool_bytes"] / GB for s in series]
    unreachable = [s["fsck_unreachable"] for s in series]
    dangling = [s["fsck_dangling"] for s in series]

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 8), sharex=True)

    ax1.plot(t, logical_gb, label="referenced/logical bytes (sum system.parts)", color="tab:blue")
    pt = [(x, y) for x, y in zip(t, pool_gb) if y is not None]
    if pt:
        ax1.plot([x for x, _ in pt], [y for _, y in pt],
                 label="physical pool bytes (S3 LIST)", color="tab:red")
    ax1.set_ylabel("GB")
    ax1.set_title("CA soak: referenced (logical) vs physical pool size")
    ax1.legend(loc="upper left")
    ax1.grid(True, alpha=0.3)

    ut = [(x, y) for x, y in zip(t, unreachable) if y is not None]
    if ut:
        ax2.plot([x for x, _ in ut], [y for _, y in ut],
                 label="fsck unreachable (M-F debris, B140)", color="tab:orange")
    dt = [(x, y) for x, y in zip(t, dangling) if y is not None]
    if dt:
        ax2.plot([x for x, _ in dt], [y for _, y in dt],
                 label="fsck dangling (must be 0; INV-NO-LOSS)", color="tab:green")
    ax2.set_ylabel("objects")
    ax2.set_xlabel("seconds from start")
    ax2.set_title("fsck: unreachable (debris) and dangling (loss — must be 0)")
    ax2.legend(loc="upper left")
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_path, dpi=110)
    plt.close(fig)
    return True


def main(argv):
    if len(argv) < 2:
        print("usage: plot.py <soak.db> [out.png]", file=sys.stderr)
        return 2
    db_path = argv[1]
    out_png = argv[2] if len(argv) > 2 else "soak_curve.png"
    series = load_series(db_path)
    if not series:
        print(f"no metrics rows in {db_path} — nothing to plot", file=sys.stderr)
        return 1

    print(f"loaded {len(series)} timestamps from {db_path} "
          f"(t_rel 0..{series[-1]['t_rel']}s)")
    if render_png(series, out_png):
        print(f"PLOT: wrote PNG -> {out_png}")
    else:
        tsv = out_png.rsplit(".", 1)[0] + ".tsv"
        write_tsv(series, tsv)
        print(f"PLOT: matplotlib NOT available — degraded to TSV -> {tsv} "
              f"(plot columns t_rel vs logical_bytes/pool_bytes/fsck_unreachable with any tool)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

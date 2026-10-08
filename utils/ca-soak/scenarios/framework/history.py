"""Append-only maintainers for RUN_HISTORY.md and BACKLOG.md.

RUN_HISTORY.md records every attempted run (one table row) so the suite has a durable log of what was
run, when, against which binary, and with what verdict. BACKLOG.md collects findings, anomalies,
missing instrumentation, flaky/inconclusive cases, suspected bugs, and proposed fixes.

Both files live in `scenarios/` (committed). Run artifacts live under `scenarios/runs/<ts>/`
(gitignored). The append is idempotent on the header: the table/section header is written once.
"""

import time
from pathlib import Path

_THIS = Path(__file__).resolve()
SCENARIOS_DIR = _THIS.parents[1]
RUN_HISTORY = SCENARIOS_DIR / "RUN_HISTORY.md"
BACKLOG = SCENARIOS_DIR / "BACKLOG.md"

_HISTORY_HEADER = """# Scenario suite run history

Every attempted scenario run is appended here (newest at the bottom). `run_dir` is relative to
`scenarios/runs/`. Status is the scenario's overall verdict (`pass` / `fail` / `inconclusive` /
`error`). See the per-run `report.md` for detail.

| started (UTC) | scenario | seed | scale | duration | status | git sha | run_dir | note |
|---|---|---|---|---|---|---|---|---|
"""

_BACKLOG_HEADER = """# Scenario suite backlog

Findings, anomalies, missing instrumentation, flaky/inconclusive cases, suspected bugs, and proposed
fixes discovered while building and running the content-addressed scenario suite. Newest at the
bottom. Each entry: a short id/title, the run it came from, what was observed, and a proposed action.

"""


def _ensure(path: Path, header: str):
    if not path.exists():
        path.write_text(header)


def append_run_history(*, scenario, seed, scale, duration_s, status, git_sha, run_dir, note=""):
    _ensure(RUN_HISTORY, _HISTORY_HEADER)
    ts = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())
    dur = f"{duration_s}s"
    sha = (git_sha or "")[:12]
    note = (note or "").replace("\n", " ").replace("|", "/")
    row = f"| {ts} | {scenario} | {seed} | {scale} | {dur} | {status} | {sha} | {run_dir} | {note} |\n"
    with open(RUN_HISTORY, "a") as fh:
        fh.write(row)


def append_backlog(*, item_id, title, run_dir="", observed="", action="", severity="finding"):
    """Append a backlog entry. `severity` is one of finding/anomaly/missing-instrumentation/
    flaky/suspected-bug/proposed-fix (free text, for grep)."""
    _ensure(BACKLOG, _BACKLOG_HEADER)
    ts = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime())
    block = [f"## {item_id}: {title}", "",
             f"- **Logged (UTC):** {ts}",
             f"- **Severity:** {severity}"]
    if run_dir:
        block.append(f"- **Run:** {run_dir}")
    if observed:
        block.append(f"- **Observed:** {observed}")
    if action:
        block.append(f"- **Proposed action:** {action}")
    block.append("")
    with open(BACKLOG, "a") as fh:
        fh.write("\n".join(block) + "\n")

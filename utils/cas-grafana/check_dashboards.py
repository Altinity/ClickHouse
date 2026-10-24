#!/usr/bin/env python3
"""Check dashboards/*.json offline. Exit 1 on any problem."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from casgrafana import checks  # noqa: E402


def main() -> int:
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    known, async_prefixes = checks.load_known_names(root)
    boards = []
    for uid in ("cas-overview", "cas-drilldown"):
        with open(os.path.join(HERE, "dashboards", f"{uid}.json")) as f:
            boards.append(json.load(f))
    problems = checks.check_dashboards(boards, known, async_prefixes)
    for p in problems:
        print("PROBLEM:", p)
    print(f"{len(problems)} problems in {len(boards)} dashboards")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

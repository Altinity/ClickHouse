#!/usr/bin/env python3
"""Build dashboards/*.json from the Python definitions. Usage: build_dashboards.py [--skeleton]"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from casgrafana import panels as P  # noqa: E402

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboards")


def boards(skeleton: bool) -> list:
    if skeleton:
        return [P.dashboard("cas-overview", "CAS / Overview", "skeleton", [], tags=["cas"], links=[]),
                P.dashboard("cas-drilldown", "CAS / Drilldown", "skeleton", [], tags=["cas"], links=[])]
    from casgrafana import overview, drilldown  # noqa: E402
    return [overview.build(), drilldown.build()]


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    for d in boards("--skeleton" in sys.argv):
        path = os.path.join(OUT, f"{d['uid']}.json")
        with open(path, "w") as f:
            json.dump(d, f, indent=2, sort_keys=True)
            f.write("\n")
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

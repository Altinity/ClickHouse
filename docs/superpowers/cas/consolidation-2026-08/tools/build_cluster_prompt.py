#!/usr/bin/env python3
"""Build a chunk-clustering prompt from the template.

Usage: build_cluster_prompt.py CHUNK_ID IN_PATH OUT_PATH

Reads $WORKDIR/cluster-prompt-template.txt, fills in {CHUNK_ID}, {IN_PATH}
and {OUT_PATH} with plain str.replace, and prints the result to stdout.
"""
import sys
from pathlib import Path

def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    chunk_id, in_path, out_path = sys.argv[1:4]

    workdir = Path(__file__).resolve().parent.parent
    template = (workdir / "cluster-prompt-template.txt").read_text()

    prompt = (
        template
        .replace("{CHUNK_ID}", chunk_id)
        .replace("{IN_PATH}", in_path)
        .replace("{OUT_PATH}", out_path)
    )
    sys.stdout.write(prompt)

if __name__ == "__main__":
    main()

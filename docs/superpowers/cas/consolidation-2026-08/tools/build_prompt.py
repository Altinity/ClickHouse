#!/usr/bin/env python3
"""Build a map-phase extraction prompt from the template.

Usage: build_prompt.py BATCH_ID OUT_PATH [FILE_LIST_PATH]

Reads $WORKDIR/map-prompt-template.txt and $WORKDIR/batches.tsv (unless
FILE_LIST_PATH is given, in which case that file supplies the file list
instead — used by the single-file probe), fills in {BATCH_ID}, {OUT_PATH}
and {FILE_LIST} with plain str.replace (never sed/awk: the file list is
multi-line and a stream-editor substitution mangles it), and prints the
result to stdout.
"""
import sys
from pathlib import Path

def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    batch_id = sys.argv[1]
    out_path = sys.argv[2]
    file_list_override = sys.argv[3] if len(sys.argv) == 4 else None

    workdir = Path(__file__).resolve().parent.parent
    template = (workdir / "map-prompt-template.txt").read_text()

    if file_list_override is not None:
        files = [file_list_override]
    else:
        batches_tsv = (workdir / "batches.tsv").read_text().splitlines()
        files = [
            line.split("\t", 1)[1]
            for line in batches_tsv[1:]
            if line.split("\t", 1)[0] == batch_id
        ]
        if not files:
            print(f"error: no rows for batch {batch_id!r} in batches.tsv", file=sys.stderr)
            sys.exit(1)

    file_list = "\n".join(files)

    prompt = (
        template
        .replace("{BATCH_ID}", batch_id)
        .replace("{OUT_PATH}", out_path)
        .replace("{FILE_LIST}", file_list)
    )
    sys.stdout.write(prompt)

if __name__ == "__main__":
    main()

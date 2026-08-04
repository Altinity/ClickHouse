#!/usr/bin/env python3
"""Build a cross-chunk merge-detection prompt from the template.

Usage: build_merge_prompt.py TARGET_ID IN_PATH OUT_PATH
"""
import sys
from pathlib import Path

def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    target_id, in_path, out_path = sys.argv[1:4]

    workdir = Path(__file__).resolve().parent.parent
    template = (workdir / "cross-chunk-merge-prompt-template.txt").read_text()

    prompt = (
        template
        .replace("{TARGET_ID}", target_id)
        .replace("{IN_PATH}", in_path)
        .replace("{OUT_PATH}", out_path)
    )
    sys.stdout.write(prompt)

if __name__ == "__main__":
    main()

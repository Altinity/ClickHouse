#!/usr/bin/env python3
"""Style gate: frontmatter, anchors, volume, mermaid. Usage: check_page.py <file.md>..."""
import re, subprocess, sys, os

REQUIRED_FM = ["description", "sidebar_label", "sidebar_position", "slug", "title", "doc_type"]
rc = 0
for path in sys.argv[1:]:
    text = open(path).read()
    errs = []
    m = re.match(r'^---\n(.*?)\n---\n', text, re.S)
    if not m:
        errs.append("missing frontmatter")
    else:
        for k in REQUIRED_FM:
            if not re.search(rf'^{k}:', m.group(1), re.M):
                errs.append(f"frontmatter missing {k}")
        if "antalya/cas" in path and "slug: /antalya/cas" not in m.group(1).replace("'", ""):
            errs.append("slug must start with /antalya/cas")
    body = text[m.end():] if m else text
    in_code = False
    for i, line in enumerate(body.splitlines(), 1):
        if line.startswith("```"):
            in_code = not in_code
        if not in_code and re.match(r'^#{1,6} ', line) and not re.search(r'\{#[a-z0-9-]+\}$', line):
            errs.append(f"line {i}: heading without {{#anchor}}: {line[:60]}")
    n = len(text.splitlines())
    if n > 500:
        errs.append(f"HARD volume limit: {n} lines > 500")
    elif n > 300:
        print(f"WARN {path}: {n} lines > 300 soft limit")
    for block in re.findall(r'```mermaid\n(.*?)```', text, re.S):
        r = subprocess.run(["node", os.path.join(os.path.dirname(__file__), "validate_mermaid.mjs")],
                           input=block, capture_output=True, text=True)
        if r.returncode != 0:
            errs.append(f"mermaid parse failure: {r.stderr.strip()[:200]}")
    for e in errs:
        print(f"ERR {path}: {e}")
    rc |= bool(errs)
sys.exit(rc)

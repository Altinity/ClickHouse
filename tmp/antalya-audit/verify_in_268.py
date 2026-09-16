#!/usr/bin/env python3
"""
For each Altinity PR from antalya-26.6 (prs_26.6.json), check whether the functionality
it introduced is actually present in antalya-26.8's current source tree.

Method: for every file the PR touched, take the *added* lines from the original diff
(parent_mainline -> parent_feature) and check whether each significant added line is
still present (verbatim, whitespace-insensitive) in that file's current content on
antalya-26.8. This is a content heuristic, not commit ancestry -- antalya-26.8 was
independently forked/ported from the same antalya-26.5 base, so no merge commits or
matching SHAs are expected there.

Output per PR: coverage % of added lines found, per-file breakdown, and a status:
  full      >= 90% of significant added lines found
  partial   10-90%
  missing   < 10%
  n/a       version-bump PR (upstream stable-tag bump; not a feature, not comparable)
"""
import json
import re
import subprocess
import sys

HEAD = "origin/antalya-26.8"


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def git_show(ref, path):
    r = run(["git", "show", f"{ref}:{path}"])
    if r.returncode != 0:
        return None
    return r.stdout


_file_cache = {}


def get_268_content(path):
    if path not in _file_cache:
        _file_cache[path] = git_show(HEAD, path)
    return _file_cache[path]


def significant(line):
    s = line.strip()
    if len(s) < 12:
        return False
    if re.fullmatch(r"[{}()\[\];,]+", s):
        return False
    if s in ("public:", "private:", "protected:"):
        return False
    return True


def added_lines_for_file(p1, p2, path):
    r = run(["git", "diff", f"{p1}...{p2}", "--", path])
    if r.returncode != 0:
        return []
    lines = []
    for line in r.stdout.splitlines():
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("+"):
            content = line[1:]
            if significant(content):
                lines.append(content.strip())
    return lines


def file_status(path, added):
    if not added:
        return {"path": path, "found": 0, "total": 0, "status": "no-signal"}
    current = get_268_content(path)
    if current is None:
        return {"path": path, "found": 0, "total": len(added), "status": "path-not-found"}
    found = sum(1 for l in added if l in current)
    pct = found / len(added)
    status = "full" if pct >= 0.9 else ("partial" if pct >= 0.1 else "missing")
    return {"path": path, "found": found, "total": len(added), "status": status, "pct": pct}


def verify_pr(pr, changed_files):
    file_results = []
    total_added = 0
    total_found = 0
    for path in changed_files:
        added = added_lines_for_file(pr["parent_mainline"], pr["parent_feature"], path)
        fr = file_status(path, added)
        if fr["total"] > 0:
            total_added += fr["total"]
            total_found += fr["found"]
        file_results.append(fr)

    if total_added == 0:
        overall_status = "no-signal"
        coverage = None
    else:
        coverage = total_found / total_added
        overall_status = "full" if coverage >= 0.9 else ("partial" if coverage >= 0.1 else "missing")

    return {
        "pr_number": pr["pr_number"],
        "title": pr["title"],
        "branch": pr["branch"],
        "category": pr["category"],
        "html_url": pr["html_url"],
        "coverage": coverage,
        "status": overall_status,
        "num_files": len(changed_files),
        "total_added_lines": total_added,
        "total_found_lines": total_found,
        "files": [f for f in file_results if f.get("total", 0) > 0],
    }


def changed_files(p1, p2):
    r = run(["git", "diff", "--name-only", f"{p1}...{p2}"])
    return [f for f in r.stdout.splitlines() if f]


def main():
    prs = json.load(open("tmp/antalya-audit/prs_26.6.json"))
    results = []
    for i, pr in enumerate(prs, 1):
        is_bump = pr["branch"].startswith("bump/")
        print(f"[{i}/{len(prs)}] PR #{pr['pr_number']} ({pr['branch']})"
              f"{'  [bump, skipping content check]' if is_bump else ''}", file=sys.stderr)
        if is_bump:
            results.append({
                "pr_number": pr["pr_number"], "title": pr["title"], "branch": pr["branch"],
                "category": pr["category"], "html_url": pr["html_url"],
                "coverage": None, "status": "n/a", "num_files": pr["num_files_changed"],
                "total_added_lines": 0, "total_found_lines": 0, "files": [],
            })
            continue
        files = changed_files(pr["parent_mainline"], pr["parent_feature"])
        results.append(verify_pr(pr, files))

    json.dump(results, open("tmp/antalya-audit/verify_268.json", "w"), indent=2)

    print("\n" + "=" * 100)
    for status in ("missing", "partial", "full", "no-signal", "n/a"):
        items = [r for r in results if r["status"] == status]
        print(f"\n--- {status.upper()} ({len(items)}) ---")
        for r in items:
            cov = f"{r['coverage']*100:.0f}%" if r["coverage"] is not None else "-"
            print(f"  #{r['pr_number']:<5} [{r['category']:<9}] cov={cov:<5} "
                  f"{r['title']}  ({r['branch']})")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
List all Altinity-branch PRs merged into a given antalya-<version> branch (as the delta
from its immediate predecessor branch) and classify each as cicd / qa / dev / uncertain.

Classification method (cross-checked, per user decision):
  1. path-based guess: look at the files changed by the PR (diff between the two merge
     parents) and bucket each file into cicd / qa / dev / other by path prefix. Take the
     dominant bucket (dev wins over accompanying qa test files, cicd wins over accompanying
     qa test files; dev+cicd together, or only 'other'/docs files, is 'uncertain').
  2. label-based guess: look at the repo's 'cicd' / 'cicd-failure' GitHub labels on the PR.
  3. final = agree -> that category; label says cicd but path disagrees -> uncertain;
     no label signal -> trust the path-based guess (marked path-only).

Usage:
  python3 classify_prs.py --base antalya-26.5 --head antalya-26.6 [--repo Altinity/ClickHouse]
                           [--out prs_26.6.json]
"""
import argparse
import csv
import json
import subprocess
import sys


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def git(*args):
    return run(["git", *args]).strip()


# --- path -> category rules -------------------------------------------------

CICD_PREFIXES = (
    ".github/",
    "ci/",
    "docker/",
    "build_docker/",
    "packages/",
    "tests/ci/",
    "tests/ci_",
)

QA_PREFIXES = (
    "tests/queries/",
    "tests/integration/",
    "tests/performance/",
    "tests/stress/",
    "tests/sqllogic/",
    "tests/sqlstorm/",
    "tests/fuzz/",
    "tests/jepsen.clickhouse/",
    "tests/casa_del_dolor/",
    "tests/config/",
    "tests/lexer/",
    "tests/docker_scripts/",
    "tests/clickhouse-test",
    "tests/broken_tests.yaml",
    "tests/analyzer_tech_debt.txt",
    "tests/async_insert_blacklist.txt",
    "tests/parallel_replicas_blacklist.txt",
    "tests/tsan_ignorelist.txt",
    "tests/ubsan_ignorelist.txt",
)

DEV_PREFIXES = (
    "src/",
    "base/",
    "programs/",
    "rust/",
    "cmake/",
    "utils/",
    "contrib/",
    "benchmark/",
    "CMakeLists.txt",
    ".gitmodules",
)


def classify_path(path):
    if path.startswith(CICD_PREFIXES):
        return "cicd"
    if path.startswith(QA_PREFIXES):
        return "qa"
    if path.startswith(DEV_PREFIXES):
        return "dev"
    return "other"


def path_based_guess(files):
    buckets = {"cicd": 0, "qa": 0, "dev": 0, "other": 0}
    for f in files:
        buckets[classify_path(f)] += 1

    has_dev = buckets["dev"] > 0
    has_cicd = buckets["cicd"] > 0
    has_qa = buckets["qa"] > 0

    if has_dev and not has_cicd:
        return "dev", buckets
    if has_cicd and not has_dev:
        return "cicd", buckets
    if has_qa and not has_dev and not has_cicd:
        return "qa", buckets
    return "uncertain", buckets


LABEL_TO_CATEGORY = {
    "cicd": "cicd",
    "cicd-failure": "cicd",
}


def label_based_guess(labels):
    for label in labels:
        if label in LABEL_TO_CATEGORY:
            return LABEL_TO_CATEGORY[label]
    return None


def final_category(path_guess, label_guess):
    if label_guess is None:
        return path_guess, "path-only"
    if label_guess == path_guess:
        return path_guess, "agree"
    return "uncertain", f"conflict(path={path_guess},label={label_guess})"


# --- data collection ---------------------------------------------------------

def list_altinity_merges(base, head):
    """Merge commits from Altinity/... branches in the base..head range, oldest first."""
    log = git(
        "log", f"origin/{base}..origin/{head}", "--merges",
        "--format=%H|%P|%s",
    )
    out = []
    for line in log.splitlines():
        sha, parents, subject = line.split("|", 2)
        if "Merge pull request #" not in subject or "from Altinity/" not in subject:
            continue
        pr_num = subject.split("#", 1)[1].split(" ", 1)[0]
        branch = subject.split("from Altinity/", 1)[1].strip()
        parent_list = parents.split(" ")
        if len(parent_list) != 2:
            # Not a plain 2-parent merge (octopus or something unusual) - skip, flag later.
            continue
        p1, p2 = parent_list
        out.append({
            "pr_number": int(pr_num),
            "branch": branch,
            "merge_sha": sha,
            "parent_mainline": p1,
            "parent_feature": p2,
        })
    out.reverse()  # oldest first
    return out


def changed_files(p1, p2):
    diff = git("diff", "--name-only", f"{p1}...{p2}")
    return [f for f in diff.splitlines() if f]


def fetch_pr_meta(repo, pr_number):
    raw = run([
        "gh", "api", f"repos/{repo}/pulls/{pr_number}",
        "--jq", '{title,user:.user.login,labels:[.labels[].name],merged_at,base_ref:.base.ref,html_url}',
    ])
    return json.loads(raw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="antalya-26.5", help="predecessor branch")
    ap.add_argument("--head", default="antalya-26.6", help="branch whose delta PRs to list")
    ap.add_argument("--repo", default="Altinity/ClickHouse")
    ap.add_argument("--out", default=None, help="write JSON results here")
    ap.add_argument("--csv", default=None, help="write CSV results here")
    args = ap.parse_args()

    merges = list_altinity_merges(args.base, args.head)
    print(f"# Found {len(merges)} Altinity-branch PRs merged between "
          f"{args.base} and {args.head}\n", file=sys.stderr)

    results = []
    for i, m in enumerate(merges, 1):
        print(f"[{i}/{len(merges)}] PR #{m['pr_number']} ({m['branch']})", file=sys.stderr)
        files = changed_files(m["parent_mainline"], m["parent_feature"])
        path_guess, buckets = path_based_guess(files)
        try:
            meta = fetch_pr_meta(args.repo, m["pr_number"])
        except subprocess.CalledProcessError as e:
            print(f"  ! gh api failed for #{m['pr_number']}: {e.stderr}", file=sys.stderr)
            meta = {"title": None, "user": None, "labels": [], "merged_at": None,
                     "base_ref": None, "html_url": None}
        label_guess = label_based_guess(meta.get("labels") or [])
        category, confidence = final_category(path_guess, label_guess)

        results.append({
            **m,
            "title": meta.get("title"),
            "author": meta.get("user"),
            "labels": meta.get("labels") or [],
            "merged_at": meta.get("merged_at"),
            "base_ref": meta.get("base_ref"),
            "html_url": meta.get("html_url"),
            "num_files_changed": len(files),
            "file_buckets": buckets,
            "path_guess": path_guess,
            "label_guess": label_guess,
            "category": category,
            "confidence": confidence,
        })

    # --- summary ---
    by_cat = {}
    for r in results:
        by_cat.setdefault(r["category"], []).append(r)

    print("\n" + "=" * 100)
    print(f"Summary: {len(results)} PRs total\n")
    for cat in ("dev", "cicd", "qa", "uncertain"):
        items = by_cat.get(cat, [])
        print(f"--- {cat.upper()} ({len(items)}) ---")
        for r in items:
            print(f"  #{r['pr_number']:<5} [{r['confidence']:<10}] {r['title']}  ({r['branch']})")
        print()

    if args.out:
        with open(args.out, "w") as f:
            json.dump(results, f, indent=2)
        print(f"Wrote {args.out}", file=sys.stderr)

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["pr_number", "category", "confidence", "path_guess", "label_guess",
                        "title", "author", "branch", "merge_sha", "merged_at", "labels",
                        "num_files_changed", "html_url"])
            for r in results:
                w.writerow([
                    r["pr_number"], r["category"], r["confidence"], r["path_guess"],
                    r["label_guess"], r["title"], r["author"], r["branch"], r["merge_sha"],
                    r["merged_at"], ";".join(r["labels"]), r["num_files_changed"], r["html_url"],
                ])
        print(f"Wrote {args.csv}", file=sys.stderr)


if __name__ == "__main__":
    main()

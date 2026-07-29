#!/usr/bin/env python3
"""
Check upstream master for recent "fixed flaky test" commits and report
which ones are not yet ported to the current branch.

Uses the GitHub API to list upstream commits (avoids shallow-fetch issues
with the ClickHouse repo), then checks each candidate against the local
git log by subject line.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime, timedelta, timezone


UPSTREAM_BASE_URL = "https://github.com/{repo}/commit/{sha}"
GITHUB_API = "https://api.github.com"


def since_iso(days: int) -> str:
    cutoff = datetime.now(timezone.utc) - timedelta(days=days)
    return cutoff.strftime("%Y-%m-%dT%H:%M:%SZ")


def github_commits(repo: str, ref: str, since: str) -> list:
    """Fetch all commits on repo/ref since the given ISO timestamp, paginating as needed."""
    token = os.environ.get("GITHUB_TOKEN")
    headers = {"User-Agent": "check_flaky_fix_backports"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    else:
        print("Warning: GITHUB_TOKEN not set; using unauthenticated API (60 req/hour limit)", file=sys.stderr)

    commits = []
    url = (
        f"{GITHUB_API}/repos/{repo}/commits"
        f"?sha={ref}&since={since}&per_page=100"
    )
    while url:
        print(f"Fetching {url} ...", file=sys.stderr)
        req = urllib.request.Request(url, headers=headers)
        try:
            with urllib.request.urlopen(req) as resp:
                commits.extend(json.loads(resp.read()))
                link = resp.headers.get("Link", "")
                url = None
                for part in link.split(","):
                    if 'rel="next"' in part:
                        url = part.split(";")[0].strip().strip("<>")
        except urllib.error.HTTPError as e:
            print(f"GitHub API error: {e}", file=sys.stderr)
            sys.exit(1)
    return commits


def flaky_commits(commits: list, pattern: str) -> list:
    """Filter to commits whose subject matches the pattern (case-insensitive).
    Returns list of (sha, subject, date) tuples."""
    results = []
    for c in commits:
        subject = c["commit"]["message"].splitlines()[0]
        if pattern.lower() in subject.lower():
            date = c["commit"]["committer"]["date"]
            results.append((c["sha"], subject, date))
    return results


def run_git(*args) -> str:
    import subprocess
    result = subprocess.run(
        ["git"] + list(args), text=True, capture_output=True
    )
    if result.returncode != 0:
        print(f"git {' '.join(args)} failed:\n{result.stderr}", file=sys.stderr)
        sys.exit(1)
    return result.stdout.strip()


def is_ported(subject: str) -> bool:
    """Return True if a commit with this subject exists on the current branch."""
    out = run_git("log", "--fixed-strings", f"--grep={subject}", "--format=%H", "HEAD")
    return bool(out)


def missing_commits(candidates: list) -> list:
    return [(sha, subj, date) for sha, subj, date in candidates if not is_ported(subj)]


def current_branch() -> str:
    branch = run_git("rev-parse", "--abbrev-ref", "HEAD")
    return branch if branch != "HEAD" else run_git("rev-parse", "HEAD")[:12]


def render_markdown(
    branch: str,
    upstream_repo: str,
    upstream_ref: str,
    days: int,
    pattern: str,
    all_upstream: list,
    missing: list,
) -> str:
    lines = []
    lines.append("## Flaky Fix Backport Check")
    lines.append("")
    lines.append(f"**Branch:** `{branch}`  ")
    lines.append(f"**Upstream:** `{upstream_repo}` @ `{upstream_ref}`  ")
    lines.append(f"**Window:** last {days} day(s)  ")
    lines.append(f"**Pattern:** `{pattern}`  ")
    lines.append("")
    lines.append(f"Upstream commits matching pattern: **{len(all_upstream)}**  ")
    lines.append(f"Missing from current branch: **{len(missing)}**  ")
    lines.append("")

    if not missing:
        lines.append(":white_check_mark: No missing flaky-fix backports found.")
    else:
        lines.append(":warning: The following upstream fixes are not yet ported:")
        lines.append("")
        for sha, subject, date in missing:
            url = UPSTREAM_BASE_URL.format(repo=upstream_repo, sha=sha)
            short = sha[:12]
            lines.append(f"- [`{short}`]({url}) {subject}  _(committed {date})_")

    lines.append("")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Report upstream flaky-fix commits not yet ported to the current branch.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--since-days",
        type=int,
        default=7,
        metavar="N",
        help="how many days back to scan upstream",
    )
    parser.add_argument(
        "--pattern",
        default="fix flaky test",
        help="case-insensitive substring to match in commit subjects",
    )
    parser.add_argument(
        "--upstream-repo",
        default="ClickHouse/ClickHouse",
        metavar="OWNER/REPO",
        help="upstream GitHub repository",
    )
    parser.add_argument(
        "--upstream-ref",
        default="master",
        help="upstream branch or ref to scan",
    )
    parser.add_argument(
        "--md-output",
        metavar="PATH",
        help="write markdown report to this file (default: stdout)",
    )
    parser.add_argument(
        "--json-output",
        metavar="PATH",
        help="write machine-readable JSON report to this file",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    cutoff = since_iso(args.since_days)
    branch = current_branch()

    commits = github_commits(args.upstream_repo, args.upstream_ref, cutoff)
    all_upstream = flaky_commits(commits, args.pattern)
    missing = missing_commits(all_upstream)

    report = render_markdown(
        branch=branch,
        upstream_repo=args.upstream_repo,
        upstream_ref=args.upstream_ref,
        days=args.since_days,
        pattern=args.pattern,
        all_upstream=all_upstream,
        missing=missing,
    )

    if args.md_output:
        with open(args.md_output, "w", encoding="utf-8") as f:
            f.write(report)
        print(f"Markdown report written to {args.md_output}", file=sys.stderr)
    else:
        print(report)

    if args.json_output:
        payload = {
            "branch": branch,
            "upstream_repo": args.upstream_repo,
            "upstream_ref": args.upstream_ref,
            "missing": [
                {"sha": sha, "subject": subj, "date": date}
                for sha, subj, date in missing
            ],
        }
        with open(args.json_output, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=2)
        print(f"JSON report written to {args.json_output}", file=sys.stderr)

    if missing:
        print(f"\n{len(missing)} missing flaky-fix commit(s) found.", file=sys.stderr)


if __name__ == "__main__":
    main()

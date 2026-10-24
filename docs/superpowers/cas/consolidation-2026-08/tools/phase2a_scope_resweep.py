#!/usr/bin/env python3
"""T7-remediation Phase 2(a): scope re-sweep.

Root cause (per verdicts/audit-t7/gate-c-audit-report.md, class 1): Tier A's
search roots were src/, tests/, programs/, utils/ca-soak/ only -- it never
searched docs/ (including docs/superpowers/{models,cas,specs,plans}/) or
.github/workflows/, even though many claims are ABOUT documents or CI config
in those trees. Every open/stale/unverifiable verdict whose canonical_claim
cites a backticked identifier is a candidate: re-grep those identifiers over
the WIDENED scope (adds docs/ in full and .github/workflows/) and report
which candidates got a genuinely new hit outside the original scope, for a
human/Claude decision pass -- this script does not itself change any verdict.
"""
import glob
import json
import os
import re
import subprocess

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

ORIGINAL_ROOTS = ["src", "tests", "programs", "utils/ca-soak"]
# High-confidence widened scope: the specific directories the audit's class-1
# root cause names (Tier A never searched any of docs/, and these are the
# CAS-project-internal doc/model/spec trees + CI config claims are actually
# about). Kept separate from a broader docs/ sweep, which is too noisy (a
# short/common identifier matches incidentally all over docs/en/ and inflates
# the candidate count to near-100% with no signal).
# Structured: precise, low-noise -- a hit here is strong signal (matches the
# earlier Tier B-deep experience with .tla/.cfg files).
STRUCTURED_ROOTS = ["docs/superpowers/models", ".github/workflows"]
# Prose: free-form text at real volume -- any 5+-char identifier incidentally
# matches somewhere, so a hit here is only a lead, not confirmation. Excludes
# consolidation-2026-08/ itself: that's this task's own working directory
# (extracted/, clusters/, verdicts/), and a claim's text trivially "hits"
# there because that's where the claim TEXT LIVES -- pure self-reference,
# not independent evidence.
PROSE_ROOTS = [
    "docs/superpowers/cas", "docs/superpowers/specs", "docs/superpowers/plans",
    "docs/superpowers/worklogs", "docs/superpowers/reports",
]
PROSE_EXCLUDE_GLOB = "!docs/superpowers/cas/consolidation-2026-08/**"

BACKTICK_RE = re.compile(r"`([^`]+)`")

# A term is only worth re-grepping if it's identifier-shaped, not an ordinary
# English word that happens to be backticked (e.g. `rejected`, `ephemeral`
# turned out to match constantly in TLA+/prose comments and produced almost
# no real signal -- see phase2a's first two runs). Accept: CamelCase with
# >=2 humps, snake_case with an underscore, a qualified `A::B` name, a
# dotted filename, or an ALL-CAPS SQL/SYSTEM-command fragment (2+ words).
IDENTIFIER_SHAPED_RE = re.compile(
    r"([A-Z][a-z0-9]+){2,}"          # CamelCase, >=2 humps
    r"|[a-z0-9]+_[a-z0-9_]+"          # snake_case
    r"|\w+::\w+"                      # qualified name
    r"|\.[A-Za-z0-9]{1,5}$"           # dotted filename suffix
    r"|^[A-Z][A-Z_ ]+[A-Z]$"          # ALL CAPS SQL/SYSTEM fragment
)


def is_identifier_shaped(term: str) -> bool:
    return bool(IDENTIFIER_SHAPED_RE.search(term))


def rg(term, roots, exclude_glob=None):
    cmd = ["rg", "-n", "--max-count=3", "-a", "-F"]
    if exclude_glob:
        cmd += ["-g", exclude_glob]
    cmd += [term] + roots
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True)
    return out.stdout.decode("utf-8", errors="replace").strip()


def main():
    clusters_by_id = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters_by_id[c["cluster_id"]] = c

    verdicts = []
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            verdicts.append(json.loads(line))

    candidates = [v for v in verdicts if v["verdict"] in ("open", "stale", "unverifiable")]
    print(f"candidate pool: {len(candidates)}")

    out_path = os.path.join(WORKDIR, "verdicts", "audit-t7", "phase2a-new-hits.jsonl")
    n_checked = 0
    n_new_hits = 0
    with open(out_path, "w") as out:
        for v in candidates:
            c = clusters_by_id.get(v["cluster_id"])
            if not c:
                continue
            terms = BACKTICK_RE.findall(c["canonical_claim"])
            terms = [t for t in terms if len(t) >= 3]
            if not terms:
                continue
            n_checked += 1
            structured_hits = {}
            prose_hits = {}
            for t in terms[:8]:
                if len(t) < 5 or not is_identifier_shaped(t):
                    continue
                shit = rg(t, STRUCTURED_ROOTS)
                if shit:
                    structured_hits[t] = shit.splitlines()[:3]
                phit = rg(t, PROSE_ROOTS, exclude_glob=PROSE_EXCLUDE_GLOB)
                if phit:
                    prose_hits[t] = phit.splitlines()[:3]
            if structured_hits or prose_hits:
                n_new_hits += 1
                out.write(json.dumps({
                    "cluster_id": v["cluster_id"],
                    "verdict": v["verdict"],
                    "canonical_claim": c["canonical_claim"],
                    "current_evidence": v["evidence"],
                    "structured_hits": structured_hits,
                    "prose_hits": prose_hits,
                }) + "\n")
            if n_checked % 200 == 0:
                print(f"  ...{n_checked}/{len(candidates)} checked, {n_new_hits} with new hits so far")

    print(f"checked {n_checked} candidates with >=1 backticked term (len>=5); {n_new_hits} have a NEW hit in the widened scope not in the original one")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()

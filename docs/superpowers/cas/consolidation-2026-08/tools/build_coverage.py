#!/usr/bin/env python3
"""Task 15 part 1: build the Gate D coverage matrix.

For every row of corpus-manifest.tsv (421 files), resolve its fate by
chaining: file -> extracted records citing it (by source path, stripping
#anchor) -> clusters containing those records (via member_ids) -> final
verdicts -> a destination.

Fate values (see FATE_* constants and classify_file() for the exact rules):
  - "ephemeral: <reason>"       -- every record from this file landed in an
                                    ephemeral/unverifiable(-target-none)
                                    cluster, or the file itself never
                                    produced any claim.
  - "-> <page1>, <page2>, ..."  -- doc-fact/done material landed on live
                                    docs/en/antalya/cas/** pages.
  - "-> BACKLOG"                -- an open/BACKLOG-target item that survived
                                    T13's aggressive prune (content-matched
                                    against the live BACKLOG.md).
  - "superseded: <reason>"      -- an open/BACKLOG-target item whose backing
                                    cluster was pruned as no-longer-relevant
                                    (does not appear in the live BACKLOG.md).
  - "KEEP-IN-PLACE: <reason>"   -- always-keep set (models/**, BACKLOG.md,
                                    .claude/agents/*.md, in-tree src/
                                    READMEs, live operational logs) or a spec
                                    whose verdict profile is mostly-open
                                    (still-actual design, not absorbed).

Exits non-zero if any manifest file is left without a fate -- no silent
gaps allowed.
"""
import csv
import glob
import json
import os
import re
import sys
from collections import defaultdict

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))

DOC_PAGES_ROOT = os.path.join(REPO_ROOT, "docs", "en", "antalya", "cas")
BACKLOG_PATH = os.path.join(REPO_ROOT, "docs", "superpowers", "cas", "BACKLOG.md")

# ---------------------------------------------------------------------------
# Always-keep set (independent of verdict content)
# ---------------------------------------------------------------------------
# Files with zero extracted records (map/extract found no verifiable claims
# in them) -- resolved by direct inspection, not by the join chain.
ZERO_RECORD_OVERRIDES = {
    ".superpowers/sdd/2026-07-28-cas-ref-chain-stage-b-catalog/progress.md":
        "ephemeral: SDD execution ledger (status/progress log for a sub-project), no extractable factual claims, meta-process artifact",
    "docs/superpowers/Structurizr.md":
        "ephemeral: stray/untracked artifact referencing a 'sandbox:/mnt/data/...' download link, no CAS-specific content, not part of the project's own writing",
    "docs/superpowers/cas/consolidation-2026-08/non-doc-debris.md":
        "ephemeral: this consolidation task's own T1 corpus-freeze meta-artifact (lists non-doc debris found during the scan), not itself doc content",
}

ALWAYS_KEEP_EXACT = {
    "docs/superpowers/cas/BACKLOG.md": "live backlog, actively maintained, not part of the deleted corpus",
    "docs/superpowers/cas/deferred-docs-fixes.md": "PENDING-CONFIRMATION -- live-append prose-fix queue, status not yet confirmed stopped; do not delete or list yet",
    ".claude/agents/ca-arch.md": "live agent role definition",
    ".claude/agents/ca-fix.md": "live agent role definition",
    ".claude/agents/ca-impl.md": "live agent role definition",
    ".claude/agents/ca-review.md": "live agent role definition",
    ".claude/agents/ca-surgeon.md": "live agent role definition",
    "src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/README.md": "in-tree source documentation next to the code, not part of the docs/superpowers planning corpus",
    "src/Disks/DiskObjectStorage/MetadataStorages/ContentAddressed/Formats/README.md": "in-tree source documentation next to the code, not part of the docs/superpowers planning corpus",
    "utils/ca-soak/scenarios/RUN_HISTORY.md": "live, actively-appended operational run log for the soak framework, not part of the docs/superpowers planning corpus",
    "utils/ca-soak/scenarios/BACKLOG.md": "the ca-soak scenario suite's own live backlog (distinct from docs/superpowers/cas/BACKLOG.md), actively maintained",
    "utils/ca-soak/README.md": "live operational documentation for the soak framework",
    "utils/ca-soak/requirements.txt": "live dependency manifest for the soak framework",
    "utils/ca-soak/scenarios/ASSUMPTIONS.md": "live operational documentation for the soak framework",
    "utils/ca-soak/scenarios/README.md": "live operational documentation for the soak framework",
    "utils/ca-soak/scenarios/framework/API.md": "live operational documentation for the soak framework",
}


def always_keep_prefix_reason(path):
    if path.startswith("docs/superpowers/models/"):
        return "TLA+ model source/results record, always-keep per the model-artifact policy"
    return None


# Files whose content was absorbed by docs/superpowers/cas/AGENTS.md itself
# (a Task 14 artifact created after the corpus freeze, so it can never appear
# as a cluster `target` and the join can never discover it automatically).
# Verified directly against AGENTS.md's own frontmatter, which states
# verbatim: "Absorbs INTENT.md and the agent-relevant parts of
# 02-methodology.md." Added as an extra destination alongside whatever
# technical-page destinations the join found for the same file.
ABSORBED_BY_AGENTS_MD = {
    "docs/superpowers/cas/INTENT.md",
    "docs/superpowers/cas/02-methodology.md",
}
AGENTS_MD_DEST = "docs/superpowers/cas/AGENTS.md"


# Specs whose verdict profile is plurality-open (the largest single verdict
# bucket among that file's clusters is "open") -- these are still-actual
# designs, not absorbed history, so they stay in place rather than being
# deleted. Computed by tools/find_mostly_open_specs.py against
# verdicts.jsonl + clusters.jsonl; listed explicitly (not re-derived at
# matrix-build time) so the reason for each is auditable by name.
MOSTLY_OPEN_SPECS = {
    "docs/superpowers/specs/2026-07-14-cas-readonly-replica-snapshot-pin-design.md":
        "plurality of this spec's clusters verdict 'open' -- still-actual design, not yet absorbed into a live docs/en page or superseded",
    "docs/superpowers/specs/2026-08-03-cas-branch-reconstruction-script-design.md":
        "plurality of this spec's clusters verdict 'open' -- still-actual design, not yet absorbed into a live docs/en page or superseded",
    "docs/superpowers/specs/2026-07-28-cas-fence-observability-and-write-grace-design.md":
        "plurality of this spec's clusters verdict 'open' -- still-actual design, not yet absorbed into a live docs/en page or superseded",
    "docs/superpowers/specs/2026-07-02-cas-copy-forward-condemned-evidence.md":
        "plurality of this spec's clusters verdict 'open' -- still-actual design, not yet absorbed into a live docs/en page or superseded",
}


# ---------------------------------------------------------------------------
# Loading
# ---------------------------------------------------------------------------

def load_manifest():
    rows = []
    with open(os.path.join(WORKDIR, "corpus-manifest.tsv")) as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append(row)
    return rows


def load_records():
    """id -> {sources: [bare paths], kind, suggested_target}"""
    records = {}
    files = glob.glob(os.path.join(WORKDIR, "extracted", "*.jsonl"))
    for fn in files:
        with open(fn) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                bare_sources = [s.split("#")[0] for s in d.get("sources", [])]
                records[d["id"]] = {
                    "sources": bare_sources,
                    "kind": d.get("kind"),
                    "suggested_target": d.get("suggested_target"),
                    "claim": d.get("claim", ""),
                }
    return records


def load_clusters():
    clusters = {}
    record_to_cluster = {}
    with open(os.path.join(WORKDIR, "clusters", "clusters.jsonl")) as f:
        for line in f:
            c = json.loads(line)
            clusters[c["cluster_id"]] = c
            for m in c["member_ids"]:
                record_to_cluster[m] = c["cluster_id"]
    return clusters, record_to_cluster


def load_verdicts():
    verdicts = {}
    with open(os.path.join(WORKDIR, "verdicts", "verdicts.jsonl")) as f:
        for line in f:
            v = json.loads(line)
            verdicts[v["cluster_id"]] = v
    return verdicts


def load_backlog_text():
    with open(BACKLOG_PATH) as f:
        return f.read()


def load_doc_pages():
    """target key (e.g. 'architecture/garbage-collection') -> page path,
    for every page under docs/en/antalya/cas/."""
    pages = {}
    for fn in glob.glob(os.path.join(DOC_PAGES_ROOT, "**", "*.md"), recursive=True):
        rel = os.path.relpath(fn, DOC_PAGES_ROOT)
        key = rel[:-3]  # strip .md
        pages[key] = os.path.relpath(fn, REPO_ROOT)
    return pages


# ---------------------------------------------------------------------------
# Join: file -> clusters
# ---------------------------------------------------------------------------

def build_file_to_clusters(manifest_rows, records, record_to_cluster):
    file_to_records = defaultdict(set)
    for rid, r in records.items():
        for src in r["sources"]:
            file_to_records[src].add(rid)

    file_to_clusters = {}
    for row in manifest_rows:
        path = row["path"]
        rids = file_to_records.get(path, set())
        cids = set()
        unmatched = []
        for rid in rids:
            cid = record_to_cluster.get(rid)
            if cid:
                cids.add(cid)
            else:
                unmatched.append(rid)
        file_to_clusters[path] = {"record_ids": rids, "cluster_ids": cids, "unmatched_records": unmatched}
    return file_to_clusters


# ---------------------------------------------------------------------------
# BACKLOG survival check
# ---------------------------------------------------------------------------

def backlog_survives(cluster, backlog_text):
    """A cluster's BACKLOG-target item 'survives' the T13 prune only if one of
    its issue_ids appears as a bracketed ID in the live BACKLOG.md.

    A prior version of this check also accepted a distinctive quoted
    substring of the canonical_claim appearing anywhere in BACKLOG.md. That
    fallback was removed: at BACKLOG.md's actual size (1811 lines of dense
    technical prose) generic terms and symbol names it flagged as
    "distinctive" (e.g. `existsFile`, `CasGc.cpp`, `Cas::Store`, `Removing`)
    recur incidentally throughout the document, so a substring hit is not
    evidence the specific item survived -- the same incidental-hit failure
    mode the T7 audit already measured at 83-96% for unscoped prose citation
    (see gate-c-audit-report.md). The bracketed-ID check has no such failure
    mode: BACKLOG.md's own header states IDs are never renumbered, so a
    bracket match names the exact live item, not merely nearby vocabulary.
    """
    for iid in cluster.get("issue_ids", []):
        if re.search(r"\[" + re.escape(iid) + r"\b", backlog_text):
            return True, f"issue_id [{iid}] present in BACKLOG.md"
    return False, None


# ---------------------------------------------------------------------------
# Fate classification
# ---------------------------------------------------------------------------

def classify_file(path, group, join, clusters, verdicts, backlog_text, doc_pages, specs_open_ratio):
    if group == "docs/en":
        # These live product-documentation pages (proper frontmatter, published
        # /operations/... and /sql-reference/... slugs) were pulled into the
        # T1 corpus freeze only so their CAS-related passages could be
        # cross-checked for consistency against the new docs/en/antalya/cas/**
        # pages. They are not planning-corpus debris and are never deletion
        # candidates, regardless of what their extracted clusters resolved to.
        return "KEEP-IN-PLACE: live published docs/en page, corpus-frozen only for cross-consistency checking, not part of the deleted planning corpus"
    if path in ALWAYS_KEEP_EXACT:
        return f"KEEP-IN-PLACE: {ALWAYS_KEEP_EXACT[path]}"
    if path in ZERO_RECORD_OVERRIDES:
        return ZERO_RECORD_OVERRIDES[path]
    if path in MOSTLY_OPEN_SPECS:
        return f"KEEP-IN-PLACE: {MOSTLY_OPEN_SPECS[path]}"
    prefix_reason = always_keep_prefix_reason(path)
    if prefix_reason:
        return f"KEEP-IN-PLACE: {prefix_reason}"

    cids = join["cluster_ids"]

    if not join["record_ids"]:
        # No claims were ever extracted from this file.
        return None  # handled by caller with a manual override table

    if not cids:
        return None  # unmatched records -- should not happen; caller flags it

    verdict_rows = []
    for cid in cids:
        v = verdicts.get(cid)
        c = clusters.get(cid)
        if v is None or c is None:
            continue
        verdict_rows.append((cid, c, v))

    if not verdict_rows:
        return None

    # All-ephemeral (or unverifiable-with-target-none, the other mechanical
    # step-1 bucket) -> the file's content was pure history/no-home material.
    if all(v["verdict"] in ("ephemeral",) or (v["verdict"] == "unverifiable" and c.get("target") == "none")
           for _, c, v in verdict_rows):
        return "ephemeral: every extracted claim from this file landed in an ephemeral (design-history) or no-home cluster"

    destinations = set()
    backlog_survivors = []
    backlog_superseded = []
    other_notes = []

    for cid, c, v in verdict_rows:
        target = c.get("target")
        verdict = v["verdict"]
        if verdict in ("done", "doc-fact") and target and target not in ("none", "BACKLOG"):
            page = doc_pages.get(target)
            if page:
                destinations.add(page)
            else:
                other_notes.append(f"{cid}: target '{target}' has no matching live page")
        elif target == "BACKLOG" or (verdict == "open"):
            survives, why = backlog_survives(c, backlog_text)
            if survives:
                backlog_survivors.append((cid, why))
            else:
                backlog_superseded.append(cid)
        elif verdict in ("ephemeral", "unverifiable", "rejected"):
            pass  # contributes nothing to destination; not a hard stop
        elif verdict == "stale":
            pass  # superseded content; contributes nothing to destination
        else:
            other_notes.append(f"{cid}: verdict={verdict} target={target} unclassified")

    if path in ABSORBED_BY_AGENTS_MD:
        destinations.add(AGENTS_MD_DEST)

    parts = []
    if destinations:
        parts.append("-> " + ", ".join(sorted(destinations)))
    if backlog_survivors:
        parts.append("-> BACKLOG (" + "; ".join(f"{cid}: {why}" for cid, why in backlog_survivors[:3]) + (f" +{len(backlog_survivors)-3} more" if len(backlog_survivors) > 3 else "") + ")")
    if backlog_superseded and not backlog_survivors and not destinations:
        parts.append(f"superseded: {len(backlog_superseded)} BACKLOG/open cluster(s) pruned from live BACKLOG.md, not matched: {', '.join(backlog_superseded[:5])}")

    if not parts:
        nothing_verdicts = {"stale", "unverifiable", "rejected", "ephemeral"}
        if all(v["verdict"] in nothing_verdicts for _, c, v in verdict_rows):
            seen = sorted({v["verdict"] for _, c, v in verdict_rows})
            return f"ephemeral: every extracted claim from this file resolved to {'/'.join(seen)} -- nothing carried forward (superseded, unverifiable, or rejected)"
        return None  # unresolved -- caller flags

    return "; ".join(parts)


def main():
    manifest_rows = load_manifest()
    records = load_records()
    clusters, record_to_cluster = load_clusters()
    verdicts = load_verdicts()
    backlog_text = load_backlog_text()
    doc_pages = load_doc_pages()

    file_to_clusters = build_file_to_clusters(manifest_rows, records, record_to_cluster)

    results = []
    unresolved = []
    for row in manifest_rows:
        path = row["path"]
        group = row["group"]
        join = file_to_clusters[path]
        fate = classify_file(path, group, join, clusters, verdicts, backlog_text, doc_pages, {})
        results.append({"path": path, "group": group, "tracked": row["tracked"], "fate": fate, "join": join})
        if fate is None:
            unresolved.append(path)

    print(f"total manifest files: {len(manifest_rows)}")
    print(f"resolved: {len(manifest_rows) - len(unresolved)}")
    print(f"UNRESOLVED: {len(unresolved)}")
    for p in unresolved:
        j = file_to_clusters[p]
        print(f"  - {p}  (records={len(j['record_ids'])}, clusters={len(j['cluster_ids'])}, unmatched_records={j['unmatched_records']})")

    with open(os.path.join(WORKDIR, "verdicts", "audit-t7", "coverage-draft.json"), "w") as f:
        json.dump([{k: (list(v) if isinstance(v, set) else v) for k, v in r.items() if k != "join"} | {
            "record_ids": sorted(r["join"]["record_ids"]),
            "cluster_ids": sorted(r["join"]["cluster_ids"]),
        } for r in results], f, indent=2)

    if unresolved:
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()

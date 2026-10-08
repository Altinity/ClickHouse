#!/usr/bin/env python3
"""T7-remediation Phase 2(b): casing re-sweep.

Root cause (per verdicts/audit-t7/gate-c-audit-report.md, class 2): a
`Cas*` -> `CAS*` symbol-casing rename broke literal-string verification --
a claim citing the old-cased symbol (e.g. `CasRefGcCleanupAuthority`) greps
to nothing because the live symbol is now `CASRefGcCleanupAuthority`, so the
claim was marked `stale` when the mechanism is unchanged and just renamed.

For every `stale` verdict whose claim cites a `Cas[A-Z]...`-shaped
identifier, try the CAS-prefix substitution (`Cas` -> `CAS` at the start of
each such identifier) and a case-insensitive grep, over the ORIGINAL search
roots (src/, tests/, programs/, utils/ca-soak/ -- this is a same-scope
casing bug, not a scope bug). Reports candidates where the renamed form
resolves; does not itself change any verdict.
"""
import json
import os
import re
import subprocess

WORKDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO_ROOT = os.path.abspath(os.path.join(WORKDIR, "..", "..", "..", ".."))
SEARCH_ROOTS = ["src", "tests", "programs", "utils/ca-soak"]

CAS_IDENT_RE = re.compile(r"\bCas[A-Z][A-Za-z0-9_]*")


TEST_MACRO_RE = re.compile(r"\bTEST(?:_F|_P)?\s*\(\s*")


def rg(term, extra_args=None):
    cmd = ["rg", "-n", "--max-count=8", "-a", "-F"] + (extra_args or []) + [term] + SEARCH_ROOTS
    out = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True)
    return out.stdout.decode("utf-8", errors="replace").strip()


def is_real_symbol_hit(line: str, ident: str) -> bool:
    """A gtest TEST(CASFoo, ...)/TEST_F(CASFoo, ...) suite-name argument is
    NOT evidence that a production symbol named CASFoo exists -- gtest suite
    names follow their own CAS-prefix convention independent of the actual
    class/function names in the code under test (confirmed by hand: e.g.
    TEST(CASObjectStorageBackend, ...) exists while the real class is
    `ObjectStorageBackend`, no prefix at all). Only count a hit as real
    evidence of a renamed symbol if it's a definition/qualified-use:
    `class Ident`, `struct Ident`, `Ident::`, or `Ident(` outside a TEST
    macro's first argument position."""
    if TEST_MACRO_RE.search(line) and re.search(rf"\(\s*{re.escape(ident)}\s*,", line):
        return False
    if re.search(rf"\b(class|struct)\s+{re.escape(ident)}\b", line):
        return True
    if re.search(rf"\b{re.escape(ident)}::", line):
        return True
    return False


def cas_prefix_variant(ident: str) -> str:
    # Cas -> CAS at the start of the identifier, or after any `_`/word boundary
    return re.sub(r"\bCas(?=[A-Z])", "CAS", ident)


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

    candidates = [v for v in verdicts if v["verdict"] == "stale"]

    out_path = os.path.join(WORKDIR, "verdicts", "audit-t7", "phase2b-casing-hits.jsonl")
    n_checked = 0
    n_resolved = 0
    with open(out_path, "w") as out:
        for v in candidates:
            c = clusters_by_id.get(v["cluster_id"])
            if not c:
                continue
            idents = set(CAS_IDENT_RE.findall(c["canonical_claim"]))
            if not idents:
                continue
            n_checked += 1
            resolved = {}
            for ident in idents:
                variant = cas_prefix_variant(ident)
                if variant == ident:
                    continue
                hit = rg(variant)
                if not hit:
                    continue
                lines = hit.splitlines()
                real_lines = [ln for ln in lines if is_real_symbol_hit(ln, variant)]
                # Report both: a real (non-test-suite-label) hit is strong
                # evidence; a TEST()-only hit is weaker and needs a human/
                # Claude judgment call on whether the test's behavior
                # actually confirms the specific claim (see C-2039 in the
                # phase-2b writeup: a TEST(CASObjectStorageBackend, ...)
                # suite existing does NOT confirm a method-shape claim about
                # a same-named class, because gtest suite names don't map
                # 1:1 to production symbol names in this codebase).
                resolved[ident] = {
                    "renamed_to": variant,
                    "real_symbol_hits": real_lines[:3],
                    "test_suite_only_hits": [] if real_lines else lines[:3],
                }
            if resolved:
                n_resolved += 1
                out.write(json.dumps({
                    "cluster_id": v["cluster_id"],
                    "canonical_claim": c["canonical_claim"],
                    "current_evidence": v["evidence"],
                    "resolved": resolved,
                }) + "\n")

    print(f"stale verdicts with Cas[A-Z]-pattern identifiers: {n_checked}")
    print(f"resolved under CAS-prefix rename: {n_resolved}")
    print(f"wrote {out_path}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Parse ci/tmp/test_result.txt from a CA-default stateless run and classify
each failing test as category-1 (a recognized CA capability-gate firing -> tag
no-content-addressed-storage) or category-2 (a real bug -> backlog).

Output:
  CAT1 <test>    <matched-signature>
  CAT2 <test>    <first-error-line>
  PASS/SKIP counts summary.
"""
import re
import sys

RESULT = sys.argv[1] if len(sys.argv) > 1 else "ci/tmp/test_result.txt"

# Category-1 capability-gate signatures. A failure is category-1 IFF its error
# text contains one of these. Everything else is a real bug (category-2).
CAT1_SIGNATURES = [
    "ReplicatedMergeTree is not supported on a content_addressed disk",
    "ALTER TABLE commands are not supported on immutable disk",
    "Mutations are not supported for immutable disk",
    "is not supported on a content_addressed disk",          # partition-clone / projections / generic CA gate
    "Table projections are not supported on a content_addressed disk",
    "non_replicated_deduplication_window is not supported",
    "non_replicated_deduplication_window",                    # dedup-window gate message variants
    "Autocommit writes are not supported for part files on a content-addressed disk",  # B35 RESTORE-onto-CA
    "content_addressed disk yet",
    "immutable disk",
    "does not support transactions",                          # B39 transactions gated on CA
    "Transactions are not supported",
    "throw_on_unsupported_query_inside_transaction",
]


def parse():
    with open(RESULT, errors="replace") as f:
        lines = f.readlines()

    # strip leading timestamp "YYYY-MM-DD HH:MM:SS " if present
    def strip_ts(s):
        return re.sub(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} ", "", s)

    lines = [strip_ts(l.rstrip("\n")) for l in lines]

    results = {}   # test -> OK/FAIL/SKIPPED
    # The per-test status lines look like:  <name>: <pad> [ OK ] 0.20 sec.
    status_re = re.compile(r"^([0-9A-Za-z_.\-]+):\s+\[ (OK|FAIL|SKIPPED|FAILED) \]")
    for l in lines:
        m = status_re.match(l)
        if m:
            name = m.group(1)
            st = m.group(2)
            # FAIL wins over later OK (retries); keep worst
            prev = results.get(name)
            if prev == "FAIL":
                continue
            results[name] = "FAIL" if st in ("FAIL", "FAILED") else st

    text = "\n".join(lines)
    return results, text, lines


def find_error_block(lines, test):
    """Return a chunk of error text following the FAIL line for `test`."""
    out = []
    status_line = re.compile(r"^[0-9A-Za-z_.\-]+:\s+\[ (OK|FAIL|SKIPPED|FAILED) \]")
    for i, l in enumerate(lines):
        if re.match(rf"^{re.escape(test)}:\s+\[ (FAIL|FAILED) \]", l):
            # grab lines until the NEXT test status line (bounded to this test)
            out = [l]
            for nxt in lines[i + 1 : i + 80]:
                if status_line.match(nxt):
                    break
                out.append(nxt)
            break
    return "\n".join(out)


def main():
    results, text, lines = parse()
    passed = [t for t, s in results.items() if s == "OK"]
    skipped = [t for t, s in results.items() if s == "SKIPPED"]
    failed = [t for t, s in results.items() if s == "FAIL"]

    cat1 = []
    cat2 = []
    for t in sorted(failed):
        block = find_error_block(lines, t)
        sig = None
        for s in CAT1_SIGNATURES:
            if s in block:
                sig = s
                break
        if sig:
            cat1.append((t, sig))
        else:
            # capture first informative error line
            err = ""
            for l in block.split("\n")[1:]:
                if "Code:" in l or "Exception" in l or "Error" in l or "Received signal" in l or "Differ" in l or "result differs" in l:
                    err = l.strip()
                    break
            cat2.append((t, err or block.split("\n")[1][:160] if len(block.split("\n")) > 1 else ""))

    print(f"SUMMARY passed={len(passed)} skipped={len(skipped)} failed={len(failed)} cat1={len(cat1)} cat2={len(cat2)}")
    print("=== CAT1 (tag no-content-addressed-storage) ===")
    for t, s in cat1:
        print(f"CAT1\t{t}\t{s}")
    print("=== CAT2 (real bug -> backlog) ===")
    for t, e in cat2:
        print(f"CAT2\t{t}\t{e}")
    # emit a space-separated CAT1 list for piping to the tagger
    print("=== CAT1_STEMS ===")
    print(" ".join(t for t, _ in cat1))


if __name__ == "__main__":
    main()

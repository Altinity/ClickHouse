#!/usr/bin/env python3
"""Fail-closed semantic checks for generated final SBOM artifacts."""

from __future__ import annotations

import csv
import json
import sys
import zipfile
from collections import defaultdict
from pathlib import Path


def exactly_one(directory: Path, pattern: str) -> Path:
    matches = list(directory.glob(pattern))
    if len(matches) != 1:
        raise RuntimeError(f"expected one {pattern}, found {len(matches)}")
    return matches[0]


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: verify_outputs.py OUTPUT_DIR")
    directory = Path(sys.argv[1])
    spdx_path = exactly_one(directory, "*-production.spdx.json")
    matrix_path = exactly_one(directory, "*-component-inclusion.csv")
    manifest_path = exactly_one(directory, "*-channel-manifest.csv")
    statement_path = exactly_one(directory, "SBOM-FINALIZATION-*.md")
    evidence_path = exactly_one(directory, "*-evidence.json")
    bundle_path = exactly_one(directory, "*-final-sbom-bundle.zip")

    document = json.loads(spdx_path.read_text())
    evidence = json.loads(evidence_path.read_text())
    packages = document.get("packages", [])
    package_ids = [package["SPDXID"] for package in packages]
    if len(package_ids) != len(set(package_ids)):
        raise RuntimeError("duplicate SPDX package identifiers")
    known_ids = set(package_ids) | {"SPDXRef-DOCUMENT"}
    for relationship in document.get("relationships", []):
        if relationship["spdxElementId"] not in known_ids or relationship["relatedSpdxElement"] not in known_ids:
            raise RuntimeError("relationship references an unknown SPDX identifier")

    rows = list(csv.DictReader(matrix_path.open()))
    required = {"component", "version", "supplier", "purl", "license", "included_in_final_binary", "architectures", "evidence"}
    if not rows or set(rows[0]) != required:
        raise RuntimeError("component matrix is empty or has an unexpected schema")
    for row in rows:
        if any(not row[field] for field in required):
            raise RuntimeError(f"component row has a blank required value: {row.get('component')}")
        if row["included_in_final_binary"] not in {"YES", "NO"}:
            raise RuntimeError(f"invalid inclusion decision: {row['component']}")
        if row["included_in_final_binary"] == "YES" and row["license"] == "NOASSERTION":
            raise RuntimeError(f"included component has no concluded license: {row['component']}")
        if not row["purl"].startswith("pkg:"):
            raise RuntimeError(f"invalid PURL: {row['component']}")

    manifest = list(csv.DictReader(manifest_path.open()))
    package_rows = [row for row in manifest if row["format"] in {"deb", "rpm", "tgz"}]
    if len(package_rows) != 6:
        raise RuntimeError(f"expected six release package rows, found {len(package_rows)}")
    hashes: dict[str, set[str]] = defaultdict(set)
    formats: dict[str, set[str]] = defaultdict(set)
    for row in package_rows:
        hashes[row["architecture"]].add(row["clickhouse_sha256"])
        formats[row["architecture"]].add(row["format"])
    for arch in ("amd64", "arm64"):
        if len(hashes[arch]) != 1 or formats[arch] != {"deb", "rpm", "tgz"}:
            raise RuntimeError(f"release package identity gate failed for {arch}")
        if next(iter(hashes[arch])) != evidence["binaries"][arch]["sha256"]:
            raise RuntimeError(f"manifest/evidence executable mismatch for {arch}")
    if evidence.get("container"):
        container_rows = [row for row in manifest if row["format"] == "oci"]
        if {row["architecture"] for row in container_rows} != {"amd64", "arm64"}:
            raise RuntimeError("container manifest does not cover amd64 and arm64")

    expected_bundle = {spdx_path.name, matrix_path.name, manifest_path.name, statement_path.name, evidence_path.name}
    with zipfile.ZipFile(bundle_path) as archive:
        if set(archive.namelist()) != expected_bundle:
            raise RuntimeError("final bundle contents differ from the validated deliverable set")
        if archive.testzip() is not None:
            raise RuntimeError("final bundle failed ZIP integrity testing")
    print(json.dumps({
        "status": "valid", "components": len(rows),
        "included": sum(row["included_in_final_binary"] == "YES" for row in rows),
        "excluded": sum(row["included_in_final_binary"] == "NO" for row in rows),
        "packages": len(packages), "relationships": len(document.get("relationships", [])),
    }, indent=2))


if __name__ == "__main__":
    main()

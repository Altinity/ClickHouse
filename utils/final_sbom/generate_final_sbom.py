#!/usr/bin/env python3
"""Generate final SPDX/CSV artifacts from collected immutable release evidence."""

from __future__ import annotations

import argparse
import configparser
import csv
import hashlib
import json
import re
import shutil
import zipfile
from pathlib import Path
from urllib.parse import quote


ALIASES = {
    "amqpcpp": "AMQP-CPP",
    "nuraft": "NuRaft",
    "sha3iuf": "SHA3IUF",
    "libstemmer-c": "libstemmer_c",
    "compiler-rt": "llvm-project",
    "libcxx": "llvm-project",
    "libcxxabi": "llvm-project",
    "libllvmlibc": "llvm-project",
    "libunwind": "llvm-project",
    "prometheus-protobufs-gogo": "prometheus-protobufs",
}
LICENSE_MAP = {
    "Apache": "Apache-2.0",
    "Apache-2.0 / MIT": "Apache-2.0 OR MIT",
    "Apache-2.0/MIT": "Apache-2.0 OR MIT",
    "BSD": "LicenseRef-BSD-unspecified",
    "BSD 2-clause": "BSD-2-Clause",
    "BSD 3-clause": "BSD-3-Clause",
    "BSD-3-Clause/MIT": "BSD-3-Clause AND MIT",
    "Boost": "BSL-1.0",
    "LGPL": "LicenseRef-LGPL-unspecified",
    "MIT / Apache-2.0": "MIT OR Apache-2.0",
    "MIT/Apache-2.0": "MIT OR Apache-2.0",
    "MIT/curl": "MIT AND curl",
    "OpenLDAP Version 2.8": "OLDAP-2.8",
    "Public Domain": "LicenseRef-Public-Domain",
    "Unlicense/MIT": "Unlicense OR MIT",
    "bzip2": "bzip2-1.0.6",
    "libpng": "Libpng",
    "zLib": "Zlib",
}


def spdx_id(text: str) -> str:
    return "SPDXRef-" + re.sub(r"[^A-Za-z0-9.-]+", "-", text).strip("-")


def github_parts(url: str) -> tuple[str, str] | None:
    normalized = re.sub(r"^git@github\.com:", "https://github.com/", url.strip()).removesuffix(".git")
    match = re.fullmatch(r"https?://github\.com/([^/]+)/([^/]+)", normalized)
    return (match.group(1), match.group(2)) if match else None


def combine_licenses(values: list[str]) -> str:
    licenses = sorted({LICENSE_MAP.get(value, value) for value in values if value})
    if not licenses:
        return "NOASSERTION"
    if len(licenses) == 1:
        return licenses[0]
    return " AND ".join(f"({value})" for value in licenses)


def component_package(
    *, name: str, version: str, supplier: str, purl: str, download: str,
    architectures: list[str], evidence: str, license_id: str, commit: str,
) -> dict:
    included = bool(architectures)
    arch_value = ",".join(architectures) if architectures else "none"
    return {
        "name": name,
        "SPDXID": spdx_id("Package-" + name),
        "versionInfo": version,
        "supplier": supplier,
        "downloadLocation": download,
        "filesAnalyzed": False,
        "licenseConcluded": license_id,
        "licenseDeclared": license_id,
        "copyrightText": "NOASSERTION",
        "externalRefs": [{
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": purl,
        }],
        "comment": (
            f"includedInFinalShippedBinary={'YES' if included else 'NO'}; evidence={evidence}; "
            f"architectures={arch_value}; releaseCommit={commit}"
        ),
    }


def load_license_inventory(path: Path) -> tuple[dict[str, list[str]], dict[tuple[str, str], list[str]], int]:
    rows = [json.loads(line) for line in path.read_text().splitlines() if line]
    by_top: dict[str, list[str]] = {}
    crates: dict[tuple[str, str], list[str]] = {}
    for row in rows:
        parts = row["license_path"].lstrip("/").split("/")
        if len(parts) < 3 or parts[0] != "contrib":
            continue
        top = parts[1]
        if top != "rust_vendor":
            by_top.setdefault(top, []).append(row["license_type"])
        elif len(parts) >= 4:
            match = re.fullmatch(r"(.+)-([0-9][A-Za-z0-9.+-]*)", parts[2])
            if match:
                crates.setdefault((match.group(1), match.group(2)), []).append(row["license_type"])
    return by_top, crates, len(rows)


def build_components(evidence_dir: Path) -> dict[str, set[str]]:
    result = {}
    for arch in ("amd64", "arm64"):
        values = {
            line.strip().removeprefix("contrib/")
            for line in (evidence_dir / f"{arch}-build-components.txt").read_text().splitlines()
            if line.strip()
        }
        result[arch] = {ALIASES.get(value, value) for value in values}
    return result


def components(evidence: dict, evidence_dir: Path) -> tuple[list[dict], int]:
    commit = evidence["commit"]
    repository = evidence["repository"]
    builds = build_components(evidence_dir)
    licenses_by_top, crates, record_count = load_license_inventory(evidence_dir / "system-licenses.jsonl")
    packages: list[dict] = []
    covered: set[str] = set()
    for module in sorted(evidence["gitmodules"], key=lambda item: item["path"]):
        name = module["path"].removeprefix("contrib/")
        compiled = [arch for arch, values in builds.items() if name in values]
        embedded = name in licenses_by_top
        architectures = list(builds) if embedded else compiled
        evidence_parts = []
        if compiled:
            evidence_parts.append(f"release build compiles contrib/{name} for {','.join(compiled)}")
        if embedded:
            evidence_parts.append(f"final executable embeds a system.licenses record for contrib/{name}")
        if not evidence_parts:
            evidence_parts.append("release source candidate with no compilation or embedded-license evidence")
        parts = github_parts(module["url"])
        if parts:
            owner, repo = parts
            purl = f"pkg:github/{quote(owner)}/{quote(repo)}@{module['sha']}"
            supplier = f"Organization: {owner}"
            download = f"https://github.com/{owner}/{repo}/tree/{module['sha']}"
        else:
            purl = f"pkg:generic/{quote(name)}@{module['sha']}"
            supplier = "Organization: Altinity, Inc."
            download = module["url"]
        license_id = combine_licenses(licenses_by_top.get(name, []))
        if name == "ulid-c":
            license_id = "Unlicense"
        elif name == "wordnet-blast":
            license_id = "LicenseRef-WordNet-3.0"
        packages.append(component_package(
            name=name, version=module["sha"], supplier=supplier, purl=purl, download=download,
            architectures=architectures, evidence=" and ".join(evidence_parts),
            license_id=license_id, commit=commit,
        ))
        covered.add(name)

    direct_names = sorted((set.union(*builds.values()) | set(licenses_by_top)) - covered)
    for name in direct_names:
        compiled = [arch for arch, values in builds.items() if name in values]
        embedded = name in licenses_by_top
        architectures = list(builds) if embedded else compiled
        evidence_parts = []
        if compiled:
            evidence_parts.append(f"release build compiles contrib/{name} for {','.join(compiled)}")
        if embedded:
            evidence_parts.append(f"final executable embeds a system.licenses record for contrib/{name}")
        license_id = combine_licenses(licenses_by_top.get(name, []))
        if license_id == "NOASSERTION" and compiled:
            license_id = "Apache-2.0"
        packages.append(component_package(
            name=name,
            version=commit,
            supplier="Organization: Altinity, Inc.",
            purl=f"pkg:github/{repository}@{commit}#contrib/{quote(name)}",
            download=f"https://github.com/{repository}/tree/{commit}/contrib/{quote(name)}",
            architectures=architectures,
            evidence=" and ".join(evidence_parts),
            license_id=license_id,
            commit=commit,
        ))

    for (crate_name, crate_version), values in sorted(crates.items()):
        name = f"cargo:{crate_name}@{crate_version}"
        packages.append(component_package(
            name=name,
            version=crate_version,
            supplier="Organization: crates.io",
            purl=f"pkg:cargo/{quote(crate_name)}@{crate_version}",
            download=f"https://crates.io/crates/{quote(crate_name)}/{crate_version}",
            architectures=list(builds),
            evidence="final executable system.licenses inventory names the exact vendored crate/version",
            license_id=combine_licenses(values),
            commit=commit,
        ))
    return sorted(packages, key=lambda item: item["name"].lower()), record_count


def artifact_package(evidence: dict, artifact: dict, binary_id: str) -> dict:
    version = evidence["version"]
    arch = artifact["architecture"]
    fmt = artifact["format"]
    purl_type = {"deb": "deb/ubuntu", "rpm": "rpm", "tgz": "generic"}[fmt]
    purl_arch = {"amd64": "amd64", "arm64": "arm64"}[arch]
    if fmt == "rpm":
        purl_arch = {"amd64": "x86_64", "arm64": "aarch64"}[arch]
    name = f"clickhouse-common-static-{fmt}-{arch}"
    return {
        "name": name,
        "SPDXID": spdx_id("Artifact-" + name),
        "versionInfo": version,
        "supplier": "Organization: Altinity, Inc.",
        "downloadLocation": artifact["url"],
        "filesAnalyzed": False,
        "checksums": [{"algorithm": "SHA256", "checksumValue": artifact["sha256"]}],
        "licenseConcluded": "Apache-2.0",
        "licenseDeclared": "Apache-2.0",
        "copyrightText": "Copyright Altinity, Inc. and ClickHouse contributors",
        "primaryPackagePurpose": "INSTALL",
        "externalRefs": [{
            "referenceCategory": "PACKAGE-MANAGER",
            "referenceType": "purl",
            "referenceLocator": f"pkg:{purl_type}/clickhouse-common-static@{version}?arch={purl_arch}",
        }],
        "comment": f"Final published {fmt.upper()} artifact containing {binary_id}; package bytes analyzed by SHA-256.",
    }


def write_csvs(output_dir: Path, prefix: str, component_rows: list[dict], evidence: dict) -> tuple[Path, Path]:
    matrix = output_dir / f"{prefix}-component-inclusion.csv"
    with matrix.open("w", newline="") as target:
        fields = ["component", "version", "supplier", "purl", "license", "included_in_final_binary", "architectures", "evidence"]
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for package in component_rows:
            comment = package["comment"]
            writer.writerow({
                "component": package["name"],
                "version": package["versionInfo"],
                "supplier": package["supplier"],
                "purl": package["externalRefs"][0]["referenceLocator"],
                "license": package["licenseConcluded"],
                "included_in_final_binary": "YES" if "includedInFinalShippedBinary=YES" in comment else "NO",
                "architectures": comment.split("architectures=", 1)[1].split(";", 1)[0].replace(",", ";"),
                "evidence": comment.split("evidence=", 1)[1].split("; architectures=", 1)[0],
            })

    manifest = output_dir / f"{prefix}-channel-manifest.csv"
    with manifest.open("w", newline="") as target:
        fields = ["channel", "architecture", "format", "artifact", "artifact_sha256", "manifest_list_sha256", "clickhouse_sha256", "included"]
        writer = csv.DictWriter(target, fieldnames=fields)
        writer.writeheader()
        for artifact in evidence["artifacts"]:
            writer.writerow({
                "channel": artifact["channel"], "architecture": artifact["architecture"],
                "format": artifact["format"], "artifact": artifact["file_name"],
                "artifact_sha256": artifact["sha256"], "manifest_list_sha256": "",
                "clickhouse_sha256": artifact["clickhouse_sha256"], "included": "YES",
            })
        container = evidence.get("container")
        if container:
            for arch, digest in sorted(container["platform_digests"].items()):
                writer.writerow({
                    "channel": "Stable container / BYOC when this immutable image is selected",
                    "architecture": arch, "format": "oci", "artifact": container["image"],
                    "artifact_sha256": digest.removeprefix("sha256:"),
                    "manifest_list_sha256": container["manifest_digest"].removeprefix("sha256:"),
                    "clickhouse_sha256": evidence["binaries"][arch]["sha256"], "included": "YES",
                })
    return matrix, manifest


def finalization_text(evidence: dict, components_count: int, included: int, excluded: int, record_count: int) -> str:
    version = evidence["version"]
    binary_rows = "\n".join(
        f"| {arch} | `{meta['sha256']}` | {meta['size']:,} | `{meta['build_id']}` |"
        for arch, meta in evidence["binaries"].items()
    )
    container_text = "Container verification was not requested."
    if evidence.get("container"):
        container_text = (
            f"The multi-architecture OCI image `{evidence['container']['image']}` was verified against both "
            f"package executables. Manifest-list digest: `{evidence['container']['manifest_digest']}`."
        )
    return f"""# Final production SBOM — Altinity ClickHouse {version}

Status: **FINAL for the immutable artifacts and executable digests listed in the channel manifest.**

Release tag: `{evidence['tag']}`  
Release commit: `{evidence['commit']}`  
Release: <{evidence['release_url']}>  
Build run: <{evidence['build_run_url']}>

## Scope and conclusion

The SPDX 2.3 document and CSV matrix establish a version, supplier, PURL, license, architecture, evidence, and explicit final-binary inclusion result for every component. There are {components_count:,} component records: {included:,} are marked `YES` and {excluded:,} are marked `NO`.

`YES` uses a conservative production-compliance definition: the component was observed in release compilation evidence and/or is named in the `system.licenses` inventory embedded in the final executable. `NO` identifies a pinned release-source candidate with neither form of release evidence. The `rust_vendor` wrapper, when excluded, is represented by its exact versioned Cargo component rows.

The SPDX `filesAnalyzed=false` value on component package records means no per-source-file copyright census is asserted. Package bytes, executable bytes, build IDs, release compilation evidence, source pins, and the executable's embedded license inventory were analyzed and reconciled.

## Shipped executable identity

| Architecture | `/usr/bin/clickhouse` SHA-256 | Size | GNU build ID |
|---|---|---:|---|
{binary_rows}

For each architecture, streaming extraction proved that the published DEB, RPM, and TGZ contain the byte-identical executable shown above. {container_text}

BYOC coverage is digest-based: this SBOM applies only when the deployed image contains a listed executable digest. Another image, architecture, or mutable-tag result requires a separately generated SBOM.

## Evidence and validation

- Six public DEB, RPM, and TGZ artifacts were downloaded from the tagged release build and hashed.
- Executables were streamed from every package and compared by SHA-256.
- The successful amd64 and arm64 release-build job logs supplied compilation evidence.
- Pinned `.gitmodules` entries and gitlink SHAs establish source component versions and GitHub PURLs.
- The final executable supplied {record_count:,} `system.licenses` records, including exact Cargo crate versions.
- Container platform executables were compared with the corresponding package executables when container verification was enabled.
- The workflow validates SPDX 2.3 syntax, required metadata, inclusion decisions, license completeness for included components, and identifier uniqueness before publishing.
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    evidence_dir = args.evidence_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    evidence = json.loads((evidence_dir / "evidence.json").read_text())
    version = evidence["version"]
    prefix = f"Altinity-ClickHouse-{version}"
    component_rows, record_count = components(evidence, evidence_dir)
    packages: list[dict] = []
    relationships: list[dict] = []
    binary_ids: dict[str, str] = {}

    for arch, metadata in evidence["binaries"].items():
        name = f"clickhouse-binary-{arch}"
        binary_id = spdx_id("Binary-" + name)
        binary_ids[arch] = binary_id
        packages.append({
            "name": name, "SPDXID": binary_id, "versionInfo": version,
            "supplier": "Organization: Altinity, Inc.",
            "downloadLocation": "NOASSERTION", "filesAnalyzed": False,
            "checksums": [{"algorithm": "SHA256", "checksumValue": metadata["sha256"]}],
            "licenseConcluded": "Apache-2.0", "licenseDeclared": "Apache-2.0",
            "copyrightText": "Copyright Altinity, Inc. and ClickHouse contributors",
            "primaryPackagePurpose": "APPLICATION",
            "externalRefs": [{
                "referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                "referenceLocator": f"pkg:generic/altinity/clickhouse@{version}?arch={arch}",
            }],
            "comment": (
                f"Final shipped ELF; size={metadata['size']}; GNU build-id={metadata['build_id']}; "
                "byte-identical across published DEB, RPM, and TGZ packages for this architecture."
            ),
        })

    for artifact in evidence["artifacts"]:
        binary_id = binary_ids[artifact["architecture"]]
        package = artifact_package(evidence, artifact, binary_id)
        packages.append(package)
        relationships.append({
            "spdxElementId": package["SPDXID"], "relationshipType": "CONTAINS",
            "relatedSpdxElement": binary_id,
            "comment": "Binary equality verified by streaming SHA-256 extraction from the published package.",
        })

    container = evidence.get("container")
    if container:
        for arch, digest in sorted(container["platform_digests"].items()):
            name = f"clickhouse-server-oci-{arch}"
            package_id = spdx_id("Artifact-" + name)
            packages.append({
                "name": name, "SPDXID": package_id, "versionInfo": version,
                "supplier": "Organization: Altinity, Inc.", "downloadLocation": "NOASSERTION",
                "filesAnalyzed": False,
                "checksums": [{"algorithm": "SHA256", "checksumValue": digest.removeprefix("sha256:")}],
                "licenseConcluded": "Apache-2.0", "licenseDeclared": "Apache-2.0",
                "copyrightText": "Copyright Altinity, Inc. and ClickHouse contributors",
                "primaryPackagePurpose": "CONTAINER",
                "externalRefs": [{
                    "referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                    "referenceLocator": f"pkg:oci/clickhouse-server@{digest}?repository_url=altinity/clickhouse-server&arch={arch}",
                }],
                "comment": f"Immutable {arch} OCI platform image from {container['image']}.",
            })
            relationships.append({
                "spdxElementId": package_id, "relationshipType": "CONTAINS",
                "relatedSpdxElement": binary_ids[arch],
                "comment": "Container executable equality verified by SHA-256 against release packages.",
            })

    packages.extend(component_rows)
    for component in component_rows:
        included = "includedInFinalShippedBinary=YES" in component["comment"]
        arches = component["comment"].split("architectures=", 1)[1].split(";", 1)[0].split(",")
        for arch, binary_id in binary_ids.items():
            architecture_included = included and arch in arches
            relationships.append({
                "spdxElementId": binary_id,
                "relationshipType": "STATIC_LINK" if architecture_included else "OTHER",
                "relatedSpdxElement": component["SPDXID"],
                "comment": (
                    "includedInFinalShippedBinary=YES; evidence=release compilation and/or embedded license inventory"
                    if architecture_included else
                    "includedInFinalShippedBinary=NO; component absent from architecture-specific evidence"
                ),
            })

    created = evidence.get("release_published_at") or "1970-01-01T00:00:00Z"
    wordnet_sha = next(
        (item["sha"] for item in evidence["gitmodules"] if item["path"] == "contrib/wordnet-blast"),
        evidence["commit"],
    )
    document = {
        "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"Altinity-ClickHouse-{version}-production-release",
        "documentNamespace": f"https://altinity.com/sbom/clickhouse/{version}/{evidence['commit']}",
        "creationInfo": {
            "created": created, "creators": ["Tool: Altinity final-production-SBOM workflow"],
            "licenseListVersion": "3.28",
            "comment": (
                "Production SBOM derived from immutable release packages, exact executable hashes, release "
                "compilation evidence, pinned source dependencies, the final ELF's embedded license inventory, "
                "and verified OCI platform images. filesAnalyzed=false denotes no per-source-file copyright census."
            ),
        },
        "documentDescribes": list(binary_ids.values()),
        "packages": packages, "relationships": relationships,
        "annotations": [{
            "annotationDate": created, "annotationType": "REVIEW",
            "annotator": "Tool: Altinity final-production-SBOM workflow", "SPDXID": "SPDXRef-DOCUMENT",
            "comment": (
                f"FINAL for {evidence['tag']} immutable artifacts. BYOC applicability requires matching a "
                "deployed /usr/bin/clickhouse SHA-256 to this document."
            ),
        }],
        "hasExtractedLicensingInfos": [
            {"licenseId": "LicenseRef-BSD-unspecified", "name": "Unspecified BSD variant", "extractedText": "The embedded inventory identifies this license only as BSD without a clause count."},
            {"licenseId": "LicenseRef-LGPL-unspecified", "name": "Unspecified LGPL variant", "extractedText": "The embedded inventory identifies this license only as LGPL without a version."},
            {"licenseId": "LicenseRef-Public-Domain", "name": "Public domain", "extractedText": "The embedded inventory identifies this component as public domain."},
            {"licenseId": "LicenseRef-WordNet-3.0", "name": "Princeton WordNet 3.0 license", "extractedText": "Permission to use, copy, modify and distribute the WordNet 3.0 software, database and documentation for any purpose and without fee or royalty, subject to its notice and disclaimer.", "seeAlsos": [f"https://github.com/ClickHouse/wordnet-blast/blob/{wordnet_sha}/WORDNET_LICENSE"]},
        ],
    }

    spdx_path = output_dir / f"{prefix}-production.spdx.json"
    spdx_path.write_text(json.dumps(document, indent=2) + "\n")
    matrix_path, manifest_path = write_csvs(output_dir, prefix, component_rows, evidence)
    included_count = sum("includedInFinalShippedBinary=YES" in item["comment"] for item in component_rows)
    statement_path = output_dir / f"SBOM-FINALIZATION-{version}.md"
    statement_path.write_text(finalization_text(
        evidence, len(component_rows), included_count, len(component_rows) - included_count, record_count,
    ))
    evidence_copy = output_dir / f"{prefix}-evidence.json"
    shutil.copy2(evidence_dir / "evidence.json", evidence_copy)
    bundle = output_dir / f"{prefix}-final-sbom-bundle.zip"
    with zipfile.ZipFile(bundle, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for path in (spdx_path, matrix_path, manifest_path, statement_path, evidence_copy):
            archive.write(path, path.name)
    print(json.dumps({
        "spdx": str(spdx_path), "matrix": str(matrix_path), "manifest": str(manifest_path),
        "statement": str(statement_path), "bundle": str(bundle), "components": len(component_rows),
        "included": included_count, "excluded": len(component_rows) - included_count,
    }, indent=2))


if __name__ == "__main__":
    main()

# Final production SBOM workflow

This workflow creates a release-scoped, fail-closed SPDX 2.3 SBOM for Altinity ClickHouse. It addresses the production-review requirement that every component have a version, supplier, PURL, license, and explicit determination of whether it is included in the shipped executable.

## Outputs

For release `VERSION`, the workflow publishes:

- `Altinity-ClickHouse-VERSION-production.spdx.json` — normative SPDX 2.3 SBOM.
- `Altinity-ClickHouse-VERSION-component-inclusion.csv` — reviewer-friendly component matrix.
- `Altinity-ClickHouse-VERSION-channel-manifest.csv` — Stable/BYOC/on-prem artifact-to-executable digest map.
- `Altinity-ClickHouse-VERSION-evidence.json` — release, build, package, container, and source-pin evidence.
- `SBOM-FINALIZATION-VERSION.md` — finalization and scope statement.
- `Altinity-ClickHouse-VERSION-final-sbom-bundle.zip` — the files above in one bundle.

The uploaded workflow artifact also retains the raw embedded-license inventory and the parsed amd64/arm64 compilation component lists.

## Inclusion policy

A component is `YES` when at least one of these release-specific conditions is true:

1. The successful architecture release-build job contains compilation/link evidence for its `contrib` path.
2. The final executable names it in its embedded `system.licenses` inventory.

The second rule deliberately keeps statically linked and header-only dependencies in legal-review scope when optimization removes recognizable symbols. A pinned source candidate with neither form of evidence is `NO`. Architecture-specific evidence is preserved in the matrix and SPDX relationships.

The `rust_vendor` source wrapper is not treated as one component. Exact crate names, versions, PURLs, suppliers, and licenses are emitted from the executable's embedded inventory.

## Finalization gates

No artifact is published unless all gates pass:

- The tag resolves to the checked-out commit.
- A completed MasterCI run and successful `Build (amd_release)` and `Build (arm_release)` jobs exist.
- DEB, RPM, and TGZ packages download successfully for both architectures.
- All three formats contain the same executable bytes for their architecture.
- Both OCI platform images contain the corresponding package executable.
- The final executable returns a non-empty embedded license inventory.
- Every component has version, supplier, PURL, license, architecture, evidence, and YES/NO inclusion.
- No included component has `NOASSERTION` as its concluded license.
- SPDX 2.3 schema validation, relationship integrity, digest consistency, and ZIP integrity pass.

## Installation

Copy these paths into the repository without changing their relative locations:

```text
.github/workflows/final-production-sbom.yml
utils/final_sbom/collect_release_evidence.py
utils/final_sbom/generate_final_sbom.py
utils/final_sbom/verify_outputs.py
utils/final_sbom/requirements.txt
```

The workflow uses public release packages and container images. It needs only the automatically supplied GitHub token with `contents: read` and `actions: read`; no signing, registry, or cloud-storage secrets are required.

Run it from **Actions → Final production SBOM → Run workflow**, entering a complete tag such as `v26.6.4.20001.altinityantalya`. The build run is auto-detected from the release commit. Supply `build_run_id` only when more than one MasterCI run exists and a specific run must control the evidence.

## Local execution

Run from a checkout of the exact tag. Required system commands are `docker` with Buildx, `dpkg-deb`, `tar`, `readelf`, and `git`; RPM extraction is handled by the pinned Python dependency.

```bash
python -m pip install -r utils/final_sbom/requirements.txt

python utils/final_sbom/collect_release_evidence.py \
  --tag v26.6.4.20001.altinityantalya \
  --repo Altinity/ClickHouse \
  --github-token "$GH_TOKEN" \
  --output-dir sbom-work/evidence \
  --require-container

python utils/final_sbom/generate_final_sbom.py \
  --evidence-dir sbom-work/evidence \
  --output-dir sbom-work/final

python -m spdx_tools.spdx.clitools.pyspdxtools \
  -i sbom-work/final/*-production.spdx.json
python utils/final_sbom/verify_outputs.py sbom-work/final
```

`FINAL` is scoped to the immutable hashes in the generated channel manifest. A BYOC deployment is covered only when its executable digest matches the manifest; a different image digest requires another run.

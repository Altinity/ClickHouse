#!/usr/bin/env python3
"""Collect reproducible evidence for a finalized Altinity ClickHouse SBOM."""

from __future__ import annotations

import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import BinaryIO


ARCHES = {
    "amd64": {"build": "amd", "rpm_arch": "x86_64", "docker_arch": "amd64"},
    "arm64": {"build": "arm", "rpm_arch": "aarch64", "docker_arch": "arm64"},
}
TAG_RE = re.compile(r"^v?[0-9][0-9A-Za-z.+_-]*$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")


class EvidenceError(RuntimeError):
    pass


class StripCrossHostAuthorization(urllib.request.HTTPRedirectHandler):
    """Do not forward the GitHub token to a signed log-download host."""

    def redirect_request(self, request, fp, code, msg, headers, new_url):
        redirected = super().redirect_request(request, fp, code, msg, headers, new_url)
        if redirected and urllib.parse.urlsplit(request.full_url).netloc != urllib.parse.urlsplit(new_url).netloc:
            redirected.remove_header("Authorization")
        return redirected


def run(args: list[str], *, stdout: BinaryIO | int | None = None, cwd: Path | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run(args, cwd=cwd, stdout=stdout or subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode:
        stderr = result.stderr.decode("utf-8", "replace")[-4000:]
        raise EvidenceError(f"command failed ({result.returncode}): {' '.join(args)}\n{stderr}")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_and_hash(source: BinaryIO, destination: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with destination.open("wb") as target:
        while block := source.read(8 * 1024 * 1024):
            target.write(block)
            digest.update(block)
            size += len(block)
    destination.chmod(destination.stat().st_mode | stat.S_IXUSR)
    return digest.hexdigest(), size


class GitHub:
    def __init__(self, repo: str, token: str):
        self.repo = repo
        self.token = token

    def request(self, path_or_url: str, *, accept: str = "application/vnd.github+json") -> bytes:
        url = path_or_url if path_or_url.startswith("https://") else f"https://api.github.com/repos/{self.repo}/{path_or_url.lstrip('/')}"
        headers = {"Accept": accept, "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "final-sbom-workflow"}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        request = urllib.request.Request(url, headers=headers)
        try:
            opener = urllib.request.build_opener(StripCrossHostAuthorization())
            with opener.open(request, timeout=120) as response:
                return response.read()
        except urllib.error.HTTPError as exc:
            raise EvidenceError(f"GitHub request failed: {url}: HTTP {exc.code}") from exc

    def json(self, path: str) -> dict:
        return json.loads(self.request(path))


def download(url: str, path: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "final-sbom-workflow"})
    try:
        with urllib.request.urlopen(request, timeout=300) as response, path.open("wb") as target:
            shutil.copyfileobj(response, target, length=8 * 1024 * 1024)
    except Exception:
        path.unlink(missing_ok=True)
        raise


def extract_deb_binary(package: Path, output: Path) -> tuple[str, int]:
    producer = subprocess.Popen(["dpkg-deb", "--fsys-tarfile", str(package)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert producer.stdout is not None
    consumer = subprocess.Popen(["tar", "-xOf", "-", "./usr/bin/clickhouse"], stdin=producer.stdout, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    producer.stdout.close()
    assert consumer.stdout is not None
    digest, size = copy_and_hash(consumer.stdout, output)
    _, consumer_err = consumer.communicate()
    assert producer.stderr is not None
    producer_err = producer.stderr.read()
    producer.wait()
    if producer.returncode or consumer.returncode:
        raise EvidenceError(f"failed to extract DEB binary: {producer_err.decode()} {consumer_err.decode()}")
    return digest, size


def extract_rpm_binary(package: Path, output: Path) -> tuple[str, int]:
    import rpmfile

    with rpmfile.open(str(package)) as archive:
        candidates = [member for member in archive.getmembers() if member.name.endswith("/usr/bin/clickhouse")]
        if len(candidates) != 1:
            raise EvidenceError(f"expected one usr/bin/clickhouse in {package}, found {len(candidates)}")
        source = archive.extractfile(candidates[0])
        if source is None:
            raise EvidenceError(f"cannot extract clickhouse from {package}")
        return copy_and_hash(source, output)


def extract_tgz_binary(package: Path, output: Path) -> tuple[str, int]:
    with tarfile.open(package, "r:gz") as archive:
        candidates = [member for member in archive.getmembers() if member.name.endswith("/usr/bin/clickhouse")]
        if len(candidates) != 1:
            raise EvidenceError(f"expected one usr/bin/clickhouse in {package}, found {len(candidates)}")
        source = archive.extractfile(candidates[0])
        if source is None:
            raise EvidenceError(f"cannot extract clickhouse from {package}")
        return copy_and_hash(source, output)


def build_id(binary: Path) -> str:
    output = run(["readelf", "-n", str(binary)]).stdout.decode("utf-8", "replace")
    match = re.search(r"Build ID:\s*([0-9a-f]+)", output)
    if not match:
        raise EvidenceError(f"GNU build ID not found in {binary}")
    return match.group(1)


def collect_licenses(binary: Path, destination: Path) -> int:
    query = (
        "SELECT library_name, license_type, license_path FROM system.licenses "
        "ORDER BY license_path FORMAT JSONEachRow"
    )
    result = run([str(binary), "local", "--query", query])
    destination.write_bytes(result.stdout)
    rows = [json.loads(line) for line in result.stdout.splitlines() if line]
    if not rows:
        raise EvidenceError("the final executable returned an empty system.licenses inventory")
    required = {"library_name", "license_type", "license_path"}
    if any(set(row) != required for row in rows):
        raise EvidenceError("unexpected system.licenses row format")
    return len(rows)


def find_master_run(github: GitHub, commit: str, requested: int | None) -> tuple[int, list[dict]]:
    run_id = requested
    if run_id is None:
        response = github.json(f"actions/runs?head_sha={commit}&per_page=100")
        candidates = [
            item for item in response.get("workflow_runs", [])
            if item.get("name") == "MasterCI" and item.get("status") == "completed"
        ]
        if not candidates:
            raise EvidenceError(f"no completed MasterCI run found for {commit}")
        successful = [item for item in candidates if item.get("conclusion") == "success"]
        selected = (successful or candidates)[0]
        run_id = int(selected["id"])
    jobs = github.json(f"actions/runs/{run_id}/jobs?per_page=100").get("jobs", [])
    return run_id, jobs


def parse_build_components(log: str) -> set[str]:
    components: set[str] = set()
    for line in log.splitlines():
        build_line = re.search(r"Building (?:C|CXX|ASM)|Linking (?:C|CXX)|\brustc\b", line)
        compiler_line = re.search(r"\b(?:clang\+\+|clang|gcc|g\+\+|c\+\+)\b.*(?:^|\s)-c(?:\s|$)", line)
        if not build_line and not compiler_line:
            continue
        components.update(re.findall(r"\bcontrib[/\\]([^/\\\s:]+)", line))
    return components


def collect_build_components(github: GitHub, jobs: list[dict], output_dir: Path) -> dict[str, dict]:
    result: dict[str, dict] = {}
    for arch, expected_name in (("amd64", "Build (amd_release)"), ("arm64", "Build (arm_release)")):
        matches = [job for job in jobs if job.get("name") == expected_name and job.get("conclusion") == "success"]
        if len(matches) != 1:
            raise EvidenceError(f"expected one successful {expected_name} job, found {len(matches)}")
        job = matches[0]
        log = github.request(f"actions/jobs/{job['id']}/logs", accept="application/vnd.github+json").decode("utf-8", "replace")
        (output_dir / f"{arch}-build.log").write_text(log)
        components = parse_build_components(log)
        if not components:
            raise EvidenceError(f"no contrib compilation evidence parsed from {expected_name}")
        path = output_dir / f"{arch}-build-components.txt"
        path.write_text("\n".join(sorted(components)) + "\n")
        result[arch] = {"job_id": job["id"], "job_name": expected_name, "components": sorted(components)}
    return result


def git_source_evidence(repo_root: Path) -> tuple[list[dict], str]:
    modules_path = repo_root / ".gitmodules"
    modules_text = modules_path.read_text()
    config = configparser.ConfigParser()
    config.read_string(modules_text)
    modules = {config[s]["path"]: config[s]["url"] for s in config.sections()}
    output = run(["git", "ls-tree", "-r", "HEAD", "contrib"], cwd=repo_root).stdout.decode()
    gitlinks = {}
    for line in output.splitlines():
        metadata, path = line.split("\t", 1)
        mode, obj_type, sha = metadata.split()
        if mode == "160000" and obj_type == "commit":
            gitlinks[path] = sha
    missing = sorted(set(modules) - set(gitlinks))
    if missing:
        raise EvidenceError(f"gitlinks missing for .gitmodules entries: {missing[:5]}")
    records = [{"path": path, "url": url, "sha": gitlinks[path]} for path, url in sorted(modules.items())]
    return records, modules_text


def container_evidence(image: str, binaries: dict[str, dict], output_dir: Path, required: bool) -> dict | None:
    if not shutil.which("docker"):
        if required:
            raise EvidenceError("docker is required for container verification")
        return None
    raw = run(["docker", "buildx", "imagetools", "inspect", image, "--raw"]).stdout
    (output_dir / "container-manifest.json").write_bytes(raw)
    manifest_digest = "sha256:" + hashlib.sha256(raw).hexdigest()
    manifest = json.loads(raw)
    instances: dict[str, str] = {}
    for item in manifest.get("manifests", []):
        platform = item.get("platform", {})
        arch = platform.get("architecture")
        if arch in ARCHES and platform.get("os") == "linux":
            instances[arch] = item["digest"]
    if not instances and manifest.get("config"):
        instances["amd64"] = manifest_digest
    for arch in ARCHES:
        if arch not in instances:
            raise EvidenceError(f"container manifest has no linux/{arch} image")
        platform = f"linux/{ARCHES[arch]['docker_arch']}"
        run(["docker", "pull", "--platform", platform, image])
        container_id = run(["docker", "create", "--platform", platform, image]).stdout.decode().strip()
        destination = output_dir / f"clickhouse-container-{arch}"
        try:
            run(["docker", "cp", f"{container_id}:/usr/bin/clickhouse", str(destination)])
        finally:
            run(["docker", "rm", "-f", container_id])
        digest = sha256_file(destination)
        if digest != binaries[arch]["sha256"]:
            raise EvidenceError(
                f"container/package binary mismatch for {arch}: {digest} != {binaries[arch]['sha256']}"
            )
        destination.unlink()
    return {"image": image, "manifest_digest": manifest_digest, "platform_digests": instances}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", required=True)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--github-token", default=os.environ.get("GH_TOKEN", ""))
    parser.add_argument("--build-run-id", type=int)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--artifact-base-url")
    parser.add_argument("--container-image")
    parser.add_argument("--require-container", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if not TAG_RE.fullmatch(args.tag) or "/" not in args.repo:
        raise EvidenceError("invalid release tag or repository name")
    tag = args.tag if args.tag.startswith("v") else "v" + args.tag
    version = tag.removeprefix("v")
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    github = GitHub(args.repo, args.github_token)
    release = github.json(f"releases/tags/{tag}")
    commit = release.get("target_commitish", "")
    if not SHA_RE.fullmatch(commit):
        commit = run(["git", "rev-list", "-n", "1", tag], cwd=args.repo_root).stdout.decode().strip()
    checkout_commit = run(["git", "rev-parse", "HEAD"], cwd=args.repo_root).stdout.decode().strip()
    if checkout_commit != commit:
        raise EvidenceError(f"checkout is {checkout_commit}, release {tag} resolves to {commit}")

    base_url = args.artifact_base_url or (
        f"https://altinity-build-artifacts.s3.amazonaws.com/REFs/{tag}/{commit}"
    )
    artifacts: list[dict] = []
    binaries: dict[str, dict] = {}
    extractors = {"deb": extract_deb_binary, "rpm": extract_rpm_binary, "tgz": extract_tgz_binary}
    for arch, arch_meta in ARCHES.items():
        names = {
            "deb": f"clickhouse-common-static_{version}_{arch}.deb",
            "rpm": f"clickhouse-common-static-{version}.{arch_meta['rpm_arch']}.rpm",
            "tgz": f"clickhouse-common-static-{version}-{arch}.tgz",
        }
        observed_binary_hashes = set()
        first_binary: Path | None = None
        for fmt, name in names.items():
            package = output_dir / name
            url = f"{base_url}/build_{arch_meta['build']}_release/{name}"
            download(url, package)
            extracted = output_dir / f".{name}.clickhouse"
            binary_hash, binary_size = extractors[fmt](package, extracted)
            observed_binary_hashes.add(binary_hash)
            if first_binary is None:
                first_binary = output_dir / f"clickhouse-{arch}"
                extracted.replace(first_binary)
            else:
                extracted.unlink()
            artifacts.append({
                "channel": "on-prem/public release",
                "architecture": arch,
                "format": fmt,
                "file_name": name,
                "url": url,
                "sha256": sha256_file(package),
                "size": package.stat().st_size,
                "clickhouse_sha256": binary_hash,
            })
        if len(observed_binary_hashes) != 1 or first_binary is None:
            raise EvidenceError(f"DEB/RPM/TGZ do not contain one identical {arch} executable")
        binaries[arch] = {
            "path": first_binary.name,
            "sha256": next(iter(observed_binary_hashes)),
            "size": binary_size,
            "build_id": build_id(first_binary),
        }

    license_count = collect_licenses(output_dir / binaries["amd64"]["path"], output_dir / "system-licenses.jsonl")
    run_id, jobs = find_master_run(github, commit, args.build_run_id)
    builds = collect_build_components(github, jobs, output_dir)
    modules, modules_text = git_source_evidence(args.repo_root)
    (output_dir / ".gitmodules").write_text(modules_text)
    image = args.container_image or f"altinity/clickhouse-server:{version}"
    container = container_evidence(image, binaries, output_dir, args.require_container)

    evidence = {
        "schema_version": 1,
        "repository": args.repo,
        "tag": tag,
        "version": version,
        "commit": commit,
        "release_url": release["html_url"],
        "release_published_at": release.get("published_at"),
        "build_run_id": run_id,
        "build_run_url": f"https://github.com/{args.repo}/actions/runs/{run_id}",
        "artifact_base_url": base_url,
        "artifacts": artifacts,
        "binaries": binaries,
        "container": container,
        "builds": builds,
        "gitmodules": modules,
        "embedded_license_records": license_count,
    }
    (output_dir / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(json.dumps({
        "release": tag,
        "commit": commit,
        "artifacts": len(artifacts),
        "embedded_license_records": license_count,
        "build_run_id": run_id,
        "container_verified": container is not None,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except EvidenceError as exc:
        print(f"evidence collection failed: {exc}", file=sys.stderr)
        raise SystemExit(1)

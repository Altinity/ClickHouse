---
description: 'Implementation plan for updating every active RustFS test pin from 1.0.0-beta.12 to 1.0.0-rc.3 and validating the stateless, integration, and soak consumers'
sidebar_label: 'RustFS 1.0.0-rc.3 migration plan'
sidebar_position: 8
slug: /superpowers/plans/rustfs-rc3-migration
title: 'RustFS 1.0.0-rc.3 migration implementation plan'
doc_type: 'plan'
---

# RustFS 1.0.0-rc.3 Migration Implementation Plan {#rustfs-rc3-migration-implementation-plan}

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move every active ClickHouse RustFS test consumer from `1.0.0-beta.12` to
`1.0.0-rc.3`, correct the live release-hygiene record, and validate all three consuming lanes.

**Architecture:** Keep the existing repository convention: the stateless launcher owns its literal
binary version and every Compose file owns its literal image tag. Make the eleven active pin edits
mechanically, retain the scanner/heal settings, and update only live comments and status documents;
historical measurement records remain immutable.

**Tech Stack:** Python 3, Docker Compose, RustFS, Praktika integration runner, Bash, AWS CLI.

**Spec:** `docs/superpowers/specs/2026-08-25-rustfs-rc3-migration-design.md`

## Global constraints {#global-constraints}

- The target release is exactly `1.0.0-rc.3`; the previous active release is exactly
  `1.0.0-beta.12`.
- Keep all eleven operational pins literal and self-contained. Do not add a shared env file,
  symlink, generator, updater, digest, or permanent anti-duplication check.
- Keep `RUSTFS_SCANNER_ENABLED=false` and `RUSTFS_HEAL_ENABLED=false` everywhere they are currently
  set.
- Do not change
  `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md`; its `beta.12`
  image is historical evidence.
- Do not change the behavior or wiring of `utils/ca-soak/scripts/orphan_reaper.sh`; remove obsolete
  release- and issue-specific prose only.
- Redirect every test command to a unique `build/test_rustfs_rc3_*.log` file and dispatch a subagent
  to return a concise analysis of each log before proceeding.
- Do not rebase or amend. Add new commits only, and never commit unrelated dirty-worktree files.

---

### Task 1: Update all active RustFS pins {#task-1-update-all-active-rustfs-pins}

**Files:**

- Modify: `ci/jobs/scripts/clickhouse_proc.py:168`
- Modify: `tests/integration/compose/docker_compose_rustfs.yml:6`
- Modify: `utils/ca-soak/docker-compose.yml:13`
- Modify: `utils/ca-soak/docker-compose-tuned.yml:18`
- Modify: `utils/ca-soak/docker-compose-10replicas.yml:24`
- Modify: `utils/ca-soak/docker-compose-gc_shards2.yml:10`
- Modify: `utils/ca-soak/docker-compose-gc_shards8.yml:10`
- Modify: `utils/ca-soak/docker-compose-multidisk.yml:11`
- Modify: `utils/ca-soak/docker-compose-s38.yml:20`
- Modify: `utils/ca-soak/docker-compose-s3faultproxy.yml:32`
- Modify: `utils/ca-soak/docker-compose-s41.yml:15`

**Interfaces:**

- Consumes: the upstream release tag `1.0.0-rc.3`, the Docker repository `rustfs/rustfs`, and the
  GitHub binary naming convention
  `rustfs-linux-{x86_64,aarch64}-musl-v{RUSTFS_VERSION}.zip`.
- Produces: eleven active consumers selecting the same RustFS release without changing how any
  consumer receives configuration.

- [ ] **Step 1: Capture the failing pre-migration inventory**

Run this from the repository root:

```bash
rg -n '1\.0\.0-beta\.12' \
  ci/jobs/scripts/clickhouse_proc.py \
  tests/integration/compose/docker_compose_rustfs.yml \
  utils/ca-soak/docker-compose*.yml \
  > build/test_rustfs_rc3_pre_migration_inventory.log 2>&1
test "$(wc -l < build/test_rustfs_rc3_pre_migration_inventory.log)" -eq 11
```

Expected: the assertion passes with exactly eleven old active pins. Dispatch a subagent to verify
that the log contains one Python pin, one integration pin, and nine soak pins.

- [ ] **Step 2: Replace the Python release and all ten image tags**

Use `apply_patch` to make only these substitutions:

```python
RUSTFS_VERSION = "1.0.0-rc.3"
```

```yaml
image: rustfs/rustfs:1.0.0-rc.3
```

The YAML replacement occurs once in the integration Compose file and once in each of the nine soak
Compose files listed above. Do not replace historical prose or add interpolation syntax.

- [ ] **Step 3: Verify the active inventory after the edit**

```bash
{
  if rg -n '1\.0\.0-beta\.12' \
      ci/jobs/scripts/clickhouse_proc.py \
      tests/integration/compose/docker_compose_rustfs.yml \
      utils/ca-soak/docker-compose*.yml
  then
    echo 'old active RustFS pin remains'
    exit 1
  fi
  rg -n '1\.0\.0-rc\.3' \
    ci/jobs/scripts/clickhouse_proc.py \
    tests/integration/compose/docker_compose_rustfs.yml \
    utils/ca-soak/docker-compose*.yml
} > build/test_rustfs_rc3_active_inventory.log 2>&1
test "$(wc -l < build/test_rustfs_rc3_active_inventory.log)" -eq 11
```

Expected: exactly eleven `rc.3` lines and no `beta.12` line. Dispatch a subagent to classify the
eleven lines by consumer.

- [ ] **Step 4: Render every affected Compose configuration**

```bash
{
  docker compose \
    -f tests/integration/compose/docker_compose_rustfs.yml \
    config --quiet
  (
    cd utils/ca-soak
    for compose_file in \
      docker-compose.yml \
      docker-compose-tuned.yml \
      docker-compose-10replicas.yml \
      docker-compose-gc_shards2.yml \
      docker-compose-gc_shards8.yml \
      docker-compose-multidisk.yml \
      docker-compose-s38.yml \
      docker-compose-s3faultproxy.yml \
      docker-compose-s41.yml
    do
      docker compose -f "${compose_file}" config --quiet
    done
  )
} > build/test_rustfs_rc3_compose_config.log 2>&1
```

Expected: exit code 0 with no missing-variable or invalid-Compose error. Dispatch a subagent to
analyze the log before committing.

- [ ] **Step 5: Commit the operational pin migration**

```bash
git add \
  ci/jobs/scripts/clickhouse_proc.py \
  tests/integration/compose/docker_compose_rustfs.yml \
  utils/ca-soak/docker-compose.yml \
  utils/ca-soak/docker-compose-tuned.yml \
  utils/ca-soak/docker-compose-10replicas.yml \
  utils/ca-soak/docker-compose-gc_shards2.yml \
  utils/ca-soak/docker-compose-gc_shards8.yml \
  utils/ca-soak/docker-compose-multidisk.yml \
  utils/ca-soak/docker-compose-s38.yml \
  utils/ca-soak/docker-compose-s3faultproxy.yml \
  utils/ca-soak/docker-compose-s41.yml
git diff --cached --check
git commit -m "ci: update RustFS test backend to rc3"
```

### Task 2: Correct live comments and release-hygiene records {#task-2-correct-live-comments-and-release-hygiene-records}

**Files:**

- Modify: `utils/ca-soak/configs/rustfs.env:1-17`
- Modify: `utils/ca-soak/scripts/orphan_reaper.sh:1-15`
- Modify: `utils/ca-soak/docker-compose.yml:23-26`
- Modify: `utils/ca-soak/docker-compose-tuned.yml:28-31`
- Modify: `utils/ca-soak/docker-compose-s3faultproxy.yml:42-45`
- Modify: `docs/superpowers/cas/final-checks-todo.md:139-151`
- Modify: `docs/superpowers/cas/opus-review-triage.md:107,176,1672-1677`

**Interfaces:**

- Consumes: the current operational decision to keep scanner and heal disabled in the single-disk
  ephemeral fixture.
- Produces: concise current comments with obsolete history removed, plus release records
  marking the beta-pin finding closed.

- [ ] **Step 1: Simplify `rustfs.env` without changing runtime variables**

Delete the historical experiment and upstream-issue paragraphs. Leave only this current operational
comment:

```text
# RustFS environment for the CA soak pool.
# Scanner remains OFF because scanner/auto-heal namespace locks produced multi-minute
# `503 ServiceUnavailable` bursts in this single-disk ephemeral fixture. Heal remains OFF because
# the fixture has no redundant disk to repair.
```

Leave these assignments byte-for-byte unchanged:

```dotenv
RUSTFS_SCANNER_ENABLED=false
RUSTFS_HEAL_ENABLED=false
RUSTFS_ACCESS_KEY=clickhouse
RUSTFS_SECRET_KEY=clickhouse
```

- [ ] **Step 2: Delete obsolete reaper prose without changing behavior**

Change comments only. In `orphan_reaper.sh`, delete the introductory lines that name the affected
release or upstream issue; retain the safety contract, usage, and implementation unchanged. In the
three Compose files, delete the complete orphan-reaper comment blocks. Do not replace deleted text
with new historical commentary.

- [ ] **Step 3: Close the beta-pin release-hygiene finding**

In `final-checks-todo.md`, replace the live beta warning with a resolved statement saying that the
stateless binary, integration Compose service, and all nine soak Compose files use
`1.0.0-rc.3`. State that literal tag pins were deliberately retained to match existing third-party
image practice.

In `opus-review-triage.md`:

- change the `m31` summary row from confirmed/untracked to closed by the `rc.3` migration;
- change the release-hygiene summary from an outstanding beta-image concern to a completed
  `1.0.0-rc.3` update;
- replace the detailed `m31` paragraph with the eleven-pin inventory, the corrected count of nine
  soak files, and the current `1.0.0-rc.3` state.

Do not retain the old tag or the original finding narrative in the replacement paragraph; Git
history records both. Do not touch the historical performance report.

- [ ] **Step 4: Verify comment accuracy and documentation formatting**

```bash
{
  rg -n '1\.0\.0-rc\.3|scanner|heal' \
    utils/ca-soak/configs/rustfs.env \
    utils/ca-soak/scripts/orphan_reaper.sh \
    docs/superpowers/cas/final-checks-todo.md \
    docs/superpowers/cas/opus-review-triage.md
  git diff --check -- \
    utils/ca-soak/configs/rustfs.env \
    utils/ca-soak/scripts/orphan_reaper.sh \
    utils/ca-soak/docker-compose.yml \
    utils/ca-soak/docker-compose-tuned.yml \
    utils/ca-soak/docker-compose-s3faultproxy.yml \
    docs/superpowers/cas/final-checks-todo.md \
    docs/superpowers/cas/opus-review-triage.md
} > build/test_rustfs_rc3_live_docs.log 2>&1
```

Expected: exit code 0 and no whitespace error. Dispatch a subagent to inspect the complete diff and
verify that active configuration comments contain only the current scanner/heal rationale.

- [ ] **Step 5: Commit the comments and release records**

```bash
git add \
  utils/ca-soak/configs/rustfs.env \
  utils/ca-soak/scripts/orphan_reaper.sh \
  utils/ca-soak/docker-compose.yml \
  utils/ca-soak/docker-compose-tuned.yml \
  utils/ca-soak/docker-compose-s3faultproxy.yml \
  docs/superpowers/cas/final-checks-todo.md \
  docs/superpowers/cas/opus-review-triage.md
git diff --cached --check
git commit -m "docs: close the RustFS beta pin finding"
```

### Task 3: Validate the native binary {#task-3-validate-the-native-binary}

**Files:**

- Create temporarily: `tmp/rustfs-rc3-native-smoke.sh`
- Test log: `build/test_rustfs_rc3_native_binary.log`

**Interfaces:**

- Consumes: the same musl release asset URL shape used by `download_rustfs` and local `curl` and
  `unzip` executables.
- Produces: evidence that the host-architecture binary starts with scanner and heal disabled.

- [ ] **Step 1: Create the native-binary smoke script under `tmp`**

Use `apply_patch` to create this untracked script:

```bash
#!/usr/bin/env bash
set -Eeuo pipefail

readonly VERSION='1.0.0-rc.3'
readonly PORT='19131'
mkdir -p tmp
readonly WORK_DIR="$(mktemp -d tmp/rustfs-rc3-native.XXXXXX)"
readonly MACHINE="$(uname -m)"

case "${MACHINE}" in
    x86_64) ARCH='x86_64' ;;
    aarch64|arm64) ARCH='aarch64' ;;
    *) echo "unsupported architecture: ${MACHINE}" >&2; exit 1 ;;
esac

readonly URL="https://github.com/rustfs/rustfs/releases/download/${VERSION}/rustfs-linux-${ARCH}-musl-v${VERSION}.zip"
curl -sSfL --retry 3 --retry-delay 5 -o "${WORK_DIR}/rustfs.zip" "${URL}"
unzip -q "${WORK_DIR}/rustfs.zip" -d "${WORK_DIR}"
chmod +x "${WORK_DIR}/rustfs"
mkdir "${WORK_DIR}/data"

RUSTFS_SCANNER_ENABLED=false \
RUSTFS_HEAL_ENABLED=false \
"${WORK_DIR}/rustfs" server \
    --address "127.0.0.1:${PORT}" \
    --access-key clickhouse \
    --secret-key clickhouse \
    "${WORK_DIR}/data" &
rustfs_pid=$!

cleanup()
{
    kill "${rustfs_pid}" 2>/dev/null || true
    wait "${rustfs_pid}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

for attempt in $(seq 1 120)
do
    status="$(curl --silent --output /dev/null --write-out '%{http_code}' "http://127.0.0.1:${PORT}/" || true)"
    if [[ "${status}" == '200' || "${status}" == '403' ]]
    then
        echo "READY version=${VERSION} arch=${ARCH} status=${status}"
        exit 0
    fi
    kill -0 "${rustfs_pid}"
    sleep 0.25
done

echo 'RustFS did not become ready' >&2
exit 1
```

- [ ] **Step 2: Run and analyze the native-binary smoke**

```bash
bash tmp/rustfs-rc3-native-smoke.sh \
  > build/test_rustfs_rc3_native_binary.log 2>&1
```

Expected: exit code 0 and one `READY version=1.0.0-rc.3` line. Dispatch a subagent to inspect the
entire log for download, startup, or scanner/heal errors.

### Task 4: Validate ClickHouse integration and soak consumers {#task-4-validate-clickhouse-integration-and-soak-consumers}

**Files:**

- Test: `tests/integration/test_cas_s3/test.py`
- Test configuration: `tests/integration/compose/docker_compose_rustfs.yml`
- Test configuration: `utils/ca-soak/docker-compose.yml`
- Test log: `build/test_rustfs_rc3_integration_cas_s3.log`
- Test log: `build/test_rustfs_rc3_soak_services.log`
- Test log: `build/test_rustfs_rc3_final_verification.log`

**Interfaces:**

- Consumes: the updated integration and soak image pins, the existing configured ClickHouse build,
  Praktika, and Docker Compose.
- Produces: end-to-end evidence that ClickHouse can mount, write, merge, restart, and drop a CAS S3
  table on `rc.3`, plus an isolated soak RustFS service that creates and exposes the expected bucket.

- [ ] **Step 1: Run the representative CAS S3 integration test**

```bash
python3 -m ci.praktika run "integration" --test test_cas_s3 \
  > build/test_rustfs_rc3_integration_cas_s3.log 2>&1
```

Expected: Praktika reports `test_cas_s3` passing. Dispatch a subagent to analyze the full log and
artifacts, including RustFS startup, CAS mount, insert/dedup, `OPTIMIZE`, restart, and drop.

- [ ] **Step 2: Create the isolated soak-service smoke script**

Use `apply_patch` to create `tmp/rustfs-rc3-soak-services.sh`:

```bash
#!/usr/bin/env bash
set -Eeuo pipefail

readonly PROJECT="rustfs-rc3-soak-smoke-$$"
readonly COMPOSE_FILE='utils/ca-soak/docker-compose.yml'

cleanup()
{
    docker compose --project-name "${PROJECT}" -f "${COMPOSE_FILE}" down -v \
        >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

docker compose \
    --project-name "${PROJECT}" \
    -f "${COMPOSE_FILE}" \
    up --abort-on-container-exit --exit-code-from createbucket rustfs1 createbucket

echo "SOAK_SERVICES_OK project=${PROJECT} image=rustfs/rustfs:1.0.0-rc.3 bucket=test"
```

The unique project name prevents this smoke from touching any existing `ca-soak` stack.

- [ ] **Step 3: Run and analyze the isolated soak-service smoke**

```bash
bash tmp/rustfs-rc3-soak-services.sh \
  > build/test_rustfs_rc3_soak_services.log 2>&1
```

Expected: exit code 0, successful `mc mb`/`mc ls`, and one `SOAK_SERVICES_OK` line. Dispatch a
subagent to inspect the full log and verify that the unique Compose project was removed while any
pre-existing `ca-soak` project remained untouched.

- [ ] **Step 4: Run the final scoped verification**

```bash
{
  git diff --check -- \
    ci/jobs/scripts/clickhouse_proc.py \
    tests/integration/compose/docker_compose_rustfs.yml \
    utils/ca-soak/docker-compose*.yml \
    utils/ca-soak/configs/rustfs.env \
    utils/ca-soak/scripts/orphan_reaper.sh \
    docs/superpowers/cas/final-checks-todo.md \
    docs/superpowers/cas/opus-review-triage.md
  echo 'active rc3 pins:'
  rg -n '1\.0\.0-rc\.3' \
    ci/jobs/scripts/clickhouse_proc.py \
    tests/integration/compose/docker_compose_rustfs.yml \
    utils/ca-soak/docker-compose*.yml
  echo 'remaining beta12 references:'
  rg -n '1\.0\.0-beta\.12' \
    ci/jobs/scripts/clickhouse_proc.py \
    tests/integration/compose/docker_compose_rustfs.yml \
    utils/ca-soak/docker-compose*.yml \
    docs/superpowers/cas/final-checks-todo.md \
    docs/superpowers/cas/opus-review-triage.md \
    || true
  rg -n '1\.0\.0-beta\.12' \
    docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md
  docker ps --format '{{.Names}} {{.Image}} {{.Status}}' \
    | rg 'rustfs-rc3-soak-smoke' \
    && exit 1 \
    || true
} > build/test_rustfs_rc3_final_verification.log 2>&1
```

Expected: eleven active `rc.3` pins, no `beta.12` reference in active configuration or live
release-hygiene records, the unchanged historical performance-report reference, no whitespace
error, and no temporary smoke container. Dispatch a subagent to distinguish the allowed historical
reference from accidental operational pins.

- [ ] **Step 5: Report the completed migration**

Report:

- the two implementation commit hashes;
- the eleven updated active pins;
- the unchanged scanner/heal policy;
- the native binary, integration, and isolated soak results with their build-log paths;
- the intentional historical `beta.12` reference retained in the performance report;
- the fact that no digest, shared version source, guard-test, or reaper behavior change was
  introduced.

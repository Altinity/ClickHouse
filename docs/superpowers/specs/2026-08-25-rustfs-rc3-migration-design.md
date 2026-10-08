---
description: 'Design for updating every active ClickHouse RustFS test pin from 1.0.0-beta.12 to 1.0.0-rc.3 without introducing a RustFS-specific version-management mechanism'
sidebar_label: 'RustFS 1.0.0-rc.3 migration'
sidebar_position: 8
slug: /superpowers/specs/rustfs-rc3-migration-design
title: 'RustFS 1.0.0-rc.3 migration'
doc_type: 'design'
---

# RustFS 1.0.0-rc.3 migration {#rustfs-rc3-migration-design}

**Status:** APPROVED, rev.2 (2026-08-25).

This specification updates the RustFS release used by ClickHouse test infrastructure from
`1.0.0-beta.12` to `1.0.0-rc.3`. RustFS is not shipped with ClickHouse; it supplies the
S3-compatible backend used by the CAS stateless, integration, and soak test lanes.

The migration is deliberately mechanical. ClickHouse integration Compose files normally pin
third-party images with literal tags, downloaded test binaries normally keep their release in the
launching script, and the repository has no shared version manifest or per-image anti-duplication
check. This change follows those conventions instead of introducing a RustFS-specific
version-management layer.

## Decision {#decision}

Replace every active `1.0.0-beta.12` pin with `1.0.0-rc.3` in one change and leave every Compose
file self-contained. Do not add a digest pin, shared env file, symlink, generated Compose file,
updater script, or permanent guard against literal RustFS tags.

The migration covers eleven active pins:

1. `ci/jobs/scripts/clickhouse_proc.py` — the release used to construct the GitHub download URL for
   the stateless-lane RustFS binary.
2. `tests/integration/compose/docker_compose_rustfs.yml` — the integration-test image.
3. Nine `utils/ca-soak/docker-compose*.yml` files — the default soak stack and its eight variants.

The exact soak files are:

- `utils/ca-soak/docker-compose.yml`;
- `utils/ca-soak/docker-compose-tuned.yml`;
- `utils/ca-soak/docker-compose-10replicas.yml`;
- `utils/ca-soak/docker-compose-gc_shards2.yml`;
- `utils/ca-soak/docker-compose-gc_shards8.yml`;
- `utils/ca-soak/docker-compose-multidisk.yml`;
- `utils/ca-soak/docker-compose-s38.yml`;
- `utils/ca-soak/docker-compose-s3faultproxy.yml`;
- `utils/ca-soak/docker-compose-s41.yml`.

Future RustFS updates will continue to touch these locations. That repetition is accepted because
updates are rare, each Compose file remains independently usable, and the alternative would create
a dependency-management convention that no other third-party image in this repository follows.

## Target artifacts {#target-artifacts}

The target release exists as both artifacts used by ClickHouse:

- the multi-architecture Docker image `rustfs/rustfs:1.0.0-rc.3`, with `linux/amd64` and
  `linux/arm64` variants;
- the GitHub release binaries named
  `rustfs-linux-{x86_64,aarch64}-musl-v1.0.0-rc.3.zip`.

## Runtime settings and comments {#runtime-settings-and-comments}

Keep `RUSTFS_SCANNER_ENABLED=false` and `RUSTFS_HEAL_ENABLED=false` in both CI and Compose
configurations. The scanner was disabled because scanner/auto-heal namespace locks caused
multi-minute `503 ServiceUnavailable` bursts in the single-disk ephemeral fixture; `rc.3` has not
been qualified with those components enabled. Heal remains unnecessary for that fixture.

Delete obsolete release and upstream-issue history from active comments instead of rewriting it.
Keep only the current operational reason for disabling scanner and heal.

Do not change the behavior or wiring of `utils/ca-soak/scripts/orphan_reaper.sh` as part of this
migration. Delete its release- and issue-specific introductory prose while preserving its safety
contract and usage documentation. Delete the false reaper-wiring comments from active Compose
files instead of replacing them.

## Documentation updates {#documentation-updates}

Update the live release-hygiene records so they no longer report a beta pin:

- `docs/superpowers/cas/final-checks-todo.md` records that all active RustFS consumers now use
  `1.0.0-rc.3`;
- `docs/superpowers/cas/opus-review-triage.md` marks `m31` closed by the migration, corrects the soak
  count from eight to nine, and records the resulting active pins.

Do not rewrite `docs/superpowers/cas/2026-08-22-unconditional-blob-publication-performance.md`.
Its `beta.12` tag and image ID describe the environment in which that historical measurement was
actually collected.

## Validation {#validation}

Validation has three layers:

1. **Static inventory.** Search tracked source and active configuration for `1.0.0-beta.12`. The
   only allowed remaining occurrence is the historical performance report and any prose that
   explicitly compares old releases. Search active RustFS pins and verify that all eleven resolve
   to `1.0.0-rc.3`.
2. **Configuration and artifact smoke.** Render the integration Compose file and all nine soak
   Compose files with `docker compose config`. Verify the stateless download URL constructed for
   the host architecture and start the downloaded `rc.3` binary with scanner and heal disabled.
3. **Behavior smoke.** Run one representative CAS integration test against the integration RustFS
   service and bring up the default soak RustFS plus bucket-creation services.

All test output is redirected to uniquely named files under the configured build directory, and a
subagent summarizes each log as required by the repository instructions.

## Non-goals {#non-goals}

- No RustFS behavior change in ClickHouse production code.
- No digest pin, binary checksum manifest, dependency bot, updater script, or generic third-party
  image-version framework.
- No scanner or auto-heal re-enablement.
- No behavior or wiring change for the existing orphan-reaper script.
- No rewrite of historical benchmark environments.

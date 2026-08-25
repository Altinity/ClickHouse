---
description: 'Design for updating every active ClickHouse RustFS test pin from 1.0.0-beta.12 to 1.0.0-rc.3 without introducing a RustFS-specific version-management mechanism'
sidebar_label: 'RustFS 1.0.0-rc.3 migration'
sidebar_position: 8
slug: /superpowers/specs/rustfs-rc3-migration-design
title: 'RustFS 1.0.0-rc.3 migration'
doc_type: 'design'
---

# RustFS 1.0.0-rc.3 migration {#rustfs-rc3-migration-design}

**Status:** DRAFT for review (2026-08-25).

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

## Compatibility evidence {#compatibility-evidence}

The target release exists as both artifacts used by ClickHouse:

- the multi-architecture Docker image `rustfs/rustfs:1.0.0-rc.3`, with `linux/amd64` and
  `linux/arm64` variants;
- the GitHub release binaries named
  `rustfs-linux-{x86_64,aarch64}-musl-v1.0.0-rc.3.zip`.

The direct unversioned-overwrite probe has already compared `beta.7`, `beta.12`, and `rc.3` with
200 overwrites of one 512 KiB key. `beta.7` retained 200 live UUID directories, while both
`beta.12` and `rc.3` retained one live UUID directory and produced the correct final payload. The
overwrite leak therefore was fixed before `beta.12`; upgrading to `rc.3` neither introduces nor
removes that behavior.

Both `beta.12` and `rc.3` moved the replaced bodies into `.trash` during the running process and
removed that trash on the next startup even with `RUSTFS_SCANNER_ENABLED=false` and
`RUSTFS_HEAL_ENABLED=false`. This proves startup cleanup only. It does not prove periodic cleanup
for a long-lived process, so the version migration must not claim that scanner-disabled long runs
have a newly verified periodic cleanup guarantee.

## Runtime settings and the legacy reaper {#runtime-settings-and-legacy-reaper}

Keep `RUSTFS_SCANNER_ENABLED=false` and `RUSTFS_HEAL_ENABLED=false` in both CI and Compose
configurations. The scanner was disabled because scanner/auto-heal namespace locks caused
multi-minute `503 ServiceUnavailable` bursts in the single-disk ephemeral fixture; `rc.3` has not
been qualified with those components enabled. Heal remains unnecessary for that fixture.

Update comments that describe rustfs/rustfs#3231 as an open `beta.7+` defect or imply that raising
the RustFS version still waits on an upstream fix. Preserve the historical observation that the
scanner did not reclaim the old leaked layout during the experiment that motivated disabling it.

Do not delete or wire `utils/ca-soak/scripts/orphan_reaper.sh` as part of this migration. It is a
legacy, currently unwired mitigation, and deciding whether to remove it requires a separate audit
of long-lived cleanup rather than the startup-only evidence available here. Comments in active
Compose files must not claim that `run_24h.sh` launches it when the script contains no such launch.

## Documentation updates {#documentation-updates}

Update the live release-hygiene records so they no longer report a beta pin:

- `docs/superpowers/cas/final-checks-todo.md` records that all active RustFS consumers now use
  `1.0.0-rc.3`;
- `docs/superpowers/cas/opus-review-triage.md` marks `m31` closed by the migration, corrects the soak
  count from eight to nine, and records the deliberate decision to retain ordinary tag pins rather
  than add a digest or single-source mechanism.

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
   service, bring up the default soak RustFS plus bucket-creation services, and repeat the direct
   unversioned-overwrite probe against `rc.3`. The final payload must match and exactly one live UUID
   directory must remain.

All test output is redirected to uniquely named files under the configured build directory, and a
subagent summarizes each log as required by the repository instructions.

## Non-goals {#non-goals}

- No RustFS behavior change in ClickHouse production code.
- No new direct-overwrite regression test in the ClickHouse repository; the overwrite probe remains
  migration evidence rather than product behavior owned by ClickHouse.
- No digest pin, binary checksum manifest, dependency bot, updater script, or generic third-party
  image-version framework.
- No scanner or auto-heal re-enablement.
- No removal of the legacy orphan reaper without a separate long-lived cleanup audit.
- No rewrite of historical benchmark environments.

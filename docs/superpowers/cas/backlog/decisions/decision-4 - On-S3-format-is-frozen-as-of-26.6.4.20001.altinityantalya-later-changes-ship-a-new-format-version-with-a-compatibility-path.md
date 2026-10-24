---
id: decision-4
title: >-
  On-S3 format is frozen as of 26.6.4.20001.altinityantalya; later changes ship
  a new format version with a compatibility path
date: '2026-09-26 06:35'
status: accepted
---
## Context

Until 2026-09-25 the branch treated the on-S3 layout as pre-release and allowed breaking changes without compatibility code.

## Decision

Release `26.6.4.20001.altinityantalya` is the first fixed format version. Every later change to manifests, ref logs, checkpoints, `gc/state`, snapshots or key layout ships a new format version and a compatibility path (read old, write new, documented upgrade order).

## Consequences

Items tagged `touches:on-s3-format` need a version bump and a migration plan before implementation. AGENTS.md rule 7 states the same.

---
id: decision-1
title: HEAD-before-PUT on blobs stays; protocol-step optimisations are vetoed
date: '2026-09-26 06:35'
status: accepted
---
## Context

Every blob publication does a `HEAD` before the `PUT` so that an existing object is never overwritten and the writer learns the incarnation it is racing. Several proposals tried to skip or merge that step to save a request.

## Decision

The `HEAD`-before-`PUT` protocol step on blobs stays. Optimisations that remove or reorder protocol steps are vetoed by the owner (2026-08); saving requests is done elsewhere (writer reads, repoints, GC).

## Consequences

Backlog items that propose skipping the `HEAD` are closed as contradicting this decision. The PUT-only publish decision (decision-5) concerns catalog and `_ckpt` reads, not the blob `HEAD`.

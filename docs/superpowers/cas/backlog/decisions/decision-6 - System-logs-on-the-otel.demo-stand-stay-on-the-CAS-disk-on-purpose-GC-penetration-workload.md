---
id: decision-6
title: >-
  System logs on the otel.demo stand stay on the CAS disk on purpose (GC
  penetration workload)
date: '2026-09-26 06:35'
status: accepted
---
## Context

otel.demo (`chi-otel-otel-0-0`, 2 replicas, CAS as the default disk) writes ~143k parts/day, 86% of them `system.*` log tables on the CAS disk.

## Decision

The stand's purpose is to penetrate CAS and GC with tiny-part churn; the system logs stay on the CAS disk on purpose (owner, 2026-09-25).

## Consequences

Findings about that workload become production docs (recommend a local storage policy for `system.*` logs), never a stand change.

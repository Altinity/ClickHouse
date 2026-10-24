---
id: decision-2
title: 'S3 LIST trust: cold recovery LIST is trusted, hot LIST is not; do not re-argue'
date: '2026-09-26 06:35'
status: accepted
---
## Context

S3 `LIST` is eventually consistent on some providers. The ref-ledger recovery walks the log with a cold `LIST`; hot-path code was tempted to use `LIST` as a journal.

## Decision

A cold recovery `LIST` (after the writer is fenced) is trusted; a hot `LIST` while writers are live is not, and no acked-floor state is introduced to make it so. Recorded in `docs/superpowers/cas/2026-08-03-list-trust-verdict.md`; do not re-argue.

## Consequences

Designs that rely on hot `LIST` results are rejected at review. GC discovery in spec 2026-09-25 replaces the global `LIST` with per-life probes, consistent with this.

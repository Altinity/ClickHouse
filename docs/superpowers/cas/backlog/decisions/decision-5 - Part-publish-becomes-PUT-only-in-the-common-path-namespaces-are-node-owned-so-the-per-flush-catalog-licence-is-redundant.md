---
id: decision-5
title: >-
  Part publish becomes PUT-only in the common path; namespaces are node-owned so
  the per-flush catalog licence is redundant
date: '2026-09-26 06:35'
status: accepted
---
## Context

A part publish paid five S3 reads (two catalog `GET`s, two `_ckpt` `GET`s, one manifest re-read), 125 ms of 389 ms; the ref lane wait was 748 ms of an 887 ms insert on otel.demo (audit F30/F31).

## Decision

Namespaces are node-owned (`<server_root_id>/store/<uuid>@cas@`), so the per-flush catalog licence and the `_ckpt` read-modify-write are redundant: a publish becomes PUT-only in the common path; `promote` re-reads the manifest only after an `Unresolved` PUT; the part-folder view is seeded from the staged bytes. Verified 2026-09-25: the only Live->Removing catalog write is `CasRefCatalog::beginRemoving`, reachable from the node's own drop path or from `CasDecommission` after it claims the victim's lease.

## Consequences

Milestone M3. The blob `HEAD` (decision-1) is unaffected.

---
id: m-2
title: "M3 Write path with zero GETs"
---

## Description

Owner decision 2026-09-25: a part publish becomes PUT-only in the common path (drop per-flush catalog GET and _ckpt GET, manifest re-read only after an Unresolved PUT); plus delete_tmp repoint elision (audit F2). Audit docs/superpowers/reports/2026-09-25-otel-demo-cas-s3-budget-audit.md F30/F31.

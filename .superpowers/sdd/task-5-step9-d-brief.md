# Step 9 Slice D — zero-survivor audit

## Objective

Prove the retired namespace-cleanup marker/snapshot handshake has no surviving executable surface. This is a mechanical audit after Step 9 A/B/C; do not redesign runtime, GC, janitor, catalog, or wire behavior.

## Exact retired identifiers

Audit the whole tracked tree for these exact identifiers:

- `namespaceIsRemoved`
- `refCleanupMarkerKey`
- `RefNsCleanupState`
- `ns_cleanup_items`
- `cleanup_markers`
- `publishRemovedSnapshotNow`
- `runNamespaceCleanupPasses`

Also inspect literal `/_cleanup/` and `_cleanup`, but classify rather than blindly delete.

## Allowed survivors

- Historical design/plan/review prose under `docs/superpowers/**` that explicitly describes the retired alternative or its deletion.
- Negative tests that assert no `/_cleanup/` object is written.
- Legacy/parser tests that deliberately construct an old-format `_cleanup` key and assert rejection.
- `_cleanup` inside unrelated ClickHouse concepts or generic cleanup names.
- GC phase name `namespace_cleanup`, which names the current perpetual janitor phase and is not the retired marker.

Every allowed survivor must be classified in the report with file:line and reason. A bare grep count is not evidence.

## Required changes

- Delete or rewrite stale production comments/contracts that imply any retired identifier, marker publication, marker-driven promotion, or durable `Removed` snapshot still exists.
- Delete obsolete positive tests/fixtures/helpers that exercise retired behavior. Preserve negative and legacy rejection pins.
- Do not edit authoritative historical prose merely to achieve zero grep; classify it.
- Do not touch `.superpowers/sdd/task-5-report.md`.

## Verification

- Run exact `rg` commands over tracked source/tests/docs and include outputs/classification in `.superpowers/sdd/task-5-step9-d-report.md`.
- If source/test files change, run the smallest relevant build and focused tests, redirecting to unique `build_debug/step9_d_*` logs; use independent log analysis.
- Run `git diff --check` on owned changes.
- Independent spec/quality review before commit.
- Commit only owned changes plus this brief/report. Do not switch branch, rebase, or amend. Preserve all shared dirty files and the aggregate report.

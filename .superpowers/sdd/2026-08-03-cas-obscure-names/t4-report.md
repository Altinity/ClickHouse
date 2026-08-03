# Task 4 — `system.cas_mounts`: `started_at_ms`/`expires_at_ms` → `started_at`/`expires_at`

## Enumeration

`grep -n 'started_at_ms\|expires_at_ms'` tree-wide (minus `docs/superpowers/`, `.superpowers/`,
`contrib/`): **77 hits across 16 files**. Only three are the column spelling:
`StorageSystemContentAddressedMounts.cpp` :47/:48, `05012_cas_mounts_typed_columns.sql`,
`cas_mounts.md` :31/:32. All three changed.

Every other hit is a place where `_ms` is **honest** and was left:

- the `MountLease` struct members (`CasServerRootFormats.h`) and the ~60 C++ reads/writes/comments of
  them — plain `uint64_t` epoch milliseconds;
- the persisted lease body keys, which are `"sat"` / `"eat"` anyway, not `started_at_ms`;
- `CasInspect.cpp` `.add("started_at_ms", jsonUInt(…))` — a `cas-inspect` JSON key carrying a raw
  millisecond integer, so the suffix describes the value correctly;
- the `CasEvent::detail` key `holder_expires_at_ms`, likewise a stringified millisecond integer;
- log-message field names such as `expires_at_ms={}` in `CasPool` / `CasServerRoot`, same reason.

The decision's rationale was specifically that the *column* is `DateTime64(3)`, so the `_ms` suffix
lied about the type. That rationale does not extend to any of the above, so none of them changed.

## Verification

- `05012_cas_mounts_typed_columns.reference` needs no edit and is still a real fence: it selects
  `type` for the three names ordered by name, and the alphabetical order of
  `expires_at`/`server_uuid`/`started_at` matches the old order of
  `expires_at_ms`/`server_uuid`/`started_at_ms`, so the reference lines are unchanged — but if either
  column name were missing the query would return two rows instead of three and the reference would
  mismatch.
- `grep -n 'started_at_ms\|expires_at_ms'` over `src/Storages/`, `tests/queries/`, `docs/en/`: two
  hits, both the internal `m.lease.started_at_ms` / `m.lease.expires_at_ms` struct reads that feed the
  renamed columns.
- `ninja -C build clickhouse unit_tests_dbms` → `NINJA_EXIT=0` (`build/obscure_t4_build.log`).

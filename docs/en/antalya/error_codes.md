---
description: 'Stable numeric identities for Antalya-specific errors and migration from legacy codes.'
sidebar_label: 'Error codes'
sidebar_position: 20
slug: /antalya/error-codes
title: 'Antalya error codes'
doc_type: 'reference'
---

# Antalya error codes {#antalya-error-codes}

Antalya-specific errors use a separate registry. Their numeric code is
`10000 + local_id`, where local IDs are positive, append-only, and never reused.
Upstream errors retain their existing numbers. Symbolic names remain in
`DB::ErrorCodes`; applications should prefer symbolic names over numeric codes
when possible.

The high range is an Antalya convention, not a reservation recognized by
upstream ClickHouse. Build-time checks reject duplicate Antalya IDs or names,
overlapping numeric ranges, and names shared with upstream errors. Wire codes
must fit in `UInt16` because
`system.part_log.error` and `system.background_schedule_pool_log.error` use that
type. Local IDs therefore cannot exceed `55535`; this limit is enforced at build
time. Adding a new error requires a new local ID in `src/Common/AntalyaErrorCodes.h`.

## Migration from legacy numbers {#migration-from-legacy-numbers}

| Name | Legacy code | New code |
|---|---:|---:|
| `CATALOG_NAMESPACE_DISABLED` | 779 | 10001 |
| `PENDING_MUTATIONS_NOT_ALLOWED` | 1009 | 10002 |
| `EXPORT_PARTITION_ALREADY_EXPORTED` | 1010 | 10003 |
| `PARTITION_EXPORT_FAILED` | 1011 | 10004 |
| `CAS_WRITE_UNATTRIBUTED` | 1037 | 10005 |
| `CAS_DELETE_MARKER` | 1038 | 10006 |

This is a numeric compatibility break. Update clients, monitoring rules, and
scripts that compare the legacy numbers. There are no legacy numeric aliases:
some legacy values identify different errors in upstream ClickHouse. Error
messages and symbolic names are unchanged.

## Client and mixed-version behavior {#client-and-mixed-version-behavior}

Exception packets retain their existing layout and signed 32-bit code field.
No protocol revision or peer negotiation is required. Antalya clients built
with this registry display the symbolic name. Older or upstream clients retain
the numeric code and message but may display an empty symbolic name.

Distributed-query forwarding preserves the numeric identity, including codes
unknown to the intermediate server. However, preserving the code does not make
older Antalya servers recognize it in code-specific handling, such as partition
export conflict handling. Mixed-version feature behavior is not guaranteed;
upgrade participating servers together when relying on Antalya-specific error
handling. New servers likewise do not reinterpret legacy numbers as new errors.

A shell exit status is only eight bits. Do not use `$?` to recover the full
numeric error code; inspect the client's error output instead. Query failure
continues to produce a nonzero exit status.

## Accounting {#accounting}

`system.errors`, `system.error_log`, and Prometheus error metrics include
registered Antalya errors. Counter storage grows with the number of registered
errors, not with the largest numeric code. Local and remote counters remain
separate. Unknown codes retain their identity in exceptions. Codes outside the
upstream array, unless registered by Antalya, share an unnamed out-of-range
accounting slot. Unassigned slots inside the upstream array keep their existing
accounting behavior. Unknown codes do not acquire a registered name or increment
an Antalya error's counters. The aggregated error log skips unnamed slots, as do
`system.errors` and Prometheus, rather than reporting the shared slot number as
an exception identity. `system.query_log.exception_code` retains the original
numeric code.

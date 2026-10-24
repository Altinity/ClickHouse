---
description: 'Design for disabling dynamic CAS disks by default behind a server-owned safety gate'
sidebar_label: 'Dynamic CAS disk gate'
sidebar_position: 9
slug: /superpowers/specs/dynamic-cas-disk-gate
title: 'Dynamic CAS disk gate design'
doc_type: 'guide'
---

# Dynamic CAS disk gate design {#dynamic-cas-disk-gate-design}

## Problem {#problem}

The SQL expression `disk(type=object_storage, metadata_type=cas, ...)` creates a dynamic disk through
`DiskFromAST`. A dynamic disk is not limited to the table that first mentions it: `Context` caches it
for the lifetime of the server process. A `CAS` disk also joins a process-wide storage pool, may use
credentials owned by the server, and starts background work.

Today the `object_storage` disk factory ignores its `custom_disk` argument. Consequently, permission
to create a table is sufficient to create a persistent process-wide `CAS` pool member. This bypasses
the operator-controlled boundary used by disks declared in `storage_configuration` and is outside the
scope of the `SYSTEM CAS` privilege model.

## Decision {#decision}

Add the top-level server setting `cas_allow_unsafe_dynamic_disks`, with a default of `false`.

When the `object_storage` disk factory receives `custom_disk=true` and the explicit
`metadata_type=cas`, it checks this setting before creating object-storage clients, local pool or
scratch directories, metadata storage, or background tasks. When the setting is false, construction
fails with `BAD_ARGUMENTS`. The exception explains that the disk joins a process-wide shared pool and
may use server credentials, recommends defining the disk in `storage_configuration`, and names the
opt-in setting.

The check applies uniformly whenever ClickHouse constructs the dynamic disk, including `CREATE`,
user-issued `ATTACH`, and loading table metadata during startup. It is deliberately not a user or
query setting: the server operator owns the credentials, process-wide pool membership, mount slot,
and background resources that the setting protects.

When `cas_allow_unsafe_dynamic_disks=true`, current dynamic `CAS` disk behavior remains unchanged.
Disks declared in `storage_configuration` have `custom_disk=false` and remain enabled regardless of
the new setting.

## Scope {#scope}

The gate recognizes the exact explicit value `metadata_type=cas`, matching
`MetadataStorageFactory`. It does not change dynamic disks with `local`, `plain`,
`plain_rewritable`, `web`, or `keeper` metadata. Expanding the policy to another metadata type
requires a separate security decision.

The setting is a normal non-changeable server setting. Changing it requires the same restart-bound
configuration workflow as other server settings that are not listed as changeable without restart.
No runtime teardown of already-cached dynamic disks is introduced.

## Testing and documentation {#testing-and-documentation}

A unit test calls `DiskFactory::create` with `custom_disk=true` and a valid local object-storage
`CAS` configuration. With the default setting it must receive `BAD_ARGUMENTS`; neither the pool nor
scratch directory may have been created. Those filesystem assertions pin the gate ahead of all disk
construction side effects. A paired test passes the same configuration with `custom_disk=false` and
must mount it successfully, proving that an operator-configured `CAS` disk does not require the
opt-in.

A separate one-node integration test starts ClickHouse without the opt-in and issues a real
`CREATE TABLE ... SETTINGS disk = disk(...)` query. It asserts `BAD_ARGUMENTS`, the setting name,
absence of the table, and absence of both configured pool and scratch directories. This test pins the
security-critical `DiskFromAST` wiring that supplies `custom_disk=true`; the factory unit test alone
cannot detect a regression in that call site.

The shared stateless test-server configuration explicitly sets
`cas_allow_unsafe_dynamic_disks=true`. Inline `CAS` disks occur in general stateless jobs as well as
the dedicated CAS lanes, so a single version-gated config fragment is installed for every compatible
test binary. Existing inline-disk tests then provide positive coverage for the opt-in path without
editing each SQL test. A representative existing test is run locally together with the new negative
unit and integration tests. The complete `DiskObjectStorageTest` suite also runs so the shared
creator's existing non-CAS behavior remains covered.

The CAS configuration guide lists the setting and warns against enabling it on multi-user servers.
It directs operators to `storage_configuration` as the supported deployment boundary.

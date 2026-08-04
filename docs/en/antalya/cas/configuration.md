---
description: 'Every disk-level and server-level setting content-addressed storage exposes, generated from ContentAddressedSettings and ServerSettings at HEAD.'
sidebar_label: 'Configuration'
sidebar_position: 3
slug: /antalya/cas/configuration
title: 'CAS Configuration Reference'
doc_type: 'reference'
---

# Configuration reference {#configuration-reference}

## The disk config block {#disk-config}

A `CAS` disk is an `object_storage` disk with `metadata_type` set to `cas` and an explicit
`server_root_id`:

```xml
<clickhouse>
    <storage_configuration>
        <disks>
            <cas>
                <type>object_storage</type>
                <object_storage_type>s3</object_storage_type>
                <metadata_type>cas</metadata_type>
                <server_root_id>{replica}</server_root_id>
                <endpoint>https://bucket.s3.amazonaws.com/cas/</endpoint>
                <access_key_id>...</access_key_id>
                <secret_access_key>...</secret_access_key>
            </cas>
        </disks>
        <policies>
            <cas>
                <volumes>
                    <main>
                        <disk>cas</disk>
                    </main>
                </volumes>
            </cas>
        </policies>
    </storage_configuration>
</clickhouse>
```

`type`, `object_storage_type`, `metadata_type`, `endpoint`, `access_key_id`, `secret_access_key`,
and the other generic object-storage/disk keys (`path`, `name`, `region`,
`use_environment_credentials`, `readonly`, `use_fake_transaction`, and a handful more) belong to the
shared disk layer, not to `CAS` — they are accepted inside the same block but are not `CAS`
settings. Every key below this line, and every key not in that shared set, is rejected as unknown.

## Disk-level settings {#disk-settings}

Source: `ContentAddressedSettings.cpp` (`LIST_OF_CONTENT_ADDRESSED_SETTINGS`). None of these keys
carry a `cas_`/`ca_` prefix — the disk block already scopes them.

| Setting | Default | Description |
|---|---|---|
| `server_root_id` | — (required) | Explicit layout subtree identity; macros expand as in the `s3` `endpoint` |
| `scratch_path` | server data path | Server-local scratch dir for the write-buffer spill; a relative value is anchored to the server data path |
| `gc_enabled` | `true` | Run the background GC scheduler on this disk |
| `gc_interval_sec` | `60` | Seconds between background GC rounds (≥ 1) |
| `blob_hash` | `cityhash128` | Pool blob content-hash function (`cityhash128` \| `xxh3-128` \| `sha256`); fixed at pool creation |
| `blob_hash_allow_new` | `false` | Explicit opt-in to admit a new hash algorithm into an existing pool's `algos_used` |
| `skip_access_check` | `false` | Skip the boot-time capability probe (start now, fix later) |
| `deduplication_cache_bytes` | 64 MiB | Byte budget of the blob presence cache (`0` disables) |
| `deduplication_head_first_min_bytes` | 1 MiB | Minimum blob size to try a `HEAD` before uploading the body |
| `gc_snapshot_generations_to_keep` | `3` | GC snapshot generations retained |
| `gc_shards` | `1` | Blob-hash-prefix reducer shards (≥ 1); creation-time only |
| `manifest_sweep_list_budget_keys` | `1000` | Orphan-manifest sweep `LIST` budget per round |
| `manifest_sweep_delete_budget_keys` | `100` | Orphan-manifest sweep `DELETE` budget per round |
| `gc_round_graduation_budget` | `5000` | Blob graduation (condemned → delete-pending) cohort cap per round (`0` = unbounded) |
| `gc_round_redelete_budget` | `5000` | Blob redelete (exact-token delete of a prior delete-pending row) cohort cap per round (`0` = unbounded) |
| `gc_round_sweep_namespace_budget` | `20` | Orphan-manifest sweep: distinct namespaces per page whose protection view may be built (`0` = unbounded) |
| `gc_round_sweep_recovery_op_budget` | `5000` | Orphan-manifest sweep: committed-tail ref-log GET/decode ops the recovery walk may spend per round (`0` = unbounded) |
| `gc_round_ref_cleanup_budget` | `5000` | Ref-object cleanup (covered log/snapshot deletes) cap per round (`0` = unbounded) |
| `gc_round_prefix_wholesale_budget` | `20000` | Generation-prefix wholesale delete (prune only) object cap per round (`0` = unbounded) |
| `gc_round_handoff_prefix_wholesale_budget` | `5000` | Post-CAS hand-off generation-prefix reclaim object cap per round, reserved separately from `gc_round_prefix_wholesale_budget` so a prune-heavy round cannot starve the one-shot hand-off (`0` = unbounded) |
| `gc_round_outcome_entry_budget` | `5000` | GC outcomes per-round entry cap across the redelete/spared audit log (`0` = unbounded) |
| `gcs_max_conditional_put_bytes` | 1 GiB | GCS single-`PUT` budget for conditional writes (generation-token stores only) |
| `part_folder_cache_bytes` | 64 MiB | Part-folder view cache byte budget (`0` disables retention) |
| `part_folder_cache_max_entries` | `10000` | Part-folder view cache entry cap |
| `part_folder_cache_max_entry_bytes` | 16 MiB | Oversized part-folder views bypass retention above this size |
| `part_folder_validate` | `always` | `ForceFresh` body re-proof policy (`always` \| `never` \| `age <seconds>`) |
| `manifest_decode_cache_bytes` | 128 MiB | Manifest decode cache byte budget (`0` disables) |
| `gc_meta_pool_size` | `16` | Bounded pool size for GC per-hash freshness-meta writes |
| `staging_backend` | `local` | Blob staging backend (`local` \| `s3`); `s3` is opt-in |

## Server-level settings {#server-settings}

Source: `ServerSettings.cpp`. Unlike the disk-level list, these carry the `cas_` prefix because they
are process-wide, not scoped to one disk block.

| Setting | Default | Description |
|---|---|---|
| `cas_blob_upload_pool_size` | `16` | Size of the dedicated server-wide thread pool used to upload blobs in parallel when committing a `CAS` part. Zero is rejected: the pool must have at least one thread |
| `cas_condemned_upload_memory_bytes` | `0` | Aggregate memory budget for the one `CAS` upload branch that materializes a whole blob body in memory (resurrecting a condemned incarnation with a local source). `0` derives the budget from `cas_blob_upload_pool_size` times a 64 MiB per-task budget; a single resurrection exceeding the whole budget is still admitted alone rather than waiting forever |

## `SYSTEM CAS` commands {#system-commands}

`SYSTEM CAS GC RUN`, `SYSTEM CAS GC STOP`, `SYSTEM CAS GC START`, `SYSTEM CAS GC REBUILD`,
`SYSTEM CAS FSCK`, `SYSTEM CAS FORGET`, and `SYSTEM CAS DROP POOL MEMBER '<server_root_id>' FROM
DISK '<disk>'` operate on a mounted `CAS` disk. Introspection lives in `system.cas_log`,
`system.cas_gc_log`, and `system.cas_mounts`.

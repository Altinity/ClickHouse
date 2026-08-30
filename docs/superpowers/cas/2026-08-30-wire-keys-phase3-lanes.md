---
description: 'Stateless and integration lane results for the CAS semantic wire-key cut, taken at the freeze commit.'
sidebar_label: 'Wire keys phase 3 lanes'
sidebar_position: 95
slug: /superpowers/cas/wire-keys-phase3-lanes
title: 'CAS wire keys phase 3 — the external lanes'
doc_type: 'reference'
---

# Wire-keys phase 3 — the external lanes {#wire-keys-phase-3-the-external-lanes}

The unit battery proves the codecs agree with themselves. These two lanes prove the cut through a
running server and a real object store, which is a different claim: the stateless lane exercises
the formats via SQL, and the integration lane includes suites that parse persisted bodies with
their own readers, so a wrong key spelling fails them loudly rather than silently round-tripping.

Both lanes ran at the freeze commit `e9f1c3d867c` (gate 2250/2250).

## Binary provenance {#binary-provenance}

The version banner is not a build marker. The stateless run's log reports the server as
`26.6.2.20000.altinityantalya @ 164a356e367`, which is an earlier commit and reads exactly like a
stale-binary run. It is not one: an incremental build does not regenerate the embedded version
string. Provenance was established by content instead —
`ci/tmp/clickhouse` is a symlink to `build/programs/clickhouse`, and that binary contains
`"namespace"` and contains no occurrence of the retired `"root_namespace"`, which is the freeze
commit's part-manifest key change. Every later local lane that mounts `build/programs/clickhouse`
inherits the same provenance as long as nothing rebuilds into `build/`.

## Stateless lane {#stateless-lane}

Job `Stateless tests (amd_binary, cas s3 storage, parallel)`, selector built from the 41 CAS tests
in `tests/queries/0_stateless` (36 parallel + 5 sequential, as the runner reported).

**41 passed, 0 failed, 0 skipped, no hung queries.**

Coverage of the selector was checked rather than assumed: the CAS tests follow two naming
conventions, `*_cas[_.]*` and `*content_addressed*`, and only the first was used to build the
selector. Enumerating `.sh` and `.sql` files under both conventions yields the same 41 files, so
nothing was missed — the `*content_addressed*` matches in the directory are orphaned `.stdout` and
`.stderr` artifacts of renamed tests, not tests.

## Integration lane {#integration-lane}

Twelve suites match `tests/integration/test_cas_*`; the directory list was taken at run time so a
suite added since the plan was written could not be silently skipped, and it matched the plan's
twelve. Each ran in its own `python -m ci.praktika run "integration" --test <suite>` invocation, so
a stuck suite could not mask the others.

| suite | result |
|---|---|
| `test_cas_drop_pool_member` | 2 passed |
| `test_cas_file_cache` | 2 passed |
| `test_cas_gc_s3` | 1 passed |
| `test_cas_gc_sharded` | 1 passed |
| `test_cas_gcs` | 34 passed |
| `test_cas_insert_fault_recovery` | 1 passed |
| `test_cas_lazy_load_recovery` | 1 passed |
| `test_cas_mount_renewal_retry` | 2 passed |
| `test_cas_ref_snaplog` | 1 passed |
| `test_cas_replicated_relink` | 11 passed |
| `test_cas_s3` | 3 passed |
| `test_cas_shared_pool` | 2 passed |

**61 passed, 0 failed.**

Three of these suites parse persisted bodies and were re-spelled during the cut without ever being
executed, so they are the point of the lane rather than incidental coverage:

- `test_cas_gc_sharded` reads `snap_generation` and `snap_attempt` out of `gc/state`, and its
  reader now raises on an unreadable-but-present object instead of returning the absent sentinel;
- `test_cas_gcs` rewrites and asserts `cas_blob_meta` bodies in its mock;
- `test_cas_mount_renewal_retry` reads the lease's `seq` and `write_attempt_id`, both deliberately
  left unchanged by the cut — so this suite would also catch an over-eager rename.

## Triage {#triage}

The plan's six outcome classes (stale-assertion, cut-defect, pre-existing, environment/harness,
inconclusive, unclassified) were not needed for a test result: no test in either lane failed.

One environment/harness observation is worth recording so a later reader does not mistake it for a
failure. Every integration log contains two lines reading
`ERROR: command failed after 1/1 attempt(s), exit code: 1`. They come from praktika's
`docker info > /dev/null 2>&1` probe, which it tolerates and proceeds past; the suites then start
containers successfully. It is a probe result, not a test outcome.

## What these lanes do not cover {#what-these-lanes-do-not-cover}

They do not measure anything — no throughput, byte, or capacity claim is discharged here.

They also reach far fewer formats byte-wise than the pass counts suggest. None of the seventeen
registry type strings appears anywhere in either lane's test sources, and only the three suites
named above assert on a persisted object's fields at all; every other test exercises the formats
only through whatever the server happens to write and read back, which a symmetric rename cannot
break. That is a real limit on what a green lane proves, and it is why the format coverage question
is answered by the soak-card table rather than here.

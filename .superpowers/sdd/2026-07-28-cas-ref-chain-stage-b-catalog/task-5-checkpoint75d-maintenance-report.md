# Checkpoint 7.5d: `GcMaintenanceState`

Added the standalone raw `cas_gc_maintenance_state` control object at
`gc/maintenance_state`. It stores only the bounded opaque janitor cursor and uses one materialized
read plus one expected-token or expected-absence CAS. There is no janitor invocation, `GcState`, fold,
catalog, DEFER, or round-publication coupling.

The strict codec requires `cur`, uses a 64 KiB decoded cursor limit, and is registered as
`FormatId::GcMaintenanceState = 25` with `{7, 7}` change points and 512 KiB raw control-object caps.
`CORRUPTED_DATA` is classified as corrupt with its exact token; future-version and backend failures
propagate. Conflict paths do not retry, reread, or adopt a winner.

## Evidence

- Compile RED: `build_asan/build_stageb_75d_red.log` failed because the test referenced the absent
  codec/store interfaces.
- Controlled key-alias RED: `build_asan/test_stageb_75d_mutation_red.log` rejected aliasing the
  maintenance key to `gc/state`.
- Batched token/corrupt RED: `build_asan/test_stageb_75d_batched_red.log` rejected both omitted
  expected-token forwarding and treating corrupt bytes as valid empty state.
- Final focused ASan GREEN: `build_asan/build_stageb_75d_final_green.log` and
  `build_asan/test_stageb_75d_final_green.log`.

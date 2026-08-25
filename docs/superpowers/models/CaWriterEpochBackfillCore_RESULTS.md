# `CaWriterEpochBackfillCore` — mandatory numeric epoch backfill {#writer-epoch-backfill-results}

`CaWriterEpochBackfillCore.tla` isolates the numeric part of INV-2 that the ref-stream model does
not enumerate: a namespace active at epoch 1 can be inactive while global writer epochs advance, but
its first later consumer must close each skipped epoch in order with an empty sequence-1 `EpochSeal`.
The single transition is `E -> E + 1`; a direct `E -> E + 2` link cannot authorize recovery, fold or
destruction because the missing `E + 1` seal fails `INV_NO_EPOCH_SKIP`.

Runner: `TLC_WORKERS=1 ./run_writer_epoch_backfill.sh`. It uses
`../../../tmp/tla2tools-official.jar`, pins its SHA-256 through
`tlc_temporal_gate.sh`, asserts the exact invariant for the negative control, and writes individual
TLC logs under `tmp/`. Results are recorded after the focused gate below.

Focused run on 2026-08-02: all five expectations met. TLC reported `00s` for every individual
configuration; the suite runner's integer wall-clock accounting was `1s`, `0s`, `0s`, `1s`, and
`0s` in row order.

| cfg | expected | observed | states generated / distinct | depth | runner s |
|---|---|---|---:|---:|---:|
| `_sab_direct_skip` | `INV_NO_EPOCH_SKIP` violation | exact named violation after direct `1 -> 3`, PASS | 25 / 21 | 4 | 1 |
| `_sab_frontier_after_terminal` | no recovery authorization past a decoded terminal seal in the same epoch | exact `INV_NO_SAME_EPOCH_FRONTIER_AUTHORIZATION` violation, PASS | 23 / 19 | 3 | 0 |
| `_sab_snapshot_base_is_seal` | no recovered owner set from an `EpochSeal` checkpoint base | exact `INV_OWNER_SET_BASE_IS_NOT_EPOCH_SEAL` violation, PASS | 7 / 7 | 2 | 0 |
| `_witness_authorizations` | reachability witness violation after all three grants | exact named violation after recovery → fold → destructive, PASS | 35 / 29 | 4 | 1 |
| `_safe` | green | `No error has been found`, PASS | 420 / 335 | 9 | 0 |

# GC round cost map, 2026-09-24: key facts from the exploration agent, cited in CasGc.cpp unless noted {#gc-round-cost-map}

1. listRefPrefix = ONE pool-wide LIST of <prefix>/cas/ns/stream/ (enumerateRefPrefix ~3954). Consumers of scan.keys:
   buildRefWalkPlan (only max_log_by_life / listed_lives; listed_hint never read), changedRows (DEFER signal),
   fold_ref_group (groupRefKeys -> ref_tables), cleanupRefObjects (needs BELOW-floor keys: L < checkpoint && L <= durable_cursor,
   CasRefProtocol.h:663-671). fold_ref_intake does NOT use the listing: walks by exact key from cursor, forward only (~2046, 2560-2786).
   start-after is used only as intra-listing pagination cursor (CasObjectStorageBackend.cpp:1038-1108, CasRequests.cpp:581-597).
   => per-namespace start-after-floor listing is enough for fold + DEFER; cleanup would stop reclaiming unless it lists its own range.
2. _log key = .../<life>/_log/<16hex epoch>-<16hex seq>.zst (CasLayout.h:159, CasTypes.h:262-281, hex.h:236). Lexicographic == (epoch, seq).
   Epoch crossing = forward exact GET of witness (crossEpochFromSeal, CasRefProtocol.cpp:857-951); never below floor.
3. Read-ahead: GcReadAhead (CasGcReadAhead.h/.cpp), window = 4*concurrency, one instance per fold (~1685), ThreadPool read_pool of gc_read_concurrency (16) built in Gc ctor (~347).
   Through it: _ckpt reads, manifest bodies (foldManifestEdges), ref-log bodies (intake ~2555-2565), head_blob HEADs (~1852, topUpHeadHints ~1825).
   Bypass: peek_head (~1910, deliberate), confirm_condemned_marker loadMeta (~1933) — called from settleEntry (CasBlobInDegree.cpp:485-513) inside
   foldDeltasIntoGeneration, run SEQUENTIALLY per shard on the fold thread (~3331, 3358-3381; CasGc.h:361-362 SINGLE-THREADED).
   Memo: GcMetaWriter condemn_markers_confirmed set, populated async by scheduleCondemnMarkerWrite job success (CasGcMetaWriter.cpp:147-158)
   or sync by loadMeta hit (~1935); cleared on redelete/spared/supersede (~774, 820, 869). In-process only => empty after restart.
4. pending_deletes (PHASE 11, ~687-924, loop 700-775): per entry HEAD then conditional DELETE (removeObjectIfTokenMatches), SERIAL on the round's
   single CasOperation; no comment forbids parallelism; DELETE-SITE INVARIANT (CasGc.h:526-532) constrains where, not ordering.
5. shouldDeferRound (~297): fold if graduation_due || changed >= gc_fold_threshold (default 1) || rounds_since_last_fold >= 8.
   graduationDue (~3919): true when any shard has pending_total>0 or oldest condemned crossed floor; fail-closed true.
6. Pre-CAS destructive: pending_deletes, pruneSupersededGenerations (in round_commit ~986). CAS = op.replace(gcStateKey) ~991-1005.
   Post-CAS: handoff_reclaim, manifest_deletes, namespace janitor, ref_object_cleanup, orphan_sweep; all gated by suppress_destructive.
7. Scheduler: gc_interval_sec 60 between round starts; gc_round_mutex serializes scheduled vs manual; only heartbeat thread concurrent.
8. No time-based budget anywhere; CasPool.h:138-154 says "rounds have no time deadline". All caps are counts.

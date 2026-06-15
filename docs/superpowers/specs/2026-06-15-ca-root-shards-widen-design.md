# CA: widen `root_shards` fanout (design)

**Date:** 2026-06-15. **Branch:** `cas-mergetree-poc`. **Status:** brainstormed (self, unattended) → spec.

## Problem
`root_shards` is hardcoded to the default **8** (`CasStore.h:26`); `ContentAddressedMetadataStorage` builds `PoolConfig` without setting it (`…Storage.cpp:213-218`), and the disk factory never reads it (`MetadataStorageFactory.cpp:242`). So every CA pool funnels ALL manifest mutations (`casPut` on `roots/<t>/<ns>/<shard>`, `shard = CityHash64(ref) % 8`) through **8 keys per table**. Under load those 8 keys hit RustFS's per-object 64-permit cap (503 storm, B158) and accumulate the per-key overwrite-orphan pileup that makes `roots/` un-listable. Spreading the fanout drops per-key concurrency and per-key pileup.

## Design

### 1. Make `root_shards` a creation-time disk setting
- `MetadataStorageFactory.cpp` (the `content_addressed` registrar): read `const uint64_t root_shards = config.getUInt64(config_prefix + ".content_addressed_root_shards", 8);` next to `gc_enabled`/`gc_interval`, and pass it to the constructor.
- `ContentAddressedMetadataStorage` ctor: add `uint64_t root_shards_ = 8` (defaulted, so existing call sites and tests compile unchanged); set `pool_config.root_shards = root_shards_` before `Store::open`.
- Validation already exists (`CasPoolMeta`: `root_shards >= 1`). Creation-time only; the pool is authoritative on reopen, so **existing pools keep their shard count** — this changes only freshly-created pools.

### 2. Soak config
- `utils/ca-soak/configs/storage_conf.xml`: set `<content_addressed_root_shards>64</content_addressed_root_shards>` on the `ca` disk (the creating, read-write disk; `ca_ro` reopens the same pool and inherits). The soak drops+recreates the pool per run, so this takes effect immediately. 64 = an 8× spread → per-key in-flight ops fall from ~63/64 to ~8/64, comfortably under the cap.

### 3. Default stays 8 (deliberate)
The code default is left at 8. Widening trades **fewer write collisions** for **more GC-fold reads**: the GC and `listNamespaces`/`listRefs` iterate ALL `root_shards` per round (`CasStore.cpp:385/514/568`), so a high default would multiply GC read op-count on every pool. We tune the soak (the contended single-disk case) without changing the production default until the GC op-count interplay ([[B160]]) is addressed. 64 is chosen for the soak as a balanced spread.

## Why this is the right lever (and its limit)
Widening **distributes** the manifest overwrites across more keys → no single key approaches the 64-permit cap and no single `roots/` key accumulates ~15k orphans (so `roots/` lists fine). It does **not** reduce the TOTAL overwrite count or the TOTAL leak bytes (that is #3 / the GC); it removes the *hot-key* pathology (congestion + listability).

## Testing
- Unit: construct a CA metadata storage (or `Store::open`) with `root_shards = N` and assert `store()->poolMeta().root_shards == N`; publish several refs whose names hash to ≥2 distinct shards and confirm they resolve (distribution works). Reuse the `gtest_ca_wiring` construction pattern.
- No-regression: default (8) path unchanged — existing `Cas*`/`CaWiring*` green (defaulted ctor param).

## Scope
In: the `content_addressed_root_shards` setting + plumbing + the soak config value. Out: changing the production default; reducing total overwrites (GC/op-count, [[B160]]); the leak bytes themselves (#3).

# Task 3 — proto file/package rename — spec + plan

**Status:** design + plan (unattended ritual, 2026-06-26). Branch `cas-vfs-path-mapping`. Pure mechanical rename; pre-release (no on-disk data, no compat). Behavior-preserving (no wire/format change — protobuf package name does NOT affect the serialized bytes).

## Goal
Rename the CA protobuf schema for portability/clarity:
- file `…/Core/Proto/cas_root_shard.proto` → `cas_format.proto`
- package `DB.Cas.Proto` → `clickhouse.cas.format`
The generated header follows the basename: `cas_root_shard.pb.h` → `cas_format.pb.h`. The CMake **target**
name `clickhouse_cas_proto` STAYS (it is not the file or the package).

## Why behavior-preserving
The protobuf wire format is independent of the package/message-name (field numbers + wire types only).
Renaming the package changes the generated C++ namespace and the `.proto`/header filenames, nothing on
disk. Pre-release: no persisted data under the old names.

## Namespace mapping (the one subtlety)
Old package `DB.Cas.Proto` → C++ `DB::Cas::Proto` (nested under `DB::Cas`, so codecs inside
`namespace DB::Cas` reference it as `Proto::X`). New package `clickhouse.cas.format` → C++
**`::clickhouse::cas::format`** — a TOP-LEVEL namespace, NOT nested under `DB::Cas`. So `Proto::X` would
stop resolving. **Minimal-churn fix:** in each consuming `.cpp`/`.h`, add a file-local alias
`namespace Proto = ::clickhouse::cas::format;` so every existing `Proto::X` call site is unchanged.

## Scope (grounded)
- Rename: `…/Core/Proto/cas_root_shard.proto`.
- `…/Core/Proto/CMakeLists.txt:1` — `protobuf_generate_cpp(... cas_root_shard.proto)` → `cas_format.proto`.
- `src/CMakeLists.txt:905` — comment `<cas_root_shard.pb.h>` → `<cas_format.pb.h>`.
- C++ namespace users (6): `CasGcFormats.cpp`, `CasGcOutcomes.cpp`, `CasPoolMeta.cpp`,
  `CasRootShardCodec.cpp`, `CasRootsRegistry.cpp`, `CasWatermark.cpp`.
- `#include` of the generated header (`cas_root_shard.pb.h`): the above + `CasCodecUtil.h`,
  `CasRootShardCodec.h`, `gtest_cas_codecs.cpp`, `gtest_cas_gc_formats.cpp`.
- The `.proto` header comment: drop the "cleanup pending / file still named cas_root_shard" note; update
  the `protoc --decode DB.Cas.Proto.RootShardManifest cas_root_shard.proto` example →
  `protoc --decode clickhouse.cas.format.RootShardManifest cas_format.proto`.

## Plan (one commit)
- [ ] `git mv` the `.proto`; set `package clickhouse.cas.format;`; fix the header comment + protoc example.
- [ ] Update `Proto/CMakeLists.txt` filename; `src/CMakeLists.txt:905` comment.
- [ ] Replace every `#include "cas_root_shard.pb.h"` / `<cas_root_shard.pb.h>` → `cas_format.pb.h`.
- [ ] In each consumer, replace `DB::Cas::Proto` references: prefer adding `namespace Proto =
  ::clickhouse::cas::format;` (file-local) and leaving `Proto::X` call sites; or full-qualify if a file
  has no `Proto::` alias pattern. Remove any now-wrong `namespace Proto = DB::Cas::Proto;` /
  `using namespace DB::Cas::Proto;`.
- [ ] `cd build && cmake .` (regenerate proto rules) then `ninja unit_tests_dbms` (no -j). Iterate on
  build errors (missed include/namespace). Full `--gtest_filter='Cas*:Ca*'` sweep — only the baseline
  red `CaWiringOps.FreezeViaHardLinksIntoShadow`.
- [ ] Verify clean: `grep -rn "cas_root_shard\|DB::Cas::Proto\|DB.Cas.Proto" src/` → empty (no stragglers).
- [ ] Commit: `CA: rename cas_root_shard.proto → cas_format.proto, package → clickhouse.cas.format`.

## Verification (done)
Build clean (`-Werror`); sweep baseline-only red; the grep gate empty; golden-byte codec tests
(`gtest_cas_codecs.cpp`) still pass (proves the wire bytes are unchanged by the package rename).

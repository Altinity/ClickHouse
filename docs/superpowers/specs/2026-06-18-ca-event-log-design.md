# `system.content_addressed_log` — always-on (optional) CAS event/decision audit log

**Status:** design, awaiting review · **Date:** 2026-06-18 · **Branch:** `cas-mergetree-poc` · **Backlog:** B170
**Goal:** make content-addressed (CA) GC/durability incidents diagnosable by a SQL query instead of ad-hoc INFO log lines + `docker exec` greps + throwaway snap decoders, by emitting one structured row per CAS decision/event to a `SystemLog` table — **off by default, cheap to leave on for tests/soak/CI, toggleable in prod for incident forensics.**

**Completeness mandate (2026-06-18, user directive):** the log must be **exhaustive enough to fully reconstruct the entire lifetime of every entity** (blob / tree / ref / snap generation) from the rows alone. That means:
- **Every decision that changes CAS state is an event, WITH the full context that explains *why* the decision was taken** — not just *what* happened. (e.g. a blob delete row carries the in-degree-reached-0 cause, the parent whose strip dropped it, the round, and the recheck verdict + fence coverage.)
- **Errors and anomalies are events too** — e.g. an attempt to resolve/read a ref that points at a missing blob (`dangling_access`), a fail-closed throw (with its reason), a corrupt decode, the coherence-guard trip.
- **GC internal transitions are events** — setting the expansion marker, an in-degree reaching 0, the retire/condemn decision, the recheck verdict, the physical delete, the `forget`, writing a new snap generation + advancing the cursor, the journal trim, fence, lease acquire/steal/heartbeat.
- **It is OK to be noisy** — no sampling; the firehose IS the product. Filtering `WHERE object_hash = …` (or `ref_name = …`) ORDER BY `event_time` must yield that entity's complete story: put → every reuse → every edge add/remove → expand → retire → recheck → strip → delete → forget, and for refs: publish → repoint → drop, plus any error touching it.

## 1. Motivation

The B140-dangle recurrence diagnosis (2026-06-18) needed: piecemeal INFO audit lines (`CAGCDEL`/`CAREUSE`/`CASTRIP`/`CAROOTREM`, added ad-hoc across B90/B140/B167/B168), grepped from `syslog`-owned container logs (a permission pitfall that produced a *wrong* conclusion until re-run via `docker exec`), plus a throwaway `GcSnap` decoder. A first-class structured event log turns all of that into SQL — token-join attribution (reuse-adopt ↔ GC-delete), root-edge-removal → strip → blob-delete causal chains, snap-vs-truth divergence queries — with no log-grep, no permission pitfalls, no throwaway decoders.

## 2. Architecture

Mirror the existing `system.content_addressed_garbage_collection_log` (per-round summary), but at **per-event** granularity:

- **`ContentAddressedLog : SystemLog<ContentAddressedLogElement>`** (table `system.content_addressed_log`) + a `Context` accessor (`Context::getContentAddressedLog()`), registered like the other system logs. Async-flushed by the standard `SystemLog` machinery — never synchronous I/O on the hot path.
- **`CasEventSink`** — an abstract interface in `Core/` (`Core/CasEventSink.h`): `virtual void emit(const CasEvent &) = 0;`. The Core (`Store`, `Build`, `Gc`) holds a `CasEventSink *` (nullable) and calls `if (sink) sink->emit(CasEvent{...});` at every hook. `CasEvent` is a plain Core struct (no ClickHouse types).
- **Bridge:** `ContentAddressedMetadataStorage` constructs the concrete sink from the Context's `ContentAddressedLog` (the same place the GC round-log sink is wired) and injects it into the Core. The concrete sink maps `CasEvent → ContentAddressedLogElement →` `log->add(...)`.
- **Disabled = null sink.** When the log is not configured/enabled, the injected sink pointer is `nullptr`, so every hook is a single predictable branch `if (sink) …` with **no row construction, no allocation**. A process-global `std::atomic<bool>` mirrors enablement for hooks that aren't on an object holding the sink pointer.

## 3. Schema (`system.content_addressed_log`)

Typed core columns (JOIN-able/filterable) + a `detail Map(String,String)` for event-specific extras:

| column | type | notes |
|---|---|---|
| `event_time` | `DateTime` | |
| `event_time_microseconds` | `DateTime64(6)` | |
| `hostname` | `LowCardinality(String)` | emitting node |
| `disk_name` | `LowCardinality(String)` | CA disk / pool |
| `event_type` | `LowCardinality(String)` | the taxonomy in §4 |
| `namespace` | `String` | `roots/<ns>` (server/table) |
| `ref_name` | `String` | part name / ref (empty if N/A) |
| `object_kind` | `Enum8('none','blob','tree','pack','root','snap')` | |
| `object_hash` | `String` | lowercase hex (empty if N/A) |
| `token` | `String` | incarnation token (empty if N/A) |
| `round` | `UInt64` | GC round (0 if N/A) |
| `gen` | `UInt64` | snap generation (0 if N/A) |
| `at_version` | `UInt64` | manifest shard_version of the driving journal record (0 if N/A) |
| `outcome` | `LowCardinality(String)` | e.g. `ok`,`adopt`,`resurrect`,`deleted`,`replaced`,`spared`,`absent`,`zeroed`,`skipped` |
| `reason` | `String` | free-text cause (empty if N/A) |
| `thread_id` | `UInt64` | |
| `query_id` | `String` | for correlation with `query_log` (empty if N/A) |
| `detail` | `Map(String,String)` | event-specific extras (e.g. `freed`, `out_edges`, `parent_tree`, `fence_seq`, `build_id`, `min_active`) |

**`reason` and `detail` are mandatory and explanatory, not decorative.** Every emit fills `reason` with the human-readable *why* of the decision, and `detail` with the structured facts needed to reconstruct it. Examples:
- `blob_delete`: `reason="in-degree 0 after strip of parent <T> at round R; recheck confirmed 0 through fence_version V; exact-token delete"`, `detail={parent_tree, fence_version, indeg_at_recheck:"0", token_outcome:"deleted"}`.
- `indeg_zero`: `reason="last edge dropped"`, `detail={prev_indeg:"1", dropped_by:"root_remove(<ref>)" | "strip(<parent>)"}`.
- `root_remove`: `reason="folded Remove(<ref>) at_version=<v>"`, `detail={zeroed:"true|false"}`.
- `retire_decision`: `reason="condemn: present ∧ known ∧ inDeg=0 ∧ ¬protectedByLiveBuild"` or `"skip: protectedByLiveBuild(server <s>, min_active <m>)"`.
- `recheck_verdict`: `reason="spared: inDeg>0 after fold-through-fence"` / `"deleted"` / `"replaced: token displaced (412)"`.
- `dangling_access`: `reason="ref <ref> -> tree <T> -> blob <h> HEAD 404"`, `detail={resolved_from}`.
- `fail_closed`: `reason="<exception message>"`, `detail={code, site}`.

Default engine/TTL: standard `SystemLog` (configurable; soak sets a generous TTL).

## 4. Event taxonomy (`event_type`) — comprehensive hooks

Extensible enum (a new event = one value + one `emit` call). The set is **exhaustive by design** — every state-changing decision, every GC internal transition, and every error/anomaly:

- **blob:** `blob_put`, `blob_reuse_adopt`, `blob_reuse_resurrect`, `blob_retire`, `blob_delete` (was `CAGCDEL`), `blob_forget` (P9)
- **tree:** `tree_put`, `tree_expand` (= set the expansion marker / record child edges), `tree_retire`, `tree_delete`, `tree_strip` (was `CASTRIP`)
- **ref/root edges (the in-degree story):** `ref_publish`, `ref_drop`, `ref_repoint`, `root_add`, `root_remove` (was `CAROOTREM`), `root_repoint` (was `CAROOTREPOINT`), `indeg_zero` (a node's in-degree transitioned to 0 → a retire candidate; carries what dropped it)
- **gc round lifecycle:** `gc_fold_begin`, `gc_fold_end`, `gc_retire_observe`, `gc_retire_decision` (condemn or skip + why), `gc_recheck_verdict` (deleted / replaced / spared / absent + in-degree-at-recheck + fence coverage), `gc_fence`, `gc_snap_persist` (generation + cursor + size), `gc_cursor_advance`, `gc_trim` (records trimmed, up to which cursor), `gc_lease_acquire`, `gc_lease_steal`, `gc_lease_heartbeat`
- **build/write/gate:** `build_start`, `build_publish`, `build_abort`, `gate_revalidate`, `gate_resurrect`, `watermark_renew`, `heartbeat`
- **read/resolve:** `ref_resolve`, `read_missing` (a read hit `FILE_DOESNT_EXIST`), `dangling_access` (a resolve/read reached a ref→tree→blob whose blob is absent)
- **errors/anomalies:** `fail_closed` (a fail-closed throw, with the exception message + code + site), `corrupt_decode`, `snap_journal_incoherent` (the coherence guard tripped), `exception` (generic catch-all with code + message)

(The exact field→column mapping per event is in the implementation plan; anything not in a typed column goes in `detail`. The enum is open — a newly-discovered decision point just adds a value + an `emit` call.)

## 4.1 Lifecycle reconstruction (the acceptance bar)

The log passes only if these queries fully reconstruct an entity's life from the rows alone:
```sql
-- a blob's whole life: put -> reuses -> edge transitions -> retire -> recheck -> delete -> forget
SELECT event_time, event_type, ref_name, token, round, gen, outcome, reason, detail
FROM system.content_addressed_log WHERE object_hash = :blob ORDER BY event_time;

-- a part/ref's life: publish -> repoint -> drop, plus any dangling_access touching it
SELECT event_time, event_type, object_hash, at_version, outcome, reason
FROM system.content_addressed_log WHERE ref_name = :part ORDER BY event_time;

-- "who/why deleted blob X while a live ref referenced it" (the B140-dangle question), one query:
SELECT event_time, event_type, ref_name, round, gen, reason, detail
FROM system.content_addressed_log
WHERE object_hash = :blob AND event_type IN ('root_remove','tree_strip','indeg_zero','gc_recheck_verdict','blob_delete','blob_reuse_adopt','dangling_access')
ORDER BY event_time;
```
If any of "what put it, who referenced it, why its in-degree hit 0, what condemned it, why the recheck let the delete through" is NOT answerable from the rows, an event or a `reason`/`detail` field is missing — add it.

## 5. Enablement & overhead

- **Default OFF.** No `<content_addressed_log>` config section ⇒ the Context log is null ⇒ the Core sink is null ⇒ hooks no-op.
- **Enable** via a `<content_addressed_log>` SystemLog section in server config (database/table/flush_interval/TTL), exactly like `<query_log>`. Tests/soak/CI ship a `configs/ca_event_log.xml` config.d that enables it.
- **Overhead when on:** row built + handed to the async `SystemLog` buffer; no synchronous I/O. Volume is high (~80k+ events / 20 min at workers=2), which is acceptable for soak/CI; for prod, leave OFF and toggle for an incident. (A future `detail`-level or per-event-class filter is possible but **out of scope** for v1 — YAGNI; the firehose is the point.)
- **Overhead when off:** one `nullptr` branch per hook. No measurable cost.

## 6. Consolidation
- `CAGCDEL`, `CAREUSE`, `CASTRIP`, `CAROOTREM`/`CAROOTREPOINT` become `emit()` calls (event rows). The terse INFO lines may be dropped once the table is the primary sink (decide per-line; keep none that duplicate a row).
- The per-round `system.content_addressed_garbage_collection_log` **stays** as the coarse summary (complement, different granularity).

## 7. Tests
- gtest: `ContentAddressedLogElement` `appendToBlock`/`getColumnsDescription` round-trip.
- gtest: drive a few Core ops (`Build::publish`, `Gc::runRegularRound`) with a **capturing `CasEventSink`** and assert the expected event rows (type/kind/hash/token/round).
- gtest: with a **null sink**, assert no `emit` occurs (the no-op path).
- stateless: enable the log via config.d, run INSERT + OPTIMIZE + a GC round, assert `system.content_addressed_log` has `blob_put`/`tree_expand`/`gc_*` rows.
- soak: enable it; use it (not log-greps) for forensics.

## 8. Out of scope
- The `clickhouse-disks` decode/introspect CLI (B169) — complementary, separate.
- Per-namespace/sampling filters (v2 if volume becomes a problem).
- Subsuming the per-round GC log into a derived view (kept separate).

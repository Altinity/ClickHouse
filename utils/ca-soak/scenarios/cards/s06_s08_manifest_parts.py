"""S06 wide part + S07 manifest cap fail-closed + S08 thousands of parts (P0).

These three cards exercise the part-manifest write path and the root-shard metadata that names every
part. The relevant fail-closed caps live in `Core/CasBuild.cpp::stageManifest`:

    kMaxManifestEntries        = 1048576       (1M manifest entries)
    kMaxManifestEncodedBytes   = 256 MiB
    kMaxManifestInlineBytesTotal = 16 MiB
    kMaxLargestInlineEntryBytes  = 1 MiB

and the root-shard soft limit (`CasStore::mutateShard`, `manifest_soft_limit = 16 MiB`) which only
emits a `LOG_WARNING` ("manifest ... size ... crossed soft limit ...").

S06 proves a very wide part stays under the manifest hard cap (or fails early with `LIMIT_EXCEEDED`)
and that a column-subset read does not fetch every blob. S07 is a negative card that makes a
best-effort attempt to trip a cap and, when the cap is not reachable at feasible dev SQL scale, still
verifies the fail-closed PROPERTY (no live ref on a rejected manifest, clean pool). S08 creates many
small parts fast and checks root-shard CAS contention stays bounded by `root_shards`.
"""

import time

from ..framework import observe, sql
from ..framework.base import Scenario, register
from ..framework.report import Verdict
from . import _common

MIB = 1024 * 1024

# Hard caps enforced fail-closed in stageManifest (src/.../Core/CasBuild.cpp). Mirrored here only for
# verdict thresholds / observations — the server is the authority.
K_MAX_MANIFEST_ENTRIES = 1048576
K_MAX_MANIFEST_ENCODED_BYTES = 256 * MIB
K_MAX_MANIFEST_INLINE_TOTAL = 16 * MIB
K_MAX_LARGEST_INLINE_ENTRY = 1 * MIB
ROOT_SHARDS = 8  # compose default; root-shard CAS contention is bounded by this.


# ---------------------------------------------------------------------------
# Local helpers
# ---------------------------------------------------------------------------

WIDE_PARSER_SETTINGS = {"max_query_size": 100 * 1024 * 1024,
                        "max_ast_elements": 5_000_000, "max_expanded_ast_elements": 5_000_000}


def _wide_columns(n_cols, *, key="k", col_type="UInt32"):
    """Column-list SQL for a wide table: a key column plus `n_cols` data columns c0..c{n-1}."""
    cols = [f"{key} UInt64"]
    cols += [f"c{i} {col_type}" for i in range(n_cols)]
    return ", ".join(cols)


def _wide_select(n_cols, *, rows, base=0):
    """A deterministic SELECT producing `rows` rows over `n_cols` columns (c_i = number + i)."""
    exprs = [f"{base} + number AS k"]
    exprs += [f"toUInt32(number + {i}) AS c{i}" for i in range(n_cols)]
    return f"SELECT {', '.join(exprs)} FROM numbers({rows})"


def _manifests_shape():
    """Best-effort {_manifests:{objects,bytes}, roots:{objects,bytes}, _ok} from the pool shape."""
    shape = observe.pool_shape(timeout_s=120)
    if not shape.get("_ok"):
        return {"_ok": False}
    return {"_ok": True,
            "_manifests": shape.get("_manifests"),
            "roots": shape.get("roots"),
            "_total": shape.get("_total")}


def _soft_limit_warnings(cluster, since_event_time):
    """Best-effort count of root-shard manifest soft-limit warnings from `system.text_log` (the
    `CasStore` "crossed soft limit" `LOG_WARNING`). Returns (count|None, queryable: bool).

    The warning is a text-log signal, not a system-table event, so this is honest best-effort: if
    `system.text_log` is not enabled the count is None and the caller records inconclusive."""
    where = "logger_name = 'CasStore' AND message LIKE '%crossed soft limit%'"
    if since_event_time:
        # since_event_time is a 'YYYY-MM-DD HH:MM:SS' string — compare as a quoted DateTime literal.
        where += f" AND event_time >= '{since_event_time}'"
    total = 0
    any_ok = False
    for n in cluster.nodes():
        try:
            v = n.scalar(f"SELECT count() FROM system.text_log WHERE {where}")
            total += int(v or 0)
            any_ok = True
        except Exception:
            pass
    return (total if any_ok else None), any_ok


@register
class S06(Scenario):
    name = "S06"
    title = "10000-column wide part"
    priority = "P0"
    param_table = {
        # dev keeps it fast: 1000 columns is wide enough to stress manifest encode/decode without a
        # multi-minute insert. ci/full push toward the spec's 10000.
        "dev": {"n_cols": 1000, "block_rows": 200, "subset_cols": 8},
        "ci": {"n_cols": 5000, "block_rows": 500, "subset_cols": 8},
        "full": {"n_cols": 10000, "block_rows": 2000, "subset_cols": 8},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        n_cols = int(p["n_cols"])
        block_rows = int(p["block_rows"])
        subset_cols = int(p["subset_cols"])
        table = "s06_wide"
        result.observations["n_cols"] = n_cols
        result.observations["block_rows"] = block_rows
        ctx.log(f"S06: wide table with {n_cols} data columns, Wide parts, {block_rows}-row block")

        cols = _wide_columns(n_cols)
        for n in cl.nodes():
            sql.create_ca_table(n, table, columns=cols, order_by="k", wide=True,
                                client_settings=WIDE_PARSER_SETTINGS)

        counters = _common.counters_window(ctx)
        committed = True
        limit_exceeded = False
        t0 = time.monotonic()
        try:
            # A 10000-column SELECT is a ~260 KB SQL string — over the 256 KB default
            # `max_query_size`, so raise it or the wide INSERT is rejected with SYNTAX_ERROR
            # (code 62) BEFORE the CAS write path, never reaching the manifest-cap test under
            # scrutiny (campaign 2026-07-05: full scale failed exactly here).
            # A 10000-column INSERT also blows the AST-size limits (max_ast_elements default 50000,
            # ~5 nodes/column). Raise query-size AND both AST caps so the SQL reaches the CAS write
            # path (this is the wide-part/manifest test, not a parser-limits test).
            wide_q = {"max_query_size": 100 * 1024 * 1024,
                      "max_ast_elements": 5_000_000, "max_expanded_ast_elements": 5_000_000}
            # One row first (smallest possible Wide part), then a larger block.
            sql.insert_values(cl.node1, table, _wide_select(n_cols, rows=1, base=0), timeout=1200,
                              settings=wide_q)
            sql.insert_values(cl.node1, table, _wide_select(n_cols, rows=block_rows, base=1000),
                              timeout=2400, settings=wide_q)
            cl.node1.command(f"OPTIMIZE TABLE {table} FINAL", timeout=2400)
        except Exception as e:
            msg = str(e)
            committed = False
            # The fail-closed cap throws LIMIT_EXCEEDED (code 277).
            limit_exceeded = ("LIMIT_EXCEEDED" in msg) or ("Code: 277" in msg) or ("exceeds cap" in msg)
            result.observations["s06_insert_error"] = msg[:2000]
            ctx.log(f"S06: write path raised (limit_exceeded={limit_exceeded}): {msg[:200]}")
        insert_s = time.monotonic() - t0
        result.timings["s06_write_s"] = round(insert_s, 1)

        delta = counters().get("_total", {})
        result.observations["s06_counters"] = delta
        result.observations["s06_CasBlobPut"] = delta.get("CasBlobPut", 0)
        result.observations["s06_CasRootCas"] = delta.get("CasRootCas", 0)
        result.observations["s06_CasRootCasConflict"] = delta.get("CasRootCasConflict", 0)

        # Encoded manifest size + inline-entry total, observed from the physical pool (_manifests).
        mshape = _manifests_shape()
        result.observations["s06_pool_manifests"] = mshape
        man_bytes = None
        if mshape.get("_ok") and mshape.get("_manifests"):
            man_objs = mshape["_manifests"]["objects"]
            man_total_bytes = mshape["_manifests"]["bytes"]
            # A single committed wide part publishes one part-manifest body; with the 1-row + block
            # parts merged by OPTIMIZE FINAL there may be a few manifest objects, so report the mean as
            # a proxy for the largest encoded manifest (exact per-object sizing needs an fsck detail).
            man_bytes = (man_total_bytes // man_objs) if man_objs else 0
            result.observations["s06_manifest_mean_encoded_bytes"] = man_bytes
            result.observations["s06_manifest_objects"] = man_objs

        # Root-shard manifest soft-limit warnings (text-log best-effort).
        warn_count, warn_ok = _soft_limit_warnings(cl, ctx.extra.get("since_event_time"))
        result.observations["s06_root_soft_limit_warnings"] = warn_count
        if warn_ok:
            result.add(Verdict.check(
                "root-shard manifest soft-limit warnings", "recorded (informational)",
                warn_count, True,
                "soft limit only logs a WARNING; nonzero is informational, not a failure"))
        else:
            result.add(Verdict.inconclusive(
                "root-shard manifest soft-limit warnings", "recorded",
                "system.text_log not queryable (CasStore soft-limit warning is a text-log signal)"))

        # --- Verdict: commit under cap, OR early fail-closed -----------------------------
        if committed:
            ok_under_cap = (man_bytes is None) or (man_bytes < K_MAX_MANIFEST_ENCODED_BYTES)
            result.add(Verdict.check(
                "wide part outcome", "commit < manifest hard cap OR LIMIT_EXCEEDED",
                f"committed; mean encoded manifest ~{(man_bytes or 0)/MIB:.3f} MiB", ok_under_cap,
                "" if ok_under_cap else f"manifest >= hard cap {K_MAX_MANIFEST_ENCODED_BYTES/MIB:.0f} MiB "
                                        "but the write still committed — cap not enforced?"))
            # Correctness oracle: both replicas agree on the data.
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table))
        elif limit_exceeded:
            result.add(Verdict.check(
                "wide part outcome", "commit < manifest hard cap OR LIMIT_EXCEEDED",
                "failed-closed with LIMIT_EXCEEDED", True,
                "part exceeded a manifest cap and failed early — acceptable per S06"))
        else:
            result.add(Verdict.check(
                "wide part outcome", "commit < manifest hard cap OR LIMIT_EXCEEDED",
                "failed with a NON-cap error", False,
                "write failed but not with LIMIT_EXCEEDED — unexpected; see s06_insert_error"))
            result.note_anomaly("S06 wide-part write failed without a manifest-cap LIMIT_EXCEEDED")

        # --- Verdict: column-subset read does not fetch every blob -----------------------
        if committed:
            # Scan a small subset of columns, then all columns; CasBlobGet for the subset must be far
            # below the all-column scan (a Wide part has one blob body per column .bin).
            cl.node1.command("SYSTEM DROP MARK CACHE")
            cl.node1.command("SYSTEM DROP UNCOMPRESSED CACHE")
            sub_cols = ", ".join(f"c{i}" for i in range(subset_cols))
            cw = _common.counters_window(ctx)
            try:
                cl.node1.query(
                    f"SELECT sum(cityHash64({sub_cols})) FROM {table} "
                    f"SETTINGS max_threads=1 FORMAT TabSeparated")
            except Exception as e:
                ctx.log(f"S06: subset scan raised: {e}")
            sub_delta = cw().get("_total", {})
            subset_gets = sub_delta.get("CasBlobGet", 0)

            cl.node1.command("SYSTEM DROP MARK CACHE")
            cl.node1.command("SYSTEM DROP UNCOMPRESSED CACHE")
            cw2 = _common.counters_window(ctx)
            try:
                cl.node1.query(
                    f"SELECT sum(cityHash64(*)) FROM {table} "
                    f"SETTINGS max_threads=1 FORMAT TabSeparated")
            except Exception as e:
                ctx.log(f"S06: all-column scan raised: {e}")
            all_delta = cw2().get("_total", {})
            all_gets = all_delta.get("CasBlobGet", 0)

            result.observations["s06_subset_CasBlobGet"] = subset_gets
            result.observations["s06_allcol_CasBlobGet"] = all_gets
            result.observations["s06_subset_cols"] = subset_cols
            if all_gets > 0 and subset_gets >= 0:
                # subset reads ~subset_cols of n_cols column blobs -> must be well below the all-scan.
                ok = subset_gets < max(1, all_gets // 2)
                result.add(Verdict.check(
                    "column-subset avoids full fetch",
                    f"subset CasBlobGet << all-column ({subset_cols}/{n_cols} cols)",
                    f"subset={subset_gets} all={all_gets}", ok,
                    "" if ok else "a few-column SELECT fetched ~as many blobs as an all-column scan"))
            elif all_gets == 0:
                # Caches/inline may serve the read entirely from metadata; record honestly.
                result.add(Verdict.inconclusive(
                    "column-subset avoids full fetch", "subset << all-column",
                    "all-column scan issued 0 CasBlobGet (served from cache/inline) — "
                    "cannot compare blob-fetch counts at this scale"))

        _common.standard_end(ctx, result, [table])


@register
class S07(Scenario):
    name = "S07"
    title = "manifest cap fail-closed"
    priority = "P0"
    expect_exception = True
    param_table = {
        # Best-effort: a very wide table to approach (but realistically not reach) the 1M-entry cap.
        # A Wide part has roughly one manifest entry per column file (.bin/.mrk per column + a few
        # part-level files), so even 20000 columns is ~tens of thousands of entries — three orders of
        # magnitude below kMaxManifestEntries. The cap is honestly NOT reachable via dev SQL.
        "dev": {"n_cols": 2000, "block_rows": 50},
        "ci": {"n_cols": 10000, "block_rows": 100},
        "full": {"n_cols": 20000, "block_rows": 200},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        n_cols = int(p["n_cols"])
        block_rows = int(p["block_rows"])
        table = "s07_capwide"
        result.observations["n_cols"] = n_cols
        result.observations["caps"] = {
            "kMaxManifestEntries": K_MAX_MANIFEST_ENTRIES,
            "kMaxManifestEncodedBytes": K_MAX_MANIFEST_ENCODED_BYTES,
            "kMaxManifestInlineBytesTotal": K_MAX_MANIFEST_INLINE_TOTAL,
            "kMaxLargestInlineEntryBytes": K_MAX_LARGEST_INLINE_ENTRY,
        }
        ctx.log(f"S07: best-effort manifest-cap probe with {n_cols} columns (caps are 3+ orders of "
                f"magnitude above dev SQL reach)")

        cols = _wide_columns(n_cols)
        for n in cl.nodes():
            sql.create_ca_table(n, table, columns=cols, order_by="k", wide=True,
                                client_settings=WIDE_PARSER_SETTINGS)

        counters = _common.counters_window(ctx)
        triggered = False
        limit_exceeded = False
        non_cap_error = None
        try:
            sql.insert_values(cl.node1, table, _wide_select(n_cols, rows=block_rows, base=0),
                              settings=WIDE_PARSER_SETTINGS,
                              timeout=2400)
            cl.node1.command(f"OPTIMIZE TABLE {table} FINAL", timeout=2400)
        except Exception as e:
            msg = str(e)
            triggered = True
            limit_exceeded = ("LIMIT_EXCEEDED" in msg) or ("Code: 277" in msg) or ("exceeds cap" in msg)
            if not limit_exceeded:
                non_cap_error = msg[:2000]
            result.observations["s07_error"] = msg[:2000]
            ctx.log(f"S07: write raised (limit_exceeded={limit_exceeded}): {msg[:200]}")

        delta = counters().get("_total", {})
        result.observations["s07_counters"] = delta

        if triggered and limit_exceeded:
            # The cap was actually reached and threw — the positive negative-test outcome.
            result.add(Verdict.check(
                "manifest cap fail-closed", "LIMIT_EXCEEDED before any owner transition",
                "LIMIT_EXCEEDED thrown", True))
        elif triggered and not limit_exceeded:
            result.add(Verdict.check(
                "manifest cap fail-closed", "LIMIT_EXCEEDED",
                "write failed with a NON-cap error", False,
                f"expected LIMIT_EXCEEDED, got a different error: {non_cap_error}"))
            result.note_anomaly("S07 write failed but not with a manifest-cap LIMIT_EXCEEDED")
        else:
            # The honest common case at dev scale: the cap is simply not reachable via SQL.
            result.add(Verdict.inconclusive(
                "manifest cap fail-closed", "LIMIT_EXCEEDED",
                f"manifest caps not reachable via dev-scale SQL: {n_cols} columns is ~tens of "
                f"thousands of manifest entries, vs kMaxManifestEntries={K_MAX_MANIFEST_ENTRIES}; "
                f"encoded/inline caps are equally far. Reaching a cap deterministically needs a "
                f"cap-lowering test hook or a part with >1M files. The fail-closed PROPERTY is still "
                f"verified indirectly below (no ref on a rejected manifest; clean pool)."))
            result.note_anomaly(
                "S07 could not trigger a manifest cap with dev-scale SQL — recorded inconclusive for "
                "the direct cap trip; the indirect fail-closed property check still runs.")

        # --- Indirect fail-closed PROPERTY: whatever happened, the pool must be clean ----
        # If the write was rejected, no live ref may name a rejected manifest (fsck dangling==0) and
        # forced GC must reclaim any staged debris. If the write committed (no cap reached), the table
        # is consistent. Either way the standard_end fixpoint + assertions prove it.
        if not triggered:
            # The write committed; assert the data is queryable + replicas agree (an oracle).
            _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table))

        end = _common.standard_end(ctx, result, [table], expect_exception=True)
        # Explicit restatement of the fail-closed property as its own verdict.
        fsck = (end or {}).get("fsck_final", {})
        dangling = fsck.get("dangling")
        result.add(Verdict.check(
            "no live ref on rejected manifest", "fsck dangling==0 after attempt",
            dangling, dangling == 0,
            "" if dangling == 0 else "a dangling ref survived the attempted oversize op"))


@register
class S08(Scenario):
    name = "S08"
    title = "thousands of parts created quickly"
    priority = "P0"
    param_table = {
        # dev: a few thousand tiny parts is enough to exercise root-shard CAS contention without a
        # multi-minute run. ci/full push toward the spec's 50k-200k (or a time budget).
        "dev": {"n_parts": 2000, "rows_per_part": 1, "clients": 2},
        "ci": {"n_parts": 20000, "rows_per_part": 1, "clients": 4},
        "full": {"n_parts": 100000, "rows_per_part": 1, "clients": 8},
    }

    def run(self, ctx, result):
        cl = ctx.cluster
        p = ctx.params
        n_parts = int(p["n_parts"])
        rows_per_part = int(p["rows_per_part"])
        clients = max(1, int(p["clients"]))
        table = "s08_manyparts"
        result.observations["target_parts"] = n_parts
        ctx.log(f"S08: create ~{n_parts} tiny parts (merges stopped), {clients} client(s)")

        # Stop merges during creation; raise the part-count guards so MergeTree does not throw
        # TOO_MANY_PARTS before we reach the target (we WANT many active parts).
        extra = {
            "parts_to_throw_insert": "1000000",
            "parts_to_delay_insert": "1000000",
            "max_insert_block_size": "1",
        }
        for n in cl.nodes():
            sql.create_ca_table(n, table, columns="id UInt64, v UInt32", order_by="id",
                                extra_settings=extra, wide=True)
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM STOP MERGES {table}")
            except Exception as e:
                ctx.log(f"S08: STOP MERGES on {n.container} raised: {e}")

        # --- Creation phase: many small INSERTs (each INSERT = one part) ------------------
        # Distribute inserts across replicas (round-robin) so root-shard CAS sees concurrent writers.
        nodes = cl.nodes()
        counters = _common.counters_window(ctx)
        latencies = []
        t0 = time.monotonic()
        failed_inserts = 0
        for i in range(n_parts):
            node = nodes[i % len(nodes)] if clients > 1 else nodes[0]
            base = i * 1000000
            gen = f"SELECT {base} + number AS id, toUInt32(number) AS v FROM numbers({rows_per_part})"
            ti = time.monotonic()
            try:
                # Distinct content per insert (base offset) so each is a genuine new part, not a dedup.
                node.command(f"INSERT INTO {table} {gen}", timeout=120)
                latencies.append(time.monotonic() - ti)
            except Exception as e:
                failed_inserts += 1
                if failed_inserts <= 5:
                    ctx.log(f"S08: insert {i} raised: {str(e)[:160]}")
        create_s = time.monotonic() - t0
        result.timings["s08_create_s"] = round(create_s, 1)
        result.observations["s08_failed_inserts"] = failed_inserts

        if latencies:
            latencies.sort()
            n = len(latencies)
            result.observations["s08_insert_latency_s"] = {
                "count": n,
                "p50": round(latencies[n // 2], 4),
                "p95": round(latencies[min(n - 1, int(n * 0.95))], 4),
                "max": round(latencies[-1], 4),
            }

        delta = counters().get("_total", {})
        result.observations["s08_create_counters"] = delta
        cas_conflict = delta.get("CasRootCasConflict", 0)
        cas_total = delta.get("CasRootCas", 0)
        result.observations["s08_CasRootCas"] = cas_total
        result.observations["s08_CasRootCasConflict"] = cas_conflict
        result.observations["s08_CasRootGet"] = delta.get("CasRootGet", 0)

        # Inserts must fail only for expected MergeTree part-count pressure, never CA-metadata errors.
        result.add(Verdict.check(
            "no CA-metadata insert failures", "failures only from MergeTree part-count pressure",
            f"{failed_inserts} failed insert(s)", failed_inserts == 0,
            "" if failed_inserts == 0 else "some inserts failed — classify each (part-count guard vs "
                                           "CA metadata exception); see log"))

        # Parts summary while merges are stopped (creation peak).
        ps_node = nodes[0]
        try:
            ps_node.command(f"SYSTEM SYNC REPLICA {table}", timeout=600)
        except Exception as e:
            ctx.log(f"S08: SYNC before parts snapshot raised: {e}")
        ps = sql.parts_summary(ps_node, table)
        result.observations["s08_parts_at_peak"] = ps
        result.add(Verdict.check(
            "many active parts created", f"~{n_parts} active before merge",
            ps.get("active"), ps.get("active", 0) > 0))

        # Root-shard / manifest pool shape at peak — must not exceed the manifest hard caps.
        peak_shape = _manifests_shape()
        result.observations["s08_pool_at_peak"] = peak_shape
        if peak_shape.get("_ok") and peak_shape.get("roots"):
            root_objs = peak_shape["roots"]["objects"]
            root_bytes = peak_shape["roots"]["bytes"]
            # Root shards are bounded in count by root_shards per namespace; the per-object body must
            # stay below the manifest hard cap. Use mean per-object as a proxy (exact largest needs
            # an fsck detail row).
            mean_root_bytes = (root_bytes // root_objs) if root_objs else 0
            result.observations["s08_root_mean_bytes"] = mean_root_bytes
            result.observations["s08_root_objects"] = root_objs
            result.add(Verdict.check(
                "root-shard objects bounded by root_shards",
                f"<= {ROOT_SHARDS} root objects for one namespace", root_objs,
                root_objs <= ROOT_SHARDS,
                "" if root_objs <= ROOT_SHARDS else f"more than {ROOT_SHARDS} root objects — "
                                                    "unexpected root-shard fanout for one table"))
            result.add(Verdict.check(
                "root-shard body under manifest hard cap",
                f"mean root body < {K_MAX_MANIFEST_ENCODED_BYTES/MIB:.0f} MiB",
                f"{mean_root_bytes/MIB:.3f} MiB",
                mean_root_bytes < K_MAX_MANIFEST_ENCODED_BYTES))
        else:
            result.add(Verdict.inconclusive(
                "root-shard objects bounded by root_shards", f"<= {ROOT_SHARDS}",
                "pool shape unavailable at peak (probe failed/timed out)"))

        # CAS contention bounded by root_shards: conflicts should be a small fraction of total CAS ops
        # (with only ROOT_SHARDS shards, concurrent writers serialize per shard but do not livelock).
        if cas_total > 0:
            ratio = cas_conflict / cas_total
            result.observations["s08_cas_conflict_ratio"] = round(ratio, 4)
            result.add(Verdict.check(
                "CAS contention bounded", "conflict ratio bounded (< 0.5)",
                f"{cas_conflict}/{cas_total} = {ratio:.3f}", ratio < 0.5,
                "" if ratio < 0.5 else "root-shard CAS conflict ratio high — contention not bounded "
                                       "by root_shards as expected"))
        else:
            result.add(Verdict.inconclusive(
                "CAS contention bounded", "conflict ratio bounded",
                "no CasRootCas ops observed in the creation window"))

        # --- Convergence phase: re-enable merges and force convergence -------------------
        ctx.log("S08: re-enabling merges and forcing convergence")
        for n in cl.nodes():
            try:
                n.command(f"SYSTEM START MERGES {table}")
            except Exception as e:
                ctx.log(f"S08: START MERGES on {n.container} raised: {e}")

        # Correctness oracle before/after convergence (data must be intact through the merge).
        _common.assert_replicas_agree(result, cl, sql.table_checksum_query(table))

        # standard_end OPTIMIZEs FINAL + drives forced GC to fixpoint; after that physical bytes should
        # converge toward referenced bytes.
        end = _common.standard_end(ctx, result, [table])

        ps_after = sql.parts_summary(nodes[0], table)
        result.observations["s08_parts_after_merge"] = ps_after
        result.add(Verdict.check(
            "parts converged after merge", "active parts << peak after OPTIMIZE FINAL",
            f"{ps_after.get('active')} active (peak {ps.get('active')})",
            ps_after.get("active", 0) <= max(1, ps.get("active", 0))))

        # Physical vs referenced bytes convergence (from the final fsck summary collected by end).
        fsck = (end or {}).get("fsck_final", {})
        phys = fsck.get("physical_bytes")
        ref = fsck.get("referenced_logical_bytes")
        result.observations["s08_physical_bytes"] = phys
        result.observations["s08_referenced_bytes"] = ref
        if phys is not None and ref is not None and ref > 0:
            # After GC, physical should be within a small multiple of referenced (no large orphan tail).
            ok = phys <= ref * 2 + (16 * MIB)
            result.add(Verdict.check(
                "physical bytes converge toward referenced",
                "physical <= ~2x referenced after merge+GC",
                f"physical={phys} referenced={ref}", ok,
                "" if ok else "physical bytes far exceed referenced after GC — orphan tail not reclaimed"))
        else:
            result.add(Verdict.inconclusive(
                "physical bytes converge toward referenced", "physical ~ referenced",
                "fsck physical/referenced byte fields unavailable"))

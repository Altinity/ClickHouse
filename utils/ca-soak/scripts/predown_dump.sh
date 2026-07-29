#!/bin/bash
# Pre-teardown dump of an instrumented CA soak/scenario cluster.
#
# WHY THIS EXISTS. The ca-soak compose mounts only the binary, the configs and ./logs/chN. There is no
# volume for /var/lib/clickhouse, so every system table dies with the container. On 2026-07-29 a GC
# performance audit lost its entire queryable specimen that way — the 29 GiB pool was gone before a
# single query ran against it, and the only survivors were the host-mounted text logs. This runs
# BEFORE `docker compose down` and makes the specimen outlive its own cluster.
#
# Usage:  scripts/predown_dump.sh [label] [--from 'YYYY-MM-DD HH:MM:SS'] [--to '...'] [--focus SYMBOL]
#
#   --from/--to   restrict every time-based extract to that `event_time` window. Run-wide aggregates
#                 blend GC with insert, merge and fetch load — on the 2026-07-29 specimen that meant
#                 124k relink exceptions per node drowning the signal — so once rounds are bounded and
#                 a single round has a start and an end, pointing the aggregates at ONE round's window
#                 is what makes "where did this round spend its time" answerable at all.
#   --focus SYM   also emit `trace_<type>_focus_stacks.tsv`: the top stacks restricted to those whose
#                 INNERMOST frame demangles to a symbol matching SYM. The plain top-200 is by whole
#                 stack, so a hot leaf spread across many distinct stacks (`pthread_mutex_lock` with
#                 57k query-side Real samples on that specimen) need never reach the list — you can see
#                 the magnitude but not which mutex. This filter shows what sits beneath it.
# Writes: logs/predown/<node>/<label>/*.tsv + manifest.txt   (host side, survives teardown)
#
# NOT inside logs/<node>/ — the container writes that directory as root/syslog and the harness user
# cannot create subdirectories in it. `logs/` itself is ours, so the dump sits beside the per-node log
# dirs, which also keeps it clear of the `logs/chN -> logs/chN_pre_<tag>` archive renames.
#
# Everything here is READ-ONLY against the servers. Aggregates, not raw dumps, for trace_log — a raw
# trace_log of a 90-minute soak is gigabytes and nobody reads it; the top-stack rollups are what the
# audit actually asks for. Each frame carries its SIDE (query vs background), because a stack is not
# interpretable without knowing which lane produced it.

set -u
CA_SOAK_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$CA_SOAK_DIR" || exit 90

LABEL=""
FROM_TS=""
TO_TS=""
FOCUS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --from) FROM_TS="$2"; shift 2 ;;
        --to)   TO_TS="$2";   shift 2 ;;
        --focus) FOCUS="$2";  shift 2 ;;
        *) [ -z "$LABEL" ] && LABEL="$1"; shift ;;
    esac
done
LABEL="${LABEL:-$(date +%Y%m%dT%H%M%S)}"

# One predicate, reused by every time-based extract, so a window can never be applied to some of them
# and silently not others.
WINDOW=""
[ -n "$FROM_TS" ] && WINDOW="$WINDOW AND event_time >= '$FROM_TS'"
[ -n "$TO_TS" ]   && WINDOW="$WINDOW AND event_time <= '$TO_TS'"
NODES="${PREDOWN_NODES:-ch1:8123 ch2:8124}"
MANIFEST_ROWS=""
FAILED=0

q() {  # q <port> <sql> <outfile>
    local port="$1" sql="$2" out="$3"
    if curl -sS -m 300 --get --data-urlencode "query=$sql" "http://localhost:${port}/" > "$out" 2>"${out}.err"; then
        return 0
    fi
    return 1
}

# `allow_introspection_functions` is required for `demangle`/`addressToSymbol`; without it the stack
# columns come back as raw addresses, which are useless once the binary is gone.
INTRO="SETTINGS allow_introspection_functions = 1"

for spec in $NODES; do
    node="${spec%%:*}"
    port="${spec##*:}"
    dir="logs/predown/${node}/${LABEL}"
    mkdir -p "$dir" || { echo "cannot create $dir"; FAILED=1; continue; }
    echo "=== ${node} (localhost:${port}) -> ${dir}"

    # Flush first: system logs buffer in memory and the un-flushed tail is exactly the interesting end
    # of the run.
    curl -sS -m 120 -X POST --data-binary "SYSTEM FLUSH LOGS" "http://localhost:${port}/" >/dev/null 2>&1

    # (a) the whole GC log — small, and the audit's primary table.
    q "$port" "SELECT * FROM system.content_addressed_garbage_collection_log WHERE 1 ${WINDOW} FORMAT TSVWithNames" \
      "$dir/gc_log.tsv"

    # (b) trace_log aggregates, per trace type, SPLIT BY SIDE and carrying the side per row.
    # The label column is deliberately NOT named `trace_type`: ClickHouse resolves SELECT aliases in
    # WHERE and the alias wins, so `'CPU' AS trace_type` + `WHERE trace_type = 'CPU'` compares the
    # literal with itself and the filter never fires. That exact bug shipped every pre-2026-07-29
    # specimen as an unfiltered mix of ALL trace types in both files (found by the GC audit: __poll
    # with identical Real and CPU sample counts).
    for tt in CPU Real; do
        q "$port" "
            SELECT
                multiIf(query_id = '', 'background', 'query') AS side,
                '${tt}' AS dumped_trace_type,
                count() AS samples,
                arrayStringConcat(arrayMap(x -> demangle(addressToSymbol(x)), trace), '\n') AS stack
            FROM system.trace_log
            WHERE trace_type = '${tt}' ${WINDOW}
            GROUP BY side, stack
            ORDER BY samples DESC
            LIMIT 200
            FORMAT TSVWithNames ${INTRO}" "$dir/trace_${tt}_top_stacks.tsv"

        # Top-FRAME rollup: which symbol appears anywhere in a sampled stack, and on which side.
        q "$port" "
            SELECT
                multiIf(query_id = '', 'background', 'query') AS side,
                '${tt}' AS dumped_trace_type,
                demangle(addressToSymbol(frame)) AS symbol,
                count() AS samples
            FROM system.trace_log
            ARRAY JOIN trace AS frame
            WHERE trace_type = '${tt}' ${WINDOW}
            GROUP BY side, symbol
            ORDER BY samples DESC
            LIMIT 200
            FORMAT TSVWithNames ${INTRO}" "$dir/trace_${tt}_top_frames.tsv"
        # Optional focus extract: the stacks whose INNERMOST frame is the symbol under investigation.
        if [ -n "$FOCUS" ]; then
            q "$port" "
                SELECT
                    multiIf(query_id = '', 'background', 'query') AS side,
                    '${tt}' AS dumped_trace_type,
                    count() AS samples,
                    arrayStringConcat(arrayMap(x -> demangle(addressToSymbol(x)), trace), '\n') AS stack
                FROM system.trace_log
                WHERE trace_type = '${tt}' ${WINDOW}
                  AND length(trace) > 0
                  AND demangle(addressToSymbol(trace[1])) ILIKE '%${FOCUS}%'
                GROUP BY side, stack
                ORDER BY samples DESC
                LIMIT 200
                FORMAT TSVWithNames ${INTRO}" "$dir/trace_${tt}_focus_stacks.tsv"
        fi
    done

    # (a2) part_log and the CA event log — added 2026-07-29 after an RCA needed exactly these two and
    #      found them gone. `part_log` carries the per-part remediation timeline (event types,
    #      durations, error codes); `content_addressed_log` carries mount/lease lifecycle and the
    #      ref-plane events, which is the only way to answer "did a remove-broken path drop CAS refs,
    #      and did a re-publish follow". Both live in the container's /var/lib/clickhouse and die with
    #      it, which is the whole reason this dump exists — omitting them defeated its purpose once.
    q "$port" "SELECT * FROM system.part_log WHERE 1 ${WINDOW} FORMAT TSVWithNames" "$dir/part_log.tsv"
    q "$port" "SELECT * FROM system.content_addressed_log WHERE 1 ${WINDOW} FORMAT TSVWithNames" \
      "$dir/content_addressed_log.tsv"

    # (c) every counter, including the zeros — a counter that never moved is evidence too.
    q "$port" "SELECT event, value, description FROM system.events ORDER BY event FORMAT TSVWithNames SETTINGS system_events_show_zero_values = 1" \
      "$dir/events.tsv"

    # (d) server-side error tallies.
    q "$port" "SELECT * FROM system.errors ORDER BY value DESC FORMAT TSVWithNames" "$dir/errors.tsv"

    # (e) Error-level text_log rolled up by message SHAPE (digits and hex runs folded), so a 100k-line
    #     storm becomes one row with a count instead of a file nobody opens.
    q "$port" "
        SELECT
            count() AS occurrences,
            min(event_time) AS first_seen,
            max(event_time) AS last_seen,
            replaceRegexpAll(substring(message, 1, 200), '[0-9a-f]{8,}|[0-9]{3,}', 'N') AS shape
        FROM system.text_log
        WHERE level IN ('Error', 'Fatal') ${WINDOW}
        GROUP BY shape
        ORDER BY occurrences DESC
        LIMIT 200
        FORMAT TSVWithNames" "$dir/text_log_error_shapes.tsv"

    # Manifest, one row per file. The status distinguishes three things that are NOT the same, and
    # conflating them is the exact defect this whole dump exists to prevent:
    #   ok            — the query ran and returned rows;
    #   ok(no-rows)   — the query ran and the answer was legitimately empty (e.g. a node with no
    #                   Error-level log lines). That is an OBSERVATION, not a failure, and it must not
    #                   fail the dump — but it is marked so a reader never mistakes it for `ok`;
    #   QUERY-FAILED  — the query itself did not run. Only this fails the dump.
    {
        echo "# predown dump  node=${node}  label=${LABEL}  taken=$(date -Iseconds)"
        echo "# window: from='${FROM_TS:-(none)}' to='${TO_TS:-(none)}'  focus='${FOCUS:-(none)}'"
        for f in "$dir"/*.tsv; do
            [ -e "$f" ] || continue
            lines=$(wc -l < "$f")
            if [ -s "${f}.err" ]; then
                status="QUERY-FAILED"
                FAILED=1
            elif [ "$lines" -le 1 ]; then
                status="ok(no-rows)"
            else
                status="ok"
            fi
            printf '%-32s %8s lines  %s\n' "$(basename "$f")" "$lines" "$status"
        done
    } > "$dir/manifest.txt"
    cat "$dir/manifest.txt"
done

if [ "$FAILED" -ne 0 ]; then
    echo "PREDOWN DUMP FAILED: at least one query did not run — see QUERY-FAILED in the manifests above"
    exit 1
fi
echo "PREDOWN DUMP OK (label=${LABEL})"

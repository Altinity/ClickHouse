#!/usr/bin/env bash

# Refuse temporal verdicts from a TLC that cannot prove the tautology `<> TRUE`.
check_tlc_temporal_gate() {
  local jar="$1"
  local metadir log
  metadir="$(mktemp -d ../../../tmp/tlc-temporal-smoke.XXXXXX)" || return 1
  log="$metadir/tlc.log"

  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$jar" tlc2.TLC \
    -metadir "$metadir" -workers 1 -config TlcTemporalSmoke.cfg TlcTemporalSmoke.tla >"$log" 2>&1

  if grep -q "Temporal properties were violated" "$log"; then
    echo "refusing temporal verdicts: $jar violates the <> TRUE smoke test ($log)" >&2
    return 1
  fi
  if ! grep -q "No error has been found" "$log"; then
    echo "refusing temporal verdicts: <> TRUE smoke test did not complete ($log)" >&2
    return 1
  fi
}

#!/usr/bin/env bash

TLC_PIN_SHA256=cc4803dce2a8ffaf0f5920a9dc39df4b5ee34ab4cb53fb58ac557277a7e516b3

# Safety and temporal checking share this one official build. Refuse a merely executable jar: the
# version banner is prose, while the digest pins the exact checker bytes the recorded evidence used.
check_tlc_pin() {
  local jar="$1"
  local actual

  if [[ ! -f "$jar" ]]; then
    echo "refusing TLC checker: jar not found: $jar" >&2
    return 1
  fi
  actual="$(sha256sum -- "$jar")" || return 1
  actual="${actual%% *}"
  if [[ "$actual" != "$TLC_PIN_SHA256" ]]; then
    echo "refusing TLC checker: $jar has SHA-256 $actual, expected $TLC_PIN_SHA256" >&2
    return 1
  fi
}

# Refuse temporal verdicts from a TLC that cannot prove the tautology `<> TRUE`.
check_tlc_temporal_gate() {
  local jar="$1"
  local metadir log
  metadir="$(mktemp -d ../../../tmp/tlc-temporal-smoke.XXXXXX)" || return 1
  log="$metadir/tlc.log"

  /usr/bin/java -XX:+UseParallelGC ${TLC_JAVA_OPTS:-} -cp "$jar" tlc2.TLC \
    -metadir "$metadir" -workers 1 -config TlcTemporalSmoke.cfg TlcTemporalSmoke.tla >"$log" 2>&1

  # Both message forms: the pinned jar prints the singular "Temporal property <Name> was violated."
  # for a single declared PROPERTY; older builds print only the generic plural. Matching one form
  # would let a broken checker slip past this refusal fail-open.
  if grep -qE 'Temporal propert(y|ies)( [A-Za-z0-9_]+)? (was|were) violated' "$log"; then
    echo "refusing temporal verdicts: $jar violates the <> TRUE smoke test ($log)" >&2
    return 1
  fi
  if ! grep -q "No error has been found" "$log"; then
    echo "refusing temporal verdicts: <> TRUE smoke test did not complete ($log)" >&2
    return 1
  fi
}

# Gate only suites that currently ask the runner to TRUST a temporal violation. Safety-only suites do
# not pay a redundant TLC invocation, while adding a future `temporal` row makes the smoke mandatory by
# construction. Rows use the phase-runner grammar: name, expectation, expected property/invariant.
check_tlc_temporal_expectations() {
  local jar="$1"
  shift

  local row name expectation ignored
  for row in "$@"; do
    read -r name expectation ignored <<<"$row"
    if [[ "$expectation" == "temporal" ]]; then
      check_tlc_temporal_gate "$jar"
      return
    fi
  done
}

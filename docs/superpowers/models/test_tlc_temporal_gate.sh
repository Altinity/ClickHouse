#!/usr/bin/env bash

set -uo pipefail
cd "$(dirname "$0")"
source ./tlc_temporal_gate.sh

missing_jar="../../../tmp/tlc-temporal-gate-deliberately-missing-$$.jar"
checker_jar="${TLC_TEST_JAR:-../../../tmp/tla2tools.jar}"

# Both safety and temporal runs use one exact checker build. The positive path is parameterized only so
# this test can validate the official candidate without replacing a developer's current tmp symlink.
if ! check_tlc_pin "$checker_jar"; then
  echo "the official pinned checker was rejected" >&2
  exit 1
fi
if check_tlc_pin "$missing_jar"; then
  echo "an unpinned checker was accepted" >&2
  exit 1
fi

# A suite with no temporal expectation owes no temporal-checker smoke run. In particular, it must not
# fail merely because the deliberately missing jar cannot run the smoke module.
if ! check_tlc_temporal_expectations "$missing_jar" \
    "safe green -" \
    "sabotage violation SomeSafetyInvariant"
then
  echo "safety-only expectations ran the temporal smoke checker" >&2
  exit 1
fi

# The same missing checker must be rejected as soon as one row asks the runner to trust a temporal
# verdict. This catches a future temporal row being added without making the smoke gate live.
if check_tlc_temporal_expectations "$missing_jar" \
    "safe green -" \
    "liveness_sabotage temporal EventuallyProgress"
then
  echo "temporal expectation was accepted without a working smoke checker" >&2
  exit 1
fi

echo "TEMPORAL EXPECTATION GATE TEST PASS"

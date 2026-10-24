#!/bin/bash
# seq2.sh LABEL BINARY LAT_MS SCALE: one S11 run with 300 s GC rounds (gc-long-interval.xml) and LAT_MS of object store latency (0 = none).
# Collects runs/LABEL, writes longround/LABEL.{log,txt,down.log} and appends to longround/seq.log under $GC_AB_OUT (default: current directory).
# Refuses to start while a ca-soak stack is up or a STOP file exists in $GC_AB_OUT.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export GC_AB_OUT=${GC_AB_OUT:-$PWD}
cd "$GC_AB_OUT"
mkdir -p longround
LOG=longround/seq.log
one() { local label=$1 bin=$2 lat=$3 scale=$4
  [ -e STOP ] && { echo "STOP seen before $label" >> $LOG; return 1; }
  if docker ps --format '{{.Names}}' | grep -q '^ca-soak-'; then echo "stack present before $label" >> $LOG; return 1; fi
  echo "START $label lat=$lat scale=$scale $(date +%T)" >> $LOG
  if [ "$lat" -gt 0 ]; then "$HERE/netem.sh" $lat longround/$label.netem.txt & fi
  GC_EXTRA_CONFIG=$HERE/gc-long-interval.xml "$HERE/../run-stand.sh" run $label $bin --scenario S11 --seed 20260930 --duration 300s --scale $scale > longround/$label.log 2>&1
  echo "END $label $(date +%T) $(grep -E 'RUN_RC' longround/$label.log) $(grep -E 'S11 DONE' longround/$label.log | cut -c1-90)" >> $LOG
  python3 "$HERE/../extract.py" runs/$label > longround/$label.txt 2>&1
  "$HERE/../run-stand.sh" down > longround/$label.down.log 2>&1
}
one "$@"

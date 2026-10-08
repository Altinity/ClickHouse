#!/bin/bash
# Post-run probe of a live ca-soak stack: fsck detail, outdated part counts, then one forced GC round.
#   probe.sh <out-dir> <iterations> [gc=1]   (gc=0 skips the forced round)
out=$1; n=$2; gc=${3:-1}
mkdir -p $out
q() { docker exec "$1" clickhouse-client -q "$2" 2>&1; }
for i in $(seq 1 $n); do
    ts=$(date +%H%M%S)
    docker exec ca-soak-ch1-1 clickhouse disks --config-file /etc/clickhouse-server/fsck-only.xml --disk ca_ro --query "cas-fsck --detail" > $out/fsck_$ts.txt 2>&1
    summ=$(grep -a '^reachable=' $out/fsck_$ts.txt)
    um=$(grep -a -P '^unreachable\t' $out/fsck_$ts.txt | grep -c '/manifests/')
    ub=$(grep -a -P '^unreachable\t' $out/fsck_$ts.txt | grep -vc '/manifests/')
    pg=$(grep -a -cP '^(pending-gc|awaiting-gc)\t' $out/fsck_$ts.txt)
    o1=$(q ca-soak-ch1-1 "SELECT count() FROM system.parts WHERE active=0 AND disk_name='ca'")
    o2=$(q ca-soak-ch2-1 "SELECT count() FROM system.parts WHERE active=0 AND disk_name='ca'")
    echo "$ts iter=$i unreach_manifests=$um unreach_other=$ub pending=$pg outdated_parts ch1=$o1 ch2=$o2 | $summ" | tee -a $out/probe.log
    if [ "$gc" = 1 ]; then
        q ca-soak-ch1-1 "SYSTEM CAS GC RUN ca" > /dev/null
    fi
    sleep 11
done
echo PROBE_DONE >> $out/probe.log

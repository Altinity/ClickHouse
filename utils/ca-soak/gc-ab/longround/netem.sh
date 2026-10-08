#!/bin/bash
# netem.sh LAT_MS OUTFILE: once both nodes answer, add LAT_MS egress delay on the object store container (RTT + LAT_MS per request).
# Needs docker and NET_ADMIN for a helper container; NETEM_IMAGE is any image with `tc` (iproute2).
lat=$1; out=$2
image=${NETEM_IMAGE:-series-measure-fault-tools:local}
until docker exec ca-soak-ch2-1 clickhouse-client -q "select 1" >/dev/null 2>&1; do sleep 2; done
sleep 10
docker run --rm --net container:ca-soak-rustfs1-1 --cap-add NET_ADMIN --entrypoint sh "$image" -c "tc qdisc add dev eth0 root netem delay ${lat}ms limit 100000 && tc qdisc show dev eth0" >> $out 2>&1
echo "armed $(date +%T)" >> $out
for i in 1 2 3; do docker exec ca-soak-ch1-1 curl -s -o /dev/null -w "curl rustfs1 time_total=%{time_total}\n" http://rustfs1:11121/ >> $out 2>&1; done

#!/usr/bin/env bash
# Usage: bench/compare.sh <binary A> <binary B> [runs] [workload]
# Runs A and B alternately so machine-speed drift hits both equally.
A=$1; B=$2; RUNS=${3:-5}; WL=${4:-deep}
rm -f /tmp/ab_*
for _ in $(seq "$RUNS"); do
  for pair in "A:$A" "B:$B"; do
    side=${pair%%:*}; bin=${pair#*:}
    out=$("$bin" 1000000 "$WL")
    echo "$out" | awk '/Throughput/ {print $3}' >> /tmp/ab_t$side
    echo "$out" | awk '/Latency/ {print $11}'   >> /tmp/ab_p99_$side
    echo "$out" | awk '/Latency/ {print $14}'   >> /tmp/ab_p999_$side
  done
done
med() { sort -n "$1" | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
printf "%-3s %14s %10s %10s\n" "" "ops/sec" "p99 ns" "p99.9 ns"
for side in A B; do
  printf "%-3s %14s %10s %10s\n" "$side" "$(med /tmp/ab_t$side)" "$(med /tmp/ab_p99_$side)" "$(med /tmp/ab_p999_$side)"
done

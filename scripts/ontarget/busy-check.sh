#!/bin/bash
# Validate busy% (ref-cycles) against workloads with a known duty cycle.
RH=/home/test/pmutop/pmutop
PP=/home/test/evl-pingpong/evl-pingpong
cc -O2 -o /tmp/phases /home/test/pmutop/tests/phases.c || exit 1
avg() { awk -F, -v c=$1 'NR>2 && $2==c {b+=$17; u+=$4; n++} END{printf "cpu%d busy %.1f%%  linux util %.1f%%  (%d samples)\n", c, 100*b/n, 100*u/n, n}' "$2"; }
echo "== idle"
taskset -c 0 $RH -c 4,5 -i 1000 -n 6 -o /tmp/b0.csv > /dev/null 2>&1; avg 4 /tmp/b0.csv; avg 5 /tmp/b0.csv
echo "== evl-pingpong (EVL threads on 4 and 5)"
$PP -a 4 -b 5 -d 12 -q -S /tmp/b1.md > /dev/null 2>&1 &
sleep 2; taskset -c 0 $RH -c 4,5 -i 1000 -n 8 -o /tmp/b1.csv > /dev/null 2>&1; wait
avg 4 /tmp/b1.csv; avg 5 /tmp/b1.csv
grep "Round busy" /tmp/b1.md | awk -F'|' '{print "  pingpong says A is busy" $5 "us per 1000 us round"}'
echo "== Linux task, 50% duty cycle, on CPU 4"
taskset -c 4 /tmp/phases 12 0 &
sleep 2; taskset -c 0 $RH -c 4 -i 1000 -n 8 -o /tmp/b2.csv > /dev/null 2>&1; wait
avg 4 /tmp/b2.csv

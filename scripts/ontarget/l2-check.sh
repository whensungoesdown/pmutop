#!/bin/bash
# L2 miss/K on CPU 4/5: idle, evl-pingpong, cache-thrashing task;
# cross-checked against perf stat for the same event.
RH=/home/test/pmutop/pmutop
PP=/home/test/evl-pingpong/evl-pingpong
cc -O2 -o /tmp/phases /home/test/pmutop/tests/phases.c || exit 1
col() { awk -F, -v c=$2 'NR==1{for(i=1;i<=NF;i++){if($i=="l2_mpki")L=i; if($i=="llc_mpki")C=i; if($i=="busy")B=i}}
	NR>2 && $2==c {l+=$L; m+=$C; b+=$B; n++} END{printf "cpu%d L2/K %.3f  LLC/K %.3f  busy %.1f%%\n", c, l/n, m/n, 100*b/n}' "$1"; }
echo "== idle";     taskset -c 0 $RH -c 4,5 -n 6 -o /tmp/l0.csv 2>&1 | grep -i l2; col /tmp/l0.csv 4; col /tmp/l0.csv 5
echo "== pingpong"; $PP -a 4 -b 5 -d 12 -q >/dev/null 2>&1 & sleep 2
taskset -c 0 $RH -c 4,5 -n 8 -o /tmp/l1.csv >/dev/null 2>&1; wait; col /tmp/l1.csv 4; col /tmp/l1.csv 5
echo "== 64 KiB sequential (fits L2) on cpu4"; taskset -c 4 /tmp/phases 10 0 2>/dev/null & sleep 2
taskset -c 0 $RH -c 4 -n 6 -o /tmp/l2.csv >/dev/null 2>&1; wait; col /tmp/l2.csv 4
echo "== 512 MiB random chase on cpu4"
# phases shuffles its 512 MiB array first (seconds of ordinary code):
# wait for the chase itself before measuring.
rm -f /tmp/ph.log; taskset -c 4 /tmp/phases 0 12 2>/tmp/ph.log &
until grep -q thrash /tmp/ph.log 2>/dev/null; do sleep 0.2; done; sleep 1
taskset -c 0 $RH -c 4 -n 6 -o /tmp/l3.csv >/dev/null 2>&1 &
perf stat -C 4 -e cpu_core/l2_rqsts.demand_data_rd_miss/,cpu_core/instructions/ -- sleep 4 2>&1 | grep -E "miss|instr" | awk '{print "  perf: " $1, $2}'
wait; col /tmp/l3.csv 4

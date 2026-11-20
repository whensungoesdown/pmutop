#!/bin/bash
# Counter sharing: pmutop must keep reading (scaled) values while perf
# and a second pmutop use the same counters.  CPU 2 runs llama-server
# here; CPU 4 is an isolated RT core with evl-pingpong.
RH=/home/test/pmutop/pmutop
PP=/home/test/evl-pingpong/evl-pingpong
l2() { awk -F, -v c=$2 'NR==1{for(i=1;i<=NF;i++){if($i=="l2_mpki")L=i; if($i=="ipc")P=i}}
	NR>2 && $2==c && $L!="nan" {l+=$L; p+=$P; n++} END{printf "  cpu%d  L2/K %6.2f  IPC %.2f  (%d valid samples)\n", c, l/n, p/n, n}' "$1"; }
$PP -a 4 -b 5 -d 40 -q >/dev/null 2>&1 &
sleep 1
echo "== perf alone, 5 s"
perf stat -C 2,4 -A -e '{cpu_core/l2_rqsts.demand_data_rd_miss/,cpu_core/instructions/}' -- sleep 5 2>&1 |
	awk '/rqsts|instr/{gsub(",","",$2); v[$1" "$3]=$2} END{for(c in v) if(c ~ /rqsts/){split(c,a," "); k=a[1]" cpu_core/instructions/"; printf "  %s  L2/K %6.2f\n", a[1], 1000*v[c]/v[k]}}' | sort
echo "== pmutop x2 + perf, all at once, 8 s"
taskset -c 0 $RH -c 2,4 -n 8 -o /tmp/s1.csv >/tmp/s1.txt 2>&1 &
taskset -c 1 $RH -c 2,4 -n 8 -o /tmp/s2.csv >/tmp/s2.txt 2>&1 &
sleep 2; perf stat -C 2,4 -e '{cpu_core/l2_rqsts.demand_data_rd_miss/,cpu_core/instructions/,cpu_core/cache-misses/,cpu_core/LLC-loads/,cpu_core/dTLB-loads/}' -- sleep 4 >/dev/null 2>&1
wait %2 %3 2>/dev/null; sleep 1
echo " first pmutop:";  l2 /tmp/s1.csv 2; l2 /tmp/s1.csv 4; grep -i "read\|disabled" /tmp/s1.txt
echo " second pmutop:"; l2 /tmp/s2.csv 2; l2 /tmp/s2.csv 4; grep -i "read\|disabled" /tmp/s2.txt
echo "== WARN rows (cpu2 must have none for device IRQ)"
grep -h WARN /tmp/s1.txt /tmp/s2.txt | cut -c1-6,120-200 | sort | uniq -c
wait

#!/bin/bash
RH=/home/test/pmutop/pmutop
PP=/home/test/evl-pingpong/evl-pingpong
cd /tmp
echo "===== 1: pingpong 60 s + stress on non-RT cores, no injection (expect no WARN)"
taskset -c 0 "$RH" -c 4,5 -i 1000 -o /tmp/r1.csv > /tmp/r1.txt 2>&1 & M=$!
# --cache-no-affinity: the cache stressor otherwise hops onto every CPU,
# isolated ones included, whatever taskset says.
taskset -c 1-3,7-13 stress-ng --cache 10 --cache-no-affinity --vm 4 --vm-bytes 2G -t 70 >/dev/null 2>&1 & S=$!
sleep 3
$PP -a 4 -b 5 -d 60 -q -S /tmp/r1.md > /dev/null 2>&1
kill $S $M 2>/dev/null; wait 2>/dev/null
awk -F, 'NR>1{c[$2"_"$3]++} END{for(k in c) print "  "k, c[k]}' /tmp/r1.csv | sort
grep -E "A wakeup|Real-time" /tmp/r1.md
echo "===== 2: Wi-Fi IRQ 183 moved to CPU 4 for 10 s (expect WARN naming it)"
taskset -c 0 "$RH" -c 4 -i 1000 -n 16 > /tmp/r2.txt 2>&1 & M=$!
sleep 3; echo 4 > /proc/irq/183/smp_affinity_list
GW=$(ip route | awk '/default/{print $3; exit}')
ping -c 900 -i 0.01 -s 1200 $GW >/dev/null 2>&1
echo 0-3,6-15 > /proc/irq/183/smp_affinity_list
wait $M
awk 'NR>1{print "  "$2}' /tmp/r2.txt | sort | uniq -c
grep WARN /tmp/r2.txt | head -2 | sed 's/^.*  device/  device/'

#!/bin/bash
RH=/home/test/rt-health/rt-health
cd /home/test/rt-health && cc -O2 -o /tmp/phases tests/phases.c || exit 1
taskset -c 0 "$RH" -c 4 -i 500 -n 150 -o /tmp/c.csv > /tmp/c.txt 2>&1 & M=$!
sleep 2
taskset -c 4 /tmp/phases 50 20
sleep 3; kill $M 2>/dev/null; wait $M 2>/dev/null
echo "t_rel state util ipc llc_mpki dtlb_mpki"
awk -F, 'NR==2{t0=$1} NR>1{printf "%5.1f %-4s %4.0f%% %5s %7s %7s\n",$1-t0,$3,$4*100,$5,$6,$7}' /tmp/c.csv | awk 'NR%10==1'
echo "--- WARN reasons"
grep WARN /tmp/c.txt | sed 's/^.*  \([A-Za-z].*\)$/\1/' | cut -c1-14 | sort | uniq -c

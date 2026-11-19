#!/bin/bash
# Drive -graphic for a demo and capture the screen at t=40 s:
#   0-45 s  evl-pingpong on CPU 4 and 5
#  10-25 s  cache/TLB-thrashing Linux task on CPU 5
#  15-30 s  Wi-Fi interrupt moved onto CPU 4, with traffic
RH=/home/test/rt-health/rt-health
PP=/home/test/evl-pingpong/evl-pingpong
IRQ=${IRQ:-183}
OLD=$(cat /proc/irq/$IRQ/smp_affinity_list)
cc -O2 -o /tmp/phases /home/test/rt-health/tests/phases.c || exit 1
$PP -a 4 -b 5 -d 45 -q > /dev/null 2>&1 &
( sleep 10; taskset -c 5 /tmp/phases 0 15 2>/dev/null ) &
(
	sleep 15; echo 4 > /proc/irq/$IRQ/smp_affinity_list
	GW=$(ip route | awk '/default/{print $3; exit}')
	ping -c 1300 -i 0.01 -s 1200 "$GW" > /dev/null 2>&1
	echo "$OLD" > /proc/irq/$IRQ/smp_affinity_list
) &
python3 /home/test/rt-health/tests/ptyshot.py ${ROWS:-50} ${COLS:-160} 40 /tmp/shot \
	-- taskset -c 0 $RH -c 4,5 -i 500 -graphic
echo "$OLD" > /proc/irq/$IRQ/smp_affinity_list
wait

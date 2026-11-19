#!/bin/bash
# Drive -graphic for a demo: pingpong on CPU 4-5, Wi-Fi IRQ pushed onto
# CPU 4 from t=15 s to t=30 s, screen captured at t=40 s.
RH=/home/test/rt-health/rt-health
PP=/home/test/evl-pingpong/evl-pingpong
IRQ=${IRQ:-183}
OLD=$(cat /proc/irq/$IRQ/smp_affinity_list)
$PP -a 4 -b 5 -d 45 -q > /dev/null 2>&1 &
(
	sleep 15; echo 4 > /proc/irq/$IRQ/smp_affinity_list
	GW=$(ip route | awk '/default/{print $3; exit}')
	ping -c 1300 -i 0.01 -s 1200 "$GW" > /dev/null 2>&1
	echo "$OLD" > /proc/irq/$IRQ/smp_affinity_list
) &
python3 /home/test/rt-health/tests/ptyshot.py ${ROWS:-40} ${COLS:-120} 40 /tmp/shot \
	-- taskset -c 0 $RH -c 4,5 -i 500 -graphic
echo "$OLD" > /proc/irq/$IRQ/smp_affinity_list
wait

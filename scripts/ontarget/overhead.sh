#!/bin/bash
RH=/home/test/pmutop/pmutop
PP=/home/test/evl-pingpong/evl-pingpong
cd /tmp
row() { grep -E "A wakeup|Round trip" "$1" | awk -F'|' '{gsub(/ /,"",$2); printf "  %-10s avg %6s p99.9 %6s max %6s\n", substr($2,1,10), $5, $8, $9}'; grep -q "Real-time.*PASS" "$1" && echo "  PASS" || echo "  FAIL"; }
for run in 1 2; do
	echo "== round $run: no monitor"
	$PP -a 4 -b 5 -d 60 -q -S /tmp/o-none.md >/dev/null 2>&1; row /tmp/o-none.md
	echo "== round $run: monitor 1000 ms"
	taskset -c 0 $RH -c 4,5 -i 1000 > /tmp/o1.txt 2>&1 & M=$!
	$PP -a 4 -b 5 -d 60 -q -S /tmp/o-1000.md >/dev/null 2>&1; kill $M; wait $M 2>/dev/null; row /tmp/o-1000.md
	echo "== round $run: monitor 100 ms"
	taskset -c 0 $RH -c 4,5 -i 100 > /tmp/o2.txt 2>&1 & M=$!
	$PP -a 4 -b 5 -d 60 -q -S /tmp/o-100.md >/dev/null 2>&1; kill $M; wait $M 2>/dev/null; row /tmp/o-100.md
done
echo "monitor rows with WARN/FAIL:"; grep -cE "WARN|FAIL" /tmp/o1.txt /tmp/o2.txt

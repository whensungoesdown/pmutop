#!/bin/bash
# Smoke test: watch a busy thread, then a memory-thrashing one.
# Without root (perf_event_paranoid > 0 or no PMU in a VM) only the
# software events appear; on the real-time target run it as root.
cd "$(dirname "$0")/.." || exit 1

busy() { while :; do :; done; }
busy &
P=$!
echo "== busy loop, pid $P"
./pmutop -p "$P" -i 300 -n 3
kill "$P"

# 256 MiB random walk: defeats the caches and the TLB.
cat > /tmp/thrash.c <<'C'
#include <stdlib.h>
#include <stdint.h>
int main(void)
{
	size_t n = (256u << 20) / sizeof(uint64_t), i, j = 0;
	uint64_t *a = malloc(n * sizeof(*a)), s = 0;

	for (i = 0; i < n; i++)
		a[i] = (i * 2654435761u) % n;
	for (;;) {
		j = a[j];
		s += j;
		if (s == 42)
			return 1;
	}
}
C
cc -O2 -o /tmp/thrash /tmp/thrash.c || exit 1
/tmp/thrash &
P=$!
sleep 1
echo "== memory thrash, pid $P"
./pmutop -p "$P" -i 300 -n 3
kill "$P"

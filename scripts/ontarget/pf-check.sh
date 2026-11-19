#!/bin/bash
# Page-fault counting on an RT core: pingpong alone (expect 0), then a
# Linux task on CPU 4 that keeps touching fresh memory (expect many).
RH=/home/test/rt-health/rt-health
PP=/home/test/evl-pingpong/evl-pingpong
cat > /tmp/faulter.c <<'C'
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
int main(void)
{
	for (int k = 0; k < 400; k++) {		/* ~8 s: 4 MiB fresh pages per 20 ms */
		char *p = mmap(NULL, 4 << 20, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		memset(p, 1, 4 << 20);
		munmap(p, 4 << 20);
		usleep(20000);
	}
	return 0;
}
C
cc -O2 -o /tmp/faulter /tmp/faulter.c || exit 1
$PP -a 4 -b 5 -d 20 -q > /dev/null 2>&1 &
sleep 1
taskset -c 0 $RH -c 4,5 -i 1000 -n 18 -o /tmp/pf.csv > /dev/null 2>&1 &
M=$!
sleep 6; taskset -c 4 /tmp/faulter; wait $M; wait
awk -F, 'NR==1{for(i=1;i<=NF;i++) if($i=="page_faults") c=i} NR>1{printf "%s cpu%s pf=%s\n", int($1)%1000, $2, $c}' /tmp/pf.csv | paste - - | head -18

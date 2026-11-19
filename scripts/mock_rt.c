// SPDX-License-Identifier: MIT
/*
 * mock_rt.c - stand-in for an EVL task, for testing rt-health without a
 * real-time kernel.  Publishes frame telemetry exactly as the real-time
 * side would (rft_publish(), no syscall), at a fixed period, and misses
 * every 100th deadline on purpose.
 *
 *   mock_rt <telemetry.bin> [period_us] [frames]
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>

#include "../telemetry/rt_frames.h"

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/rt-telemetry.bin";
	uint64_t period = (argc > 2 ? atoi(argv[2]) : 1000) * 1000ull;	/* ns */
	uint64_t nframes = argc > 3 ? strtoull(argv[3], NULL, 0) : 30000;
	struct rft_page *p;
	struct rft_task *t;
	struct rft_frame f;
	uint64_t t0, tk, k;
	int fd;

	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, sizeof(*p)) < 0) {
		perror(path);
		return 1;
	}
	p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(p, 0, sizeof(*p));
	p->magic = RFT_MAGIC;
	p->version = RFT_VERSION;
	p->nr_tasks = 1;
	p->period_ns = period;
	p->epoch_ns = now_ns();
	t = &p->task[0];
	snprintf(t->name, sizeof(t->name), "mock");
	t->cpu = 4;
	t->period_ns = (uint32_t)period;

	t0 = now_ns() + 100000000ull;
	t0 = t0 + period - t0 % period;

	for (k = 0; k < nframes; k++) {
		tk = t0 + k * period;
		while (now_ns() < tk)
			;

		memset(&f, 0, sizeof(f));
		f.seq = k;
		f.t_start = tk;
		f.t_wake = now_ns();
		/* the fake work: ~15 us of arithmetic, so the monitor sees a cost */
		for (volatile unsigned i = 0; i < 20000; i++)
			;
		f.t_end = now_ns();
		f.deadline = tk + period;
		if ((k + 1) % 100 == 0) {
			f.flags |= RFT_F_OVERRUN;
			f.deadline = f.t_wake;	/* pretend it was late */
		}
		rft_publish(t, &f);
	}
	munmap(p, sizeof(*p));
	close(fd);
	return 0;
}

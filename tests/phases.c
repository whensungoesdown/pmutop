// SPDX-License-Identifier: MIT
/*
 * phases.c - a workload whose cache behaviour changes on cue, to check
 * that rt-health's learned baseline catches the change.
 *
 *   phases <calm_s> <thrash_s>
 *
 * Calm: a 64 KiB working set (fits L2), read sequentially.
 * Thrash: a 512 MiB random pointer chase (defeats the LLC, the
 * prefetcher and the dTLB).
 * Both phases run at the same 50% duty cycle, so utilisation does not
 * change and only the memory behaviour does.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static volatile uint64_t sink;

int main(int argc, char **argv)
{
	double calm = argc > 1 ? atof(argv[1]) : 40, thrash = argc > 2 ? atof(argv[2]) : 20;
	size_t small = 64 << 10, big = (size_t)512 << 20;
	size_t ns = small / 8, nb = big / 8, i, j = 0;
	uint64_t *a = malloc(small), *b = malloc(big), s = 0, t0, end, slot;

	if (!a || !b)
		return 1;
	for (i = 0; i < ns; i++)
		a[i] = i;
	/* single-cycle random permutation (Sattolo) for the pointer chase */
	for (i = 0; i < nb; i++)
		b[i] = i;
	srand(1);
	for (i = nb - 1; i > 0; i--) {
		size_t k = ((size_t)rand() * RAND_MAX + rand()) % i;
		uint64_t tmp = b[i];

		b[i] = b[k];
		b[k] = tmp;
	}

	t0 = now_ns();
	fprintf(stderr, "calm %.0f s\n", calm);
	end = t0 + (uint64_t)(calm * 1e9);
	while (now_ns() < end) {
		slot = now_ns() + 500000;		/* 0.5 ms work, 0.5 ms sleep */
		while (now_ns() < slot)
			for (i = 0; i < ns; i++)
				s += a[i];
		struct timespec z = { 0, 500000 };
		nanosleep(&z, NULL);
	}
	fprintf(stderr, "thrash %.0f s\n", thrash);
	end = now_ns() + (uint64_t)(thrash * 1e9);
	while (now_ns() < end) {
		slot = now_ns() + 500000;
		while (now_ns() < slot)
			for (i = 0; i < 1000; i++)
				j = b[j];
		struct timespec z = { 0, 500000 };
		nanosleep(&z, NULL);
	}
	sink = s + j;
	return 0;
}

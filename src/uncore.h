/* SPDX-License-Identifier: MIT */
#ifndef PMUTOP_UNCORE_H
#define PMUTOP_UNCORE_H

#include <stdbool.h>

/*
 * Chip-wide ("uncore") counters: not tied to a core, read once per
 * interval.  Each value is NAN when its PMU does not exist here.
 */
struct uncore_sample {
	double	cncu_ghz;	/* average clock of uncore_cncu over the interval */
	double	sncu_ghz;	/* average clock of uncore_sncu */
	double	dram_rd_gbs;	/* DRAM read bandwidth, GB/s (10^9 bytes) */
	double	dram_wr_gbs;	/* DRAM write bandwidth */
};

/* Returns the number of uncore counters opened (0: none available). */
int  uncore_open(char *msg, size_t msglen);
void uncore_read(struct uncore_sample *u, double dt_s);
void uncore_close(void);

#endif

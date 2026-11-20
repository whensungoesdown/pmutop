/* SPDX-License-Identifier: MIT */
#ifndef PMUTOP_TELEMETRY_H
#define PMUTOP_TELEMETRY_H

#include <stdint.h>
#include <sys/types.h>

#include "rt_frames.h"

struct telem {
	int		fd;
	const struct rft_page *page;
	uint64_t	frames_seen[RFT_MAX_TASKS];
	uint64_t	late_seen[RFT_MAX_TASKS];
	uint32_t	cursor[RFT_MAX_TASKS];
};

int  telem_open(struct telem *t, const char *path);
void telem_close(struct telem *t);

/*
 * Per-task delta over the interval: frames completed, frames late, the
 * worst frame duration actually seen, and a short note for the report.
 * Returns the number of tasks that advanced.
 */
int  telem_sample(struct telem *t, uint32_t task_cpu,
		  uint64_t *frames_delta, uint64_t *late_delta,
		  uint64_t *worst_ns, uint64_t *late_flags);

#endif

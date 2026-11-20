/* SPDX-License-Identifier: MIT */
#ifndef RT_HEALTH_PERF_H
#define RT_HEALTH_PERF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

/*
 * Hardware and software events.  Not every PMU implements every hardware
 * event; each is probed separately and the group carries on without it.
 */
enum {
	EV_CYCLES = 0,
	EV_INSTRUCTIONS,
	EV_CACHE_MISSES,
	EV_LLC_MISSES,
	EV_DTLB_MISSES,
	EV_ITLB_MISSES,
	EV_L1I_MISSES,
	EV_STALL_MEM,
	EV_REF_CYCLES,		/* unhalted cycles at the fixed reference rate */
	EV_L2_MISSES,		/* model-specific raw event, see perf_setup_raw() */
	EV_L1D_MISSES,		/* model-specific raw event, see perf_setup_raw() */
	EV_TASK_CLOCK,		/* software: only meaningful for a thread */
	EV_CTX_SWITCH,		/* software: a thread leaving the OOB stage */
	EV_PAGE_FAULT,		/* software: a thread faulting and demoting */
	EV_CPU_MIGRATE,		/* software: a thread moved between cores */
	EV_NR,
};

struct ev_desc {
	uint32_t	type;
	uint64_t	config;
	const char	*name;
};

struct perf_group {
	int		cpu;
	pid_t		pid;
	int		n_ev;
	int		leader;		/* EV_CYCLES, or EV_TASK_CLOCK without a PMU */
	bool		hw;		/* hardware events available */
	bool		user_only;	/* kernel excluded: perf_event_paranoid >= 2 */
	int		fd[EV_NR];
	uint64_t	id[EV_NR];	/* kernel event id, to decode group reads */
	bool		open[EV_NR];
	uint64_t	prev[EV_NR];
	uint64_t	prev_enabled, prev_running;	/* group times, ns */
	double		running;	/* share of the last interval actually counted */
	bool		warned;		/* "not scheduled" reported once */
};

const char *perf_event_name(int ev);
double perf_ref_hz(void);
int  perf_setup_raw(int ev, const char *spec, char *msg, size_t msglen);
bool perf_raw_covers(int ev, int cpu);
int  perf_open(struct perf_group *g, int cpu, pid_t pid, char *err, size_t errlen);
void perf_close(struct perf_group *g);
int  perf_read_interval(struct perf_group *g, int64_t out[EV_NR], bool ok[EV_NR],
			char *err, size_t errlen);

#endif

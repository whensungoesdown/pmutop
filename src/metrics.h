/* SPDX-License-Identifier: MIT */
#ifndef PMUTOP_METRICS_H
#define PMUTOP_METRICS_H

#include "pmutop.h"

enum base_metric {
	M_LLC_MPKI = 0,
	M_TLB_MPKI,
	M_IPC,
	M_IRQ_RATE,
	M_STALL,
	M_NR,
};

enum learn_phase {
	PH_WARMUP = 0,		/* too few samples to judge */
	PH_LEARNING,
	PH_RUNNING,
};

enum cpu_health {
	CPU_OK = 0,
	CPU_DEGRADED,		/* a learned baseline was exceeded */
	CPU_CRITICAL,		/* frame deadlines missed on this core */
};

struct metrics {
	int		cpu;
	bool		rt_core;	/* isolated real-time core: no device IRQ expected */
	enum learn_phase phase;
	unsigned	samples;

	/* instantaneous */
	double		util;
	double		busy;
	double		ipc;
	double		llc_mpki;
	double		l2_mpki;
	double		tlb_mpki;
	double		l3_mpki;
	double		stall_pct;
	double		irq_rate;
	double		freq_mhz;
	double		temp_c;

	/* learned baseline (median) and deviation from it */
	double		med_llc, med_tlb, med_ipc, med_irq, med_stall;
	double		z_llc, z_tlb, z_ipc, z_irq, z_stall;
	double		base_util;	/* utilisation of the intervals behind the baseline */

	enum cpu_health	health;
	char		why[160];
	const char	*note;
};

void metrics_init(struct metrics *m, int cpu);
void metrics_feed(struct metrics *m, const struct cpu_sample *s, double dt_s,
		  uint64_t frames_late, const char *task_note);

#endif

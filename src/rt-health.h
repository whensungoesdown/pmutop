/* SPDX-License-Identifier: MIT */
#ifndef RT_HEALTH_H
#define RT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "perf.h"

#define MAX_CPUS	256
#define MAX_CS	8

/*
 * One sampling interval of a single core.  Raw counters are deltas over
 * the interval; ratios are filled in by the sampler.
 */
struct cpu_sample {
	int		cpu;
	bool		perf_ok;

	/* PMU, delta over the interval; -1 when the event is unsupported */
	int64_t		ev[EV_NR];
	bool		ev_ok[EV_NR];

	/* /proc/stat cpuN line, jiffies */
	uint64_t	stat[8];
	uint64_t	irqs;		/* all interrupts, delta over the interval */

	/*
	 * The same delta split by kind.  On an EVL core most interrupts are
	 * expected: the out-of-band LAPIC timer and the proxy tick are how
	 * EVL and Linux keep time.  What matters is the rest.
	 */
	uint64_t	irq_timer;	/* EVL oob timer, proxy tick, local timer */
	uint64_t	irq_thermal;	/* TRM: thermal event interrupts */
	uint64_t	irq_ipi;	/* RES, CAL, TLB, IWI, ... Linux IPIs */
	uint64_t	irq_device;	/* numbered IRQs of real devices */
	char		irq_dev_top[64];	/* busiest device IRQ, if any */

	/* power and thermal */
	uint64_t	freq_khz;	/* cpufreq scaling_cur_freq */
	double		eff_mhz;	/* measured average while awake, NAN if unknown */
	uint64_t	cs_time[MAX_CS];	/* cpuidle stateN/time, µs */
	uint32_t	nr_cs;
	int64_t		throttle;	/* package_throttle_count, last raw value */
	double		temp_c;		/* NAN when unknown */

	/* derived ratios */
	double		util;		/* 0..1, Linux's view (/proc/stat) */
	double		busy;		/* 0..1, from ref-cycles: includes EVL; NAN if unknown */
	double		ipc;
	double		llc_mpki;
	double		l2_mpki;	/* L2 misses / 1000 instr */
	double		l1d_mpki;	/* L1D misses (retired loads) / 1000 instr */
	double		l1i_mpki;
	double		itlb_mpki;
	double		tlb_mpki;
	double		cache_mpki;
	double		stall_pct;
	uint64_t	irq_rate;	/* interrupts per second on this core */
};

int  sys_read_stat(struct cpu_sample *s, uint64_t out[8]);
int  sys_read_interrupts(struct cpu_sample *s, int slot);
void sys_set_interrupts_path(const char *path);
int  sys_read_freq(struct cpu_sample *s);
int  sys_read_cstates(struct cpu_sample *s, uint64_t out[MAX_CS], uint32_t *nr);
int  sys_read_throttle(struct cpu_sample *s, int64_t *delta);
int  sys_read_temp(int cpu, double *degc);
int  sys_cpu_online(int cpu);
int  sys_rt_cpus(char *buf, size_t len);

#endif

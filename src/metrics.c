// SPDX-License-Identifier: MIT
/*
 * metrics.c - turn raw counters into ratios that mean something, learn
 * what "normal" looks like on this machine, and flag a departure.
 *
 * A raw cache-miss count says little: a streaming scan misses the LLC on
 * almost every access by construction and is still fast.  What matters
 * is misses per instruction (MPKI), how that evolves against the core's
 * own recent behaviour, and whether it coincides with frame overruns.
 * Hence: ratios, a learned baseline, and a robust z-score.  Median and
 * MAD rather than mean and standard deviation, so that one spike does
 * not widen the band until nothing alarms any more.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "metrics.h"

#define BASE_WINDOW	120		/* samples kept per metric */

struct base_ring {
	double	v[BASE_WINDOW];
	int	n;
	int	pos;
};

/* One set of baselines per monitored core, shared by all samples of it. */
static struct base_ring base[MAX_CPUS][M_NR];

static void base_push(struct base_ring *b, double v)
{
	if (!isfinite(v))
		return;
	b->v[b->pos] = v;
	b->pos = (b->pos + 1) % BASE_WINDOW;
	if (b->n < BASE_WINDOW)
		b->n++;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void base_stats(const struct base_ring *b, double *med, double *spread)
{
	double tmp[BASE_WINDOW];
	int i;

	*med = NAN;
	*spread = NAN;
	if (b->n < 8)
		return;
	memcpy(tmp, b->v, b->n * sizeof(double));
	qsort(tmp, b->n, sizeof(double), cmp_double);
	*med = tmp[b->n / 2];
	for (i = 0; i < b->n; i++)
		tmp[i] = fabs(b->v[i] - *med);
	qsort(tmp, b->n, sizeof(double), cmp_double);
	*spread = 1.4826 * tmp[b->n / 2];	/* MAD -> sigma, for a normal spread */
	/*
	 * A very steady workload has a MAD near zero, which would make any
	 * wobble look like a huge excursion.  Treat at least 5% of the
	 * median (plus a small absolute floor) as normal variation.
	 */
	if (*spread < 0.05 * fabs(*med) + 0.01)
		*spread = 0.05 * fabs(*med) + 0.01;
}

/* Robust z-score of @v against the learned band; median returned in @med. */
static double zscore(const struct base_ring *b, double v, double *med)
{
	double spread;

	base_stats(b, med, &spread);
	if (!isfinite(*med) || !isfinite(v))
		return 0.0;
	return (v - *med) / spread;
}

void metrics_init(struct metrics *m, int cpu)
{
	memset(m, 0, sizeof(*m));
	m->cpu = cpu;
	m->phase = PH_WARMUP;
	m->health = CPU_OK;
	if (cpu >= 0 && cpu < MAX_CPUS)
		memset(base[cpu], 0, sizeof(base[cpu]));
}

/*
 * @frames_delta: frames whose deadline was missed during this interval,
 * from the real-time task's own telemetry.  Frame overrun is the only
 * signal here that is a fact rather than a statistic, so it alone can
 * raise the level to CRITICAL.
 */
void metrics_feed(struct metrics *m, const struct cpu_sample *s, double dt_s,
		  uint64_t frames_delta, const char *task_note)
{
	struct base_ring *b;
	double v[M_NR];

	bool busy;

	if (m->cpu < 0 || m->cpu >= MAX_CPUS)
		return;

	m->util = s->util;
	m->busy = s->busy;
	m->ipc = s->ipc;
	m->llc_mpki = s->llc_mpki;
	m->tlb_mpki = s->tlb_mpki;
	m->cache_mpki = s->cache_mpki;
	m->stall_pct = s->stall_pct;
	m->irq_rate = (double)s->irq_rate;
	m->freq_mhz = s->freq_khz / 1000.0;
	m->temp_c = s->temp_c;

	/*
	 * An idle core counts almost nothing and therefore reports a
	 * meaningless MPKI (a handful of misses over a handful of
	 * instructions).  Mixing those intervals into the baseline would
	 * poison it and fire an alarm the moment real work starts, so a
	 * sample only enters the baseline once the core is actually busy.
	 */
	/*
	 * Linux's utilisation is ~0 on a core running EVL threads (it does
	 * not see out-of-band execution), so prefer the hardware figure.
	 * Measured on the ThinkBook: idle cores 0.6-0.7 % busy, a core
	 * running one evl-pingpong thread 2-3 %; 1.5 % separates them.
	 */
	busy = isfinite(s->busy) ? s->busy > 0.015 : s->util > 0.03;

	v[M_LLC_MPKI] = s->llc_mpki;
	v[M_TLB_MPKI] = s->tlb_mpki;
	v[M_IPC] = s->ipc;
	v[M_IRQ_RATE] = (double)s->irq_rate;
	v[M_STALL] = s->stall_pct;

	b = base[m->cpu];
	if (busy) {
		base_push(&b[M_LLC_MPKI], v[M_LLC_MPKI]);
		base_push(&b[M_TLB_MPKI], v[M_TLB_MPKI]);
		base_push(&b[M_IPC], v[M_IPC]);
		base_push(&b[M_STALL], v[M_STALL]);
		m->base_util = 0.9 * m->base_util + 0.1 * s->util;
	}
	/* An IRQ storm is a property of the core, busy or not. */
	base_push(&b[M_IRQ_RATE], v[M_IRQ_RATE]);

	m->z_llc = zscore(&b[M_LLC_MPKI], v[M_LLC_MPKI], &m->med_llc);
	m->z_tlb = zscore(&b[M_TLB_MPKI], v[M_TLB_MPKI], &m->med_tlb);
	m->z_ipc = zscore(&b[M_IPC], v[M_IPC], &m->med_ipc);
	m->z_irq = zscore(&b[M_IRQ_RATE], v[M_IRQ_RATE], &m->med_irq);
	m->z_stall = zscore(&b[M_STALL], v[M_STALL], &m->med_stall);

	m->samples++;
	if (m->samples > 30 && m->phase == PH_WARMUP)
		m->phase = PH_LEARNING;
	if (m->samples > BASE_WINDOW)
		m->phase = PH_RUNNING;

	/* ---- health assessment ----------------------------------------- */
	m->health = CPU_OK;
	m->why[0] = '\0';
	if (task_note)
		snprintf(m->why, sizeof(m->why), "%s", task_note);

	if (frames_delta > 0) {
		m->health = CPU_CRITICAL;
		snprintf(m->why, sizeof(m->why),
			 "%llu frame deadline(s) missed; %s",
			 (unsigned long long)frames_delta,
			 task_note && task_note[0] ? task_note : "no frame detail");
		return;
	}

	/*
	 * Placement facts first.  These need no baseline: an interrupt of
	 * the wrong kind on an isolated core is wrong from the first
	 * sample.  The EVL out-of-band timer and the proxy tick are not
	 * counted here; they run at ~2000/s on every busy RT core and are
	 * how EVL keeps time.  Thermal events cannot be moved and are only
	 * reported when they are clearly unusual.
	 */
	if (s->irq_device > 0 && dt_s > 0) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why),
			 "device IRQ on an RT core: %.0f/s, busiest %s",
			 (double)s->irq_device / dt_s,
			 s->irq_dev_top[0] ? s->irq_dev_top : "unknown");
		return;
	}
	if (dt_s > 0 && (double)s->irq_ipi / dt_s > 5000) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why), "IPI storm on an RT core: %.0f/s",
			 (double)s->irq_ipi / dt_s);
		return;
	}

	/*
	 * Statistics next, once a baseline exists, and only for an interval
	 * in which the core was doing work: the baseline holds busy
	 * intervals only, and an idle core's few hundred instructions say
	 * nothing about its cache or IPC.
	 */
	if (m->phase == PH_WARMUP || !busy)
		return;

	/*
	 * Thresholds are on the robust z-score, not on the raw value:
	 * "this core is doing something new" is the alarm condition, and
	 * an absolute floor keeps noise on a nearly-idle core quiet.
	 */
	if (s->perf_ok && s->ev_ok[EV_LLC_MISSES] && m->z_llc > 3.5 && s->llc_mpki > 1.0) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why), "LLC MPKI %.2f (z%+.1f, base %.2f)",
			 s->llc_mpki, m->z_llc, m->med_llc);
	} else if (s->perf_ok && s->ev_ok[EV_DTLB_MISSES] && m->z_tlb > 3.5 &&
		   s->tlb_mpki > 0.5) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why), "dTLB MPKI %.2f (z%+.1f, base %.2f)",
			 s->tlb_mpki, m->z_tlb, m->med_tlb);
	} else if (s->perf_ok && isfinite(m->med_ipc) && m->z_ipc < -3.5) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why), "IPC %.2f (z%+.1f, base %.2f)",
			 s->ipc, m->z_ipc, m->med_ipc);
	} else if (m->z_stall > 3.5 && s->stall_pct > 5.0) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why), "backend stall %.1f%% (z%+.1f)",
			 s->stall_pct, m->z_stall);
	} else if (dt_s > 0 && (double)s->irq_thermal / dt_s > 3000) {
		m->health = CPU_DEGRADED;
		snprintf(m->why, sizeof(m->why),
			 "thermal event IRQs: %.0f/s (package %.0f C)",
			 (double)s->irq_thermal / dt_s, m->temp_c);
	}

	if (s->perf_ok && !s->ev_ok[EV_LLC_MISSES])
		m->note = "PMU lacks LLC counters: cache checks disabled";
	else if (s->perf_ok && !s->ev_ok[EV_DTLB_MISSES])
		m->note = "PMU lacks dTLB counters: TLB checks disabled";
	else
		m->note = NULL;
}

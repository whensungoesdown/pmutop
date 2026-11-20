// SPDX-License-Identifier: MIT
/*
 * pmutop - per-core hardware-counter monitor: shows live, for each CPU,
 * what limits it (cache and TLB misses, IPC, page faults, frequency,
 * interrupts) and warns when a core departs from its own normal level.
 * Isolated real-time cores (isolcpus, e.g. Xenomai 4 / EVL) get stricter
 * rules and an execution-time view that includes out-of-band threads.
 *
 * The monitor is an ordinary Linux task and must not run on the cores it
 * watches: it takes no PMU samples (counting only) and sleeps on the wall
 * clock between intervals.  Its only footprint on a monitored core is the
 * counter read itself: one group read, one IPI per interval (see perf.c).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pmutop.h"
#include "metrics.h"
#include "telemetry.h"
#include "ui.h"

struct opt {
	int	interval_ms;
	int	count;			/* 0: run until interrupted */
	const char *csv;
	const char *telem;
	pid_t	pid;			/* >0: follow this thread, not a core */
	bool	graphic;		/* full-screen chart view */
	int	cpus[MAX_CPUS];
	int	nr_cpus;
	bool	all_cpus;
};

static volatile sig_atomic_t stop;
static double ref_hz;		/* reference-cycle rate, 0 if unknown */

/* Is @cpu in a list like "4-5", "4,5" or "2,4-7"? */
static bool in_cpu_list(const char *list, int cpu)
{
	const char *p = list;

	while (*p) {
		char *end;
		long lo = strtol(p, &end, 10), hi = lo;

		if (end == p)
			return false;
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		if (cpu >= lo && cpu <= hi)
			return true;
		if (*end != ',')
			break;
		p = end + 1;
	}
	return false;
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int parse_cpus(const char *s, struct opt *o)
{
	char *copy, *tok, *save = NULL;

	if (strcmp(s, "all") == 0) {
		for (int i = 0; i < MAX_CPUS; i++)
			if (sys_cpu_online(i))
				o->cpus[o->nr_cpus++] = i;
		o->all_cpus = true;
		return o->nr_cpus ? 0 : -1;
	}
	copy = strdup(s);
	if (!copy)
		return -1;
	for (tok = strtok_r(copy, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		int lo, hi;

		if (sscanf(tok, "%d-%d", &lo, &hi) == 2) {
			for (int k = lo; k <= hi && o->nr_cpus < MAX_CPUS; k++)
				o->cpus[o->nr_cpus++] = k;
		} else if (sscanf(tok, "%d", &lo) == 1) {
			if (o->nr_cpus < MAX_CPUS)
				o->cpus[o->nr_cpus++] = lo;
		} else {
			free(copy);
			return -1;
		}
	}
	free(copy);
	return o->nr_cpus ? 0 : -1;
}

/* Ratios from the raw deltas of one interval. */
static void derive(struct cpu_sample *s, const uint64_t cur[8],
		   const uint64_t prev[8], double dt_s)
{
	uint64_t d_idle = (cur[3] - prev[3]) + (cur[4] - prev[4]);
	uint64_t d_total = 0;
	int i;

	for (i = 0; i < 8; i++)
		d_total += cur[i] - prev[i];
	s->util = d_total ? 1.0 - (double)d_idle / (double)d_total : 0.0;

	/*
	 * Average clock while the core was awake: unhalted core cycles over
	 * unhalted reference cycles.  scaling_cur_freq is no use on an
	 * isolated tickless core: it is refreshed from the Linux tick.
	 */
	if (ref_hz > 0 && s->ev_ok[EV_CYCLES] && s->ev_ok[EV_REF_CYCLES] &&
	    s->ev[EV_REF_CYCLES] > 0)
		s->eff_mhz = ref_hz / 1e6 * (double)s->ev[EV_CYCLES] /
			     (double)s->ev[EV_REF_CYCLES];
	else
		s->eff_mhz = NAN;

	if (ref_hz > 0 && dt_s > 0 && s->ev_ok[EV_REF_CYCLES]) {
		s->busy = (double)s->ev[EV_REF_CYCLES] / (ref_hz * dt_s);
		if (s->busy > 1.0)
			s->busy = 1.0;
	} else {
		s->busy = NAN;
	}

	if (s->ev_ok[EV_CYCLES] && s->ev_ok[EV_INSTRUCTIONS] && s->ev[EV_CYCLES] > 0)
		s->ipc = (double)s->ev[EV_INSTRUCTIONS] / (double)s->ev[EV_CYCLES];
	else
		s->ipc = NAN;

	if (s->ev_ok[EV_INSTRUCTIONS] && s->ev[EV_INSTRUCTIONS] > 0) {
		double ins = (double)s->ev[EV_INSTRUCTIONS];

		s->llc_mpki = s->ev_ok[EV_LLC_MISSES] ?
			(double)s->ev[EV_LLC_MISSES] * 1000.0 / ins : NAN;
		s->l2_mpki = s->ev_ok[EV_L2_MISSES] ?
			(double)s->ev[EV_L2_MISSES] * 1000.0 / ins : NAN;
		s->l1d_mpki = s->ev_ok[EV_L1D_MISSES] ?
			(double)s->ev[EV_L1D_MISSES] * 1000.0 / ins : NAN;
		s->l1i_mpki = s->ev_ok[EV_L1I_MISSES] ?
			(double)s->ev[EV_L1I_MISSES] * 1000.0 / ins : NAN;
		s->itlb_mpki = s->ev_ok[EV_ITLB_MISSES] ?
			(double)s->ev[EV_ITLB_MISSES] * 1000.0 / ins : NAN;
		s->tlb_mpki = s->ev_ok[EV_DTLB_MISSES] ?
			(double)s->ev[EV_DTLB_MISSES] * 1000.0 / ins : NAN;
		s->cache_mpki = s->ev_ok[EV_CACHE_MISSES] ?
			(double)s->ev[EV_CACHE_MISSES] * 1000.0 / ins : NAN;
	} else {
		s->llc_mpki = s->l2_mpki = s->tlb_mpki = s->cache_mpki = NAN;
		s->l1d_mpki = s->l1i_mpki = s->itlb_mpki = NAN;
	}

	if (s->ev_ok[EV_STALL_MEM] && s->ev_ok[EV_CYCLES] && s->ev[EV_CYCLES] > 0)
		s->stall_pct = 100.0 * (double)s->ev[EV_STALL_MEM] /
			       (double)s->ev[EV_CYCLES];
	else
		s->stall_pct = NAN;

	/* s->irqs already holds the delta over the interval */
	s->irq_rate = dt_s > 0 ? (uint64_t)((double)s->irqs / dt_s) : 0;
}

static const char *health_str(enum cpu_health h)
{
	switch (h) {
	case CPU_OK:		return "OK";
	case CPU_DEGRADED:	return "WARN";
	case CPU_CRITICAL:	return "FAIL";
	}
	return "?";
}

static void print_header(void)
{
	printf("%-4s %-5s %6s %7s %7s %8s %8s %8s %7s %8s %6s  %s\n",
	       "cpu", "state", "cpu%", "MHz", "IPC", "LLC/K", "dTLB/K",
	       "cache/K", "stall%", "IRQ/s", "temp", "note");
}

static void fmt_or_dash(char *b, size_t n, double v, const char *fmt)
{
	if (isfinite(v))
		snprintf(b, n, fmt, v);
	else
		snprintf(b, n, "-");
}

static void print_row(const struct cpu_sample *s, const struct metrics *m)
{
	char llc[16], tlb[16], cach[16], ipc[16], stall[16], tmp[16], busy[16];

	fmt_or_dash(llc, sizeof(llc), s->llc_mpki, "%.2f");
	fmt_or_dash(tlb, sizeof(tlb), s->tlb_mpki, "%.2f");
	fmt_or_dash(cach, sizeof(cach), s->cache_mpki, "%.2f");
	fmt_or_dash(ipc, sizeof(ipc), s->ipc, "%.2f");
	fmt_or_dash(stall, sizeof(stall), s->stall_pct, "%.1f");
	fmt_or_dash(tmp, sizeof(tmp), m->temp_c, "%.1f");
	/*
	 * One usage figure: the hardware one when available (it includes
	 * out-of-band EVL execution); Linux's /proc/stat only without a PMU.
	 */
	fmt_or_dash(busy, sizeof(busy),
		    (isfinite(m->busy) ? m->busy : m->util) * 100.0, "%.1f");

	printf("%-4d %-5s %6s %7.0f %7s %8s %8s %8s %7s %8llu %6s  %s\n",
	       s->cpu, health_str(m->health), busy, m->freq_mhz,
	       ipc, llc, tlb, cach, stall,
	       (unsigned long long)s->irq_rate, tmp, m->why);
}

int main(int argc, char **argv)
{
	static struct opt o = { .interval_ms = 1000, .count = 0, .pid = -1 };
	struct perf_group pg[MAX_CPUS];
	struct cpu_sample cs[MAX_CPUS];
	struct metrics met[MAX_CPUS];
	uint64_t prevstat[MAX_CPUS][8], curstat[8];
	int64_t prev_ns, now;
	struct telem tel;
	FILE *csv = NULL;
	char err[256];
	const char *cpus_arg = NULL, *l2_spec = NULL, *l1d_spec = NULL, *l1i_spec = NULL;
	const char *show = NULL;
	char raw_msg[160] = "";
	int i, c, iter, ret;
	static const struct option lopts[] = {
		{ "graphic", no_argument, NULL, 'g' },
		{ "l2-event", required_argument, NULL, 'L' },
		{ "l1d-event", required_argument, NULL, 'D' },
		{ "l1i-event", required_argument, NULL, 'I' },
		{ "show", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	/* _only: "-graphic" works as well as "--graphic" and "-g" */
	while ((c = getopt_long_only(argc, argv, "c:D:gI:i:L:n:o:p:s:t:vh", lopts, NULL)) != -1) {
		switch (c) {
		case 'g': o.graphic = true; break;
		case 'L': l2_spec = optarg; break;
		case 'D': l1d_spec = optarg; break;
		case 'I': l1i_spec = optarg; break;
		case 's': show = optarg; break;
		case 'c': cpus_arg = optarg; break;
		case 'i': o.interval_ms = atoi(optarg); break;
		case 'n': o.count = atoi(optarg); break;
		case 'o': o.csv = optarg; break;
		case 'p': o.pid = (pid_t)atoi(optarg); break;
		case 't': o.telem = optarg; break;
		case 'v': break;
		case 'h':
		default:
			fprintf(stderr,
				"usage: pmutop [-c 4,5|all | -p tid] [-i ms] [-n count]"
				" [-o out.csv] [-t telemetry.bin] [-graphic]\n"
				"  -graphic  full-screen charts"
				" (keys: q quit, p pause, c clear events)\n"
				"  --show LIST  charts for -graphic, in order:"
				" frame,cpu,ipc,pf,l1i,l1d,l2,itlb,dtlb (default all)\n"
				"  --l2-event / --l1d-event / --l1i-event PMU:CONFIG[:CONFIG1][,...]"
				"  raw event per core type (built in for Core Ultra 200H)\n"
				"  Default CPU list: /sys/devices/system/cpu/isolated.\n"
				"  CPU-wide counters need root and a working PMU;\n"
				"  without them the /proc and sysfs signals remain.\n");
			return c == 'h' ? 0 : 2;
		}
	}
	if (o.interval_ms < 10)
		o.interval_ms = 10;

	if (o.pid > 0 && cpus_arg) {
		fprintf(stderr, "-p and -c are mutually exclusive\n");
		return 2;
	}

	if (o.pid > 0) {
		/*
		 * Following a thread: the counters belong to that task, and
		 * the software events show exactly the demotion we care
		 * about (a context switch out of the out-of-band stage, a
		 * page fault, a migration between cores).
		 */
		o.cpus[o.nr_cpus++] = -1;
	} else if (cpus_arg) {
		if (parse_cpus(cpus_arg, &o) < 0) {
			fprintf(stderr, "bad -c list: %s\n", cpus_arg);
			return 2;
		}
	} else {
		char buf[128];
		int lo, hi;

		if (sys_rt_cpus(buf, sizeof(buf)) == 0 &&
		    sscanf(buf, "%d-%d", &lo, &hi) == 2) {
			for (int k = lo; k <= hi && o.nr_cpus < MAX_CPUS; k++)
				o.cpus[o.nr_cpus++] = k;
		} else if (sys_rt_cpus(buf, sizeof(buf)) == 0 &&
			   sscanf(buf, "%d", &lo) == 1) {
			o.cpus[o.nr_cpus++] = lo;
		} else {
			o.cpus[o.nr_cpus++] = 0;
			fprintf(stderr, "no isolated CPUs found; watching CPU 0\n");
		}
	}

	/* Calibrate before anything else runs on this CPU. */
	ref_hz = perf_ref_hz();
	if (ref_hz <= 0)
		fprintf(stderr, "no ref-cycles counter: busy%% unavailable\n");

	/*
	 * L2, L1D and L1I misses are raw events, per core type; a core without a
	 * code for its type just has no data for that chart.
	 */
	{
		const struct { int ev; const char *what; const char *spec; } rawv[] = {
			{ EV_L2_MISSES, "L2", l2_spec },
			{ EV_L1D_MISSES, "L1D", l1d_spec },
			{ EV_L1I_MISSES, "L1I", l1i_spec },
		};

		for (unsigned k = 0; k < sizeof(rawv) / sizeof(rawv[0]); k++) {
			if (perf_setup_raw(rawv[k].ev, rawv[k].spec, raw_msg, sizeof(raw_msg)) == 0 ||
			    raw_msg[0])
				fprintf(stderr, "%s misses: %s\n", rawv[k].what,
					raw_msg[0] ? raw_msg : "no event configured");
			for (i = 0; i < o.nr_cpus; i++)
				if (o.cpus[i] >= 0 && !perf_raw_covers(rawv[k].ev, o.cpus[i]))
					fprintf(stderr, "cpu%d: no %s-miss event for this core type\n",
						o.cpus[i], rawv[k].what);
		}
	}

	if (o.graphic && ui_select(show, err, sizeof(err)) < 0) {
		fprintf(stderr, "--show: %s\n", err);
		return 2;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	for (i = 0; i < o.nr_cpus; i++) {
		int cpu = o.cpus[i];
		int target = cpu < 0 ? 0 : cpu;		/* cpu 0: task is unpinned */

		memset(&cs[i], 0, sizeof(cs[i]));
		cs[i].cpu = target;
		cs[i].throttle = -1;
		metrics_init(&met[i], target);
		{
			char iso[128];

			met[i].rt_core = o.pid <= 0 &&
					 sys_rt_cpus(iso, sizeof(iso)) == 0 &&
					 in_cpu_list(iso, target);
		}

		if (o.pid > 0)
			cs[i].cpu = 0;	/* task-level sample: no per-core view */
		if (o.pid < 0 && sys_read_stat(&cs[i], prevstat[i]) < 0)
			memset(prevstat[i], 0, sizeof(prevstat[i]));
		sys_read_interrupts(&cs[i], i);	/* seed the IRQ deltas */
		sys_read_cstates(&cs[i], cs[i].cs_time, &cs[i].nr_cs);

		ret = perf_open(&pg[i], cpu, o.pid, err, sizeof(err));
		if (ret < 0) {
			fprintf(stderr, "%s: %s; no perf counters\n",
				o.pid > 0 ? "thread" : "cpu", err);
			cs[i].perf_ok = false;
		} else {
			if (ret > 0)
				fprintf(stderr, "%s: %s; software events only\n",
					o.pid > 0 ? "thread" : "cpu", err);
			cs[i].perf_ok = true;
		}
	}

	if (o.telem && telem_open(&tel, o.telem) < 0)
		fprintf(stderr, "cannot map %s: %s; continuing without telemetry\n",
			o.telem, strerror(errno));

	if (o.csv) {
		csv = fopen(o.csv, "w");
		if (!csv) {
			perror(o.csv);
			return 1;
		}
		fprintf(csv, "t_s,cpu,state,util,ipc,llc_mpki,dtlb_mpki,"
			     "cache_mpki,stall_pct,irq_rate,irq_timer,irq_thermal,"
			     "irq_ipi,irq_device,freq_mhz,temp_c,busy,page_faults,l2_mpki,"
			     "l1d_mpki,l1i_mpki,itlb_mpki\n");
	}

	if (o.graphic) {
		static char mode[48];
		struct ui_info ui = {
			.interval_ms = o.interval_ms,
			.mode = mode,
			.csv = o.csv,
			.telemetry = tel.page != NULL,
		};

		if (o.pid > 0)
			snprintf(mode, sizeof(mode), "thread %d", (int)o.pid);
		else
			snprintf(mode, sizeof(mode), "%d core(s)", o.nr_cpus);
		if (ui_start(&ui) < 0) {
			fprintf(stderr, "-graphic needs a terminal\n");
			return 2;
		}
		for (i = 0; i < o.nr_cpus; i++)
			if (!cs[i].perf_ok)
				ui_event("cpu%d: no PMU counters, cache/TLB/IPC charts empty",
					 cs[i].cpu);
		ui_draw(o.nr_cpus);
	}

	prev_ns = now_ns();
	if (!o.graphic)
		print_header();

	for (iter = 0; !stop && (o.count == 0 || iter < o.count); iter++) {
		struct timespec ts = {
			.tv_sec = o.interval_ms / 1000,
			.tv_nsec = (long)(o.interval_ms % 1000) * 1000000L,
		};
		double dt_s;

		if (o.graphic) {
			if (ui_wait(o.interval_ms, o.nr_cpus))
				break;
		} else {
			nanosleep(&ts, NULL);
		}
		now = now_ns();
		dt_s = (double)(now - prev_ns) / 1e9;
		prev_ns = now;

		for (i = 0; i < o.nr_cpus; i++) {
			int64_t ev[EV_NR];
			bool ok[EV_NR];
			uint64_t fd = 0, ld = 0, worst = 0, flags = 0;
			char note[160] = "";

			if (cs[i].perf_ok) {
				int rr = perf_read_interval(&pg[i], ev, ok, err, sizeof(err));

				if (rr == 0) {
					memcpy(cs[i].ev, ev, sizeof(ev));
					memcpy(cs[i].ev_ok, ok, sizeof(ok));
				} else if (rr == -2) {
					/* not scheduled this time: say so once, keep trying */
					if (o.graphic)
						ui_event("%s", err);
					else
						fprintf(stderr, "%s\n", err);
					memset(cs[i].ev_ok, 0, sizeof(cs[i].ev_ok));
				} else {
					fprintf(stderr, "cpu%d: %s; PMU disabled\n",
						cs[i].cpu, err);
					cs[i].perf_ok = false;
				}
			}

			sys_read_interrupts(&cs[i], i);	/* deltas since last time */

			if (sys_read_stat(&cs[i], curstat) == 0) {
				derive(&cs[i], curstat, prevstat[i], dt_s);
				memcpy(prevstat[i], curstat, sizeof(curstat));
			}
			sys_read_freq(&cs[i]);
			sys_read_cstates(&cs[i], cs[i].cs_time, &cs[i].nr_cs);
			if (sys_read_temp(cs[i].cpu, &cs[i].temp_c) < 0)
				cs[i].temp_c = NAN;

			/*
			 * Frame telemetry, when the real-time task publishes
			 * it: the only signal that is a fact rather than a
			 * statistic, because the task itself reports that it
			 * missed a deadline.
			 */
			if (tel.page) {
				telem_sample(&tel, (uint32_t)cs[i].cpu, &fd, &ld,
					     &worst, &flags);
				if (worst > 0)
					snprintf(note, sizeof(note),
						 "worst frame %.1f us", worst / 1000.0);
			}

			/*
			 * A thread sample carries the demotion indicators
			 * directly: a context switch while it should have
			 * been running out of band, a page fault, a move to
			 * another core.  They are counts, not ratios, so they
			 * belong in the note rather than in a z-score.
			 */
			if (o.pid > 0) {
				snprintf(note, sizeof(note), "sw=%lld faults=%lld mig=%lld",
					 (long long)cs[i].ev[EV_CTX_SWITCH],
					 (long long)cs[i].ev[EV_PAGE_FAULT],
					 (long long)cs[i].ev[EV_CPU_MIGRATE]);
				/* Thread's own CPU share, from task-clock (ns). */
				if (cs[i].ev_ok[EV_TASK_CLOCK] && dt_s > 0)
					cs[i].util = (double)cs[i].ev[EV_TASK_CLOCK] /
						     (dt_s * 1e9);
			}

			metrics_feed(&met[i], &cs[i], dt_s, ld, note);
			if (o.graphic)
				ui_push(i, &cs[i], &met[i], dt_s, worst / 1000.0);
			else
				print_row(&cs[i], &met[i]);

			if (csv)
				fprintf(csv, "%.3f,%d,%s,%.4f,%.3f,%.3f,%.3f,%.3f,"
					"%.3f,%llu,%llu,%llu,%llu,%llu,%.0f,%.1f,%.4f,%lld,%.4f,"
					"%.4f,%.4f,%.4f\n",
					now / 1e9, cs[i].cpu,
					health_str(met[i].health), met[i].util,
					met[i].ipc, met[i].llc_mpki, met[i].tlb_mpki,
					met[i].cache_mpki, met[i].stall_pct,
					(unsigned long long)cs[i].irq_rate,
					(unsigned long long)cs[i].irq_timer,
					(unsigned long long)cs[i].irq_thermal,
					(unsigned long long)cs[i].irq_ipi,
					(unsigned long long)cs[i].irq_device,
					met[i].freq_mhz, met[i].temp_c, met[i].busy,
					cs[i].ev_ok[EV_PAGE_FAULT] ?
						(long long)cs[i].ev[EV_PAGE_FAULT] : -1LL,
					cs[i].l2_mpki, cs[i].l1d_mpki,
					cs[i].l1i_mpki, cs[i].itlb_mpki);
		}

		if (o.graphic && !ui_paused())
			ui_draw(o.nr_cpus);
		else if (!o.graphic)
			fflush(stdout);
		if (csv)
			fflush(csv);
	}

	if (o.graphic)
		ui_stop();

	for (i = 0; i < o.nr_cpus; i++)
		if (cs[i].perf_ok)
			perf_close(&pg[i]);
	telem_close(&tel);
	if (csv)
		fclose(csv);
	return 0;
}

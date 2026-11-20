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
#include <stdint.h>
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
#include "uncore.h"

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

/* Long-only options. */
enum { OPT_L1I = 256, OPT_L1D, OPT_L2, OPT_L3 };

static void usage(FILE *f)
{
	fputs(
"Usage: pmutop [OPTION]...\n"
"Live per-core view of CPU hardware performance counters.\n"
"\n"
"With no options: every online core, every chart, full-screen view\n"
"(text lines instead when the output is not a terminal).\n"
"\n"
"What to watch:\n"
"  -c, --cpus=LIST        cores: all, isolated, or a list such as 0,2,4-7\n"
"                         (default: all)\n"
"  -p, --pid=TID          follow one thread instead of cores\n"
"\n"
"How to show it:\n"
"  -g, --graphic          full-screen charts (default on a terminal)\n"
"  -b, --batch            one text line per core per sample\n"
"  -s, --show=LIST        charts in the full-screen view, in this order\n"
"                         (default: all): cpu,ipc,pf,l1i,l1d,l2,l3,itlb,dtlb,frame\n"
"  -i, --interval=MS      sampling interval in milliseconds (default 1000)\n"
"  -n, --count=N          stop after N samples (default: until q or Ctrl-C)\n"
"  -o, --csv=FILE         also write every sample to FILE\n"
"\n"
"Real-time tasks:\n"
"  -t, --telemetry=FILE   frame timing published by a real-time task\n"
"\n"
"Hardware events (raw code per core type: PMU:CONFIG[:CONFIG1][,...];\n"
"built in for Intel Core Ultra 200H):\n"
"      --l1i-event=SPEC   L1 instruction-cache misses\n"
"      --l1d-event=SPEC   L1 data-cache misses\n"
"      --l2-event=SPEC    L2 cache misses\n"
"      --l3-event=SPEC    L3 misses (loads served from DRAM)\n"
"\n"
"  -h, --help             show this help\n"
"  -V, --version          show the version\n"
"\n"
"Keys in the full-screen view: q quit, p pause, c clear events,\n"
"Left/Right or PgUp/PgDn: previous/next page when not all cores fit.\n"
"\n"
"CPU-wide counting needs root, or kernel.perf_event_paranoid <= 0.\n", f);
}

/* Whole-string decimal in [min, max]. */
static int parse_int(const char *s, long min, long max, int *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end || v < min || v > max)
		return -1;
	*out = (int)v;
	return 0;
}

static void add_cpu(struct opt *o, int cpu)
{
	for (int i = 0; i < o->nr_cpus; i++)
		if (o->cpus[i] == cpu)
			return;		/* listed twice */
	if (o->nr_cpus < MAX_CPUS)
		o->cpus[o->nr_cpus++] = cpu;
}

/*
 * "all", "isolated", or a list such as "0,2,4-7".  Offline or unknown
 * CPUs are an error rather than an empty panel.
 */
static int parse_cpus(const char *s, struct opt *o, char *err, size_t errlen)
{
	char list[512], *tok, *save = NULL;

	if (strcmp(s, "all") == 0) {
		for (int i = 0; i < MAX_CPUS; i++)
			if (sys_cpu_online(i))
				add_cpu(o, i);
		o->all_cpus = true;
		if (!o->nr_cpus) {
			snprintf(err, errlen, "no online CPU found");
			return -1;
		}
		return 0;
	}
	if (strcmp(s, "isolated") == 0) {
		if (sys_rt_cpus(list, sizeof(list)) < 0 || !list[0]) {
			snprintf(err, errlen, "no isolated CPUs (see isolcpus=)");
			return -1;
		}
	} else {
		snprintf(list, sizeof(list), "%s", s);
	}

	for (tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *end;
		long lo, hi;

		lo = strtol(tok, &end, 10);
		hi = lo;
		if (end != tok && *end == '-')
			hi = strtol(end + 1, &end, 10);
		if (end == tok || *end || lo < 0 || hi < lo || hi >= MAX_CPUS) {
			snprintf(err, errlen, "bad CPU list '%s' (want all, isolated, or e.g. 0,2,4-7)", s);
			return -1;
		}
		for (long k = lo; k <= hi; k++) {
			if (!sys_cpu_online((int)k)) {
				snprintf(err, errlen, "CPU %ld is not online", k);
				return -1;
			}
			add_cpu(o, (int)k);
		}
	}
	if (!o->nr_cpus) {
		snprintf(err, errlen, "empty CPU list");
		return -1;
	}
	return 0;
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
		s->l3_mpki = s->ev_ok[EV_L3_MISSES] ?
			(double)s->ev[EV_L3_MISSES] * 1000.0 / ins : NAN;
	} else {
		s->llc_mpki = s->l2_mpki = s->tlb_mpki = s->l3_mpki = NAN;
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

/* "P-core", "E-core", "LP E-core"; "-" on a non-hybrid CPU. */
static const char *core_type(int cpu)
{
	const char *t = perf_core_type(cpu);

	return t ? t : "-";
}

static void print_header(void)
{
	printf("%-4s %-9s %-5s %6s %7s %7s %8s %8s %8s %7s %8s %6s  %s\n",
	       "cpu", "type", "state", "cpu%", "MHz", "IPC", "LLC/K", "dTLB/K",
	       "L3/K", "stall%", "IRQ/s", "temp", "note");
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
	char llc[16], tlb[16], l3[16], ipc[16], stall[16], tmp[16], busy[16];

	fmt_or_dash(llc, sizeof(llc), s->llc_mpki, "%.2f");
	fmt_or_dash(tlb, sizeof(tlb), s->tlb_mpki, "%.2f");
	fmt_or_dash(l3, sizeof(l3), s->l3_mpki, "%.2f");
	fmt_or_dash(ipc, sizeof(ipc), s->ipc, "%.2f");
	fmt_or_dash(stall, sizeof(stall), s->stall_pct, "%.1f");
	fmt_or_dash(tmp, sizeof(tmp), m->temp_c, "%.1f");
	/*
	 * One usage figure: the hardware one when available (it includes
	 * out-of-band EVL execution); Linux's /proc/stat only without a PMU.
	 */
	fmt_or_dash(busy, sizeof(busy),
		    (isfinite(m->busy) ? m->busy : m->util) * 100.0, "%.1f");

	printf("%-4d %-9s %-5s %6s %7.0f %7s %8s %8s %8s %7s %8llu %6s  %s\n",
	       s->cpu, core_type(s->cpu), health_str(m->health), busy, m->freq_mhz,
	       ipc, llc, tlb, l3, stall,
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
	struct telem tel = { .fd = -1 };
	struct uncore_sample unc;
	FILE *csv = NULL;
	char err[256];
	const char *cpus_arg = "all", *l2_spec = NULL, *l1d_spec = NULL, *l1i_spec = NULL;
	const char *l3_spec = NULL;
	const char *show = NULL;
	char raw_msg[160] = "";
	enum { VIEW_AUTO, VIEW_GRAPHIC, VIEW_BATCH } view = VIEW_AUTO;
	bool cpus_given = false;
	int i, c, iter, ret, v;
	static const struct option lopts[] = {
		{ "cpus",      required_argument, NULL, 'c' },
		{ "pid",       required_argument, NULL, 'p' },
		{ "graphic",   no_argument,       NULL, 'g' },
		{ "batch",     no_argument,       NULL, 'b' },
		{ "show",      required_argument, NULL, 's' },
		{ "interval",  required_argument, NULL, 'i' },
		{ "count",     required_argument, NULL, 'n' },
		{ "csv",       required_argument, NULL, 'o' },
		{ "telemetry", required_argument, NULL, 't' },
		{ "l1i-event", required_argument, NULL, OPT_L1I },
		{ "l1d-event", required_argument, NULL, OPT_L1D },
		{ "l2-event",  required_argument, NULL, OPT_L2 },
		{ "l3-event",  required_argument, NULL, OPT_L3 },
		{ "help",      no_argument,       NULL, 'h' },
		{ "version",   no_argument,       NULL, 'V' },
		{ NULL, 0, NULL, 0 },
	};

	while ((c = getopt_long(argc, argv, "c:p:gbs:i:n:o:t:hV", lopts, NULL)) != -1) {
		switch (c) {
		case 'c':
			cpus_arg = optarg;
			cpus_given = true;
			break;
		case 'p':
			if (parse_int(optarg, 1, INT32_MAX, &v) < 0) {
				fprintf(stderr, "pmutop: --pid: not a thread id: %s\n", optarg);
				return 2;
			}
			o.pid = (pid_t)v;
			break;
		case 'g':
		case 'b':
			if (view != VIEW_AUTO && view != (c == 'g' ? VIEW_GRAPHIC : VIEW_BATCH)) {
				fprintf(stderr, "pmutop: --graphic and --batch are mutually exclusive\n");
				return 2;
			}
			view = c == 'g' ? VIEW_GRAPHIC : VIEW_BATCH;
			break;
		case 's':
			show = optarg;
			break;
		case 'i':
			if (parse_int(optarg, 10, 3600000, &o.interval_ms) < 0) {
				fprintf(stderr, "pmutop: --interval: want 10-3600000 ms, got %s\n",
					optarg);
				return 2;
			}
			break;
		case 'n':
			if (parse_int(optarg, 1, INT32_MAX, &o.count) < 0) {
				fprintf(stderr, "pmutop: --count: want a positive number, got %s\n",
					optarg);
				return 2;
			}
			break;
		case 'o': o.csv = optarg; break;
		case 't': o.telem = optarg; break;
		case OPT_L1I: l1i_spec = optarg; break;
		case OPT_L1D: l1d_spec = optarg; break;
		case OPT_L2: l2_spec = optarg; break;
		case OPT_L3: l3_spec = optarg; break;
		case 'V':
			printf("pmutop %s\n", PMUTOP_VERSION);
			return 0;
		case 'h':
			usage(stdout);
			return 0;
		default:
			fprintf(stderr, "Try 'pmutop --help'.\n");
			return 2;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "pmutop: unexpected argument '%s'\nTry 'pmutop --help'.\n",
			argv[optind]);
		return 2;
	}

	if (o.pid > 0 && cpus_given) {
		fprintf(stderr, "pmutop: --pid and --cpus are mutually exclusive\n");
		return 2;
	}

	/* Full-screen view by default, text lines when not on a terminal. */
	if (view == VIEW_AUTO)
		o.graphic = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	else
		o.graphic = view == VIEW_GRAPHIC;

	if (o.pid > 0) {
		/*
		 * Following a thread: the counters belong to that task, and
		 * the software events show exactly the demotion we care
		 * about (a context switch out of the out-of-band stage, a
		 * page fault, a migration between cores).
		 */
		o.cpus[o.nr_cpus++] = -1;
	} else if (parse_cpus(cpus_arg, &o, err, sizeof(err)) < 0) {
		fprintf(stderr, "pmutop: --cpus: %s\n", err);
		return 2;
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

		/*
		 * L3 misses: direct event on P-cores, L2 misses minus L3 hits on
		 * E-cores (perf_read_interval derives it).  Cores with neither,
		 * e.g. LP E-cores outside the L3, get no L3 data and no message:
		 * they have no L3 to miss.
		 */
		if (perf_setup_raw(EV_L3_MISSES, l3_spec, raw_msg, sizeof(raw_msg)) == 0 &&
		    raw_msg[0])
			fprintf(stderr, "L3 misses: %s\n", raw_msg);
		if (!l3_spec)
			perf_setup_raw(EV_L3_HITS, NULL, raw_msg, sizeof(raw_msg));
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

	/* Chip-wide counters: uncore clocks and DRAM bandwidth. */
	if (uncore_open(raw_msg, sizeof(raw_msg)) == 0)
		fprintf(stderr, "%s\n", raw_msg);
	uncore_read(&unc, 0);		/* prime the deltas */

	if (o.csv) {
		csv = fopen(o.csv, "w");
		if (!csv) {
			perror(o.csv);
			return 1;
		}
		fprintf(csv, "t_s,cpu,state,util,ipc,llc_mpki,dtlb_mpki,"
			     "l3_mpki,stall_pct,irq_rate,irq_timer,irq_thermal,"
			     "irq_ipi,irq_device,freq_mhz,temp_c,busy,page_faults,l2_mpki,"
			     "l1d_mpki,l1i_mpki,itlb_mpki,uncore_cncu_ghz,uncore_sncu_ghz,"
			     "dram_read_gbs,dram_write_gbs,core_type\n");
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
			fprintf(stderr, "pmutop: --graphic needs a terminal; use --batch\n");
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
		uncore_read(&unc, dt_s);
		if (o.graphic)
			ui_uncore(&unc);
		else if (isfinite(unc.cncu_ghz) || isfinite(unc.dram_rd_gbs))
			printf("uncore: cncu %.2f GHz  sncu %.2f GHz  DRAM read %.2f GB/s"
			       "  write %.2f GB/s\n", unc.cncu_ghz, unc.sncu_ghz,
			       unc.dram_rd_gbs, unc.dram_wr_gbs);

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
					"%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.3f,%s\n",
					now / 1e9, cs[i].cpu,
					health_str(met[i].health), met[i].util,
					met[i].ipc, met[i].llc_mpki, met[i].tlb_mpki,
					met[i].l3_mpki, met[i].stall_pct,
					(unsigned long long)cs[i].irq_rate,
					(unsigned long long)cs[i].irq_timer,
					(unsigned long long)cs[i].irq_thermal,
					(unsigned long long)cs[i].irq_ipi,
					(unsigned long long)cs[i].irq_device,
					met[i].freq_mhz, met[i].temp_c, met[i].busy,
					cs[i].ev_ok[EV_PAGE_FAULT] ?
						(long long)cs[i].ev[EV_PAGE_FAULT] : -1LL,
					cs[i].l2_mpki, cs[i].l1d_mpki,
					cs[i].l1i_mpki, cs[i].itlb_mpki,
					unc.cncu_ghz, unc.sncu_ghz,
					unc.dram_rd_gbs, unc.dram_wr_gbs,
					core_type(cs[i].cpu));
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
	uncore_close();
	if (csv)
		fclose(csv);
	return 0;
}

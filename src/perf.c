// SPDX-License-Identifier: MIT
/*
 * perf.c - hardware counters via perf_event_open(2), counting only.
 *
 * Never sampling.  On a Dovetail kernel a PMI is delivered through the
 * NMI handler (perf_event_nmi_handler, registered NMI_LOCAL), which runs
 * ahead of the interrupt pipeline and perturbs the very thing we are
 * measuring.  A counting event stays silent until it is read.
 *
 * Reading matters too.  Reading an active CPU-wide event is done on the
 * CPU that owns it: perf_event_read() -> smp_call_function_single(), an
 * IPI to the monitored core.  Disabling and enabling an event cost one
 * IPI each as well.  So the whole group is read with a single read() on
 * the leader (PERF_FORMAT_GROUP): one IPI per core per interval, whatever
 * the number of events.  On a Dovetail kernel that IPI is an in-band
 * interrupt: while a real-time thread runs it is only logged, and the
 * read completes when the core next returns to Linux.
 *
 * Two modes:
 *   pid == -1  CPU-wide group for one core (needs privileges)
 *   pid >= 0   group on one thread, counted wherever it runs
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include "perf.h"

static long perf_event_open(struct perf_event_attr *a, pid_t pid, int cpu,
			    int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, a, pid, cpu, group_fd, flags);
}

static struct ev_desc ev_tab[] = {
	[EV_CYCLES]	 = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES,
			     "cycles" },
	[EV_INSTRUCTIONS] = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS,
			      "instructions" },
	[EV_LLC_MISSES]	 = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
			     "LLC/read/miss" },
	[EV_DTLB_MISSES] = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
			     "dTLB/read/miss" },
	/*
	 * Generic iTLB miss, mapped per core type by the kernel
	 * (arch/x86/events/intel/core.c): ITLB_MISSES.WALK_COMPLETED 0xe11
	 * on Core Ultra 200H P-cores, ITLB.MISS 0x0481 on E-cores.  L1I and
	 * L1D are raw events (see raw_evs): the generic L1I miss on P-cores
	 * is L2_RQSTS.ALL_CODE_RD, which counts code requests including
	 * prefetches, and the generic L1D miss is not mapped on E-cores.
	 */
	[EV_ITLB_MISSES] = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_ITLB | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
			     "iTLB/read/miss" },
	[EV_STALL_MEM]	 = { PERF_TYPE_HARDWARE,
			     PERF_COUNT_HW_STALLED_CYCLES_BACKEND,
			     "stalled-cycles-backend" },
	[EV_REF_CYCLES]	 = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_REF_CPU_CYCLES,
			     "ref-cycles" },
	/* per core type, filled in by perf_setup_raw() */
	[EV_L2_MISSES]	 = { 0, 0, "L2 miss (per-PMU raw event)" },
	[EV_L1D_MISSES]	 = { 0, 0, "L1D miss (per-PMU raw event)" },
	[EV_L1I_MISSES]	 = { 0, 0, "L1I miss (per-PMU raw event)" },
	[EV_L3_MISSES]	 = { 0, 0, "L3 miss (per-PMU raw event)" },
	[EV_L3_HITS]	 = { 0, 0, "L3 hit (per-PMU raw event)" },
	[EV_TASK_CLOCK]	 = { PERF_TYPE_SOFTWARE, PERF_COUNT_SW_TASK_CLOCK,
			     "task-clock" },
	[EV_CTX_SWITCH]	 = { PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES,
			     "context-switches" },
	[EV_PAGE_FAULT]	 = { PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS,
			     "page-faults" },
	[EV_CPU_MIGRATE] = { PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS,
			     "cpu-migrations" },
};

/*
 * Rate of the reference-cycle counter (on x86 the TSC rate), measured by
 * counting this thread's own reference cycles over a short busy spin.
 * Busy fraction of a core = reference cycles counted while it is not
 * idle / (rate x interval).  Unlike /proc/stat this includes time spent
 * running out-of-band EVL threads, which Linux's accounting never sees.
 * Returns 0 when the counter is not available.
 */
double perf_ref_hz(void)
{
	struct perf_event_attr attr;
	struct timespec a, b;
	uint64_t count = 0;
	double dt;
	int fd;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HARDWARE;
	attr.size = sizeof(attr);
	attr.config = PERF_COUNT_HW_REF_CPU_CYCLES;
	attr.disabled = 1;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	fd = perf_event_open(&attr, 0, -1, -1, 0);
	if (fd < 0)
		return 0;
	ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
	clock_gettime(CLOCK_MONOTONIC, &a);
	do {
		clock_gettime(CLOCK_MONOTONIC, &b);
		dt = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	} while (dt < 0.2);
	ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
	if (read(fd, &count, sizeof(count)) != sizeof(count))
		count = 0;
	close(fd);
	return count ? (double)count / dt : 0;
}

/* Is @cpu in a sysfs CPU list such as "0-5" or "6-13,16"? */
static int cpu_in_list(const char *list, int cpu)
{
	const char *p = list;

	while (*p) {
		char *end;
		long lo = strtol(p, &end, 10), hi = lo;

		if (end == p)
			return 0;
		if (*end == '-')
			hi = strtol(end + 1, &end, 10);
		if (cpu >= lo && cpu <= hi)
			return 1;
		p = *end == ',' ? end + 1 : end;
		if (*p == '\n')
			break;
	}
	return 0;
}

static int read_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");

	if (!f)
		return -1;
	if (!fgets(buf, (int)n, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int cpu_model(void)
{
	char line[256];
	int fam = -1, model = -1;
	FILE *f = fopen("/proc/cpuinfo", "r");

	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f) && (fam < 0 || model < 0)) {
		if (fam < 0 && sscanf(line, "cpu family : %d", &fam) == 1)
			continue;
		if (model < 0)
			sscanf(line, "model : %d", &model);
	}
	fclose(f);
	return fam == 6 ? model : -1;
}

/*
 * Raw events chosen per PMU: hybrid CPUs need a different code per core
 * type, and the kernel publishes none of these encodings in sysfs.
 * Built-in codes are for Core Ultra 200H (family 6 model 197), taken from
 * "perf list --details":
 *
 *   L2 miss   P  cpu_core      L2_RQSTS.DEMAND_DATA_RD_MISS    0x2124
 *             E  cpu_atom      MEM_LOAD_UOPS_RETIRED.L2_MISS   0x80d1
 *   L1D miss  P  cpu_core      MEM_LOAD_RETIRED.L1_MISS        0x08d1
 *             E  cpu_atom      MEM_LOAD_UOPS_RETIRED.L1_MISS   0x40d1
 *   L1I miss  P  cpu_core      FRONTEND_RETIRED.L1I_MISS       0x03c6, frontend 0x12
 *             E  cpu_atom      FRONTEND_RETIRED.ICACHE         0x20c6
 *   L3 miss   P  cpu_core      MEM_LOAD_RETIRED.L3_MISS        0x20d1
 *             E  cpu_atom      derived: L2 miss - L3 hit
 *   L3 hit    E  cpu_atom      MEM_LOAD_UOPS_RETIRED.L3_HIT    0x1cd1
 *   LP E-cores (cpu_lowpower) share the E-core design; perf has no table
 *   for them and the E-core codes are used.  They have no L3 of their
 *   own, so there "L3 miss" is not meaningful and no code is given.
 *
 * The E-core L3 miss is retired loads that missed L2 and did not hit L3,
 * which is what the P-core event counts directly.
 *
 * Both L1D events count retired loads that missed L1D, and both L1I
 * events retired instructions fetched after an L1I miss (prefetches and
 * wrong-path fetches excluded), so P- and E-cores measure the same thing.
 * The P-core L1I event needs an extra "frontend" selector, which the
 * cpu_core PMU takes in config1 (format/frontend: config1:0-23); a code
 * is written pmu:config[:config1].  For L2 the P-core event counts demand data
 * read requests instead: close, not identical.
 */
#define MAX_RAW_PMUS	4

struct raw_pmu {
	char		name[32];
	uint32_t	type;
	uint64_t	config;
	uint64_t	config1;	/* e.g. the "frontend" field of cpu_core */
	char		cpus[128];	/* CPUs this PMU covers */
};

static struct raw_ev {
	const char	*builtin;
	struct raw_pmu	pmu[MAX_RAW_PMUS];
	int		n;
} raw_evs[EV_NR] = {
	[EV_L2_MISSES]	= { "cpu_core:0x2124,cpu_atom:0x80d1,cpu_lowpower:0x80d1" },
	[EV_L1D_MISSES]	= { "cpu_core:0x08d1,cpu_atom:0x40d1,cpu_lowpower:0x40d1" },
	[EV_L1I_MISSES]	= { "cpu_core:0x03c6:0x12,cpu_atom:0x20c6,cpu_lowpower:0x20c6" },
	/*
	 * E-cores: no EV_L3_MISSES code; perf_read_interval() derives it
	 * as EV_L2_MISSES - EV_L3_HITS, which costs one counter instead of
	 * two.
	 */
	[EV_L3_MISSES]	= { "cpu_core:0x20d1" },
	[EV_L3_HITS]	= { "cpu_atom:0x1cd1" },
};

/*
 * @spec: "pmu:config[,pmu:config...]", or NULL for the built-in codes.
 * Returns the number of PMUs configured for @ev; a PMU this machine does
 * not have is skipped.
 */
int perf_setup_raw(int ev, const char *spec, char *msg, size_t msglen)
{
	struct raw_ev *r = &raw_evs[ev];
	char copy[256], *tok, *save = NULL, path[160], buf[256];

	r->n = 0;
	msg[0] = 0;
	if (!spec) {
		if (cpu_model() != 197) {
			snprintf(msg, msglen, "no %s code known for this CPU model",
				 ev_tab[ev].name);
			return 0;
		}
		spec = r->builtin;
	}
	snprintf(copy, sizeof(copy), "%s", spec);
	for (tok = strtok_r(copy, ",", &save); tok && r->n < MAX_RAW_PMUS;
	     tok = strtok_r(NULL, ",", &save)) {
		struct raw_pmu *p = &r->pmu[r->n];
		unsigned long long config, config1 = 0;

		if (sscanf(tok, "%31[^:]:%llx:%llx", p->name, &config, &config1) < 2) {
			snprintf(msg, msglen, "bad event '%s' (want pmu:0xconfig[:0xconfig1])", tok);
			continue;
		}
		p->config1 = config1;
		snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", p->name);
		if (read_line(path, buf, sizeof(buf)) < 0)
			continue;	/* e.g. no cpu_lowpower on this chip */
		p->type = (uint32_t)atoi(buf);
		p->config = config;
		snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/cpus", p->name);
		if (read_line(path, p->cpus, sizeof(p->cpus)) < 0)
			snprintf(p->cpus, sizeof(p->cpus), "0-%d", 4095);	/* non-hybrid */
		r->n++;
	}
	return r->n;
}

/* The PMU entry of @ev covering @cpu, or NULL (thread mode: cores differ). */
static const struct raw_pmu *raw_for_cpu(int ev, int cpu)
{
	const struct raw_ev *r = &raw_evs[ev];

	for (int i = 0; cpu >= 0 && i < r->n; i++)
		if (cpu_in_list(r->pmu[i].cpus, cpu))
			return &r->pmu[i];
	return NULL;
}

/*
 * Core type of @cpu on a hybrid CPU, from the per-type PMUs' "cpus"
 * lists.  NULL on a non-hybrid machine (a single "cpu" PMU), where there
 * is nothing to tell apart.  On Intel the names are cpu_core (P-core),
 * cpu_atom (E-core) and cpu_lowpower (LP E-core).
 */
const char *perf_core_type(int cpu)
{
	static const struct { const char *pmu, *label; } types[] = {
		{ "cpu_core", "P-core" },
		{ "cpu_atom", "E-core" },
		{ "cpu_lowpower", "LP E-core" },
	};
	char path[128], buf[256];

	for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
		snprintf(path, sizeof(path),
			 "/sys/bus/event_source/devices/%s/cpus", types[i].pmu);
		if (read_line(path, buf, sizeof(buf)) == 0 && cpu_in_list(buf, cpu))
			return types[i].label;
	}
	return NULL;
}

bool perf_raw_covers(int ev, int cpu)
{
	return raw_for_cpu(ev, cpu) != NULL;
}

static bool is_raw(int ev)
{
	return raw_evs[ev].builtin != NULL;
}

const char *perf_event_name(int ev)
{
	return ev >= 0 && ev < EV_NR ? ev_tab[ev].name : "?";
}

static int open_one(struct perf_group *g, int e, int group_fd)
{
	struct perf_event_attr attr;
	int fd;

	memset(&attr, 0, sizeof(attr));
	attr.type = ev_tab[e].type;
	attr.size = sizeof(attr);
	attr.config = ev_tab[e].config;
	if (is_raw(e)) {
		const struct raw_pmu *p = raw_for_cpu(e, g->cpu);

		if (!p)
			return -ENOENT;
		attr.type = p->type;
		attr.config = p->config;
		attr.config1 = p->config1;
	}
	attr.disabled = group_fd < 0;	/* the leader starts the group */
	attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
			   PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
	attr.exclude_hv = 1;
	attr.exclude_kernel = g->user_only;
	if (g->pid < 0)
		attr.exclude_idle = 1;	/* nothing to count on an idle core */
	/*
	 * Not pinned: if another user (perf, a second pmutop) holds the
	 * counters, the kernel time-shares them instead of putting a pinned
	 * group into the error state, where read() returns 0 for good.
	 * The values are scaled by enabled/running time below.
	 */
	attr.pinned = 0;

	fd = perf_event_open(&attr, g->pid, g->cpu, group_fd, 0);
	if (fd < 0)
		return -errno;
	if (ioctl(fd, PERF_EVENT_IOC_ID, &g->id[e]) < 0) {
		close(fd);
		return -errno;
	}
	g->fd[e] = fd;
	g->open[e] = true;
	return 0;
}

int perf_open(struct perf_group *g, int cpu, pid_t pid, char *err, size_t errlen)
{
	int e, ret;

	memset(g, 0, sizeof(*g));
	g->cpu = cpu;
	g->pid = pid;
	g->n_ev = EV_NR;
	for (e = 0; e < EV_NR; e++)
		g->fd[e] = -1;

	/*
	 * Hardware leader first.  Without a PMU (most virtual machines,
	 * WSL2) or without privileges, fall back to a software leader so
	 * that the demotion indicators (context switches, page faults,
	 * migrations) remain available.
	 */
	g->leader = EV_CYCLES;
	g->hw = true;
	ret = open_one(g, EV_CYCLES, -1);
	if (ret == -EACCES && pid >= 0) {
		/*
		 * Unprivileged and perf_event_paranoid >= 2: only user-space
		 * counting of one's own tasks is allowed.  Kernel time (EVL
		 * syscalls included) is then not counted.
		 */
		g->user_only = true;
		ret = open_one(g, EV_CYCLES, -1);
	}
	if (ret < 0) {
		snprintf(err, errlen, "no hardware PMU events (%s)", strerror(-ret));
		g->leader = EV_TASK_CLOCK;
		g->hw = false;
		ret = open_one(g, EV_TASK_CLOCK, -1);
		if (ret < 0) {
			snprintf(err, errlen, "perf_event_open(cpu%d pid%d): %s",
				 cpu, pid, strerror(-ret));
			return -1;
		}
	}

	for (e = 0; e < EV_NR; e++) {
		if (e == g->leader)
			continue;
		if (!g->hw && e < EV_TASK_CLOCK)
			continue;
		if (is_raw(e) && !raw_for_cpu(e, cpu))
			continue;	/* no code for this core type */
		/*
		 * A missing member is not fatal: some PMUs lack the dTLB or
		 * LLC events, and the monitor reports which checks it had
		 * to disable.
		 */
		open_one(g, e, g->fd[g->leader]);
	}

	if (ioctl(g->fd[g->leader], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0 ||
	    ioctl(g->fd[g->leader], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
		snprintf(err, errlen, "perf enable(cpu%d): %s", cpu, strerror(errno));
		perf_close(g);
		return -1;
	}
	return g->hw ? 0 : 1;
}

void perf_close(struct perf_group *g)
{
	int e;

	if (g->fd[g->leader] >= 0)
		ioctl(g->fd[g->leader], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
	for (e = 0; e < EV_NR; e++) {
		if (g->fd[e] >= 0)
			close(g->fd[e]);
		g->fd[e] = -1;
		g->open[e] = false;
	}
}

/*
 * One read() of the leader returns every member:
 *   { u64 nr; u64 time_enabled; u64 time_running;
 *     struct { u64 value; u64 id; } v[nr]; }
 * Values are cumulative.  The deltas returned are scaled by
 * enabled/running time, so they estimate the full interval when the
 * counters had to be shared with another perf user.
 */
int perf_read_interval(struct perf_group *g, int64_t out[EV_NR],
		       bool ok[EV_NR], char *err, size_t errlen)
{
	uint64_t buf[3 + 2 * EV_NR];
	uint64_t den, drun;
	double scale;
	ssize_t n;
	uint64_t i;
	int e;

	for (e = 0; e < EV_NR; e++) {
		out[e] = -1;
		ok[e] = false;
	}
	n = read(g->fd[g->leader], buf, sizeof(buf));
	if (n < (ssize_t)(3 * sizeof(uint64_t))) {
		snprintf(err, errlen, "group read(cpu%d): %s", g->cpu,
			 n < 0 ? strerror(errno) : "short read");
		return -1;
	}
	den = buf[1] - g->prev_enabled;
	drun = buf[2] - g->prev_running;
	g->prev_enabled = buf[1];
	g->prev_running = buf[2];
	g->running = den ? (double)drun / (double)den : 0;
	if (den && !drun && !g->warned) {
		/*
		 * Enabled but never scheduled: the group needs more counters
		 * than this core's PMU has, or another user holds them all.
		 */
		snprintf(err, errlen, "cpu%d: counter group not scheduled "
			 "(too many events for this PMU, or counters in use)", g->cpu);
		g->warned = true;
		return -2;
	}
	scale = drun ? (double)den / (double)drun : 0;

	for (i = 0; i < buf[0] && 4 + 2 * i < (uint64_t)n / sizeof(uint64_t); i++) {
		uint64_t val = buf[3 + 2 * i], id = buf[4 + 2 * i];

		for (e = 0; e < EV_NR; e++) {
			if (!g->open[e] || g->id[e] != id)
				continue;
			/* never counted in this interval: no estimate */
			if (drun) {
				out[e] = (int64_t)((double)(val - g->prev[e]) * scale);
				ok[e] = true;
			}
			g->prev[e] = val;
			break;
		}
	}

	/*
	 * Where the core type has no L3-miss event (E-cores), derive it:
	 * loads that missed L2 and did not hit L3 went to DRAM.  Counts of
	 * the same interval, so the difference is consistent; clamp at 0
	 * against rounding of the time scaling.
	 */
	if (!g->open[EV_L3_MISSES] && ok[EV_L2_MISSES] && ok[EV_L3_HITS]) {
		int64_t d = out[EV_L2_MISSES] - out[EV_L3_HITS];

		out[EV_L3_MISSES] = d > 0 ? d : 0;
		ok[EV_L3_MISSES] = true;
	}
	return 0;
}

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
	[EV_CACHE_REFS]	 = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES,
			     "cache/read/access" },
	[EV_CACHE_MISSES] = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES,
			      "cache/read/miss" },
	[EV_LLC_REFS]	 = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16),
			     "LLC/read/access" },
	[EV_LLC_MISSES]	 = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_LL | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
			     "LLC/read/miss" },
	[EV_DTLB_REFS]	 = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16),
			     "dTLB/read/access" },
	[EV_DTLB_MISSES] = { PERF_TYPE_HW_CACHE,
			     PERF_COUNT_HW_CACHE_DTLB | (PERF_COUNT_HW_CACHE_OP_READ << 8)
			     | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16),
			     "dTLB/read/miss" },
	[EV_STALL_MEM]	 = { PERF_TYPE_HARDWARE,
			     PERF_COUNT_HW_STALLED_CYCLES_BACKEND,
			     "stalled-cycles-backend" },
	[EV_REF_CYCLES]	 = { PERF_TYPE_HARDWARE, PERF_COUNT_HW_REF_CPU_CYCLES,
			     "ref-cycles" },
	/* filled in by perf_setup_l2(); type 0 config 0 = not available */
	[EV_L2_MISSES]	 = { 0, 0, "L2 miss (per-PMU raw event)" },
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

/*
 * L2 misses have no generic perf event: the counter is model specific and
 * lives in a named PMU ("cpu" on most machines, "cpu_core"/"cpu_atom" on
 * Intel hybrids).  The kernel does not publish these encodings in sysfs
 * (perf carries them in its own JSON tables), so the default is the one
 * for this research platform and anything else is given with --l2-event:
 *
 *   --l2-event cpu_core:0x2124    PMU name : raw config
 *
 * Intel family 6 model 197 (Arrow Lake H, Core Ultra 200H) P-cores:
 * L2_RQSTS.DEMAND_DATA_RD_MISS = event 0x24, umask 0x21, i.e. config
 * 0x2124, as reported by "perf stat -vv -e cpu_core/l2_rqsts.demand_data_rd_miss/".
 * Returns 0 when the event will be counted on @cpu, -1 otherwise.
 */
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

/* One L2-miss event per PMU: hybrid CPUs need a different code per core type. */
#define MAX_L2_PMUS	4

static struct l2_pmu {
	char		name[32];
	uint32_t	type;
	uint64_t	config;
	char		cpus[128];	/* CPUs this PMU covers */
} l2_pmus[MAX_L2_PMUS];
static int nr_l2_pmus;

/*
 * @spec: "pmu:config[,pmu:config...]", or NULL for the built-in table.
 * Returns the number of PMUs configured; a PMU missing on this machine is
 * reported in @msg and skipped.
 */
int perf_setup_l2(const char *spec, char *msg, size_t msglen)
{
	char copy[256], *tok, *save = NULL, path[160], buf[256];

	nr_l2_pmus = 0;
	msg[0] = 0;
	if (!spec) {
		if (cpu_model() != 197) {
			snprintf(msg, msglen, "no L2 event known for this CPU model;"
				 " give one with --l2-event pmu:config");
			return 0;
		}
		/*
		 * Core Ultra 200H, from "perf list --details":
		 *   P-cores  cpu_core  L2_RQSTS.DEMAND_DATA_RD_MISS    0x2124
		 *   E-cores  cpu_atom  MEM_LOAD_UOPS_RETIRED.L2_MISS   0x80d1
		 *   LP E     cpu_lowpower: same core type as the E-cores;
		 *            perf lists no table for it, 0x80d1 counts plausibly.
		 * The E-core event counts retired loads that missed L2, the
		 * P-core one demand data reads: close, not identical.
		 */
		spec = "cpu_core:0x2124,cpu_atom:0x80d1,cpu_lowpower:0x80d1";
	}
	snprintf(copy, sizeof(copy), "%s", spec);
	for (tok = strtok_r(copy, ",", &save); tok && nr_l2_pmus < MAX_L2_PMUS;
	     tok = strtok_r(NULL, ",", &save)) {
		struct l2_pmu *p = &l2_pmus[nr_l2_pmus];
		unsigned long long config;

		if (sscanf(tok, "%31[^:]:%llx", p->name, &config) != 2) {
			snprintf(msg, msglen, "bad --l2-event '%s' (want pmu:0xconfig)", tok);
			continue;
		}
		snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", p->name);
		if (read_line(path, buf, sizeof(buf)) < 0)
			continue;	/* e.g. no cpu_lowpower on this chip */
		p->type = (uint32_t)atoi(buf);
		p->config = config;
		snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/cpus", p->name);
		if (read_line(path, p->cpus, sizeof(p->cpus)) < 0)
			snprintf(p->cpus, sizeof(p->cpus), "0-%d", 4095);	/* non-hybrid */
		nr_l2_pmus++;
	}
	return nr_l2_pmus;
}

/* The L2 PMU covering @cpu, or NULL (thread mode: none, cores differ). */
static const struct l2_pmu *l2_for_cpu(int cpu)
{
	for (int i = 0; cpu >= 0 && i < nr_l2_pmus; i++)
		if (cpu_in_list(l2_pmus[i].cpus, cpu))
			return &l2_pmus[i];
	return NULL;
}

bool perf_l2_covers(int cpu)
{
	return l2_for_cpu(cpu) != NULL;
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
	if (e == EV_L2_MISSES) {
		const struct l2_pmu *p = l2_for_cpu(g->cpu);

		if (!p)
			return -ENOENT;
		attr.type = p->type;
		attr.config = p->config;
	}
	attr.disabled = group_fd < 0;	/* the leader starts the group */
	attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID |
			   PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
	attr.exclude_hv = 1;
	attr.exclude_kernel = g->user_only;
	if (g->pid < 0)
		attr.exclude_idle = 1;	/* nothing to count on an idle core */
	/*
	 * Not pinned: if another user (perf, a second rt-health) holds the
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
		if (e == EV_L2_MISSES && !l2_for_cpu(cpu))
			continue;	/* no L2 event for this core type */
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
	return 0;
}

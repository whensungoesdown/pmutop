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

static const struct ev_desc ev_tab[] = {
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
	attr.disabled = group_fd < 0;	/* the leader starts the group */
	attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
	attr.exclude_hv = 1;
	attr.exclude_kernel = g->user_only;
	if (g->pid < 0)
		attr.exclude_idle = 1;	/* nothing to count on an idle core */
	attr.pinned = group_fd < 0;

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
 *   { u64 nr; struct { u64 value; u64 id; } v[nr]; }
 * Values are cumulative; deltas since the previous call are returned.
 */
int perf_read_interval(struct perf_group *g, int64_t out[EV_NR],
		       bool ok[EV_NR], char *err, size_t errlen)
{
	uint64_t buf[1 + 2 * EV_NR];
	ssize_t n;
	uint64_t i;
	int e;

	for (e = 0; e < EV_NR; e++) {
		out[e] = -1;
		ok[e] = false;
	}
	n = read(g->fd[g->leader], buf, sizeof(buf));
	if (n < (ssize_t)sizeof(uint64_t)) {
		snprintf(err, errlen, "group read(cpu%d): %s", g->cpu,
			 n < 0 ? strerror(errno) : "short read");
		return -1;
	}
	for (i = 0; i < buf[0] && 2 + 2 * i < (uint64_t)n / sizeof(uint64_t); i++) {
		uint64_t val = buf[1 + 2 * i], id = buf[2 + 2 * i];

		for (e = 0; e < EV_NR; e++) {
			if (!g->open[e] || g->id[e] != id)
				continue;
			out[e] = (int64_t)(val - g->prev[e]);
			g->prev[e] = val;
			ok[e] = true;
			break;
		}
	}
	return 0;
}

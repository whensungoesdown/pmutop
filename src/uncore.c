// SPDX-License-Identifier: MIT
/*
 * uncore.c - chip-wide counters outside the cores.
 *
 *   uncore_cncu/clockticks, uncore_sncu/clockticks
 *	Two clock domains of the uncore, counted in cycles: ticks per second
 *	is the domain's average frequency over the interval.  On the Core
 *	Ultra 200H reference machine, capping the uncore frequency through
 *	intel_uncore_frequency to 2.0 GHz capped cncu at exactly 2.00 G/s
 *	(so cncu is the clock that driver controls, most likely ring and
 *	L3), while sncu ran on at 2.19 G/s and rose from ~0.9 to ~2.1 G/s
 *	only under memory traffic.  Which hardware units they clock is not
 *	documented here; the names are shown as the kernel gives them.
 *
 *   uncore_imc_free_running_N/data_read, data_write
 *	Bytes moved by each memory controller.  Free-running hardware
 *	counters: no programmable counter is used, nothing else competes.
 *
 * Every event, units and scale are taken from sysfs
 * (/sys/bus/event_source/devices/<pmu>/{type,cpumask,events/}) rather
 * than hard-coded, so a PMU missing on another machine is just skipped.
 * Uncore events are opened system-wide on the CPU in the PMU's cpumask.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include "uncore.h"

#define EVDIR	"/sys/bus/event_source/devices"
#define MAX_UNC	16

enum ukind { U_CNCU, U_SNCU, U_DRAM_RD, U_DRAM_WR };

static struct unc_ev {
	enum ukind	kind;
	int		fd;
	double		scale;		/* count -> bytes (DRAM) or 1 (clocks) */
	uint64_t	prev;
	bool		primed;
} uev[MAX_UNC];
static int nuev;

static int read_str(const char *path, char *buf, size_t n)
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

/*
 * Turn "event=0xff,umask=0x20" into a config value using the PMU's own
 * format files ("config:0-7", "config:8-15").
 */
static int encode(const char *pmu, const char *spec, uint64_t *config)
{
	char copy[128], *tok, *save = NULL, path[256], fmt[64];

	*config = 0;
	snprintf(copy, sizeof(copy), "%s", spec);
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(tok, '=');
		unsigned long long v = 1;
		int lo, hi;

		if (eq) {
			*eq = 0;
			v = strtoull(eq + 1, NULL, 0);
		}
		snprintf(path, sizeof(path), EVDIR "/%s/format/%s", pmu, tok);
		if (read_str(path, fmt, sizeof(fmt)) < 0)
			return -1;
		/* only fields in "config" are used by these events */
		if (sscanf(fmt, "config:%d-%d", &lo, &hi) == 2)
			;
		else if (sscanf(fmt, "config:%d", &lo) == 1)
			hi = lo;
		else
			return -1;
		*config |= ((uint64_t)v & ((hi - lo == 63) ? ~0ULL : ((1ULL << (hi - lo + 1)) - 1))) << lo;
	}
	return 0;
}

static int open_one(const char *pmu, const char *event, enum ukind kind)
{
	char path[256], buf[128];
	struct perf_event_attr attr;
	uint64_t config;
	int type, cpu, fd;
	double scale = 1.0;

	if (nuev >= MAX_UNC)
		return -1;
	snprintf(path, sizeof(path), EVDIR "/%s/type", pmu);
	if (read_str(path, buf, sizeof(buf)) < 0)
		return -1;
	type = atoi(buf);
	snprintf(path, sizeof(path), EVDIR "/%s/cpumask", pmu);
	cpu = read_str(path, buf, sizeof(buf)) == 0 ? atoi(buf) : 0;
	snprintf(path, sizeof(path), EVDIR "/%s/events/%s", pmu, event);
	if (read_str(path, buf, sizeof(buf)) < 0 || encode(pmu, buf, &config) < 0)
		return -1;
	if (kind == U_DRAM_RD || kind == U_DRAM_WR) {
		char unit[32] = "";

		/* scale and unit turn the raw count into MiB */
		snprintf(path, sizeof(path), EVDIR "/%s/events/%s.scale", pmu, event);
		if (read_str(path, buf, sizeof(buf)) == 0)
			scale = strtod(buf, NULL);
		snprintf(path, sizeof(path), EVDIR "/%s/events/%s.unit", pmu, event);
		read_str(path, unit, sizeof(unit));
		if (strcmp(unit, "MiB") == 0)
			scale *= 1024.0 * 1024.0;
		else if (strcmp(unit, "KiB") == 0)
			scale *= 1024.0;
	}

	memset(&attr, 0, sizeof(attr));
	attr.type = (uint32_t)type;
	attr.size = sizeof(attr);
	attr.config = config;
	fd = (int)syscall(__NR_perf_event_open, &attr, -1, cpu, -1, 0);
	if (fd < 0)
		return -errno;
	uev[nuev++] = (struct unc_ev){ .kind = kind, .fd = fd, .scale = scale };
	return 0;
}

int uncore_open(char *msg, size_t msglen)
{
	DIR *d;
	struct dirent *de;
	int err = 0;

	msg[0] = 0;
	err |= open_one("uncore_cncu", "clockticks", U_CNCU);
	err |= open_one("uncore_sncu", "clockticks", U_SNCU);

	/* one free-running counter set per memory controller */
	d = opendir(EVDIR);
	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "uncore_imc_free_running_", 24) != 0)
			continue;
		open_one(de->d_name, "data_read", U_DRAM_RD);
		open_one(de->d_name, "data_write", U_DRAM_WR);
	}
	if (d)
		closedir(d);
	if (!nuev)
		snprintf(msg, msglen, "no uncore counters (cncu/sncu clocks, DRAM bandwidth)");
	(void)err;
	return nuev;
}

void uncore_read(struct uncore_sample *u, double dt_s)
{
	double sum[4] = { 0 };
	bool have[4] = { false };

	for (int i = 0; i < nuev; i++) {
		struct unc_ev *e = &uev[i];
		uint64_t v;

		if (read(e->fd, &v, sizeof(v)) != sizeof(v))
			continue;
		if (e->primed && dt_s > 0) {
			sum[e->kind] += (double)(v - e->prev) * e->scale / dt_s;
			have[e->kind] = true;
		}
		e->prev = v;
		e->primed = true;
	}
	u->cncu_ghz = have[U_CNCU] ? sum[U_CNCU] / 1e9 : NAN;
	u->sncu_ghz = have[U_SNCU] ? sum[U_SNCU] / 1e9 : NAN;
	u->dram_rd_gbs = have[U_DRAM_RD] ? sum[U_DRAM_RD] / 1e9 : NAN;
	u->dram_wr_gbs = have[U_DRAM_WR] ? sum[U_DRAM_WR] / 1e9 : NAN;
}

void uncore_close(void)
{
	for (int i = 0; i < nuev; i++)
		close(uev[i].fd);
	nuev = 0;
}

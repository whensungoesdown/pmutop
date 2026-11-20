// SPDX-License-Identifier: MIT
/*
 * sysfs.c - counters that do not need the PMU.
 *
 * All of these are read by an ordinary Linux thread.  On a Dovetail
 * kernel that thread can only run in band (a task in the out-of-band
 * stage is never scheduled by Linux), so nothing here executes on the
 * isolated cores; only the kernel's bookkeeping is read.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pmutop.h"

#define SYS_CPU	"/sys/devices/system/cpu"

static int read_file(const char *path, char *buf, size_t len)
{
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	return 0;
}

static int read_u64(const char *path, uint64_t *out)
{
	char buf[64], *end;
	unsigned long long v;

	if (read_file(path, buf, sizeof(buf)) < 0)
		return -1;
	errno = 0;
	v = strtoull(buf, &end, 10);
	if (errno || end == buf)
		return -1;
	*out = v;
	return 0;
}

/*
 * One "cpuN" line of /proc/stat:
 *   cpuN user nice system idle iowait irq softirq steal [guest guest_nice]
 * guest time is already part of user/nice in this kernel and is not added
 * again.  Returns 0 and fills @out with the eight values.
 */
int sys_read_stat(struct cpu_sample *s, uint64_t out[8])
{
	char line[512];
	FILE *f;
	int ret = -1;

	f = fopen("/proc/stat", "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof(line), f)) {
		unsigned long long v[8];
		int cpu, n;

		if (strncmp(line, "cpu", 3) != 0)
			continue;
		n = sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu",
			   &cpu, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
			   &v[6], &v[7]);
		if (n < 9 || cpu != s->cpu)
			continue;
		for (n = 0; n < 8; n++)
			out[n] = v[n];
		ret = 0;
		break;
	}
	fclose(f);
	return ret;
}

/* ------------------------------------------------------------------ */
/*
 * /proc/interrupts: an IRQ identifier ending in ':', one counter column
 * per CPU (named on the header line), then the controller and device.
 *
 * Counters are cumulative, so the previous value of every IRQ is kept per
 * monitored slot and only the delta is used.  Each IRQ is classified, so
 * that the expected traffic of an EVL core - its out-of-band timer and
 * the Linux proxy tick - is not mistaken for interference.
 */
#define MAX_IRQS	512

struct irq_prev {
	char		id[24];
	uint64_t	count;
};

static struct irq_prev irq_prev[MAX_CPUS][MAX_IRQS];
static int irq_nprev[MAX_CPUS];
static const char *irq_path = "/proc/interrupts";

/* For the unit test: read interrupts from a captured file instead. */
void sys_set_interrupts_path(const char *path)
{
	irq_path = path;
}

static uint64_t irq_delta(int slot, const char *id, uint64_t now)
{
	struct irq_prev *t = irq_prev[slot];
	uint64_t d;
	int i;

	for (i = 0; i < irq_nprev[slot]; i++) {
		if (strcmp(t[i].id, id) == 0) {
			d = now >= t[i].count ? now - t[i].count : 0;
			t[i].count = now;
			return d;
		}
	}
	if (irq_nprev[slot] < MAX_IRQS) {
		snprintf(t[i].id, sizeof(t[i].id), "%s", id);
		t[i].count = now;
		irq_nprev[slot]++;
	}
	return 0;	/* first sighting: no delta yet */
}

enum irq_class { IRQC_TIMER, IRQC_THERMAL, IRQC_IPI, IRQC_DEVICE, IRQC_OTHER };

static enum irq_class classify(const char *id, const char *desc)
{
	if (isdigit((unsigned char)id[0])) {
		/*
		 * Numbered IRQs are real devices unless they belong to
		 * Dovetail's synthetic domains: SIPIC (system vectors given
		 * an IRQ number, e.g. the out-of-band LAPIC timer) and SIRQC
		 * (software-raised in-band IRQs: proxy tick, irq work).  A
		 * device's flow handler can be "oob-fasteoi" (an out-of-band
		 * capable chip), so the word "oob" alone says nothing.
		 */
		if (strstr(desc, "SIPIC") || strstr(desc, "SIRQC"))
			return (strstr(desc, "timer") || strstr(desc, "tick")) ?
				IRQC_TIMER : IRQC_IPI;
		return IRQC_DEVICE;
	}
	if (strcmp(id, "LOC") == 0)
		return IRQC_TIMER;
	if (strcmp(id, "TRM") == 0 || strcmp(id, "THR") == 0)
		return IRQC_THERMAL;
	if (strcmp(id, "NMI") == 0 || strcmp(id, "PMI") == 0 ||
	    strcmp(id, "MCP") == 0 || strcmp(id, "ERR") == 0 ||
	    strcmp(id, "MIS") == 0)
		return IRQC_OTHER;
	return IRQC_IPI;	/* RES, CAL, TLB, IWI, PIN, NPI, PIW, ... */
}

int sys_read_interrupts(struct cpu_sample *s, int slot)
{
	char line[2048], *p, *end, id[24];
	FILE *f;
	uint64_t vec, d, best = 0;
	int ncols = 0, i;

	if (slot < 0 || slot >= MAX_CPUS)
		return -1;
	s->irqs = s->irq_timer = s->irq_thermal = 0;
	s->irq_ipi = s->irq_device = 0;
	s->irq_dev_top[0] = '\0';

	f = fopen(irq_path, "r");
	if (!f)
		return -1;
	if (fgets(line, sizeof(line), f)) {
		for (p = line; *p; ) {
			while (*p && isspace((unsigned char)*p))
				p++;
			if (!*p)
				break;
			ncols++;
			while (*p && !isspace((unsigned char)*p))
				p++;
		}
	}
	if (s->cpu >= ncols) {
		fclose(f);
		return -1;
	}

	while (fgets(line, sizeof(line), f)) {
		end = line + strlen(line);
		while (end > line && isspace((unsigned char)end[-1]))
			*--end = '\0';
		p = line;
		while (p < end && isspace((unsigned char)*p))
			p++;
		i = 0;
		while (p < end && *p != ':' && i < (int)sizeof(id) - 1)
			id[i++] = *p++;
		id[i] = '\0';
		if (p >= end || *p != ':')
			continue;
		p++;

		/* Walk the CPU columns; keep ours. */
		vec = 0;
		for (i = 0; i < ncols; i++) {
			uint64_t v;

			while (p < end && isspace((unsigned char)*p))
				p++;
			if (p >= end || !isdigit((unsigned char)*p))
				break;
			v = strtoull(p, &p, 10);
			if (i == s->cpu)
				vec = v;
		}
		while (p < end && isspace((unsigned char)*p))
			p++;		/* p: controller and device text */

		d = irq_delta(slot, id, vec);
		if (!d)
			continue;
		s->irqs += d;
		switch (classify(id, p)) {
		case IRQC_TIMER:
			s->irq_timer += d;
			break;
		case IRQC_THERMAL:
			s->irq_thermal += d;
			break;
		case IRQC_IPI:
			s->irq_ipi += d;
			break;
		case IRQC_DEVICE:
			s->irq_device += d;
			if (d > best) {
				best = d;
				char txt[36];
				const char *dev = strrchr(p, ' ');

				/*
				 * The device name is the last field
				 * ("iwlwifi:default_queue"); the text before
				 * it is the controller and the flow handler.
				 */
				snprintf(txt, sizeof(txt), "%.32s", dev ? dev + 1 : p);
				snprintf(s->irq_dev_top, sizeof(s->irq_dev_top),
					 "IRQ %.10s: %s", id, txt);
			}
			break;
		default:
			break;
		}
	}
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */

int sys_read_freq(struct cpu_sample *s)
{
	char path[128];

	snprintf(path, sizeof(path), SYS_CPU "/cpu%d/cpufreq/scaling_cur_freq", s->cpu);
	return read_u64(path, &s->freq_khz);
}

int sys_read_cstates(struct cpu_sample *s, uint64_t out[MAX_CS], uint32_t *nr)
{
	char path[160];
	uint32_t i;
	uint64_t v;

	*nr = 0;
	for (i = 0; i < MAX_CS; i++) {
		snprintf(path, sizeof(path), SYS_CPU "/cpu%d/cpuidle/state%u/time",
			 s->cpu, i);
		if (read_u64(path, &v) < 0)
			break;
		out[i] = v;
		*nr = i + 1;
	}
	return *nr ? 0 : -1;
}

int sys_read_throttle(struct cpu_sample *s, int64_t *delta)
{
	char path[128];
	uint64_t v;

	snprintf(path, sizeof(path),
		 SYS_CPU "/cpu%d/thermal_throttle/package_throttle_count", s->cpu);
	if (read_u64(path, &v) < 0)
		return -1;
	*delta = (int64_t)v - s->throttle;
	s->throttle = (int64_t)v;
	return 0;
}

/*
 * Temperature.  coretemp publishes one hwmon device per package with one
 * tempN_input per core; temp2 is a reasonable package proxy.  Falls back
 * to thermal_zone0 without hwmon coretemp.
 */
int sys_read_temp(int cpu, double *degc)
{
	char path[160], buf[160];
	int i;

	(void)cpu;
	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
		if (read_file(path, buf, sizeof(buf)) < 0)
			continue;
		if (strncmp(buf, "coretemp", 8) != 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp2_input", i);
		if (read_file(path, buf, sizeof(buf)) == 0) {
			*degc = atoi(buf) / 1000.0;
			return 0;
		}
	}
	if (read_file("/sys/class/thermal/thermal_zone0/temp", buf, sizeof(buf)) == 0) {
		*degc = atoi(buf) / 1000.0;
		return 0;
	}
	return -1;
}

int sys_cpu_online(int cpu)
{
	char path[128], buf[8];

	snprintf(path, sizeof(path), SYS_CPU "/cpu%d/online", cpu);
	if (read_file(path, buf, sizeof(buf)) < 0)
		return cpu == 0;	/* cpu0 has no online file */
	return buf[0] == '1';
}

/* /sys/devices/system/cpu/isolated: "4-5", "4,5" or empty */
int sys_rt_cpus(char *buf, size_t len)
{
	if (read_file(SYS_CPU "/isolated", buf, len) < 0)
		return -1;
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

// SPDX-License-Identifier: MIT
/*
 * test_irq.c - replay two /proc/interrupts snapshots taken from the
 * ThinkBook (Core Ultra 9 285H, EVL kernel, 16 CPUs) and check that the
 * deltas seen by CPU 4 land in the right class.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "rt-health.h"

static const char *hdr =
"           CPU0       CPU1       CPU2       CPU3       CPU4       CPU5       CPU6       CPU7       CPU8       CPU9       CPU10      CPU11      CPU12      CPU13      CPU14      CPU15\n";

/* CPU 4 is the sixth column.  Each line: id, 16 counters, description. */
static void line(FILE *f, const char *id, unsigned long cpu4, const char *desc)
{
	fprintf(f, "%5s:", id);
	for (int c = 0; c < 16; c++)
		fprintf(f, " %10lu", c == 4 ? cpu4 : 1000ul);
	fprintf(f, "   %s\n", desc);
}

static void snapshot(const char *path, unsigned long k)
{
	FILE *f = fopen(path, "w");

	assert(f);
	fputs(hdr, f);
	/* a device whose chip is out-of-band capable: still a device */
	line(f, "14", 0 + 7 * k, "IR-IO-APIC     14 oob-fasteoi   INTC105E:00");
	line(f, "120", 20 + 3 * k, "SIRQC    120      in-band irq work");
	line(f, "123", 231174 + 100 * k, "SIRQC    123      proxy tick");
	line(f, "189", 0 + 500 * k, "IR-PCI-MSIX-0000:00:14.3    6-edge      iwlwifi:queue_6");
	line(f, "524548", 1000 + 2000 * k, "SIPIC      4 oob     Out-of-band LAPIC timer interrupt");
	line(f, "NMI", 925 + k, "Non-maskable interrupts");
	line(f, "LOC", 50 + 9 * k, "Local timer interrupts");
	line(f, "RES", 3960 + 11 * k, "Rescheduling interrupts");
	line(f, "TRM", 4150 + 800 * k, "Thermal event interrupts");
	fclose(f);
}

int main(void)
{
	const char *path = "/tmp/rt-health-test-interrupts";
	struct cpu_sample s;

	memset(&s, 0, sizeof(s));
	s.cpu = 4;
	sys_set_interrupts_path(path);

	snapshot(path, 0);
	assert(sys_read_interrupts(&s, 0) == 0);	/* seeds, no delta */
	assert(s.irqs == 0);

	snapshot(path, 1);
	assert(sys_read_interrupts(&s, 0) == 0);

	printf("total %llu timer %llu thermal %llu ipi %llu device %llu top '%s'\n",
	       (unsigned long long)s.irqs, (unsigned long long)s.irq_timer,
	       (unsigned long long)s.irq_thermal, (unsigned long long)s.irq_ipi,
	       (unsigned long long)s.irq_device, s.irq_dev_top);

	assert(s.irq_timer == 100 + 2000 + 9);	/* proxy tick + oob LAPIC + LOC */
	assert(s.irq_thermal == 800);
	assert(s.irq_ipi == 3 + 11);		/* irq work + RES */
	assert(s.irq_device == 7 + 500);	/* touchpad + Wi-Fi */
	assert(strcmp(s.irq_dev_top, "IRQ 189: iwlwifi:queue_6") == 0);
	assert(s.irqs == 100 + 2000 + 9 + 800 + 14 + 507 + 1);	/* + NMI */

	puts("test_irq: OK");
	return 0;
}

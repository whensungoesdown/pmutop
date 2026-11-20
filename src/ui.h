/* SPDX-License-Identifier: MIT */
#ifndef RT_HEALTH_UI_H
#define RT_HEALTH_UI_H

#include "rt-health.h"
#include "metrics.h"

struct ui_info {
	int		interval_ms;
	const char	*mode;		/* "cpu", "thread 1234" */
	const char	*csv;		/* NULL when not recording */
	bool		telemetry;
};

/* Full-screen terminal view (-graphic).  Plain ANSI escapes, no curses. */
int  ui_start(const struct ui_info *info);	/* -1: stdout is not a tty */
void ui_stop(void);
void ui_push(int slot, const struct cpu_sample *s, const struct metrics *m,
	     double dt_s, double worst_frame_us);
void ui_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_draw(int nslots);
/* Sleep up to @ms, serving the keyboard; returns 1 when the user quits. */
int  ui_wait(int ms, int nslots);
bool ui_paused(void);
/* --show: chart keys (frame,cpu,pf,l1i,l1d,l2,itlb,dtlb) or "all" */
int  ui_select(const char *spec, char *err, size_t errlen);

#endif

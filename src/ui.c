// SPDX-License-Identifier: MIT
/*
 * ui.c - htop-like full-screen view: one panel per monitored core, each
 * metric drawn as a scrolling bar chart of its recent history.
 *
 * Every chart cell is coloured by the health state of the sample it
 * shows (green OK, yellow WARN, red FAIL), so a degradation is visible
 * as a colour change moving across the screen, and the metric that
 * changed with it is right next to it.
 *
 * Plain ANSI escape sequences and UTF-8 block characters; no curses.
 * The whole frame is built in memory and written with one write(2), so
 * the display does not flicker.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "ui.h"

#define HIST		1024		/* samples kept per metric */
#define NEVENTS		64
#define LABEL_W		20		/* "LLC miss/K  12.34 " */

enum {
	G_UTIL = 0,
	G_IPC,
	G_LLC,
	G_TLB,
	G_IRQ,
	G_DEV,
	G_FRAME,
	G_NR,
};

struct graph_desc {
	const char	*name;
	bool		log;		/* log10 scale */
	double		lo, hi;		/* fixed range: comparable over time */
	const char	*fmt;
};

/*
 * Fixed ranges, so a bar of a given height always means the same value:
 * an auto-scaled chart would magnify noise on a quiet core.
 */
static const struct graph_desc gd[G_NR] = {
	/* /proc/stat: Linux's view, blind to out-of-band (EVL) execution */
	[G_UTIL]  = { "util% Linux", false, 0,     100,    "%6.1f" },
	[G_IPC]   = { "IPC",         false, 0,     5,      "%6.2f" },
	[G_LLC]   = { "LLC miss/K",  true,  0.001, 100,    "%6.2f" },
	[G_TLB]   = { "dTLB miss/K", true,  0.001, 100,    "%6.2f" },
	[G_IRQ]   = { "IRQ/s all",   true,  1,     100000, "%6.0f" },
	[G_DEV]   = { "IRQ/s device", true, 1,     10000,  "%6.0f" },
	[G_FRAME] = { "worst frame us", false, 0,  1000,   "%6.1f" },
};

struct slot_hist {
	double		v[G_NR][HIST];
	unsigned char	st[HIST];
	unsigned	n;		/* samples pushed so far */
	int		cpu;
	enum cpu_health	last_state;
	char		why[160];
	struct cpu_sample s;
	struct metrics	m;
	double		dt;		/* measured length of the last interval */
};

static struct slot_hist hist[MAX_CPUS];
static char events[NEVENTS][200];
static unsigned nevents;
static struct termios saved_tio;
static bool active, paused, tio_saved;
static struct ui_info info;
static int64_t t_start;
static volatile sig_atomic_t resized;

static int64_t mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void on_winch(int sig)
{
	(void)sig;
	resized = 1;
}

/* ---------------------------------------------------------------- output */

static char *frame;
static size_t flen, fcap;

static void put(const char *s, size_t n)
{
	if (flen + n > fcap) {
		size_t nc = (fcap ? fcap * 2 : 65536) + n;
		char *p = realloc(frame, nc);

		if (!p)
			return;
		frame = p;
		fcap = nc;
	}
	memcpy(frame + flen, s, n);
	flen += n;
}

static void puts_(const char *s)
{
	put(s, strlen(s));
}

static void printf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void printf_(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0)
		put(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* ASCII text clipped to @w columns, padded with blanks. */
static void text_w(const char *s, int w)
{
	int n = (int)strlen(s);

	if (n > w)
		n = w;
	if (n > 0)
		put(s, n);
	for (; n < w; n++)
		put(" ", 1);
}

static void eol(void)
{
	puts_("\033[K\r\n");	/* clear the rest of the line */
}

static void flush_frame(void)
{
	size_t off = 0;

	while (off < flen) {
		ssize_t n = write(STDOUT_FILENO, frame + off, flen - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}
	flen = 0;
}

static const char *state_color(int st)
{
	switch (st) {
	case CPU_OK:		return "\033[32m";	/* green */
	case CPU_DEGRADED:	return "\033[33m";	/* yellow */
	case CPU_CRITICAL:	return "\033[31m";	/* red */
	}
	return "\033[0m";
}

static const char *state_name(int st)
{
	switch (st) {
	case CPU_OK:		return " OK ";
	case CPU_DEGRADED:	return "WARN";
	case CPU_CRITICAL:	return "FAIL";
	}
	return " ?? ";
}

/* -------------------------------------------------------------- terminal */

static void term_size(int *rows, int *cols)
{
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
}

int ui_start(const struct ui_info *in)
{
	struct termios tio;
	struct sigaction sa;

	if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO))
		return -1;
	info = *in;
	t_start = mono_ns();

	if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
		tio_saved = true;
		tio = saved_tio;
		tio.c_lflag &= ~(ICANON | ECHO);	/* keys without Enter, no echo */
		tio.c_cc[VMIN] = 0;
		tio.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);

	/* alternate screen, hidden cursor */
	puts_("\033[?1049h\033[?25l\033[2J");
	flush_frame();
	active = true;
	atexit(ui_stop);
	return 0;
}

void ui_stop(void)
{
	if (!active)
		return;
	active = false;
	puts_("\033[0m\033[?25h\033[?1049l");
	flush_frame();
	if (tio_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
}

/* ---------------------------------------------------------------- model */

void ui_event(const char *fmt, ...)
{
	char msg[160];
	va_list ap;
	double t = (mono_ns() - t_start) / 1e9;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	snprintf(events[nevents % NEVENTS], sizeof(events[0]), "%8.1fs  %s", t, msg);
	nevents++;
}

void ui_push(int slot, const struct cpu_sample *s, const struct metrics *m,
	     double dt, double worst_frame_us)
{
	struct slot_hist *h = &hist[slot];
	unsigned i = h->n % HIST;

	if (h->n == 0)
		h->last_state = CPU_OK;
	h->cpu = s->cpu;
	h->v[G_UTIL][i] = m->util * 100.0;
	h->v[G_IPC][i] = m->ipc;
	h->v[G_LLC][i] = m->llc_mpki;
	h->v[G_TLB][i] = m->tlb_mpki;
	h->v[G_IRQ][i] = (double)s->irq_rate;
	h->v[G_DEV][i] = dt > 0 ? s->irq_device / dt : NAN;
	h->v[G_FRAME][i] = worst_frame_us > 0 ? worst_frame_us : NAN;
	h->st[i] = (unsigned char)m->health;
	h->s = *s;
	h->m = *m;
	h->dt = dt;
	h->n++;

	/* Log transitions, and a changed reason while still degraded. */
	if (m->health != h->last_state) {
		if (m->health == CPU_OK)
			ui_event("cpu%d back to OK", s->cpu);
		else
			ui_event("cpu%d %s: %s", s->cpu, state_name(m->health), m->why);
	} else if (m->health != CPU_OK && strncmp(h->why, m->why, 24) != 0) {
		ui_event("cpu%d %s: %s", s->cpu, state_name(m->health), m->why);
	}
	h->last_state = m->health;
	snprintf(h->why, sizeof(h->why), "%s", m->why);
}

/* ---------------------------------------------------------------- charts */

static const char *const blocks[9] = {
	" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
};

/* Value -> 0..8 eighths of a cell, on the metric's fixed scale. */
static int level(const struct graph_desc *g, double v)
{
	double f;

	if (!isfinite(v))
		return -1;
	if (g->log) {
		if (v <= g->lo)
			return v > 0 ? 1 : 0;
		f = (log10(v) - log10(g->lo)) / (log10(g->hi) - log10(g->lo));
	} else {
		f = (v - g->lo) / (g->hi - g->lo);
	}
	if (f < 0)
		f = 0;
	if (f > 1)
		f = 1;
	return 1 + (int)lround(f * 7);	/* non-zero values stay visible */
}

static void chart_row(const struct slot_hist *h, int g, int width)
{
	char lab[64], val[24];
	unsigned n = h->n < HIST ? h->n : HIST;
	unsigned shown = n < (unsigned)width ? n : (unsigned)width;
	double cur = h->n ? h->v[g][(h->n - 1) % HIST] : NAN;
	int last_st = -1, k;

	if (isfinite(cur))
		snprintf(val, sizeof(val), gd[g].fmt, cur);
	else
		snprintf(val, sizeof(val), "%6s", "-");
	snprintf(lab, sizeof(lab), " %-13s%s ", gd[g].name, val);
	puts_("\033[0m");
	text_w(lab, LABEL_W + 2);

	/* left padding for a history shorter than the chart */
	for (k = 0; k < width - (int)shown; k++)
		put(" ", 1);
	for (unsigned j = h->n - shown; j < h->n; j++) {
		unsigned i = j % HIST;
		int lv = level(&gd[g], h->v[g][i]);

		if (h->st[i] != last_st) {
			puts_(state_color(h->st[i]));
			last_st = h->st[i];
		}
		puts_(lv < 0 ? "·" : blocks[lv]);
	}
	puts_("\033[0m");
	eol();
}

static void cpu_header(const struct slot_hist *h, int cols)
{
	const struct cpu_sample *s = &h->s;
	const struct metrics *m = &h->m;
	char line[256], t[16];
	double r = h->dt > 0 ? 1.0 / h->dt : 0;	/* counts -> per second */

	if (isfinite(m->temp_c))
		snprintf(t, sizeof(t), "%.0fC", m->temp_c);
	else
		snprintf(t, sizeof(t), "-");
	printf_("\033[1m CPU %-3d\033[0m %s\033[7m %s \033[0m", h->cpu,
		state_color(m->health), state_name(m->health));
	snprintf(line, sizeof(line),
		 "  %4.0f MHz  %s   IRQ/s %llu = timer %.0f + thermal %.0f + IPI %.0f + device %.0f",
		 m->freq_mhz, t, (unsigned long long)s->irq_rate,
		 s->irq_timer * r, s->irq_thermal * r,
		 s->irq_ipi * r, s->irq_device * r);
	text_w(line, cols - 16 > 0 ? cols - 16 : 0);
	eol();
	if (m->why[0]) {
		puts_(state_color(m->health));
		snprintf(line, sizeof(line), "   %s", m->why);
		text_w(line, cols - 1);
		puts_("\033[0m");
	}
	eol();
}

void ui_draw(int nslots)
{
	int rows, cols, width, per_slot, ncharts, used, i, g;
	int order[G_NR], norder = 0;
	char line[256];
	time_t wall = time(NULL);
	struct tm tm;

	if (!active)
		return;
	term_size(&rows, &cols);
	width = cols - LABEL_W - 3;
	if (width < 10)
		width = 10;

	/* Metrics in order of importance, frame time only with telemetry. */
	if (info.telemetry)
		order[norder++] = G_FRAME;
	order[norder++] = G_IPC;
	order[norder++] = G_LLC;
	order[norder++] = G_TLB;
	order[norder++] = G_DEV;
	order[norder++] = G_IRQ;
	order[norder++] = G_UTIL;

	/* 2 title lines, 2 header lines per core, keep >= 4 lines of events */
	per_slot = nslots ? (rows - 2 - 5) / nslots : 0;
	ncharts = per_slot - 3;		/* header, reason, blank */
	if (ncharts > norder)
		ncharts = norder;
	if (ncharts < 0)
		ncharts = 0;

	flen = 0;
	puts_("\033[H");
	localtime_r(&wall, &tm);
	snprintf(line, sizeof(line),
		 " rt-health  %02d:%02d:%02d  %s  every %d ms%s%s%s",
		 tm.tm_hour, tm.tm_min, tm.tm_sec, info.mode, info.interval_ms,
		 info.csv ? "  csv " : "", info.csv ? info.csv : "",
		 paused ? "  [PAUSED]" : "");
	puts_("\033[7m");
	text_w(line, cols - 30 > 20 ? cols - 30 : 20);
	text_w(" q quit  p pause  c clear ", cols - (cols - 30 > 20 ? cols - 30 : 20));
	puts_("\033[0m");
	eol();
	puts_("\033[2m");
	text_w(" newest sample at right; colour = state: green OK, yellow WARN, red FAIL;"
	       " MPKI and IRQ charts are log scale", cols - 1);
	puts_("\033[0m");
	eol();
	used = 2;

	for (i = 0; i < nslots; i++) {
		cpu_header(&hist[i], cols);
		used += 2;
		for (g = 0; g < ncharts; g++) {
			chart_row(&hist[i], order[g], width);
			used++;
		}
		eol();
		used++;
	}

	/* event log, newest first */
	puts_("\033[1m Events\033[0m");
	eol();
	used++;
	for (unsigned k = 0; k < NEVENTS && k < nevents && used < rows - 1; k++) {
		const char *e = events[(nevents - 1 - k) % NEVENTS];

		if (strstr(e, "FAIL"))
			puts_(state_color(CPU_CRITICAL));
		else if (strstr(e, "WARN"))
			puts_(state_color(CPU_DEGRADED));
		else
			puts_(state_color(CPU_OK));
		text_w(e, cols - 1);
		puts_("\033[0m");
		eol();
		used++;
	}
	if (nevents == 0 && used < rows - 1) {
		puts_("\033[2m");
		text_w("   no state changes yet", cols - 1);
		puts_("\033[0m");
		eol();
	}
	puts_("\033[J");	/* clear anything below */
	flush_frame();
}

/* ------------------------------------------------------------- keyboard */

int ui_wait(int ms, int nslots)
{
	int64_t end = mono_ns() + (int64_t)ms * 1000000;
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	char ch;

	for (;;) {
		int64_t left = (end - mono_ns()) / 1000000;

		if (left <= 0)
			return 0;
		if (poll(&pfd, 1, (int)left) <= 0) {
			if (resized) {		/* EINTR from SIGWINCH */
				resized = 0;
				ui_draw(nslots);
			}
			continue;
		}
		if (read(STDIN_FILENO, &ch, 1) != 1)
			continue;
		switch (ch) {
		case 'q':
		case 'Q':
			return 1;
		case 'p':
		case 'P':
			paused = !paused;
			ui_draw(nslots);
			break;
		case 'c':
		case 'C':
			nevents = 0;
			ui_draw(nslots);
			break;
		default:
			break;
		}
	}
}

/* Paused: sampling goes on (baselines keep learning), drawing stops. */
bool ui_paused(void)
{
	return paused;
}

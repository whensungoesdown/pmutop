// SPDX-License-Identifier: MIT
/*
 * ui.c - htop-like full-screen view.
 *
 * Monitored cores sit side by side, one column each.  Within a column the
 * metrics are stacked, and each metric is a scrolling chart several rows
 * tall, one bar per sample, newest at the right:
 *
 *   ▲ metrics where higher is better (IPC) grow up from the bottom;
 *   ▼ metrics where lower is better (cache and TLB misses, interrupts,
 *     frame time) hang down from the top;
 *   CPU busy grows up, as a plain level.
 *
 * So in every chart "more ink" in the direction of the arrow means the
 * metric moved that way, and a bad sample of a ▼ chart reaches further
 * down.  Each bar is coloured by the health state of its sample (green
 * OK, yellow WARN, red FAIL).
 *
 * The frame is composed in a cell grid, then turned into ANSI escape
 * sequences with an attribute change only where needed, and written with
 * one write(2): no flicker, no curses.
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
#define AXIS_W		7		/* scale labels left of each chart */
#define MAX_CHART_H	8		/* rows per chart at most */

enum {
	G_BUSY = 0,
	G_IPC,
	G_PF,
	G_L1I,
	G_L1D,
	G_L2,
	G_ITLB,
	G_TLB,
	G_FRAME,
	G_NR,
};

struct graph_desc {
	const char	*name;
	bool		down;		/* lower is better: bars hang from the top */
	bool		neutral;	/* neither better nor worse (CPU busy) */
	bool		log;		/* log10 scale */
	double		lo, hi;		/* fixed range: comparable over time */
	const char	*fmt;
	/*
	 * Colour bands of the value itself: past @warn yellow, past @bad
	 * red.  For a higher-is-better metric (IPC) "past" means below.
	 */
	double		warn, bad;
	/*
	 * Range ladder for automatic scaling (0 = fixed range @hi).  Each
	 * sample is drawn on the smallest rung covering the recent samples,
	 * and its bar is coloured by that rung: the same bar height on a
	 * higher rung means a larger value.
	 */
	double		rung[4];
};

/*
 * Fixed ranges, so a bar of a given height always means the same value;
 * an auto-scaled chart would magnify noise on a quiet core.
 */
/*
 * Every rate is per second on a linear scale.  Ranges and bands come from
 * the ThinkBook measurements; a value past the range is drawn as a full
 * bar with '+' at its end.
 */
static const struct graph_desc gd[G_NR] = {
	/*                  name               down   neutr  log    lo  hi       fmt     warn   bad */
	[G_BUSY]  = { "CPU usage %",      false, true,  false, 0,  100,     "%.1f", 70,    90 },
	/*
	 * Instructions per cycle, higher is better: grows up, fixed range
	 * 0-5.  On every core green, yellow below 0.5, red below 0.2;
	 * pingpong cores run at 0.7-0.9, the 512 MiB random chase drops to
	 * 0.03.  Meaningless on an idle core (a few hundred instructions).
	 */
	[G_IPC]   = { "IPC",             false, false, false, 0,  5,       "%.2f", 0.5,   0.2 },
	/*
	 * A page fault in an EVL thread demotes it to Linux, so on an RT
	 * core any fault is suspect; a running pingpong core takes none.
	 */
	[G_PF]    = { "page fault/s",    true,  false, false, 0,  200,     NULL,   0.5,   100, { 200, 2000, 20000, 200000 } },
	/*
	 * L2 is private to each P-core (3 MiB), so its misses come from the
	 * core's own working set, unlike the shared L3.  evl-pingpong: about
	 * 30k/s; a cache-hostile task: millions per second.
	 */
	[G_L2]    = { "L2 miss/s",       true,  false, false, 0,  100000,  NULL,   50000, 100000, { 10000, 100000, 1000000, 20000000 } },
	/* evl-pingpong: tens per second; a TLB-hostile task: millions */
	/*
	 * L1 and iTLB, measured on the ThinkBook (per second):
	 *                 idle core  pingpong core  + memory-heavy neighbours
	 *   L1I miss        200      3.6-5.5k        45k
	 *   iTLB miss         8          2.6k
	 * L1I counts retired instructions fetched after an L1I miss
	 * (FRONTEND_RETIRED.*); L1D retired loads missing L1D: see README.
	 */
	[G_L1I]   = { "L1I miss/s",      true,  false, false, 0,  10000,   NULL,   20000, 100000, { 10000, 100000, 1000000, 10000000 } },
	[G_L1D]   = { "L1D miss/s",      true,  false, false, 0,  50000,   NULL,   200000, 1000000, { 50000, 500000, 5000000, 50000000 } },
	[G_ITLB]  = { "iTLB miss/s",     true,  false, false, 0,  5000,    NULL,   10000, 50000,  { 5000, 50000, 500000, 5000000 } },
	[G_TLB]   = { "dTLB miss/s",     true,  false, false, 0,  10000,   NULL,   1000,  5000, { 1000, 10000, 1000000, 20000000 } },
	/* against a 1 ms period */
	[G_FRAME] = { "worst frame us",  true,  false, false, 0,  1000,    "%.1f", 500,   900 },
};


struct slot_hist {
	double		v[G_NR][HIST];
	unsigned char	st[HIST];
	unsigned char	rung[G_NR][HIST];	/* range rung each sample was drawn on */
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

/* ------------------------------------------------------------ cell grid */

enum { C_DEF = 0, C_GREEN, C_YELLOW, C_RED, C_DIM, C_CYAN, C_BLUE, C_MAGENTA };

/* Green / yellow / red for a value, from its metric's bands. */
static unsigned char level_col(const struct graph_desc *d, double v)
{
	bool up = !d->down && !d->neutral;	/* higher is better */

	if (!isfinite(v))
		return C_DIM;
	if (up ? v < d->bad : v > d->bad)
		return C_RED;
	if (up ? v < d->warn : v > d->warn)
		return C_YELLOW;
	return C_GREEN;
}

#define A_BOLD	0x10
#define A_REV	0x20

struct cell {
	char		ch[4];		/* one UTF-8 character, not terminated */
	unsigned char	len;
	unsigned char	attr;		/* colour | A_BOLD | A_REV */
};

static struct cell *grid;
static int grows, gcols;

static void grid_reset(int rows, int cols)
{
	if (rows * cols > grows * gcols || !grid) {
		free(grid);
		grid = calloc((size_t)rows * cols, sizeof(*grid));
	}
	grows = rows;
	gcols = cols;
	for (int i = 0; i < rows * cols; i++) {
		grid[i].ch[0] = ' ';
		grid[i].len = 1;
		grid[i].attr = C_DEF;
	}
}

static void put_cell(int r, int c, const char *utf8, unsigned char attr)
{
	struct cell *x;
	size_t n = strlen(utf8);

	if (!grid || r < 0 || c < 0 || r >= grows || c >= gcols || n > 4)
		return;
	x = &grid[r * gcols + c];
	memcpy(x->ch, utf8, n);
	x->len = (unsigned char)n;
	x->attr = attr;
}

/* UTF-8 text at (r, c), one cell per character, clipped to @w columns. */
static void put_text(int r, int c, int w, const char *s, unsigned char attr)
{
	char one[5];
	int col = 0;

	while (*s && col < w) {
		int n = 1;

		if ((unsigned char)*s >= 0xf0)
			n = 4;
		else if ((unsigned char)*s >= 0xe0)
			n = 3;
		else if ((unsigned char)*s >= 0xc0)
			n = 2;
		memcpy(one, s, n);
		one[n] = 0;
		put_cell(r, c + col, one, attr);
		s += n;
		col++;
	}
}

static void put_textf(int r, int c, int w, unsigned char attr, const char *fmt, ...)
	__attribute__((format(printf, 5, 6)));
static void put_textf(int r, int c, int w, unsigned char attr, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	put_text(r, c, w, buf, attr);
}

/* -------------------------------------------------------------- output */

static char *out;
static size_t olen, ocap;

static void emit(const char *s, size_t n)
{
	if (olen + n > ocap) {
		size_t nc = (ocap ? ocap * 2 : 1 << 17) + n;
		char *p = realloc(out, nc);

		if (!p)
			return;
		out = p;
		ocap = nc;
	}
	memcpy(out + olen, s, n);
	olen += n;
}

static void emits(const char *s)
{
	emit(s, strlen(s));
}

static void flush_out(void)
{
	size_t off = 0;

	while (off < olen) {
		ssize_t n = write(STDOUT_FILENO, out + off, olen - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}
	olen = 0;
}

static void emit_attr(unsigned char a)
{
	static const char *const fg[] = {
		[C_DEF] = "", [C_GREEN] = ";32", [C_YELLOW] = ";33",
		[C_RED] = ";31", [C_DIM] = ";2", [C_CYAN] = ";96",
		[C_BLUE] = ";94", [C_MAGENTA] = ";95",
	};
	char buf[32];

	snprintf(buf, sizeof(buf), "\033[0%s%s%sm", fg[a & 0x0f],
		 a & A_BOLD ? ";1" : "", a & A_REV ? ";7" : "");
	emits(buf);
}

static void grid_flush(void)
{
	int last = -1;

	emits("\033[H");
	for (int r = 0; r < grows; r++) {
		if (r)
			emits("\r\n");
		for (int c = 0; c < gcols; c++) {
			const struct cell *x = &grid[r * gcols + c];

			if (x->attr != last) {
				emit_attr(x->attr);
				last = x->attr;
			}
			emit(x->ch, x->len);
		}
	}
	emits("\033[0m");
	flush_out();
}

/* ------------------------------------------------------------ terminal */

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

	/* alternate screen, hidden cursor, no line wrap */
	emits("\033[?1049h\033[?25l\033[?7l\033[2J");
	flush_out();
	active = true;
	atexit(ui_stop);
	return 0;
}

void ui_stop(void)
{
	if (!active)
		return;
	active = false;
	emits("\033[0m\033[?7h\033[?25h\033[?1049l");
	flush_out();
	if (tio_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
}

/* --------------------------------------------------------------- model */

static const char *state_name(int st)
{
	switch (st) {
	case CPU_OK:		return "OK";
	case CPU_DEGRADED:	return "WARN";
	case CPU_CRITICAL:	return "FAIL";
	}
	return "??";
}

static unsigned char state_col(int st)
{
	switch (st) {
	case CPU_OK:		return C_GREEN;
	case CPU_DEGRADED:	return C_YELLOW;
	case CPU_CRITICAL:	return C_RED;
	}
	return C_DEF;
}

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

#define RUNG_WINDOW	10	/* samples a range must cover before stepping down */

static const unsigned char rung_col[4] = { C_GREEN, C_CYAN, C_BLUE, C_MAGENTA };

/*
 * Smallest rung covering the last RUNG_WINDOW samples (this one included):
 * the range steps up at once and back down only after a quiet stretch,
 * so it does not flap.
 */
static unsigned char pick_rung(const struct slot_hist *h, int g, unsigned i)
{
	const struct graph_desc *d = &gd[g];
	double mx = 0;
	unsigned k, r;

	if (d->rung[0] <= 0)
		return 0;
	for (k = 0; k < RUNG_WINDOW && k <= h->n; k++) {
		double v = h->v[g][(i + HIST - k) % HIST];

		if (isfinite(v) && v > mx)
			mx = v;
	}
	for (r = 0; r < 3 && mx > d->rung[r]; r++)
		;
	return (unsigned char)r;
}

/* Length of the reason up to its first number: "LLC MPKI ", "device IRQ ..." */
static size_t reason_kind(const char *why)
{
	size_t n = 0;

	while (why[n] && !(why[n] >= '0' && why[n] <= '9'))
		n++;
	return n;
}

void ui_push(int slot, const struct cpu_sample *s, const struct metrics *m,
	     double dt, double worst_frame_us)
{
	struct slot_hist *h = &hist[slot];
	unsigned i = h->n % HIST;

	if (h->n == 0)
		h->last_state = CPU_OK;
	h->cpu = s->cpu;
	h->v[G_BUSY][i] = (isfinite(m->busy) ? m->busy : m->util) * 100.0;
	h->v[G_IPC][i] = m->ipc;
	h->v[G_PF][i] = s->ev_ok[EV_PAGE_FAULT] && dt > 0 ?
			s->ev[EV_PAGE_FAULT] / dt : NAN;
	h->v[G_L2][i] = s->ev_ok[EV_L2_MISSES] && dt > 0 ?
			s->ev[EV_L2_MISSES] / dt : NAN;
	h->v[G_TLB][i] = s->ev_ok[EV_DTLB_MISSES] && dt > 0 ?
			 s->ev[EV_DTLB_MISSES] / dt : NAN;
	h->v[G_L1I][i] = s->ev_ok[EV_L1I_MISSES] && dt > 0 ?
			 s->ev[EV_L1I_MISSES] / dt : NAN;
	h->v[G_L1D][i] = s->ev_ok[EV_L1D_MISSES] && dt > 0 ?
			 s->ev[EV_L1D_MISSES] / dt : NAN;
	h->v[G_ITLB][i] = s->ev_ok[EV_ITLB_MISSES] && dt > 0 ?
			  s->ev[EV_ITLB_MISSES] / dt : NAN;
	h->v[G_FRAME][i] = worst_frame_us > 0 ? worst_frame_us : NAN;
	h->st[i] = (unsigned char)m->health;
	for (int g = 0; g < G_NR; g++)
		h->rung[g][i] = pick_rung(h, g, i);
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
	} else if (m->health != CPU_OK &&
		   (reason_kind(h->why) != reason_kind(m->why) ||
		    strncmp(h->why, m->why, reason_kind(m->why)) != 0)) {
		/* a different kind of problem, not just new numbers */
		ui_event("cpu%d %s: %s", s->cpu, state_name(m->health), m->why);
	}
	h->last_state = m->health;
	snprintf(h->why, sizeof(h->why), "%s", m->why);
}

/* -------------------------------------------------------------- charts */

static const char *const blocks[9] = {
	" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█",
};

/* Value -> 0..1 on the metric's fixed scale; -1 when unknown. */
static double frac(const struct graph_desc *g, double v)
{
	double f;

	if (!isfinite(v))
		return -1;
	if (g->log) {
		if (v <= 0)
			return 0;
		f = (log10(v) - log10(g->lo)) / (log10(g->hi) - log10(g->lo));
	} else {
		f = (v - g->lo) / (g->hi - g->lo);
	}
	return f < 0 ? 0 : f > 1 ? 1 : f;
}

static void human(char *b, size_t n, double v)
{
	if (v >= 1e6)
		snprintf(b, n, "%.2fM", v / 1e6);
	else if (v >= 1e4)
		snprintf(b, n, "%.1fk", v / 1e3);
	else
		snprintf(b, n, "%.0f", v);
}

static void axis_label(char *b, size_t n, double v)
{
	if (v >= 1e6)
		snprintf(b, n, "%gM", v / 1e6);
	else if (v >= 1000)
		snprintf(b, n, "%gk", v / 1000);
	else
		snprintf(b, n, "%g", v);
}

/*
 * One chart: a title row, then @h rows of bars, @w columns wide,
 * at (r, c).
 */
static void draw_chart(const struct slot_hist *h, int g, int r, int c, int w, int ht)
{
	const struct graph_desc *d = &gd[g];
	bool laddered = d->rung[0] > 0;
	/* red alarm only where a high value is a fault: isolated cores */
	/* IPC and CPU usage use their bands on every core */
	bool alarm = h->m.rt_core || g == G_BUSY || g == G_IPC;
	unsigned n = h->n < HIST ? h->n : HIST;
	int pw = w - AXIS_W;			/* plot width */
	unsigned shown = n < (unsigned)pw ? n : (unsigned)pw;
	unsigned last = h->n ? (h->n - 1) % HIST : 0;
	double cur = h->n ? h->v[g][last] : NAN;
	double hi_now = laddered && h->n ? d->rung[h->rung[g][last]] : d->hi;
	unsigned char col_now = laddered && h->n ? rung_col[h->rung[g][last]] : C_DIM;
	char val[32], top[16], bot[16];
	int k;

	/* title: arrow, name, current value, direction hint */
	if (!isfinite(cur))
		snprintf(val, sizeof(val), "-");
	else if (d->fmt)
		snprintf(val, sizeof(val), d->fmt, cur);
	else
		human(val, sizeof(val), cur);
	put_cell(r, c, d->neutral ? "■" : d->down ? "▼" : "▲",
		 d->neutral ? C_CYAN : d->down ? C_YELLOW : C_GREEN);
	put_textf(r, c + 2, w - 2, A_BOLD, "%s", d->name);
	put_textf(r, c + 2 + (int)strlen(d->name) + 1, 12,
		  A_BOLD | (alarm ? level_col(d, cur) : C_DEF), "%s", val);

	/* range legend: every rung in its colour, the current one bracketed */
	if (laddered && w > 50) {
		char lab[16], item[24];
		int x = c + w - 1;

		for (int q = 3; q >= 0; q--) {
			axis_label(lab, sizeof(lab), d->rung[q]);
			snprintf(item, sizeof(item), q == h->rung[g][last] && h->n ? "[%s]" : " %s ", lab);
			x -= (int)strlen(item);
			put_text(r, x, (int)strlen(item), item,
				 rung_col[q] | (q == h->rung[g][last] && h->n ? A_BOLD : 0));
		}
		put_text(r, x - 7, 6, "range:", C_DIM);
	} else if (!d->neutral && w > 46) {
		const char *hint = d->down ? "lower = better" : "higher = better";
		int nh = (int)strlen(hint);

		put_text(r, c + w - nh - 1, nh, hint, C_DIM);
	}

	/* scale labels: the baseline (where bars start) and the current far end */
	axis_label(top, sizeof(top), d->down ? d->lo : hi_now);
	axis_label(bot, sizeof(bot), d->down ? hi_now : d->lo);
	put_textf(r + 1, c, AXIS_W - 1, d->down ? C_DIM : col_now, "%*s", AXIS_W - 1, top);
	put_textf(r + ht, c, AXIS_W - 1, d->down ? col_now : C_DIM, "%*s", AXIS_W - 1, bot);
	for (k = 1; k <= ht; k++)
		put_cell(r + k, c + AXIS_W - 1, "│", C_DIM);

	for (unsigned j = h->n - shown, x = c + AXIS_W + (pw - shown); j < h->n; j++, x++) {
		unsigned i = j % HIST;
		double v = h->v[g][i];
		struct graph_desc dd = *d;
		unsigned char col;
		double f;
		int total;

		/* each bar on the range it was taken on, in that range's colour */
		if (laddered) {
			dd.hi = d->rung[h->rung[g][i]];
			col = rung_col[h->rung[g][i]];
		} else {
			col = alarm ? level_col(d, v) : C_CYAN;
		}
		if (alarm && isfinite(v) && level_col(d, v) == C_RED)
			col = C_RED;
		f = frac(&dd, v);
		if (f < 0) {			/* no data for this sample */
			put_cell(d->down ? r + 1 : r + ht, x, "·", C_DIM);
			continue;
		}
		total = (int)lround(f * ht * 8);
		if (v > dd.hi) {
			/* off the largest range: full bar, '+' at its far end */
			for (k = 0; k < ht; k++)
				put_cell(d->down ? r + 1 + k : r + ht - k, x,
					 k == ht - 1 ? "+" : blocks[8],
					 k == ht - 1 ? (col | A_REV | A_BOLD) : col);
			continue;
		}
		if (total == 0 && v > d->lo)
			total = 1;		/* non-zero stays visible */

		for (k = 0; k < ht; k++) {
			int fill = total - 8 * k;

			if (fill <= 0)
				break;
			if (fill > 8)
				fill = 8;
			if (!d->down) {
				/* grow up: row k counted from the bottom */
				put_cell(r + ht - k, x, blocks[fill], col);
			} else if (fill == 8) {
				/* hang down: row k counted from the top */
				put_cell(r + 1 + k, x, blocks[8], col);
			} else {
				/*
				 * A cell filled from the top: draw the lower
				 * block of the empty part in reverse video, so
				 * the coloured background shows above it.
				 */
				put_cell(r + 1 + k, x, blocks[8 - fill], col | A_REV);
			}
		}
	}
}

/* Charts to show, in order; set by ui_select() (--show). */
static const struct { const char *key; int g; } chart_keys[] = {
	{ "frame", G_FRAME }, { "cpu", G_BUSY }, { "ipc", G_IPC }, { "pf", G_PF },
	{ "l1i", G_L1I }, { "l1d", G_L1D }, { "l2", G_L2 },
	{ "itlb", G_ITLB }, { "dtlb", G_TLB },
};
#define NKEYS	((int)(sizeof(chart_keys) / sizeof(chart_keys[0])))
static int sel[G_NR], nsel;

/*
 * @spec: comma-separated chart keys in display order, "all", or NULL for
 * all of them.  Returns -1 with a message on an unknown key.
 */
int ui_select(const char *spec, char *err, size_t errlen)
{
	char copy[256], *tok, *save = NULL;

	nsel = 0;
	if (!spec || strcmp(spec, "all") == 0) {
		for (int k = 0; k < NKEYS; k++)
			sel[nsel++] = chart_keys[k].g;
		return 0;
	}
	snprintf(copy, sizeof(copy), "%s", spec);
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		int k, dup = 0;

		for (k = 0; k < NKEYS && strcmp(tok, chart_keys[k].key); k++)
			;
		if (k == NKEYS) {
			int n = snprintf(err, errlen, "unknown chart '%s'; choose from all", tok);

			for (k = 0; k < NKEYS && n > 0 && (size_t)n < errlen; k++)
				n += snprintf(err + n, errlen - n, ",%s", chart_keys[k].key);
			return -1;
		}
		for (int j = 0; j < nsel; j++)
			dup |= sel[j] == chart_keys[k].g;
		if (!dup && nsel < G_NR)
			sel[nsel++] = chart_keys[k].g;
	}
	if (!nsel) {
		snprintf(err, errlen, "no chart selected");
		return -1;
	}
	return 0;
}

static void draw_panel(const struct slot_hist *h, int r0, int c0, int w, int ph)
{
	const struct cpu_sample *s = &h->s;
	const struct metrics *m = &h->m;
	double rate = h->dt > 0 ? 1.0 / h->dt : 0;
	int order[G_NR], norder = 0, ncharts, ht, r, i;
	char t[16], b[16];

	for (i = 0; i < nsel; i++)
		if (sel[i] != G_FRAME || info.telemetry)
			order[norder++] = sel[i];

	/*
	 * header: 3 rows.  A core isolated from Linux (isolcpus) gets an
	 * "ISOLATED" tag in reverse magenta and heavy magenta dividers, so it
	 * stands out from the ordinary cores.
	 */
	put_textf(r0, c0, 8, A_BOLD | (h->m.rt_core ? C_MAGENTA : 0), "CPU %d", h->cpu);
	put_textf(r0, c0 + 8, 6, state_col(m->health) | A_REV | A_BOLD, " %-4s ",
		  state_name(m->health));
	int tagw = 0;

	if (h->m.rt_core) {
		const char *tag = " ISOLATED ";

		tagw = (int)strlen(tag) + 1;
		put_text(r0, c0 + w - tagw + 1, tagw - 1, tag, C_MAGENTA | A_REV | A_BOLD);
		w -= tagw;		/* keep the status line clear of it */
	}
	if (isfinite(m->temp_c))
		snprintf(t, sizeof(t), "%.0fC", m->temp_c);
	else
		snprintf(t, sizeof(t), "-");
	/* hardware usage (includes EVL); Linux's figure only without a PMU */
	snprintf(b, sizeof(b), "%.1f%%", (isfinite(m->busy) ? m->busy : m->util) * 100);
	put_textf(r0, c0 + 15, w - 15, C_DEF, "CPU %s  %.0f MHz  %s",
		  b, m->freq_mhz, t);
	w += tagw;		/* full width again below the tag */
	put_textf(r0 + 1, c0, w, C_DIM,
		  "IRQ/s: timer %.0f  thermal %.0f  IPI %.0f  device %.0f",
		  s->irq_timer * rate, s->irq_thermal * rate,
		  s->irq_ipi * rate, s->irq_device * rate);
	if (m->why[0])
		put_text(r0 + 2, c0, w, m->why, state_col(m->health));

	/* charts share what is left; drop the least important if cramped */
	ncharts = norder;
	for (;;) {
		ht = ncharts ? (ph - 3) / ncharts - 1 : 0;
		if (ht >= 2 || ncharts <= 1)
			break;
		ncharts--;
	}
	if (ht > MAX_CHART_H)
		ht = MAX_CHART_H;
	if (ht < 1)
		return;
	r = r0 + 3;
	for (i = 0; i < ncharts; i++) {
		draw_chart(h, order[i], r, c0, w, ht);
		r += ht + 1;
	}
}

/* Charts a panel will draw: the selection, less frame time without telemetry. */
static int charts_shown(void)
{
	int n = 0;

	for (int i = 0; i < nsel; i++)
		n += sel[i] != G_FRAME || info.telemetry;
	return n;
}

#define MIN_PANEL_W	44	/* header and range legend still readable */
#define PANEL_GAP	3
#define MIN_CHART_ROWS	3	/* title + 2 rows of bars */

static int page;		/* current page when not all panels fit */

void ui_draw(int nslots)
{
	int rows, cols, ev_rows, avail, need, cpr, prow, per_page, pages;
	int ph, pw, first, last, i;
	time_t wall = time(NULL);
	struct tm tm;
	char title[256], pg[64] = "";

	if (!active)
		return;
	term_size(&rows, &cols);
	grid_reset(rows, cols);

	ev_rows = rows / 6;
	if (ev_rows < 4)
		ev_rows = 4;
	if (ev_rows > 9)
		ev_rows = 9;
	avail = rows - 2 - ev_rows;

	/*
	 * Panels in a grid: as many columns as keep each panel at least
	 * MIN_PANEL_W wide, as many rows as keep every selected chart at
	 * least two rows tall.  What does not fit goes to further pages.
	 */
	need = 3 + charts_shown() * MIN_CHART_ROWS;
	cpr = (cols + PANEL_GAP) / (MIN_PANEL_W + PANEL_GAP);
	if (cpr < 1)
		cpr = 1;
	if (cpr > nslots)
		cpr = nslots ? nslots : 1;
	prow = avail / need;
	if (prow < 1)
		prow = 1;
	if (prow > (nslots + cpr - 1) / cpr)
		prow = (nslots + cpr - 1) / cpr;
	per_page = cpr * prow;
	pages = (nslots + per_page - 1) / per_page;
	if (page >= pages)
		page = pages - 1;
	if (page < 0)
		page = 0;
	ph = avail / prow;
	pw = (cols - PANEL_GAP * (cpr - 1)) / cpr;
	first = page * per_page;
	last = first + per_page < nslots ? first + per_page : nslots;
	if (pages > 1)
		snprintf(pg, sizeof(pg), "  cores %d-%d of %d, page %d/%d",
			 first + 1, last, nslots, page + 1, pages);

	/* title bar */
	localtime_r(&wall, &tm);
	snprintf(title, sizeof(title),
		 " pmutop  %02d:%02d:%02d  %s  every %d ms%s%s%s%s",
		 tm.tm_hour, tm.tm_min, tm.tm_sec, info.mode, info.interval_ms, pg,
		 info.csv ? "  csv " : "", info.csv ? info.csv : "",
		 paused ? "  [PAUSED]" : "");
	for (i = 0; i < cols; i++)
		put_cell(0, i, " ", A_REV);
	put_text(0, 0, cols, title, A_REV | A_BOLD);
	{
		const char *keys = pages > 1 ? " q quit  p pause  c clear  \xe2\x86\x90\xe2\x86\x92 page "
					     : " q quit  p pause  c clear ";
		int kw = pages > 1 ? 37 : 26;

		if (cols > (int)strlen(title) + kw)
			put_text(0, cols - kw, kw, keys, A_REV);
	}
	put_text(1, 0, cols,
		 " \xe2\x96\xb2 grows up: higher is better   \xe2\x96\xbc hangs down: lower is better"
		 "   bar colour = range it was drawn on: green < light blue < blue < magenta;"
		 " yellow/red = high/too high", C_DIM);

	for (i = first; i < last; i++) {
		int k = i - first, pr = k / cpr, pc = k % cpr;
		int r0 = 2 + pr * ph, c0 = pc * (pw + PANEL_GAP);
		int top = pr ? 1 : 0;	/* a rule above every panel row but the first */

		if (top)
			for (int x = c0; x < c0 + pw; x++)
				put_cell(r0, x, "─", C_DIM);
		draw_panel(&hist[i], r0 + top, c0, pw, ph - top);
		/*
		 * Divider to the left; the edges of an isolated core's panel
		 * are heavy and magenta, so the real-time cores stand out.
		 */
		if (pc) {
			bool hot = hist[i - 1].m.rt_core || hist[i].m.rt_core;

			for (int r = r0 + top; r < r0 + ph; r++)
				put_cell(r, c0 - 2, hot ? "┃" : "│",
					 hot ? C_MAGENTA : C_DIM);
		}
	}

	/* event log, newest first */
	int r = rows - ev_rows;

	for (i = 0; i < cols; i++)
		put_cell(r, i, "─", C_DIM);
	put_text(r, 1, 8, " Events ", A_BOLD);
	r++;
	if (nevents == 0)
		put_text(r, 2, cols - 2, "no state changes yet", C_DIM);
	for (unsigned k = 0; k < NEVENTS && k < nevents && r < rows; k++, r++) {
		const char *e = events[(nevents - 1 - k) % NEVENTS];
		unsigned char a = strstr(e, "FAIL") ? C_RED :
				  strstr(e, "WARN") ? C_YELLOW : C_GREEN;

		put_text(r, 0, cols, e, a);
	}
	grid_flush();
}

/* ------------------------------------------------------------- keyboard */

int ui_wait(int ms, int nslots)
{
	int64_t end = mono_ns() + (int64_t)ms * 1000000;
	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	char buf[32];
	ssize_t n;

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
		n = read(STDIN_FILENO, buf, sizeof(buf));
		for (ssize_t i = 0; i < n; i++) {
			/*
			 * Paging: Right / PgDn next, Left / PgUp previous.
			 * Arrows arrive as ESC [ C / ESC [ D, PgUp / PgDn as
			 * ESC [ 5 ~ / ESC [ 6 ~.  ui_draw() clamps the page.
			 */
			if (buf[i] == 27 && i + 2 < n && buf[i + 1] == '[') {
				char k = buf[i + 2];

				i += 2;
				if (k == 'C')
					page++;
				else if (k == 'D')
					page--;
				else if ((k == '5' || k == '6') && i + 1 < n && buf[i + 1] == '~') {
					page += k == '6' ? 1 : -1;
					i++;
				} else {
					continue;
				}
				ui_draw(nslots);
				continue;
			}
			switch (buf[i]) {
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
}

/* Paused: sampling goes on (baselines keep learning), drawing stops. */
bool ui_paused(void)
{
	return paused;
}

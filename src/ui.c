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
	G_LLC,
	G_TLB,
	G_DEV,
	G_IRQ,
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
};

/*
 * Fixed ranges, so a bar of a given height always means the same value;
 * an auto-scaled chart would magnify noise on a quiet core.
 */
static const struct graph_desc gd[G_NR] = {
	[G_BUSY]  = { "CPU busy %",    false, true,  false, 0,     100,    "%.1f" },
	[G_IPC]   = { "IPC",           false, false, false, 0,     4,      "%.2f" },
	[G_LLC]   = { "LLC miss/K",    true,  false, true,  0.001, 100,    "%.3f" },
	[G_TLB]   = { "dTLB miss/K",   true,  false, true,  0.001, 100,    "%.3f" },
	[G_DEV]   = { "device IRQ/s",  true,  false, true,  1,     10000,  "%.0f" },
	/* thermal + IPI + device: everything except EVL's own timer */
	[G_IRQ]   = { "non-timer IRQ/s", true, false, true, 1,     100000, "%.0f" },
	[G_FRAME] = { "worst frame us", true, false, false, 0,     1000,   "%.1f" },
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

/* ------------------------------------------------------------ cell grid */

enum { C_DEF = 0, C_GREEN, C_YELLOW, C_RED, C_DIM, C_CYAN };

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
		[C_RED] = ";31", [C_DIM] = ";2", [C_CYAN] = ";36",
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
	h->v[G_BUSY][i] = m->busy * 100.0;
	h->v[G_IPC][i] = m->ipc;
	h->v[G_LLC][i] = m->llc_mpki;
	h->v[G_TLB][i] = m->tlb_mpki;
	h->v[G_DEV][i] = dt > 0 ? s->irq_device / dt : NAN;
	h->v[G_IRQ][i] = dt > 0 ? (s->irq_thermal + s->irq_ipi + s->irq_device) / dt
				: NAN;
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

static void axis_label(char *b, size_t n, double v)
{
	if (v >= 1000)
		snprintf(b, n, "%.0fk", v / 1000);
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
	unsigned n = h->n < HIST ? h->n : HIST;
	int pw = w - AXIS_W;			/* plot width */
	unsigned shown = n < (unsigned)pw ? n : (unsigned)pw;
	double cur = h->n ? h->v[g][(h->n - 1) % HIST] : NAN;
	char val[32], top[16], bot[16];
	int k;

	/* title: arrow, name, current value, direction hint */
	if (isfinite(cur))
		snprintf(val, sizeof(val), d->fmt, cur);
	else
		snprintf(val, sizeof(val), "-");
	put_cell(r, c, d->neutral ? "■" : d->down ? "▼" : "▲",
		 d->neutral ? C_CYAN : d->down ? C_YELLOW : C_GREEN);
	put_textf(r, c + 2, w - 2, A_BOLD, "%s", d->name);
	put_textf(r, c + 2 + (int)strlen(d->name) + 1, 12, A_BOLD | C_CYAN, "%s", val);
	if (!d->neutral && w > 46) {
		const char *hint = d->down ? "lower = better" : "higher = better";
		int n = (int)strlen(hint);

		put_text(r, c + w - n - 1, n, hint, C_DIM);
	}

	/* scale labels: the baseline (where bars start) and the far end */
	axis_label(top, sizeof(top), d->down ? d->lo : d->hi);
	axis_label(bot, sizeof(bot), d->down ? d->hi : d->lo);
	put_textf(r + 1, c, AXIS_W - 1, C_DIM, "%*s", AXIS_W - 1, top);
	put_textf(r + ht, c, AXIS_W - 1, C_DIM, "%*s", AXIS_W - 1, bot);
	for (k = 1; k <= ht; k++)
		put_cell(r + k, c + AXIS_W - 1, "│", C_DIM);

	for (unsigned j = h->n - shown, x = c + AXIS_W + (pw - shown); j < h->n; j++, x++) {
		unsigned i = j % HIST;
		double f = frac(d, h->v[g][i]);
		unsigned char col = state_col(h->st[i]);
		int total;

		if (f < 0) {			/* no data for this sample */
			put_cell(d->down ? r + 1 : r + ht, x, "·", C_DIM);
			continue;
		}
		total = (int)lround(f * ht * 8);
		if (total == 0 && h->v[g][i] > d->lo)
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

static void draw_panel(const struct slot_hist *h, int r0, int c0, int w, int ph)
{
	const struct cpu_sample *s = &h->s;
	const struct metrics *m = &h->m;
	double rate = h->dt > 0 ? 1.0 / h->dt : 0;
	int order[G_NR], norder = 0, ncharts, ht, r, i;
	char t[16], b[16];

	if (info.telemetry)
		order[norder++] = G_FRAME;
	order[norder++] = G_BUSY;
	order[norder++] = G_IPC;
	order[norder++] = G_LLC;
	order[norder++] = G_TLB;
	order[norder++] = G_DEV;
	order[norder++] = G_IRQ;

	/* header: 3 rows */
	put_textf(r0, c0, 8, A_BOLD, "CPU %d", h->cpu);
	put_textf(r0, c0 + 8, 6, state_col(m->health) | A_REV | A_BOLD, " %-4s ",
		  state_name(m->health));
	if (isfinite(m->temp_c))
		snprintf(t, sizeof(t), "%.0fC", m->temp_c);
	else
		snprintf(t, sizeof(t), "-");
	if (isfinite(m->busy))
		snprintf(b, sizeof(b), "%.1f%%", m->busy * 100);
	else
		snprintf(b, sizeof(b), "-");
	put_textf(r0, c0 + 15, w - 15, C_DEF,
		  "busy %s  (Linux sees %.1f%%)  %.0f MHz  %s",
		  b, m->util * 100, m->freq_mhz, t);
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

void ui_draw(int nslots)
{
	int rows, cols, ev_rows, ph, pw, gap = 3, i;
	time_t wall = time(NULL);
	struct tm tm;
	char title[256];

	if (!active)
		return;
	term_size(&rows, &cols);
	grid_reset(rows, cols);

	/* title bar */
	localtime_r(&wall, &tm);
	snprintf(title, sizeof(title),
		 " rt-health  %02d:%02d:%02d  %s  every %d ms%s%s%s",
		 tm.tm_hour, tm.tm_min, tm.tm_sec, info.mode, info.interval_ms,
		 info.csv ? "  csv " : "", info.csv ? info.csv : "",
		 paused ? "  [PAUSED]" : "");
	for (i = 0; i < cols; i++)
		put_cell(0, i, " ", A_REV);
	put_text(0, 0, cols, title, A_REV | A_BOLD);
	if (cols > 70)
		put_text(0, cols - 28, 28, " q quit  p pause  c clear ", A_REV);
	put_text(1, 0, cols,
		 " \xe2\x96\xb2 grows up: higher is better   \xe2\x96\xbc hangs down: lower is better"
		 "   colour = state of that sample: green OK, yellow WARN, red FAIL", C_DIM);

	/* panels side by side; the event log takes the bottom rows */
	ev_rows = rows / 6;
	if (ev_rows < 4)
		ev_rows = 4;
	if (ev_rows > 9)
		ev_rows = 9;
	ph = rows - 2 - ev_rows;
	pw = nslots ? (cols - gap * (nslots - 1)) / nslots : cols;
	for (i = 0; i < nslots; i++)
		draw_panel(&hist[i], 2, i * (pw + gap), pw, ph);
	for (i = 1; i < nslots; i++)
		for (int r = 2; r < 2 + ph; r++)
			put_cell(r, i * (pw + gap) - 2, "│", C_DIM);

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

/* SPDX-License-Identifier: MIT */
/*
 * rt_frames.h - in-band-free frame telemetry shared between an EVL
 * real-time task and an ordinary Linux observer (pmutop).
 *
 * The real-time side only stores values and flips an atomic sequence
 * word: no syscall, no lock, no allocation.  The observer reads with a
 * seqlock-style check and never writes to the shared page.
 *
 * Map the file shared, first by the real-time process (it creates and
 * initialises it), then by the observer (read-only).
 */
#ifndef RT_FRAMES_H
#define RT_FRAMES_H

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>

#define RFT_MAGIC		0x52544652u	/* "RTFR" */
#define RFT_VERSION		1u
#define RFT_MAX_TASKS		32
#define RFT_NAME_MAX		32
#define RFT_HIST_BINS		16

enum rft_state {
	RFT_RUNNING = 0,
	RFT_DEGRADED,			/* occasional overruns / timeouts */
	RFT_OVERRUN,			/* frames missed their deadline */
};

/* Per-frame flags, bit positions in struct rft_frame::flags. */
enum rft_frame_flag {
	RFT_F_OVERRUN	= 1u << 0,	/* finished after the deadline */
	RFT_F_TIMEOUT	= 1u << 1,	/* waited for the peer past deadline */
	RFT_F_DEMOTED	= 1u << 2,	/* left the out-of-band stage */
	RFT_F_FAULT	= 1u << 3,	/* data error detected */
};

struct rft_frame {
	uint64_t seq;			/* frame number */
	uint64_t t_start;		/* scheduled start of the frame, ns */
	uint64_t t_wake;		/* actual wake-up, ns */
	uint64_t t_end;			/* frame work finished, ns */
	uint64_t deadline;		/* t_start + period, ns */
	uint32_t flags;
	uint32_t pad;
};

struct rft_task {
	char		name[RFT_NAME_MAX];
	uint32_t	cpu;		/* CPU the task is bound to */
	uint32_t	period_ns;
	uint64_t	frames;		/* frames completed */
	uint64_t	late;		/* frames with flag OVERRUN */
	uint64_t	worst_ns;	/* worst frame duration */

	/*
	 * Ring written by the real-time thread.  A writer stores the record
	 * first, then bumps wr_seq with release ordering; the reader reads
	 * wr_seq, then the record, then wr_seq again and retries on change.
	 */
	_Atomic uint32_t wr_seq;
	uint32_t	 pad0;
	struct rft_frame ring[64];
} __attribute__((aligned(64)));

struct rft_page {
	uint32_t	magic;
	uint32_t	version;
	uint32_t	nr_tasks;
	uint32_t	pad0;
	uint64_t	period_ns;	/* nominal frame period of the system */
	uint64_t	epoch_ns;	/* CLOCK_MONOTONIC time of frame 0 */
	struct rft_task	task[RFT_MAX_TASKS];
};

/*
 * Publisher API.  All of it runs on the real-time side, in the
 * out-of-band stage; none of it may call into Linux.
 */
static inline void rft_publish(struct rft_task *t, const struct rft_frame *f)
{
	uint32_t s;
	int i;

	i = (int)(f->seq & (sizeof(t->ring) / sizeof(t->ring[0]) - 1));
	s = atomic_load_explicit(&t->wr_seq, memory_order_relaxed);
	t->ring[i] = *f;		/* plain stores: visible after the seq bump */
	atomic_store_explicit(&t->wr_seq, s + 1, memory_order_release);

	t->frames = f->seq + 1;
	if (f->flags & RFT_F_OVERRUN)
		t->late++;
	if (f->t_end - f->t_wake > t->worst_ns)
		t->worst_ns = f->t_end - f->t_wake;
}

/*
 * Reader API.  Returns 0 and fills @out, or -EAGAIN when the writer
 * moved while we were copying (only possible on a weak memory model,
 * or if the reader is preempted between the two seq loads).
 */
static inline int rft_peek(const struct rft_task *t, uint32_t *cursor,
			   struct rft_frame *out)
{
	unsigned n = sizeof(t->ring) / sizeof(t->ring[0]);
	uint32_t s1, s2;
	int i;

	s1 = atomic_load_explicit(&t->wr_seq, memory_order_acquire);
	if (s1 == *cursor)
		return -EAGAIN;
	i = (int)(*cursor & (n - 1));
	*out = t->ring[i];
	s2 = atomic_load_explicit(&t->wr_seq, memory_order_acquire);
	if (s1 != s2)
		return -EAGAIN;
	*cursor = s1;
	return 0;
}

#endif /* RT_FRAMES_H */

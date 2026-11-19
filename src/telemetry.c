// SPDX-License-Identifier: MIT
/*
 * telemetry.c - read-only view of the real-time task's frame records.
 *
 * The page is mapped read-only.  The writer (the EVL task) only ever
 * stores to its own ring and bumps a sequence word, so the reader can
 * never disturb it; a torn read is detected with the usual double-check.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "telemetry.h"

int telem_open(struct telem *t, const char *path)
{
	struct stat st;
	void *p;

	memset(t, 0, sizeof(*t));
	t->fd = -1;
	if (!path)
		return -1;

	t->fd = open(path, O_RDONLY);
	if (t->fd < 0)
		return -1;
	if (fstat(t->fd, &st) < 0 || (size_t)st.st_size < sizeof(struct rft_page)) {
		close(t->fd);
		t->fd = -1;
		return -1;
	}
	p = mmap(NULL, sizeof(struct rft_page), PROT_READ, MAP_SHARED, t->fd, 0);
	if (p == MAP_FAILED) {
		close(t->fd);
		t->fd = -1;
		return -1;
	}
	t->page = p;
	if (t->page->magic != RFT_MAGIC || t->page->version != RFT_VERSION) {
		munmap(p, sizeof(struct rft_page));
		close(t->fd);
		t->fd = -1;
		return -1;
	}
	return 0;
}

void telem_close(struct telem *t)
{
	if (t->page)
		munmap((void *)t->page, sizeof(struct rft_page));
	if (t->fd >= 0)
		close(t->fd);
	t->page = NULL;
	t->fd = -1;
}

int telem_sample(struct telem *t, uint32_t task_cpu,
		 uint64_t *frames_delta, uint64_t *late_delta,
		 uint64_t *worst_ns, uint64_t *late_flags)
{
	const struct rft_task *tk;
	struct rft_frame f;
	uint64_t worst = 0;
	uint32_t i, ntasks;
	int advanced = 0;

	*frames_delta = 0;
	*late_delta = 0;
	*worst_ns = 0;
	*late_flags = 0;
	if (!t->page)
		return 0;

	ntasks = t->page->nr_tasks;
	if (ntasks > RFT_MAX_TASKS)
		ntasks = RFT_MAX_TASKS;

	for (i = 0; i < ntasks; i++) {
		tk = &t->page->task[i];
		if (tk->cpu != task_cpu)
			continue;
		(*frames_delta) += tk->frames - t->frames_seen[i];
		(*late_delta) += tk->late - t->late_seen[i];
		t->frames_seen[i] = tk->frames;
		t->late_seen[i] = tk->late;
		advanced = 1;

		/*
		 * Drain the new records for the worst frame of the interval.
		 * Late frames are counted once, from the task's own flag
		 * and counter; recomputing them from the timestamps here
		 * would double count.
		 */
		while (rft_peek(tk, &t->cursor[i], &f) == 0) {
			if (f.t_end - f.t_wake > worst)
				worst = f.t_end - f.t_wake;
		}
	}

	*worst_ns = worst;
	return advanced;
}

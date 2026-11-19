# SPDX-License-Identifier: MIT
CC	?= cc
CFLAGS	?= -O2 -g -Wall -Wextra -std=gnu11
CPPFLAGS += -Isrc -Itelemetry
LDLIBS	+= -lm

SRCS	:= src/main.c src/perf.c src/sysfs.c src/metrics.c src/telemetry.c src/ui.c
OBJS	:= $(SRCS:.c=.o)

all: rt-health

rt-health: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

tests/test_irq: tests/test_irq.c src/sysfs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^

check: tests/test_irq
	./tests/test_irq

clean:
	rm -f rt-health $(OBJS) tests/test_irq

.PHONY: all check clean

.DEFAULT_GOAL := all
CC = gcc
AR = ar
SCHED ?= PSJF
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
CPPFLAGS += -D_GNU_SOURCE -D$(SCHED)
LDLIBS += -pthread
BUILD = build/$(SCHED)

ifeq ($(filter $(SCHED),PSJF MLFQ CFS),)
$(error SCHED must be PSJF, MLFQ, or CFS)
endif

.PHONY: all clean test benchmarks FORCE
all: libthread-worker.a

$(BUILD):
	mkdir -p $@

$(BUILD)/thread-worker.o: thread-worker.c thread-worker.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@

$(BUILD)/libthread-worker.a: $(BUILD)/thread-worker.o
	$(AR) rcs $@ $<

libthread-worker.a: $(BUILD)/libthread-worker.a FORCE
	cp $< $@

$(BUILD)/runtime: tests/runtime.c $(BUILD)/libthread-worker.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(BUILD)/libthread-worker.a $(LDLIBS) -o $@

$(BUILD)/policy: tests/policy.c thread-worker.c thread-worker.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDLIBS) -o $@

test: $(BUILD)/runtime $(BUILD)/policy
	python3 tests/run.py $(BUILD)

benchmarks: all
	$(MAKE) -C benchmarks

clean:
	rm -rf build libthread-worker.a
	$(MAKE) -C benchmarks clean

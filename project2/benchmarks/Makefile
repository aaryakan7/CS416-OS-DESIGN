CC = gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
USE_WORKERS ?= 1
CPPFLAGS += -D_GNU_SOURCE -DUSE_WORKERS=$(USE_WORKERS)
PROGRAMS = parallel_cal vector_multiply external_cal test
ifeq ($(USE_WORKERS),1)
LIBRARY = ../libthread-worker.a
endif

.PHONY: all clean FORCE
all: $(PROGRAMS)
$(PROGRAMS): %: %.c benchmark.h ../thread-worker.h $(LIBRARY) FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIBRARY) -pthread -o $@
clean:
	rm -f $(PROGRAMS) *.o

CC = gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
FUSE_CFLAGS = $(shell pkg-config --cflags fuse 2>/dev/null)
FUSE_LIBS = $(shell pkg-config --libs fuse 2>/dev/null)
FUSE_AVAILABLE = $(shell pkg-config --exists fuse 2>/dev/null && echo yes)
ifeq ($(FUSE_AVAILABLE),yes)
TEST_FLAGS = $(FUSE_CFLAGS)
else
TEST_FLAGS = -DRUFS_TEST_STUB
endif

.PHONY: all clean test check-fuse FORCE
all: rufs
check-fuse:
	@pkg-config --exists fuse || { echo 'Install libfuse 2 development headers and pkg-config.'; exit 1; }
rufs: rufs.c rufs.h block.c block.h check-fuse
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) rufs.c block.c $(FUSE_LIBS) -o $@
test: build/test_rufs
	./build/test_rufs
build/test_rufs: tests/test_rufs.c tests/fuse_stub.h rufs.c rufs.h block.c block.h FORCE
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_FLAGS) tests/test_rufs.c block.c $(FUSE_LIBS) -o $@
clean:
	rm -rf build rufs *.o

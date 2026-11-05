CC = gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
AR = ar
CONFIG = $(CPPFLAGS) $(CFLAGS)

.PHONY: all clean test benchmarks FORCE
all: libmy_vm.a

# Recompile when configuration flags change between invocations.
my_vm.o: my_vm.c my_vm.h FORCE
	$(CC) $(CONFIG) -pthread -c my_vm.c -o $@
libmy_vm.a: my_vm.o
	$(AR) rcs $@ $<
benchmarks: all
	$(MAKE) -C benchmark CC=$(CC) CFLAGS='$(CFLAGS)' CPPFLAGS='$(CPPFLAGS)'

test:
	mkdir -p build
	$(CC) $(CONFIG) -pthread -DMEMSIZE=33554432 -DPGSIZE=4096 -DTLB_ENTRIES=3 tests/test_vm.c my_vm.c -o build/test-4k
	./build/test-4k
	$(CC) $(CONFIG) -pthread -DMEMSIZE=33554432 -DPGSIZE=8192 -DTLB_ENTRIES=1 tests/test_vm.c my_vm.c -o build/test-8k
	./build/test-8k
	$(CC) $(CONFIG) -pthread -DMEMSIZE=33554432 -DPGSIZE=16384 -DTLB_ENTRIES=3 tests/test_vm.c my_vm.c -o build/test-16k
	./build/test-16k
	$(CC) $(CONFIG) -pthread -DMEMSIZE=33554432 -DPGSIZE=65536 -DTLB_ENTRIES=7 tests/test_vm.c my_vm.c -o build/test-64k
	./build/test-64k
	$(CC) $(CONFIG) -pthread -DMEMSIZE=8192 -DPGSIZE=4096 tests/test_exhaustion.c my_vm.c -o build/test-exhaustion
	./build/test-exhaustion
clean:
	rm -rf build *.o *.a
	$(MAKE) -C benchmark clean

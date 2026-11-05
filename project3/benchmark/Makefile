CC = gcc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
.PHONY: all clean FORCE
all: test mtest
test: test.c ../my_vm.h ../libmy_vm.a FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) $< ../libmy_vm.a -pthread -o $@
mtest: multi_test.c ../my_vm.h ../libmy_vm.a FORCE
	$(CC) $(CPPFLAGS) $(CFLAGS) $< ../libmy_vm.a -pthread -o $@
clean:
	rm -f test mtest

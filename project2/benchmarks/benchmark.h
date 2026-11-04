#ifndef BENCHMARK_H
#define BENCHMARK_H
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline int parse_thread_count(int argc, char **argv, int fallback) {
    if (argc == 1) return fallback;
    char *end;
    errno = 0;
    long count = argc == 2 ? strtol(argv[1], &end, 10) : 0;
    if (argc != 2 || errno || end == argv[1] || *end || count < 1 || count > 10000) {
        fprintf(stderr, "Usage: %s [threads: 1..10000]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    return (int)count;
}

static inline void *checked_malloc(size_t bytes) {
    void *memory = malloc(bytes);
    if (!memory) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    return memory;
}

static inline void check_thread(int error) {
    if (error) {
        fprintf(stderr, "Thread operation failed: %s\n", strerror(error));
        exit(EXIT_FAILURE);
    }
}

static inline int64_t elapsed_us(struct timespec start, struct timespec end) {
    return (int64_t)(end.tv_sec - start.tv_sec) * 1000000 +
           (end.tv_nsec - start.tv_nsec) / 1000;
}

static inline void read_value(FILE *file, int *value) {
    if (fscanf(file, "%d", value) != 1 || *value < 0 || *value > 65535) {
        fputs("Invalid or incomplete record file. Run ./genRecord.sh.\n", stderr);
        exit(EXIT_FAILURE);
    }
}
#endif

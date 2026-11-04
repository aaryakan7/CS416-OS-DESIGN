#include "benchmark.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../thread-worker.h"

#define DEFAULT_THREAD_NUM 4
#ifndef VECTOR_SIZE
#define VECTOR_SIZE 3000000
#endif

/* Global variables */
pthread_mutex_t   mutex;
int thread_num;
int* counter;
pthread_t *thread;
int r[VECTOR_SIZE];
int s[VECTOR_SIZE];
uint64_t sum = 0;

/* A CPU-bound task to do vector multiplication */
void *vector_multiply(void* arg) {
	int i = 0;
	int n = *((int*) arg);

	for (i = n; i < VECTOR_SIZE; i += thread_num) {
		check_thread(pthread_mutex_lock(&mutex));
		sum += (uint64_t)r[i] * (unsigned)s[i];
		check_thread(pthread_mutex_unlock(&mutex));
	}

	return NULL;
}

void verify(void) {
	int i = 0;
	sum = 0;
	for (i = 0; i < VECTOR_SIZE; i += 1) {
		sum += (uint64_t)r[i] * (unsigned)s[i];
	}
	//printf("verified sum is: %d\n", sum);
}

int main(int argc, char **argv) {

	int i = 0;

	thread_num = parse_thread_count(argc, argv, DEFAULT_THREAD_NUM);

	// initialize counter
	counter = (int*)checked_malloc(thread_num*sizeof(int));
	for (i = 0; i < thread_num; ++i)
		counter[i] = i;

	// initialize pthread_t
	thread = (pthread_t*)checked_malloc(thread_num*sizeof(pthread_t));

	// initialize data array
	for (i = 0; i < VECTOR_SIZE; ++i) {
		r[i] = i;
		s[i] = i;
	}

	check_thread(pthread_mutex_init(&mutex, NULL));

	struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);

	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_create(&thread[i], NULL, &vector_multiply, &counter[i]));

	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_join(thread[i], NULL));

        fprintf(stderr, "***************************\n");

        clock_gettime(CLOCK_MONOTONIC, &end);

        printf("Total run time: %" PRId64 " micro-seconds\n", elapsed_us(start, end));

	check_thread(pthread_mutex_destroy(&mutex));
	uint64_t threaded_sum = sum;
	verify();
	if (sum != threaded_sum) {
		fprintf(stderr, "Sum mismatch: threaded=%" PRIu64 " serial=%" PRIu64 "\n", threaded_sum, sum);
		return EXIT_FAILURE;
	}

	// Free memory on Heap
	free(thread);
	free(counter);

#if USE_WORKERS
        fprintf(stderr , "Total sum is: %" PRIu64 "\n", threaded_sum);
        print_app_stats();
        fprintf(stderr, "***************************\n");
#endif


	return 0;
}

#include "benchmark.h"
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include "../thread-worker.h"

#define DEFAULT_THREAD_NUM 4
#ifndef C_SIZE
#define C_SIZE 10000
#endif
#ifndef R_SIZE
#define R_SIZE 1000
#endif

pthread_mutex_t   mutex;
int thread_num;
int* counter;
pthread_t *thread;
int* a[R_SIZE];
uint64_t pSum[R_SIZE];
uint64_t sum = 0;


/* A CPU-bound task to do parallel array addition */
void *parallel_calculate(void* arg) {

	int i = 0, j = 0;
	int n = *((int*) arg);

	for (j = n; j < R_SIZE; j += thread_num) {
		for (i = 0; i < C_SIZE; ++i) {
			pSum[j] += (uint64_t)a[j][i] * (unsigned)i;
		}
	}
	for (j = n; j < R_SIZE; j += thread_num) {
		check_thread(pthread_mutex_lock(&mutex));
		sum += pSum[j];
		check_thread(pthread_mutex_unlock(&mutex));
	}

	return NULL;
}


/* verification function */
void verify(void) {

	int i = 0, j = 0;
	sum = 0;
	memset(&pSum, 0, sizeof(pSum));

	for (j = 0; j < R_SIZE; j += 1) {
		for (i = 0; i < C_SIZE; ++i) {
			pSum[j] += (uint64_t)a[j][i] * (unsigned)i;
		}
	}
	for (j = 0; j < R_SIZE; j += 1) {
		sum += pSum[j];
	}
	//printf("verified sum is: %d\n", sum);
}

int main(int argc, char **argv) {

	int i = 0, j = 0;

	thread_num = parse_thread_count(argc, argv, DEFAULT_THREAD_NUM);

	// initialize counter
	counter = (int*)checked_malloc(thread_num*sizeof(int));
	for (i = 0; i < thread_num; ++i)
		counter[i] = i;

	// initialize pthread_t
	thread = (pthread_t*)checked_malloc(thread_num*sizeof(pthread_t));

	// initialize data array
	for (i = 0; i < R_SIZE; ++i)
		a[i] = (int*)checked_malloc(C_SIZE*sizeof(int));

	for (i = 0; i < R_SIZE; ++i)
		for (j = 0; j < C_SIZE; ++j)
			a[i][j] = j;

	memset(&pSum, 0, sizeof(pSum));
	// mutex init
	check_thread(pthread_mutex_init(&mutex, NULL));

	struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_create(&thread[i], NULL, &parallel_calculate, &counter[i]));

	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_join(thread[i], NULL));

        fprintf(stderr, "***************************\n");

	clock_gettime(CLOCK_MONOTONIC, &end);

        printf("Total run time: %" PRId64 " micro-seconds\n", elapsed_us(start, end));

	// mutex destroy
	check_thread(pthread_mutex_destroy(&mutex));

	// feel free to verify your answer here:
	uint64_t threaded_sum = sum;
	verify();
	if (sum != threaded_sum) {
		fprintf(stderr, "Sum mismatch: threaded=%" PRIu64 " serial=%" PRIu64 "\n", threaded_sum, sum);
		return EXIT_FAILURE;
	}
	// Free memory on Heap
	free(thread);
	free(counter);

	for (i = 0; i < R_SIZE; ++i)
		free(a[i]);

#if USE_WORKERS
        fprintf(stderr , "Total sum is: %" PRIu64 "\n", threaded_sum);
        print_app_stats();
        fprintf(stderr, "***************************\n");
#endif

	return 0;
}

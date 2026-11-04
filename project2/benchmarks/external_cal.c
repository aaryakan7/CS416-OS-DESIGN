#include "benchmark.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <pthread.h>
#include "../thread-worker.h"

#define DEFAULT_THREAD_NUM 4
#ifndef RECORD_NUM
#define RECORD_NUM 10
#endif
#ifndef RECORD_SIZE
#define RECORD_SIZE 4194304
#endif

/* Global variables */
pthread_mutex_t   mutex;
int thread_num;
int* counter;
pthread_t *thread;
int *mem = NULL;
uint64_t sum = 0;
#ifndef RECORD_VALUES
#define RECORD_VALUES 4096
#endif
int itr = RECORD_VALUES / 4;

void *external_calculate(void* arg) {
	
	int i = 0, j = 0, k = 0;
	int n = *((int*) arg);

	for (k = n; k < RECORD_NUM; k += thread_num) {

	char path[64];

		snprintf(path, sizeof(path), "./record/%d", k);

		FILE *f;
		f = fopen(path, "r");
		if (!f) {
			printf("failed to open file %s, please run ./genRecord.sh first\n", path);
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < itr; ++i) {
			// read 16B from nth record into memory from mem[n*4]
			for (j = 0; j < 4; ++j) {
				read_value(f, &mem[k*4 + j]);
				check_thread(pthread_mutex_lock(&mutex));
				sum += mem[k*4 + j];
				check_thread(pthread_mutex_unlock(&mutex));
			}
		}
		fclose(f);
	}

	return NULL;
}


void verify(void) {
	
	int i = 0, j = 0, k = 0;
	char path[64];

	sum = 0;
	memset(mem, 0, (RECORD_NUM * 4 * (int)sizeof(int)));

	for (k = 0; k < RECORD_NUM; ++k) {
		snprintf(path, sizeof(path), "./record/%d", k);

		FILE *f;
		f = fopen(path, "r");
		if (!f) {
			printf("failed to open file %s, please run ./genRecord.sh first\n", path);
			exit(EXIT_FAILURE);
		}

		for (i = 0; i < itr; ++i) {
			// read 16B from nth record into memory from mem[n*4]
			for (j = 0; j < 4; ++j) {
				read_value(f, &mem[k*4 + j]);
				sum += mem[k*4 + j];
			}
		}
		fclose(f);
	}
}


void sig_handler(int signum) {
	printf("%d\n", signum);
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
	mem = (int*)checked_malloc((RECORD_NUM * 4 * (int)sizeof(int)));
	memset(mem, 0, (RECORD_NUM * 4 * (int)sizeof(int)));

	check_thread(pthread_mutex_init(&mutex, NULL));

	struct timespec start, end;
        clock_gettime(CLOCK_MONOTONIC, &start);
 
	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_create(&thread[i], NULL, &external_calculate, &counter[i]));
	


	for (i = 0; i < thread_num; ++i)
		check_thread(pthread_join(thread[i], NULL));


        fprintf(stderr, "***************************\n");

        clock_gettime(CLOCK_MONOTONIC, &end);

        printf("Total run time: %" PRId64 " micro-seconds\n", elapsed_us(start, end));

	check_thread(pthread_mutex_destroy(&mutex));

	// feel free to verify your answer here:
	uint64_t threaded_sum = sum;
	verify();
	if (sum != threaded_sum) {
		fprintf(stderr, "Sum mismatch: threaded=%" PRIu64 " serial=%" PRIu64 "\n", threaded_sum, sum);
		return EXIT_FAILURE;
	}
	
	free(mem);
	free(thread);
	free(counter);

#if USE_WORKERS
	fprintf(stderr , "Total sum is: %" PRIu64 "\n", threaded_sum);
        print_app_stats();
	fprintf(stderr, "***************************\n");
#endif

	return 0;
}

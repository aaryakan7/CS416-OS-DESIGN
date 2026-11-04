/* CS416 Project 2 — Aarya Dalal and Aarya Kansara */
#ifndef WORKER_T_H
#define WORKER_T_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <ucontext.h>

/* Build benchmarks with -DUSE_WORKERS=0 to use native pthreads. */
#ifndef USE_WORKERS
#define USE_WORKERS 1
#endif
#ifndef TARGET_LATENCY
#define TARGET_LATENCY 20
#endif
#ifndef MIN_SCHED_GRN
#define MIN_SCHED_GRN 1
#endif
#ifndef QUANTUM
#define QUANTUM 10
#endif
#ifndef NUM_MLFQ_LEVELS
#define NUM_MLFQ_LEVELS 4
#endif
#ifndef S_PROMOTION_PERIOD
#define S_PROMOTION_PERIOD 5000
#endif
#define WORKER_STACK_SIZE (128 * 1024)
#define HEAP_DEFAULT_CAPACITY 64

_Static_assert(QUANTUM > 0 && TARGET_LATENCY > 0 && MIN_SCHED_GRN > 0,
               "Scheduling intervals must be positive");
_Static_assert(NUM_MLFQ_LEVELS > 0 && NUM_MLFQ_LEVELS < 16,
               "MLFQ must have between 1 and 15 levels");
_Static_assert(S_PROMOTION_PERIOD > 0, "Boost period must be positive");

typedef unsigned int worker_t;
typedef enum { READY, RUNNING, BLOCKED, TERMINATED, JOIN_BLOCKED } thread_status;
struct TCB;
typedef struct Queue {
    struct TCB *head;
    struct TCB *tail;
} queue;
typedef struct CFS_Heap {
    struct TCB **threads;
    int size;
    int capacity;
} cfs_heap;

typedef struct TCB {
    worker_t tid;
    thread_status status;
    ucontext_t context;
    void *stack;
    void *retval;
    void *(*function)(void *);
    void *arg;
    struct TCB *next;
    struct TCB *next_global;
    struct TCB *joiner;
    struct TCB *waiting_on;
    long elapsed_quantums;
    long vruntime;
    long start_time;
    long first_schedule_time;
    int is_first_run;
    int mlfq_level;
    long level_runtime;
    int saved_errno;
} tcb;

typedef struct worker_mutex_t {
    atomic_flag flag;
    queue blocked_queue;
    tcb *owner;
    int initialized;
} worker_mutex_t;

int worker_create(worker_t *, pthread_attr_t *, void *(*)(void *), void *);
int worker_yield(void);
void worker_exit(void *);
int worker_join(worker_t, void **);
int worker_mutex_init(worker_mutex_t *, const pthread_mutexattr_t *);
int worker_mutex_lock(worker_mutex_t *);
int worker_mutex_unlock(worker_mutex_t *);
int worker_mutex_destroy(worker_mutex_t *);
void print_app_stats(void);

#if USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif
#endif

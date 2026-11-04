/* CS416 Project 2 — Aarya Dalal and Aarya Kansara */
#include "thread-worker.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if (defined(PSJF) + defined(MLFQ) + defined(CFS)) != 1
#error "Select PSJF, MLFQ, or CFS"
#endif

static void schedule(void);
static void enqueue(queue *, tcb *);
static tcb *dequeue(queue *);
#ifdef PSJF
static queue run_queue;
static tcb *remove_thread(queue *, tcb *, tcb *);
#endif
#ifdef MLFQ
static queue mlfq_queues[NUM_MLFQ_LEVELS];
static long last_boost_time;
#endif
#ifdef CFS
static cfs_heap cfs_run_queue;
static int heap_init(cfs_heap *, int);
static int heap_grow(cfs_heap *);
static void heap_insert(cfs_heap *, tcb *);
static tcb *heap_extract_min(cfs_heap *);
static int heap_is_empty(cfs_heap *);
static void heapify_down(cfs_heap *, int);
static void heapify_up(cfs_heap *, int);
#endif

long tot_cntx_switches;
double avg_turn_time;
double avg_resp_time;
static long total_threads_completed;
static double total_turnaround_time;
static double total_response_time;
static worker_t next_tid = 1;
static ucontext_t scheduler_context;
static tcb main_tcb_data;
static tcb *current_thread;
static tcb *all_threads_list_head;
static int is_scheduler_initialized;
static int live_tcbs;
static long last_scheduled_cpu;
static long current_slice_us;

static long time_us(clockid_t clock_id) {
    struct timespec now;
    if (clock_gettime(clock_id, &now) == -1) abort();
    return now.tv_sec * 1000000L + now.tv_nsec / 1000;
}

static void block_timer(sigset_t *oldmask) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPROF);
    if (sigprocmask(SIG_BLOCK, &mask, oldmask) == -1) abort();
}

static void restore_mask(const sigset_t *oldmask) {
    if (sigprocmask(SIG_SETMASK, oldmask, NULL) == -1) abort();
}

static void timer_for_slice_setup(long slice_us) {
    struct itimerval timer = {0};
    timer.it_value.tv_sec = slice_us / 1000000;
    timer.it_value.tv_usec = slice_us % 1000000;
    if (setitimer(ITIMER_PROF, &timer, NULL) == -1) abort();
}

/* Discard an old slice's pending tick before arming the next slice. */
static void stop_timer(void) {
    sigset_t mask;
    struct timespec zero = {0};
    timer_for_slice_setup(0);
    sigemptyset(&mask);
    sigaddset(&mask, SIGPROF);
    while (sigtimedwait(&mask, NULL, &zero) >= 0) {}
}

static tcb *find_tcb(worker_t tid) {
    for (tcb *thread = all_threads_list_head; thread; thread = thread->next_global) {
        if (thread->tid == tid) return thread;
    }
    return NULL;
}

static void ready_thread(tcb *thread) {
    thread->status = READY;
#ifdef PSJF
    enqueue(&run_queue, thread);
#elif defined(MLFQ)
    enqueue(&mlfq_queues[thread->mlfq_level], thread);
#else
    heap_insert(&cfs_run_queue, thread);
#endif
}

static void switch_to_scheduler(void) {
    current_thread->saved_errno = errno;
    tot_cntx_switches++;
    if (swapcontext(&current_thread->context, &scheduler_context) == -1) abort();
}

static void timer_interrupt_handler(int signum) {
    (void)signum;
    switch_to_scheduler();
}

/* Use a zero-argument trampoline so makecontext never transports pointers. */
static void start_worker(void) {
    void *result = current_thread->function(current_thread->arg);
    worker_exit(result);
}

/* Called with SIGPROF blocked, before any timer is started. */
static int first_time_setup(void) {
    if (getcontext(&scheduler_context) == -1) return errno;
    void *stack = malloc(WORKER_STACK_SIZE);
    if (!stack) return ENOMEM;
#ifdef CFS
    int error = heap_init(&cfs_run_queue, HEAP_DEFAULT_CAPACITY);
    if (error) {
        free(stack);
        return error;
    }
#endif
    scheduler_context.uc_stack.ss_sp = stack;
    scheduler_context.uc_stack.ss_size = WORKER_STACK_SIZE;
    scheduler_context.uc_stack.ss_flags = 0;
    scheduler_context.uc_link = NULL;
    sigaddset(&scheduler_context.uc_sigmask, SIGPROF);
    makecontext(&scheduler_context, schedule, 0);

    struct sigaction action = {0};
    action.sa_handler = timer_interrupt_handler;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGPROF);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGPROF, &action, NULL) == -1) {
        int error = errno;
#ifdef CFS
        free(cfs_run_queue.threads);
#endif
        free(stack);
        return error;
    }
    main_tcb_data.status = RUNNING;
    main_tcb_data.start_time = time_us(CLOCK_MONOTONIC);
    current_thread = &main_tcb_data;
    all_threads_list_head = current_thread;
    live_tcbs = 1;
    last_scheduled_cpu = time_us(CLOCK_PROCESS_CPUTIME_ID);
#ifdef MLFQ
    last_boost_time = time_us(CLOCK_MONOTONIC);
#endif
    is_scheduler_initialized = 1;
    return 0;
}

int worker_create(worker_t *thread, pthread_attr_t *attr,
                  void *(*function)(void *), void *arg) {
    (void)attr;
    if (!thread || !function) return EINVAL;
    sigset_t oldmask;
    block_timer(&oldmask);
    int error = 0;
    if (!is_scheduler_initialized) error = first_time_setup();
    if (error) {
        restore_mask(&oldmask);
        return error;
    }
    if (next_tid == UINT_MAX || live_tcbs == INT_MAX) {
        restore_mask(&oldmask);
        return EAGAIN;
    }
#ifdef CFS
    /* Wakeups and scheduler operations cannot fail for lack of heap space. */
    if (live_tcbs == cfs_run_queue.capacity) error = heap_grow(&cfs_run_queue);
    if (error) {
        restore_mask(&oldmask);
        return error;
    }
#endif
    tcb *new_tcb = calloc(1, sizeof(*new_tcb));
    if (!new_tcb) {
        restore_mask(&oldmask);
        return ENOMEM;
    }
    new_tcb->stack = malloc(WORKER_STACK_SIZE);
    if (!new_tcb->stack) error = ENOMEM;
    else if (getcontext(&new_tcb->context) == -1) error = errno;
    if (error) {
        free(new_tcb->stack);
        free(new_tcb);
        restore_mask(&oldmask);
        return error;
    }
    new_tcb->context.uc_stack.ss_sp = new_tcb->stack;
    new_tcb->context.uc_stack.ss_size = WORKER_STACK_SIZE;
    new_tcb->context.uc_stack.ss_flags = 0;
    new_tcb->context.uc_link = &scheduler_context;
    new_tcb->context.uc_sigmask = oldmask;
    sigdelset(&new_tcb->context.uc_sigmask, SIGPROF);
    new_tcb->function = function;
    new_tcb->arg = arg;
    makecontext(&new_tcb->context, start_worker, 0);
    new_tcb->tid = next_tid++;
    new_tcb->start_time = time_us(CLOCK_MONOTONIC);
    new_tcb->is_first_run = 1;
    new_tcb->next_global = all_threads_list_head;
    all_threads_list_head = new_tcb;
    live_tcbs++;
    ready_thread(new_tcb);
    *thread = new_tcb->tid;
    /* Start/restart preemption when the first ready worker is created. */
    if (live_tcbs == 2) {
        last_scheduled_cpu = time_us(CLOCK_PROCESS_CPUTIME_ID);
        timer_for_slice_setup(QUANTUM * 1000L);
    }
    restore_mask(&oldmask);
    return 0;
}

int worker_yield(void) {
    if (!is_scheduler_initialized) return 0;
    sigset_t oldmask;
    block_timer(&oldmask);
    current_thread->status = READY;
    switch_to_scheduler();
    restore_mask(&oldmask);
    return 0;
}

void worker_exit(void *value_ptr) {
    if (!is_scheduler_initialized) exit(0);
    block_timer(NULL);
    current_thread->retval = value_ptr;
    current_thread->status = TERMINATED;
    if (current_thread->joiner) {
        current_thread->joiner->waiting_on = NULL;
        ready_thread(current_thread->joiner);
    }
    tot_cntx_switches++;
    if (setcontext(&scheduler_context) == -1) abort();
    abort();
}

static int would_deadlock(tcb *target) {
    for (tcb *thread = target; thread; thread = thread->waiting_on) {
        if (thread == current_thread) return 1;
    }
    return 0;
}

int worker_join(worker_t thread, void **value_ptr) {
    if (!is_scheduler_initialized || thread == 0) return ESRCH;
    sigset_t oldmask;
    block_timer(&oldmask);
    tcb *target = find_tcb(thread);
    int error = 0;
    if (!target) error = ESRCH;
    else if (would_deadlock(target)) error = EDEADLK;
    else if (target->joiner) error = EINVAL;
    if (error) {
        restore_mask(&oldmask);
        return error;
    }
    target->joiner = current_thread;
    if (target->status != TERMINATED) {
        current_thread->status = JOIN_BLOCKED;
        current_thread->waiting_on = target;
        switch_to_scheduler();
    }
    if (value_ptr) *value_ptr = target->retval;
    tcb **link = &all_threads_list_head;
    while (*link != target) link = &(*link)->next_global;
    *link = target->next_global;
    free(target->stack);
    free(target);
    live_tcbs--;
    restore_mask(&oldmask);
    return 0;
}

int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    (void)attr;
    if (!mutex) return EINVAL;
    atomic_flag_clear(&mutex->flag);
    mutex->blocked_queue = (queue){0};
    mutex->owner = NULL;
    mutex->initialized = 1;
    return 0;
}

int worker_mutex_lock(worker_mutex_t *mutex) {
    if (!mutex || !mutex->initialized) return EINVAL;
    sigset_t oldmask;
    block_timer(&oldmask);
    tcb *self = is_scheduler_initialized ? current_thread : &main_tcb_data;
    if (mutex->owner == self) {
        restore_mask(&oldmask);
        return EDEADLK;
    }
    while (atomic_flag_test_and_set(&mutex->flag)) {
        if (would_deadlock(mutex->owner)) {
            restore_mask(&oldmask);
            return EDEADLK;
        }
        self->status = BLOCKED;
        self->waiting_on = mutex->owner;
        enqueue(&mutex->blocked_queue, self);
        switch_to_scheduler();
    }
    mutex->owner = self;
    restore_mask(&oldmask);
    return 0;
}

int worker_mutex_unlock(worker_mutex_t *mutex) {
    if (!mutex || !mutex->initialized) return EINVAL;
    sigset_t oldmask;
    block_timer(&oldmask);
    tcb *self = is_scheduler_initialized ? current_thread : &main_tcb_data;
    if (mutex->owner != self) {
        restore_mask(&oldmask);
        return EPERM;
    }
    mutex->owner = NULL;
    atomic_flag_clear(&mutex->flag);
    /* Waiters no longer depend on the old owner, which may now exit. */
    for (tcb *waiting = mutex->blocked_queue.head; waiting; waiting = waiting->next) {
        waiting->waiting_on = NULL;
    }
    tcb *waiting = dequeue(&mutex->blocked_queue);
    if (waiting) ready_thread(waiting);
    restore_mask(&oldmask);
    return 0;
}

int worker_mutex_destroy(worker_mutex_t *mutex) {
    if (!mutex || !mutex->initialized) return EINVAL;
    sigset_t oldmask;
    block_timer(&oldmask);
    int error = 0;
    if (mutex->owner || mutex->blocked_queue.head) error = EBUSY;
    else mutex->initialized = 0;
    restore_mask(&oldmask);
    return error;
}

#ifdef PSJF
static tcb *sched_psjf(void) {
    tcb *shortest = run_queue.head;
    if (!shortest) return NULL;
    tcb *prev_shortest = NULL;
    tcb *prev = shortest;
    for (tcb *thread = shortest->next; thread; thread = thread->next) {
        if (thread->elapsed_quantums < shortest->elapsed_quantums) {
            shortest = thread;
            prev_shortest = prev;
        }
        prev = thread;
    }
    current_slice_us = QUANTUM * 1000L;
    return remove_thread(&run_queue, shortest, prev_shortest);
}
#endif

#ifdef MLFQ
static void boost_priorities(void) {
    long now = time_us(CLOCK_MONOTONIC);
    if (now - last_boost_time < S_PROMOTION_PERIOD * 1000L) return;
    last_boost_time = now;
    for (tcb *thread = all_threads_list_head; thread; thread = thread->next_global) {
        thread->mlfq_level = 0;
        thread->level_runtime = 0;
    }
    for (int level = 1; level < NUM_MLFQ_LEVELS; level++) {
        tcb *thread;
        while ((thread = dequeue(&mlfq_queues[level]))) enqueue(&mlfq_queues[0], thread);
    }
}

static tcb *sched_mlfq(void) {
    boost_priorities();
    for (int level = 0; level < NUM_MLFQ_LEVELS; level++) {
        tcb *thread = dequeue(&mlfq_queues[level]);
        if (thread) {
            current_slice_us = QUANTUM * 1000L * (1L << level) - thread->level_runtime;
            return thread;
        }
    }
    return NULL;
}
#endif

#ifdef CFS
static tcb *sched_cfs(void) {
    if (heap_is_empty(&cfs_run_queue)) return NULL;
    current_slice_us = TARGET_LATENCY * 1000L / cfs_run_queue.size;
    if (current_slice_us < MIN_SCHED_GRN * 1000L) current_slice_us = MIN_SCHED_GRN * 1000L;
    return heap_extract_min(&cfs_run_queue);
}
#endif

static void schedule(void) {
    stop_timer();
    tcb *previous = current_thread;
    long elapsed = time_us(CLOCK_PROCESS_CPUTIME_ID) - last_scheduled_cpu;
    previous->vruntime += elapsed;
    previous->elapsed_quantums = previous->vruntime / (QUANTUM * 1000L);
#ifdef MLFQ
    previous->level_runtime += elapsed;
    long allotment = QUANTUM * 1000L * (1L << previous->mlfq_level);
    while (previous->level_runtime >= allotment) {
        previous->level_runtime -= allotment;
        if (previous->mlfq_level < NUM_MLFQ_LEVELS - 1) previous->mlfq_level++;
        allotment = QUANTUM * 1000L * (1L << previous->mlfq_level);
    }
#endif
    if (previous->status == RUNNING || previous->status == READY) ready_thread(previous);
    if (previous->status == TERMINATED && previous->tid != 0) {
        total_threads_completed++;
        total_turnaround_time += time_us(CLOCK_MONOTONIC) - previous->start_time;
        total_response_time += previous->first_schedule_time - previous->start_time;
        avg_turn_time = total_turnaround_time / total_threads_completed;
        avg_resp_time = total_response_time / total_threads_completed;
    }
    tcb *next;
#ifdef PSJF
    next = sched_psjf();
#elif defined(MLFQ)
    next = sched_mlfq();
#else
    next = sched_cfs();
#endif
    if (!next) {
        for (tcb *thread = all_threads_list_head; thread; thread = thread->next_global) {
            if (thread->status != TERMINATED) {
                const char message[] = "worker: deadlock, no runnable threads\n";
                write(STDERR_FILENO, message, sizeof(message) - 1);
                _Exit(EXIT_FAILURE);
            }
        }
        exit(0);
    }
    if (next->is_first_run) {
        next->first_schedule_time = time_us(CLOCK_MONOTONIC);
        next->is_first_run = 0;
    }
    next->status = RUNNING;
    current_thread = next;
    last_scheduled_cpu = time_us(CLOCK_PROCESS_CPUTIME_ID);
    timer_for_slice_setup(current_slice_us);
    /* setcontext restores the destination mask atomically with its registers. */
    errno = next->saved_errno;
    if (setcontext(&next->context) == -1) abort();
}

void print_app_stats(void) {
    fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
    fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
    fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}
// Helper function to add thread to tail of a queue
static void enqueue(queue* q, tcb* thread) {
	if (thread == NULL) return;
	thread->next = NULL;

	if (q->tail == NULL) {
		q->head = thread;
		q->tail = thread;
	} else {
		q->tail->next = thread;
		q->tail = thread;
	}
}

// Helper function to remove a thread from the head of a queue
static tcb* dequeue(queue* q) {
	if (q->head == NULL) {
		return NULL;
	}

	tcb* thread = q->head;
	q->head = thread->next;

	if (q->head == NULL) {
		q->tail = NULL;
	}

	thread->next = NULL;
	return thread;
}


#ifdef CFS
static int heap_parent(int i) { return (i - 1) / 2; }
static int heap_left(int i) { return 2 * i + 1; }
static int heap_right(int i) { return 2 * i + 2; }

static void heap_swap(tcb** a, tcb** b) {
	tcb* temp = *a;
	*a = *b;
	*b = temp;
}

static int heap_init(cfs_heap* h, int capacity) {
	h->threads = (tcb**) malloc(capacity * sizeof(tcb*));
	if (h->threads == NULL) {
		// This is a catastrophic failure
		return ENOMEM; 
	}
	h->size = 0;
	h->capacity = capacity;
	return 0;
}

static int heap_grow(cfs_heap* h) {
	if (h->capacity > INT_MAX / 2) return ENOMEM;
	int new_capacity = h->capacity * 2;
	tcb** new_threads = (tcb**) realloc(h->threads, new_capacity * sizeof(tcb*));
	if (new_threads == NULL) {
		return ENOMEM; 
	}
	h->threads = new_threads;
	h->capacity = new_capacity;
	return 0;
}

static void heapify_up(cfs_heap* h, int index) {
	if (index == 0) return;
	
	int p = heap_parent(index);
	// Min-heap property: parent's vruntime <= child's vruntime
	if (h->threads[p]->vruntime > h->threads[index]->vruntime) {
		heap_swap(&h->threads[p], &h->threads[index]);
		heapify_up(h, p);
	}
}

static void heap_insert(cfs_heap* h, tcb* thread) {
	/* Capacity for every live TCB is reserved by worker_create. */
	assert(h->size < h->capacity);

	// Insert new thread at the end
	h->threads[h->size] = thread;
	int index = h->size;
	h->size++;
	
	// Bubble up to maintain heap property
	heapify_up(h, index);
}

static void heapify_down(cfs_heap* h, int index) {
	int left = heap_left(index);
	int right = heap_right(index);
	int smallest = index;
	
	// Find the smallest among root, left child, and right child
	if (left < h->size && h->threads[left]->vruntime < h->threads[smallest]->vruntime) {
		smallest = left;
	}
	if (right < h->size && h->threads[right]->vruntime < h->threads[smallest]->vruntime) {
		smallest = right;
	}
	
	// If smallest is not the root, swap and recursively heapify down
	if (smallest != index) {
		heap_swap(&h->threads[index], &h->threads[smallest]);
		heapify_down(h, smallest);
	}
}

static tcb* heap_extract_min(cfs_heap* h) {
	if (h->size == 0) {
		return NULL;
	}
	
	// The min element is always at the root
	tcb* min_thread = h->threads[0];
	
	// Move the last element to the root
	h->threads[0] = h->threads[h->size - 1];
	h->size--;
	
	// Bubble down to maintain heap property
	if (h->size > 0) {
		heapify_down(h, 0);
	}
	
	return min_thread;
}

static int heap_is_empty(cfs_heap* h) {
	return h->size == 0;
}


#endif
#ifdef PSJF
static tcb* remove_thread(queue* q, tcb* thread_to_remove, tcb* prev) {
	if (thread_to_remove == NULL) return NULL;

	if (thread_to_remove == q->head) {
		return dequeue(q);
	}

	if (prev != NULL) {
		prev->next = thread_to_remove->next;

		if (thread_to_remove == q->tail) {
			q->tail = prev;
		}

		thread_to_remove->next = NULL;

		return thread_to_remove;
	}

	// This case (prev == NULL but thread != q->head) is impossible
	// But for safety, we'll traverse.
	tcb* current = q->head;
	while (current != NULL && current->next != thread_to_remove) {
		current = current->next;
	}
	if (current != NULL) {
		return remove_thread(q, thread_to_remove, current);
	}

	return NULL; // Thread not found attached to a prev
}


#endif

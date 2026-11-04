#include "../thread-worker.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static worker_mutex_t mutex;
static volatile sig_atomic_t first_started;
static volatile sig_atomic_t second_started;
static long counter;
static worker_t self_tid;
static worker_t cycle_threads[2];
static volatile sig_atomic_t cycle_ready;
static volatile sig_atomic_t waiter_started;

static long cpu_ms(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) == 0);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000;
}

static void check_mask(void) {
    sigset_t mask;
    assert(sigprocmask(SIG_SETMASK, NULL, &mask) == 0);
    assert(sigismember(&mask, SIGPROF) == 0);
}

static void *return_arg(void *arg) {
    check_mask();
    errno = EDOM;
    assert(worker_yield() == 0);
    assert(errno == EDOM);
    check_mask();
    return arg;
}

static void *exit_arg(void *arg) {
    worker_exit(arg);
    return NULL;
}

static void *spin_first(void *arg) {
    (void)arg;
    check_mask();
    first_started = 1;
    long start = cpu_ms();
    while (!second_started && cpu_ms() - start < 2000) {}
    assert(second_started);
    return NULL;
}

static void *spin_second(void *arg) {
    (void)arg;
    check_mask();
    second_started = 1;
    long start = cpu_ms();
    while (!first_started && cpu_ms() - start < 2000) {}
    assert(first_started);
    return NULL;
}

static void *increment(void *arg) {
    (void)arg;
    for (int i = 0; i < 200; i++) {
        assert(worker_mutex_lock(&mutex) == 0);
        long value = counter;
        assert(worker_yield() == 0);
        counter = value + 1;
        assert(worker_mutex_unlock(&mutex) == 0);
    }
    return NULL;
}

static void *join_self(void *arg) {
    (void)arg;
    assert(worker_join(self_tid, NULL) == EDEADLK);
    return NULL;
}

static void *unlock_other(void *arg) {
    (void)arg;
    assert(worker_mutex_unlock(&mutex) == EPERM);
    return NULL;
}

static void *create_child(void *arg) {
    worker_t child;
    void *result = NULL;
    assert(worker_create(&child, NULL, return_arg, arg) == 0);
    assert(worker_join(child, &result) == 0);
    assert(result == arg);
    return result;
}

static void *cycle_first(void *arg) {
    (void)arg;
    while (!cycle_ready) worker_yield();
    assert(worker_join(cycle_threads[1], NULL) == 0);
    return NULL;
}

static void *cycle_second(void *arg) {
    (void)arg;
    while (!cycle_ready) worker_yield();
    /* First waits on second, so joining first would complete a cycle. */
    assert(worker_join(cycle_threads[0], NULL) == EDEADLK);
    return NULL;
}

static void *handoff_waiter(void *arg) {
    (void)arg;
    waiter_started = 1;
    assert(worker_mutex_lock(&mutex) == 0);
    assert(worker_mutex_unlock(&mutex) == 0);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    worker_t threads[100];
    int values[100];
    if (!strcmp(argv[1], "returns")) {
        for (int i = 0; i < 100; i++) {
            values[i] = i;
            assert(worker_create(&threads[i], NULL, return_arg, &values[i]) == 0);
        }
        for (int i = 99; i >= 0; i--) {
            void *result = NULL;
            assert(worker_join(threads[i], &result) == 0);
            assert(result == &values[i]);
            assert(worker_join(threads[i], NULL) == ESRCH);
        }
        assert(worker_create(&threads[0], NULL, exit_arg, &values[0]) == 0);
        void *result = NULL;
        assert(worker_join(threads[0], &result) == 0);
        assert(result == &values[0]);
    } else if (!strcmp(argv[1], "preemption")) {
        assert(worker_create(&threads[0], NULL, spin_first, NULL) == 0);
        assert(worker_create(&threads[1], NULL, spin_second, NULL) == 0);
        /* Main must also be preemptible and charged for its CPU usage. */
        long start = cpu_ms();
        while ((!first_started || !second_started) && cpu_ms() - start < 4000) {}
        assert(first_started && second_started);
        assert(worker_join(threads[0], NULL) == 0);
        assert(worker_join(threads[1], NULL) == 0);
    } else if (!strcmp(argv[1], "mutex")) {
        assert(worker_mutex_init(&mutex, NULL) == 0);
        assert(worker_mutex_lock(&mutex) == 0);
        assert(worker_mutex_destroy(&mutex) == EBUSY);
        assert(worker_mutex_lock(&mutex) == EDEADLK);
        assert(worker_create(&threads[0], NULL, unlock_other, NULL) == 0);
        assert(worker_join(threads[0], NULL) == 0);
        assert(worker_mutex_unlock(&mutex) == 0);
        for (int i = 0; i < 8; i++) assert(worker_create(&threads[i], NULL, increment, NULL) == 0);
        for (int i = 0; i < 8; i++) assert(worker_join(threads[i], NULL) == 0);
        assert(counter == 1600);
        assert(worker_mutex_destroy(&mutex) == 0);
        assert(worker_mutex_lock(&mutex) == EINVAL);
    } else if (!strcmp(argv[1], "joins")) {
        assert(worker_create(NULL, NULL, return_arg, NULL) == EINVAL);
        assert(worker_create(&threads[0], NULL, NULL, NULL) == EINVAL);
        assert(worker_yield() == 0);
        assert(worker_create(&self_tid, NULL, join_self, NULL) == 0);
        assert(worker_join(self_tid, NULL) == 0);
        assert(worker_create(&threads[0], NULL, create_child, &values[0]) == 0);
        void *result = NULL;
        assert(worker_join(threads[0], &result) == 0);
        assert(result == &values[0]);
    } else if (!strcmp(argv[1], "cycle")) {
        assert(worker_create(&cycle_threads[0], NULL, cycle_first, NULL) == 0);
        assert(worker_create(&cycle_threads[1], NULL, cycle_second, NULL) == 0);
        cycle_ready = 1;
        assert(worker_join(cycle_threads[0], NULL) == 0);
    } else if (!strcmp(argv[1], "handoff")) {
        assert(worker_mutex_init(&mutex, NULL) == 0);
        assert(worker_mutex_lock(&mutex) == 0);
        assert(worker_create(&threads[0], NULL, handoff_waiter, NULL) == 0);
        while (!waiter_started) worker_yield();
        assert(worker_mutex_unlock(&mutex) == 0);
        /* The awakened waiter already owns the mutex before being scheduled. */
        assert(worker_mutex_destroy(&mutex) == EBUSY);
        assert(worker_join(threads[0], NULL) == 0);
        assert(worker_mutex_destroy(&mutex) == 0);
    } else {
        return 2;
    }
    check_mask();
    print_app_stats();
    puts("PASS");
    return 0;
}

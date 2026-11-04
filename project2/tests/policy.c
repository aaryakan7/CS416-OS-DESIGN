/* Include the implementation to check queue/accounting invariants directly. */
#include "../thread-worker.c"

int main(void) {
    tcb threads[100] = {0};
#ifdef CFS
    assert(heap_init(&cfs_run_queue, HEAP_DEFAULT_CAPACITY) == 0);
    assert(heap_grow(&cfs_run_queue) == 0);
    for (int i = 0; i < 100; i++) {
        threads[i].vruntime = 99 - i;
        ready_thread(&threads[i]);
    }
    for (int i = 0; i < 100; i++) {
        tcb *thread = sched_cfs();
        assert(thread->vruntime == i);
        long expected = TARGET_LATENCY * 1000L / (100 - i);
        if (expected < MIN_SCHED_GRN * 1000L) expected = MIN_SCHED_GRN * 1000L;
        assert(current_slice_us == expected);
    }
    free(cfs_run_queue.threads);
#elif defined(MLFQ)
    for (int i = 0; i < 3; i++) {
        threads[i].mlfq_level = i;
        threads[i].level_runtime = 100;
        threads[i].next_global = i < 2 ? &threads[i + 1] : NULL;
    }
    all_threads_list_head = threads;
    ready_thread(&threads[0]);
    ready_thread(&threads[1]);
    threads[2].status = BLOCKED;
    last_boost_time = time_us(CLOCK_MONOTONIC);
    assert(sched_mlfq() == &threads[0]);
    assert(current_slice_us == QUANTUM * 1000L - 100);
    threads[0].status = RUNNING;
    last_boost_time -= S_PROMOTION_PERIOD * 1000L;
    boost_priorities();
    for (int i = 0; i < 3; i++) {
        assert(threads[i].mlfq_level == 0);
        assert(threads[i].level_runtime == 0);
    }
    assert(sched_mlfq() == &threads[1]);
#else
    threads[0].elapsed_quantums = 4;
    threads[1].elapsed_quantums = 1;
    threads[2].elapsed_quantums = 1;
    for (int i = 0; i < 3; i++) ready_thread(&threads[i]);
    assert(sched_psjf() == &threads[1]);
    assert(sched_psjf() == &threads[2]);
    assert(sched_psjf() == &threads[0]);
#endif
    puts("PASS policy invariants");
    return 0;
}

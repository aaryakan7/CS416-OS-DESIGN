# Project 2 — User-level threads and scheduling

A pthread-style thread library built with `ucontext`, `SIGPROF`, and `ITIMER_PROF`. All worker threads share one kernel thread.

## Build and test

Requires Linux, GCC or Clang, Make, and Python 3. GitHub Actions runs the tests on Ubuntu, so an iLab account is unnecessary.

```sh
make                         # PSJF by default
make test
make SCHED=MLFQ test
make SCHED=CFS test
make SCHED=CFS benchmarks
cd benchmarks
./parallel_cal 4
./vector_multiply 4
./genRecord.sh
./external_cal 4
```

To compare native pthreads, run `make -C benchmarks USE_WORKERS=0` and run the same executables. Rebuilding selects the requested backend.

## Scheduling

- PSJF selects the worker with the least accumulated CPU time, measured in quanta.
- MLFQ uses four priority queues, tracks cumulative CPU use across yields and blocking, and periodically boosts all threads. Lower levels receive longer slices.
- CFS uses the original binary min-heap to select the lowest virtual runtime. Slices use the current runnable count and a minimum granularity. This implementation retains the min-heap instead of the assignment's min-max heap because scheduling only needs the minimum.

`QUANTUM`, `TARGET_LATENCY`, `MIN_SCHED_GRN`, and `S_PROMOTION_PERIOD` are in milliseconds and can be overridden at compilation. Response and turnaround statistics are in microseconds. Context-switch statistics count transfers into the scheduler, including voluntary yields and blocking.

Normal function returns and explicit `worker_exit` both preserve the return value for `worker_join`. Join frees the worker's stack and control block. Each thread supports one joiner. APIs return zero on success or an error number such as `EINVAL`, `ESRCH`, or `EDEADLK`.

## Limits

This is an educational Linux library. Blocking I/O blocks the entire process. It owns `SIGPROF` and `ITIMER_PROF`, and should be used from one native thread. Application calls to non-reentrant library routines need serialization across workers because timer preemption can interrupt them. The scheduler stack and heap remain allocated for the process lifetime. An unrecoverable state with blocked threads and no runnable thread terminates with a deadlock diagnostic.

The benchmarks compare threaded and serial sums, reject invalid thread counts, and use unsigned 64-bit arithmetic. Tests cover timer preemption, worker return values, nested creation, mutex contention, heap growth, and scheduling invariants.

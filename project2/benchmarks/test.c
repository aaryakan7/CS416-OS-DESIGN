#include "benchmark.h"
#include "../thread-worker.h"

static void *echo(void *arg) { return arg; }
int main(void) {
    pthread_t thread;
    int value = 416;
    void *result = NULL;
    check_thread(pthread_create(&thread, NULL, echo, &value));
    check_thread(pthread_join(thread, &result));
    if (result != &value) return EXIT_FAILURE;
    puts("Thread return value verified");
    return 0;
}

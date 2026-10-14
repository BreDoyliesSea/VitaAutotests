// The order threads pinned to one core run in: same priority threads started or woken one after
// another, threads of different priorities, a higher priority thread that a running one wakes or
// starts, delays of 0 and 1 microseconds, and a lower priority thread while a higher one spins
// without syscalls. Every thread is pinned to core 0, below the main thread, which only blocks once
// they are all set up. The threads record their steps as small numbers, printed in the order they
// were recorded, and nothing they do depends on time.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// The main thread runs above every test thread
#define MAIN_PRIORITY 0x60
#define HIGH_PRIORITY 0x80
#define PRIORITY 0x90
#define LOW_PRIORITY 0x98
#define MAX_STEPS 16
#define SPINS 10000000

static SceUID done, wake[3], high_wake;
static SceUID high_thread;
static int step_count, steps[MAX_STEPS], finished, expected_finishes;
static volatile int low_ran, delay_result;

// Records one step
static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

// Marks a test thread as finished, and wakes the main thread once all of them are
static void finish(void) {
    if (__atomic_add_fetch(&finished, 1, __ATOMIC_SEQ_CST) == expected_finishes)
        sceKernelSignalSema(done, 1);
}

static int id_of(void *argp) {
    return *(const int *)argp;
}

// Records its number
static int record_thread(SceSize args, void *argp) {
    (void)args;
    record(id_of(argp));
    finish();
    return 0;
}

// Waits for its own semaphore, then records its number
static int woken_thread(SceSize args, void *argp) {
    (void)args;
    const int id = id_of(argp);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(wake[id], 1, &timeout);
    record(id);
    finish();
    return 0;
}

// Records 0, wakes the high priority thread, then records 1
static int waking_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(0);
    sceKernelSignalSema(high_wake, 1);
    record(1);
    finish();
    return 0;
}

// Waits to be woken, then records its number
static int high_woken_thread(SceSize args, void *argp) {
    (void)args;
    const int id = id_of(argp);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(high_wake, 1, &timeout);
    record(id);
    finish();
    return 0;
}

// Records 0, starts the high priority thread, then records 2
static int starting_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(0);
    int id = 1;
    sceKernelStartThread(high_thread, sizeof(id), &id);
    record(2);
    finish();
    return 0;
}

// Records 0, delays for as long as its argument says, then records 1
static int delaying_thread(SceSize args, void *argp) {
    (void)args;
    const int us = id_of(argp);
    record(0);
    delay_result = sceKernelDelayThread(us);
    record(1);
    finish();
    return 0;
}

// Records 0, spins until the low priority thread ran or the loop ends, then records 1 if that
// thread didn't run during the spin, or 9 if it did
static int spinning_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(0);
    for (volatile int n = 0; n < SPINS && !low_ran; n++)
        ;
    record(low_ran ? 9 : 1);
    finish();
    return 0;
}

// Notes that it ran, then records 2
static int low_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    low_ran = 1;
    record(2);
    finish();
    return 0;
}

static SceUID create(SceKernelThreadEntry entry, int priority) {
    return sceKernelCreateThread("order", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
}

static void start(SceUID thread, int id) {
    sceKernelStartThread(thread, sizeof(id), &id);
}

// Starts counting a case that ends after count thread finishes
static void begin(int count) {
    step_count = 0;
    finished = 0;
    expected_finishes = count;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
}

// Blocks the main thread until every test thread finished, then joins them
static void run(SceUID *threads, int count) {
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitSema(done, 1, &timeout);
    for (int i = 0; i < count; i++) {
        timeout = TEST_WAIT_US;
        sceKernelWaitThreadEnd(threads[i], NULL, &timeout);
        sceKernelDeleteThread(threads[i]);
    }
}

static void print_steps(const char *name) {
    const int count = step_count < MAX_STEPS ? step_count : MAX_STEPS;
    PRINTF(step_count, "%s: steps recorded", name);
    for (int i = 0; i < count; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    done = sceKernelCreateSema("done", 0, 0, 1, NULL);
    high_wake = sceKernelCreateSema("high wake", 0, 0, 1, NULL);
    for (int i = 0; i < 3; i++)
        wake[i] = sceKernelCreateSema("wake", 0, 0, 1, NULL);
    SceUID threads[4];

    // Same priority threads created in the order 0, 1, 2 and started in the order 2, 0, 1
    begin(3);
    for (int i = 0; i < 3; i++)
        threads[i] = create(record_thread, PRIORITY);
    start(threads[2], 2);
    start(threads[0], 0);
    start(threads[1], 1);
    run(threads, 3);
    print_steps("same priority, started in the order 2, 0, 1");

    // Same priority threads waiting on their own semaphores, woken in the order 1, 2, 0
    begin(3);
    for (int i = 0; i < 3; i++) {
        threads[i] = create(woken_thread, PRIORITY);
        start(threads[i], i);
        test_wait_until_waiting(threads[i]);
    }
    sceKernelSignalSema(wake[1], 1);
    sceKernelSignalSema(wake[2], 1);
    sceKernelSignalSema(wake[0], 1);
    run(threads, 3);
    print_steps("same priority, woken in the order 1, 2, 0");

    // Threads 0, 1 and 2 started in that order at a low, a high and a middle priority
    begin(3);
    threads[0] = create(record_thread, LOW_PRIORITY);
    threads[1] = create(record_thread, HIGH_PRIORITY);
    threads[2] = create(record_thread, PRIORITY);
    for (int i = 0; i < 3; i++)
        start(threads[i], i);
    run(threads, 3);
    print_steps("started at a low, a high and a middle priority");

    // A running thread wakes a higher priority one, with a thread of its own priority ready behind
    // it. The woken one records 2 and the one behind records 3.
    begin(3);
    threads[0] = create(high_woken_thread, HIGH_PRIORITY);
    start(threads[0], 2);
    test_wait_until_waiting(threads[0]);
    threads[1] = create(waking_thread, PRIORITY);
    threads[2] = create(record_thread, PRIORITY);
    start(threads[1], 0);
    start(threads[2], 3);
    run(threads, 3);
    print_steps("a running thread wakes a higher priority one");

    // A running thread starts a higher priority one, which records 1, with a thread of its own
    // priority ready behind it, which records 3
    begin(3);
    high_thread = create(record_thread, HIGH_PRIORITY);
    threads[0] = create(starting_thread, PRIORITY);
    threads[1] = create(record_thread, PRIORITY);
    threads[2] = high_thread;
    start(threads[0], 0);
    start(threads[1], 3);
    run(threads, 3);
    print_steps("a running thread starts a higher priority one");

    // A thread delays, with a thread of its own priority ready behind it, which records 2
    const int delays[] = { 0, 1 };
    for (int d = 0; d < 2; d++) {
        begin(2);
        delay_result = 0xDEAD;
        threads[0] = create(delaying_thread, PRIORITY);
        threads[1] = create(record_thread, PRIORITY);
        start(threads[0], delays[d]);
        start(threads[1], 2);
        run(threads, 2);
        PRINTF(delay_result, "a delay of %d: its result", delays[d]);
        char name[48];
        sceClibSnprintf(name, sizeof(name), "a delay of %d", delays[d]);
        print_steps(name);
    }

    // A thread spins without syscalls, with a lower priority thread ready
    begin(2);
    low_ran = 0;
    threads[0] = create(low_thread, LOW_PRIORITY);
    threads[1] = create(spinning_thread, PRIORITY);
    start(threads[0], 0);
    start(threads[1], 0);
    run(threads, 2);
    print_steps("a higher priority thread spins");

    test_exit();
    return 0;
}

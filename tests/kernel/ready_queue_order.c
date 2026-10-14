// Where a thread goes in its core's ready queue among threads of its priority: started or woken
// threads in the order it happened, a ready thread whose priority is changed to the value it has,
// one raised and lowered back, one lowered and raised back, and a running thread preempted by a
// higher priority one, against a thread that was ready behind it. The main thread runs on core 1
// and the test threads on core 0. For all but the last case, a high priority gate thread spins on
// core 0 without syscalls while the main thread sets the queue up, so nothing else runs there until
// it lets go. Each thread records its number when it runs, printed in the order recorded.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define HIGH_PRIORITY 0x80
#define PRIORITY 0xA0
#define MAX_STEPS 8
// A few seconds on a Vita at most, the gate normally lets go long before
#define SPINS 400000000

static SceUID wake[3];
static volatile int gate_up, gate_open, spinning, high_done;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static int id_of(void *argp) {
    return *(const int *)argp;
}

static int record_entry(SceSize args, void *argp) {
    (void)args;
    record(id_of(argp));
    return 0;
}

// Waits on its own semaphore, then records its number
static int woken_entry(SceSize args, void *argp) {
    (void)args;
    const int id = id_of(argp);
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(wake[id], 1, &timeout);
    record(id);
    return 0;
}

static SceUID create(SceKernelThreadEntry entry, int priority) {
    return sceKernelCreateThread("order", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
}

static void start(SceUID thread, int id) {
    sceKernelStartThread(thread, sizeof(id), &id);
}

static SceUID gate;

static void begin(void) {
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
}

static void gate_close(void) {
    gate_up = gate_open = 0;
    gate = create(gate_entry, GATE_PRIORITY);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

static void join(SceUID *threads, int count) {
    for (int i = 0; i < count; i++) {
        SceUInt timeout = 3 * TEST_WAIT_US;
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

// Thread 0 is started and thread 1 woken, in the given order, behind the gate
static void start_and_wake(int wake_first, const char *name) {
    begin();
    SceUID threads[2];
    threads[1] = create(woken_entry, PRIORITY);
    start(threads[1], 1);
    test_wait_until_waiting(threads[1]);
    threads[0] = create(record_entry, PRIORITY);
    gate_close();
    if (wake_first) {
        sceKernelSignalSema(wake[1], 1);
        start(threads[0], 0);
    } else {
        start(threads[0], 0);
        sceKernelSignalSema(wake[1], 1);
    }
    gate_lift();
    join(threads, 2);
    print_steps(name);
}

// Threads 0, 1 and 2 are started in that order behind the gate, then thread 0's priority is
// changed to first, then to second
static void change_ready(int first, int second, const char *name) {
    begin();
    SceUID threads[3];
    for (int i = 0; i < 3; i++)
        threads[i] = create(record_entry, PRIORITY);
    gate_close();
    for (int i = 0; i < 3; i++)
        start(threads[i], i);
    const int r1 = sceKernelChangeThreadPriority(threads[0], first);
    const int r2 = second ? sceKernelChangeThreadPriority(threads[0], second) : 0;
    gate_lift();
    join(threads, 3);
    PRINTF(r1 | r2, "%s: the changes", name);
    print_steps(name);
}

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(0);
    spinning = 1;
    for (volatile int n = 0; n < SPINS && !high_done; n++)
        ;
    record(2);
    return 0;
}

static int high_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(wake[0], 1, &timeout);
    record(1);
    high_done = 1;
    return 0;
}

// Thread 0 runs and spins, thread 3 is ready behind it at the same priority, and a higher
// priority thread, which records 1, takes the core. Thread 0 records 2 once it runs again.
static void preempted(void) {
    begin();
    spinning = high_done = 0;
    SceUID threads[3];
    threads[0] = create(high_entry, HIGH_PRIORITY);
    sceKernelStartThread(threads[0], 0, NULL);
    test_wait_until_waiting(threads[0]);
    threads[1] = create(spinner_entry, PRIORITY);
    sceKernelStartThread(threads[1], 0, NULL);
    TEST_WAIT_FOR(spinning);
    threads[2] = create(record_entry, PRIORITY);
    start(threads[2], 3);
    sceKernelSignalSema(wake[0], 1);
    join(threads, 3);
    print_steps("a preempted thread against one ready behind it");
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (int i = 0; i < 3; i++)
        wake[i] = sceKernelCreateSema("wake", 0, 0, 1, NULL);

    start_and_wake(0, "thread 0 started, then thread 1 woken");
    start_and_wake(1, "thread 1 woken, then thread 0 started");
    change_ready(PRIORITY, 0, "thread 0 changed to its own priority");
    change_ready(HIGH_PRIORITY, PRIORITY, "thread 0 raised and lowered back");
    change_ready(PRIORITY + 0x10, PRIORITY, "thread 0 lowered and raised back");
    preempted();

    for (int i = 0; i < 3; i++)
        sceKernelDeleteSema(wake[i]);
    test_exit();
    return 0;
}

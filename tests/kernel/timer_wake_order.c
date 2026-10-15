// Where a thread made ready by a timer goes in its core's ready queue, at priorities 0x40 to
// 0x7F, where a start makes it STANDBY in the queue of its core (wake_ready_areas). All threads
// run on core 0, behind a gate thread of the highest priority that spins there without syscalls
// until the main thread, on core 1, lifts it. Each case prints the status of every thread just
// before the gate lifts, and the order they run in after it:
// - two started, then a timeout: U1 and U2 at 0x70 are started, then T's 100 ms wait at 0x70
//   times out
// - a higher priority started, then a timeout: V at 0x60 is started, then T's wait at 0x70 times
//   out
// - two timeouts: U at 0x70 is started, then T1's 100 ms and T2's 200 ms waits at 0x70 time out
// - a delay end, then a start: U at 0x70 is started, T's 100 ms delay at 0x70 ends, then W at
//   0x70 is started
// - a timeout, then a higher priority start: T's wait at 0x70 times out, then V at 0x60 is
//   started
//
// On a PS Vita TV on firmware 3.65 a thread made ready by a timer reports READY and runs before
// every STANDBY thread of its priority, whether they were ready before it or after, and threads
// made ready by timers run in the order their timers ended. A STANDBY thread of a higher
// priority still runs first. That fits a timer putting the thread in the ready queue the cores
// share, and a core taking that queue before its own one when their best threads have the same
// priority.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000
#define MAX_THREADS 3

static SceUID sema, gate;
static volatile int gate_up, gate_open, ran;
static volatile int order[MAX_THREADS];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static void gate_close(void) {
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

// What a thread does before it records that it ran
enum Kind { RUN, TIMEOUT_100, TIMEOUT_200, DELAY_100 };

struct Arg {
    int id, kind;
};

static int thread_entry(SceSize args, void *argp) {
    (void)args;
    const struct Arg arg = *(const struct Arg *)argp;
    SceUInt32 timeout = arg.kind == TIMEOUT_200 ? 200000 : 100000;
    if (arg.kind == TIMEOUT_100 || arg.kind == TIMEOUT_200)
        sceKernelWaitSema(sema, 1, &timeout);
    else if (arg.kind == DELAY_100)
        sceKernelDelayThread(100000);
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = arg.id;
    return 0;
}

static SceUID start(int id, int priority, int kind) {
    const SceUID thread = sceKernelCreateThread("t", thread_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    struct Arg arg = { id, kind };
    sceKernelStartThread(thread, sizeof(arg), &arg);
    return thread;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

// A thread of the case: its name, priority, what it does, and whether it starts before the gate
struct Step {
    const char *name;
    int priority, kind;
};

struct Case {
    const char *name;
    // Threads that wait or delay start before the gate, the others after it, in this order
    struct Step steps[MAX_THREADS];
    int count;
};

static const struct Case cases[] = {
    { "two started, then a timeout", { { "U1", 0x70, RUN }, { "U2", 0x70, RUN }, { "T", 0x70, TIMEOUT_100 } }, 3 },
    { "a higher priority started, then a timeout", { { "V", 0x60, RUN }, { "T", 0x70, TIMEOUT_100 } }, 2 },
    { "two timeouts", { { "U", 0x70, RUN }, { "T1", 0x70, TIMEOUT_100 }, { "T2", 0x70, TIMEOUT_200 } }, 3 },
    { "a delay end, then a start", { { "U", 0x70, RUN }, { "T", 0x70, DELAY_100 }, { "W", 0x70, RUN } }, 3 },
    { "a timeout, then a higher priority start", { { "T", 0x70, TIMEOUT_100 }, { "V", 0x60, RUN } }, 2 },
};

static void run_case(const struct Case *c) {
    ran = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        order[i] = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUID threads[MAX_THREADS];

    // The threads that wait first get into their waits before the gate closes
    for (int i = 0; i < c->count; i++) {
        if (c->steps[i].kind == RUN)
            continue;
        threads[i] = start(i + 1, c->steps[i].priority, c->steps[i].kind);
        test_wait_until_waiting(threads[i]);
    }
    gate_close();
    // The others start in their order, around the timers: the ones listed before the first timer
    // start before it ends, the ones after it once every timer has ended
    int timers_done = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->steps[i].kind != RUN)
            continue;
        int after_timer = 0;
        for (int j = 0; j < i; j++)
            after_timer |= c->steps[j].kind != RUN;
        if (after_timer && !timers_done) {
            sceKernelDelayThread(400000);
            timers_done = 1;
        }
        threads[i] = start(i + 1, c->steps[i].priority, RUN);
    }
    if (!timers_done)
        sceKernelDelayThread(400000);
    sceKernelDelayThread(10000);

    SceUInt32 statuses[MAX_THREADS];
    for (int i = 0; i < c->count; i++)
        statuses[i] = status_of(threads[i]);
    gate_lift();
    TEST_WAIT_FOR(ran == c->count);
    for (int i = 0; i < c->count; i++)
        PRINTF(statuses[i], "%s: %s's status before the gate lifts", c->name, c->steps[i].name);
    for (int i = 0; i < c->count; i++)
        PRINTF(order[i], "%s: run %d, by step", c->name, i + 1);
    for (int i = 0; i < c->count; i++)
        test_join_thread(threads[i]);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);
    test_exit();
    return 0;
}

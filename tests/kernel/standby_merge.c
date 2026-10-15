// When a core takes the threads that wait on it as STANDBY into its ready queue. All threads run
// at 0x70 on core 0, behind a gate thread of the highest priority that spins there until the
// main thread, on core 1, lifts it. A thread started from core 1 waits as STANDBY, and one the
// gate signals from core 0 is READY (wake_core_origin). Each case prints every status just
// before the gate lifts, and the order the threads run in after it:
// - alternating: S1 is started, R1 signaled, S2 started, R2 signaled
// - a syscall in between: S1 is started, the gate sets its own priority to the value it has,
//   then R1 is signaled
//
// On a PS Vita TV on firmware 3.65 the threads made READY run first in their order, then the
// STANDBY ones in theirs, R1, R2, S1, S2, and S1 still runs after R1 when the gate made another
// syscall in between, which left it STANDBY. So a core takes its STANDBY threads into its queue
// when it switches threads, not on every syscall of the thread it runs.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define PRIORITY 0x70
#define SPINS 400000000
#define MAX_THREADS 4

enum Action { NONE, SIGNAL_R1, SIGNAL_R2, SAME_PRIORITY };

static SceUID sema[2], gate;
static volatile int gate_up, gate_open, gate_action, gate_acted, ran;
static volatile int order[MAX_THREADS];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++) {
        const int action = gate_action;
        if (action != NONE && gate_acted != action) {
            if (action == SIGNAL_R1)
                sceKernelSignalSema(sema[0], 1);
            else if (action == SIGNAL_R2)
                sceKernelSignalSema(sema[1], 1);
            else
                sceKernelChangeThreadPriority(0, GATE_PRIORITY);
            gate_acted = action;
        }
    }
    return 0;
}

// Has the gate do action from core 0 and waits until it did
static void gate_do(int action) {
    gate_action = action;
    TEST_WAIT_FOR(gate_acted == action);
}

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

struct Arg {
    int id, sema;
};

// A thread that waits for its semaphore first when it has one, then records that it ran
static int thread_entry(SceSize args, void *argp) {
    (void)args;
    const struct Arg arg = *(const struct Arg *)argp;
    if (arg.sema >= 0) {
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema[arg.sema], 1, &timeout);
    }
    record(arg.id);
    return 0;
}

static SceUID create(void) {
    return sceKernelCreateThread("t", thread_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
}

static void start(SceUID thread, int id, int which_sema) {
    struct Arg arg = { id, which_sema };
    sceKernelStartThread(thread, sizeof(arg), &arg);
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void begin(void) {
    ran = 0;
    for (int i = 0; i < MAX_THREADS; i++)
        order[i] = 0;
    sema[0] = sceKernelCreateSema("r1", 0, 0, 1, NULL);
    sema[1] = sceKernelCreateSema("r2", 0, 0, 1, NULL);
}

static void close_gate(void) {
    gate_up = gate_open = 0;
    gate_action = gate_acted = NONE;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void finish(const char *name, const char *const names[], const SceUID threads[], int count) {
    sceKernelDelayThread(10000);
    SceUInt32 statuses[MAX_THREADS];
    for (int i = 0; i < count; i++)
        statuses[i] = status_of(threads[i]);
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    TEST_WAIT_FOR(ran == count);
    // Joined first, so every record the threads made is complete
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < count; i++)
        PRINTF(statuses[i], "%s: %s's status before the gate lifts", name, names[i]);
    for (int i = 0; i < count; i++)
        PRINTF(order[i], "%s: run %d, by the order of the names above", name, i + 1);
    sceKernelDeleteSema(sema[0]);
    sceKernelDeleteSema(sema[1]);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    {
        static const char *const names[4] = { "S1", "R1", "S2", "R2" };
        SceUID threads[4];
        begin();
        for (int i = 0; i < 4; i++)
            threads[i] = create();
        start(threads[1], 2, 0);
        start(threads[3], 4, 1);
        test_wait_until_waiting(threads[1]);
        test_wait_until_waiting(threads[3]);
        close_gate();
        start(threads[0], 1, -1);
        gate_do(SIGNAL_R1);
        start(threads[2], 3, -1);
        gate_do(SIGNAL_R2);
        finish("alternating", names, threads, 4);
    }
    {
        static const char *const names[2] = { "S1", "R1" };
        SceUID threads[2];
        begin();
        for (int i = 0; i < 2; i++)
            threads[i] = create();
        start(threads[1], 2, 0);
        test_wait_until_waiting(threads[1]);
        close_gate();
        start(threads[0], 1, -1);
        gate_do(SAME_PRIORITY);
        gate_do(SIGNAL_R1);
        finish("a syscall in between", names, threads, 2);
    }
    test_exit();
    return 0;
}

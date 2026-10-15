// Whether the core a wake comes from decides between STANDBY and READY. T and U run on core 0,
// behind a gate thread of the highest priority that spins there without syscalls until the main
// thread, on core 1, lifts it. U is started from core 1 first, then T is made ready in one of
// these ways: a signal from core 1, a signal from core 2, a signal from core 0 by the gate
// thread itself, or a start from core 0 by the gate thread. Each case prints both statuses just
// before the gate lifts, and which of the two runs first after it, at priority 0x70, in the
// queue of core 0, and at 0xA0, in the queue the cores share.
//
// On a PS Vita TV on firmware 3.65, at 0x70 a signal or a start from core 0 makes T READY, and
// it runs before U, while a signal from core 1 or core 2 makes it STANDBY behind U. At 0xA0 all
// of them make it READY behind U.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000

enum Case { SIGNAL_CORE_1, SIGNAL_CORE_2, SIGNAL_CORE_0, START_CORE_0, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = {
    "signal from core 1", "signal from core 2", "signal from core 0", "start from core 0",
};

static SceUID sema, gate, t;
static volatile int gate_up, gate_open, gate_action, gate_acted, helper_go, ran;
static volatile int order[2];

// The gate, which also signals the semaphore or starts T from core 0 when told to
static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++) {
        if (gate_action && !gate_acted) {
            if (gate_action == SIGNAL_CORE_0 + 1)
                sceKernelSignalSema(sema, 1);
            else
                sceKernelStartThread(t, 0, NULL);
            gate_acted = 1;
        }
    }
    return 0;
}

static void gate_close(void) {
    gate_up = gate_open = gate_action = gate_acted = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

// The thread on core 2 that signals the semaphore when told to
static int helper_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    TEST_WAIT_FOR(helper_go);
    sceKernelSignalSema(sema, 1);
    return 0;
}

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

static int t_entry(SceSize args, void *argp) {
    (void)argp;
    // Started with an argument, T waits for the semaphore first
    if (args) {
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    record(1);
    return 0;
}

static int u_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(int c, int priority) {
    ran = 0;
    order[0] = order[1] = 0;
    helper_go = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    SceUID helper = -1;
    if (c == SIGNAL_CORE_2) {
        helper = sceKernelCreateThread("helper", helper_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
        sceKernelStartThread(helper, 0, NULL);
    }
    if (c != START_CORE_0) {
        int wait = 1;
        sceKernelStartThread(t, sizeof(wait), &wait);
        test_wait_until_waiting(t);
    }
    gate_close();
    const SceUID u = sceKernelCreateThread("u", u_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(u, 0, NULL);

    if (c == SIGNAL_CORE_1) {
        sceKernelSignalSema(sema, 1);
    } else if (c == SIGNAL_CORE_2) {
        helper_go = 1;
        test_join_thread(helper);
    } else {
        gate_action = c + 1;
        TEST_WAIT_FOR(gate_acted);
    }
    sceKernelDelayThread(10000);

    const SceUInt32 t_status = status_of(t), u_status = status_of(u);
    gate_lift();
    TEST_WAIT_FOR(ran == 2);
    PRINTF(u_status, "%s, priority 0x%X: U's status before the gate lifts", case_names[c], priority);
    PRINTF(t_status, "%s, priority 0x%X: T's status before the gate lifts", case_names[c], priority);
    PRINTF(order[0], "%s, priority 0x%X: the first to run, 1 for T and 2 for U", case_names[c], priority);
    test_join_thread(t);
    test_join_thread(u);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < CASE_COUNT; c++)
            run_case(c, priorities[p]);
    test_exit();
    return 0;
}

// Where a ForVM suspend leaves a thread in the ready queue of its core. T and U have the same
// priority and run on core 0, behind a gate thread of the highest priority that spins there without
// syscalls until the main thread, on core 1, lifts it. Each case makes T and U ready in some order,
// suspends and resumes T on the way, and prints T's status just before the gate lifts and which of
// the two runs first after it:
// - control: T then U become ready, no suspend
// - timeout while suspended: T waits with a 100 ms timeout, is suspended, its timeout passes,
//   then U becomes ready and T is resumed
// - signal while suspended: the same with a signal instead of the timeout
// - timeout and signal while suspended, U first: the same two, with U ready before the timeout
//   passes or the signal
// - suspended while ready, before U: T is ready and suspended, U becomes ready, T is resumed
// - suspended while ready, after U: T then U are ready, T is suspended and resumed
// - priority change while suspended: the same, and T's priority is set to its own value while it
//   is suspended
// Each case runs at priority 0xA0, in the ready queue the cores share, and at 0x70, in the one of
// core 0.
//
// On a PS Vita TV on firmware 3.65 a ready thread that is suspended loses its place: its resume
// puts it behind every thread of its priority made ready meanwhile. A thread whose wait ends while
// it is suspended takes its place when the wait ends and keeps it through the resume, behind a
// thread made ready before and ahead of one made ready after. The exception is a timeout at
// 0x70, where T reports READY instead of STANDBY and runs before U even when U was ready first,
// which wake_ready_areas shows is what a timeout does at that priority with no suspend either.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000

static SceUID sema, gate;
static volatile int gate_up, gate_open, ran;
static volatile int order[2];

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

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

// T, which waits first when argp says how long, then records that it ran
static int t_entry(SceSize args, void *argp) {
    const SceUInt32 wait_us = args ? *(const SceUInt32 *)argp : 0;
    if (wait_us) {
        SceUInt32 timeout = wait_us;
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

static SceUID start(SceKernelThreadEntry entry, int priority, SceUInt32 wait_us) {
    const SceUID thread = sceKernelCreateThread("t", entry, priority, 0x4000, THREAD_ATTR_VM, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (wait_us)
        sceKernelStartThread(thread, sizeof(wait_us), &wait_us);
    else
        sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

enum Case { CONTROL, TIMEOUT, SIGNAL, TIMEOUT_U_FIRST, SIGNAL_U_FIRST, READY_BEFORE_U, READY_AFTER_U, PRIORITY_CHANGE, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = {
    "control",
    "timeout while suspended",
    "signal while suspended",
    "timeout while suspended, U first",
    "signal while suspended, U first",
    "suspended while ready, before U",
    "suspended while ready, after U",
    "priority change while suspended",
};

static void run_case(int c, int priority) {
    ran = 0;
    order[0] = order[1] = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUID t = -1, u = -1;
    int suspended = 0;

    if (c == TIMEOUT || c == SIGNAL || c == TIMEOUT_U_FIRST || c == SIGNAL_U_FIRST) {
        const int timeout = c == TIMEOUT || c == TIMEOUT_U_FIRST;
        const int u_first = c == TIMEOUT_U_FIRST || c == SIGNAL_U_FIRST;
        t = start(t_entry, priority, timeout ? 100000 : TEST_WAIT_US);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
        suspended = sceKernelSuspendThreadForVM(t);
        gate_close();
        if (u_first)
            u = start(u_entry, priority, 0);
        if (timeout)
            sceKernelDelayThread(300000);
        else
            sceKernelSignalSema(sema, 1);
        sceKernelDelayThread(10000);
        if (!u_first)
            u = start(u_entry, priority, 0);
        sceKernelResumeThreadForVM(t);
    } else {
        gate_close();
        t = start(t_entry, priority, 0);
        if (c == READY_BEFORE_U)
            suspended = sceKernelSuspendThreadForVM(t);
        u = start(u_entry, priority, 0);
        if (c == READY_AFTER_U || c == PRIORITY_CHANGE)
            suspended = sceKernelSuspendThreadForVM(t);
        if (c == PRIORITY_CHANGE)
            sceKernelChangeThreadPriority(t, priority);
        if (c != CONTROL)
            sceKernelResumeThreadForVM(t);
    }

    const SceUInt32 t_status = status_of(t);
    gate_lift();
    TEST_WAIT_FOR(ran == 2);
    // Joined first, so every record the threads made is complete
    test_join_thread(t);
    test_join_thread(u);
    if (c != CONTROL)
        PRINTF(suspended, "%s, priority 0x%X: the suspend", case_names[c], priority);
    PRINTF(t_status, "%s, priority 0x%X: T's status before the gate lifts", case_names[c], priority);
    PRINTF(order[0], "%s, priority 0x%X: the first to run, 1 for T and 2 for U", case_names[c], priority);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0xA0, 0x70 };
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < CASE_COUNT; c++)
            run_case(c, priorities[p]);
    test_exit();
    return 0;
}

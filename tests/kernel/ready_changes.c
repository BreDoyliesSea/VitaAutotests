// What a priority or affinity change does to a thread at 0x70 that is STANDBY or READY on core
// 0. T and U run at 0x70 on core 0, behind a gate thread of the highest priority that spins
// there without syscalls until the main thread, on core 1, lifts it. U is started from core 1,
// which makes it STANDBY. T is either started from core 1 before U, which makes it STANDBY, or
// made ready by its own timeout, which makes it READY (wake_core_origin). Then T's priority or
// mask changes:
// - STANDBY, same priority: T's priority is set to the 0x70 it has
// - STANDBY, higher priority: T's priority becomes 0x6F
// - STANDBY, moved to core 1: T's mask becomes core 1, where the main thread mostly sleeps
// - READY, moved to core 1: the same for a READY T
// Each case prints T's status after a priority change, whether T ran before the gate lifted and
// on which core, and which of the two ran first.
//
// On a PS Vita TV on firmware 3.65 a priority change leaves a STANDBY thread STANDBY, and one to
// the priority it has keeps its place ahead of U, unlike for a thread in a ready queue
// (ready_queue_order). A STANDBY or READY thread moved to an idle core runs there at once. A
// READY thread whose priority another core sets to the value it has is left out: it stays READY
// ahead of U on some runs and becomes STANDBY behind U on others.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define PRIORITY 0x70
#define SPINS 400000000

static SceUID sema, gate;
static volatile int gate_up, gate_open, ran, t_cpu;
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

// T, which waits with a 100 ms timeout first when started with an argument
static int t_entry(SceSize args, void *argp) {
    (void)argp;
    if (args) {
        SceUInt32 timeout = 100000;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
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

enum Change { SAME_PRIORITY, HIGHER_PRIORITY, MOVE_TO_CORE_1 };

struct Case {
    const char *name;
    int ready, change;
};

static const struct Case cases[] = {
    { "STANDBY, same priority", 0, SAME_PRIORITY },
    { "STANDBY, higher priority", 0, HIGHER_PRIORITY },
    { "STANDBY, moved to core 1", 0, MOVE_TO_CORE_1 },
    { "READY, moved to core 1", 1, MOVE_TO_CORE_1 },
};

static void run_case(const struct Case *c) {
    ran = 0;
    order[0] = order[1] = 0;
    t_cpu = -2;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const SceUID u = sceKernelCreateThread("u", u_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (c->ready) {
        int wait = 1;
        sceKernelStartThread(t, sizeof(wait), &wait);
        test_wait_until_waiting(t);
        gate_close();
        sceKernelStartThread(u, 0, NULL);
        sceKernelDelayThread(300000);
    } else {
        gate_close();
        sceKernelStartThread(t, 0, NULL);
        sceKernelStartThread(u, 0, NULL);
    }
    sceKernelDelayThread(10000);

    if (c->change == SAME_PRIORITY)
        sceKernelChangeThreadPriority(t, PRIORITY);
    else if (c->change == HIGHER_PRIORITY)
        sceKernelChangeThreadPriority(t, PRIORITY - 1);
    else
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_1);
    const SceUInt32 status = status_of(t);
    sceKernelDelayThread(50000);
    const int ran_before_lift = ran > 0 && order[0] == 1;
    gate_lift();
    TEST_WAIT_FOR(ran == 2);
    // A moved thread may already run on core 1, so its status then depends on timing
    if (c->change != MOVE_TO_CORE_1)
        PRINTF(status, "%s: T's status after the change", c->name);
    PRINTF(ran_before_lift, "%s: T ran before the gate lifted", c->name);
    PRINTF(t_cpu, "%s: the core T ran on", c->name);
    PRINTF(order[0], "%s: the first to run, 1 for T and 2 for U", c->name);
    test_join_thread(t);
    test_join_thread(u);
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

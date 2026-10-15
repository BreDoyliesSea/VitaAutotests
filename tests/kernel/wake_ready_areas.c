// Which ready queue a thread joins depending on what makes it ready, with no suspend involved. T
// and U have the same priority and run on core 0, behind a gate thread of the highest priority that
// spins there without syscalls until the main thread, on core 1, lifts it. U becomes ready first,
// then T does, by one of: being started, a signal from core 1 ending its wait, its wait's own
// timeout, or the end of its delay. Each case prints both statuses just before the gate lifts, and
// which of the two runs first after it. Each case runs at priority 0xA0, in the ready queue the
// cores share, and at 0x70, in the one of core 0.
//
// On a PS Vita TV on firmware 3.65, at 0xA0 every way makes T READY behind U. At 0x70 a start or a
// signal makes T STANDBY behind U, like U, but its own timeout or the end of its delay makes it
// READY, and it runs before U, which was ready first.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000
#define T_WAIT_US 100000

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

enum Case { STARTED, SIGNAL, TIMEOUT, DELAY_END, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = { "started", "signal", "timeout", "delay end" };

// T, which first waits in the way its argument says, then records that it ran
static int t_entry(SceSize args, void *argp) {
    const int c = args ? *(const int *)argp : STARTED;
    SceUInt32 timeout = c == TIMEOUT ? T_WAIT_US : TEST_WAIT_US;
    if (c == SIGNAL || c == TIMEOUT)
        sceKernelWaitSema(sema, 1, &timeout);
    else if (c == DELAY_END)
        sceKernelDelayThread(T_WAIT_US);
    record(1);
    return 0;
}

static int u_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

static SceUID start(SceKernelThreadEntry entry, int priority, int c) {
    const SceUID thread = sceKernelCreateThread("t", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(c), &c);
    return thread;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(int c, int priority) {
    ran = 0;
    order[0] = order[1] = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUID t = -1;
    if (c != STARTED) {
        t = start(t_entry, priority, c);
        test_wait_until_waiting(t);
    }
    gate_close();
    const SceUID u = start(u_entry, priority, STARTED);
    if (c == STARTED)
        t = start(t_entry, priority, STARTED);
    else if (c == SIGNAL)
        sceKernelSignalSema(sema, 1);
    else
        sceKernelDelayThread(3 * T_WAIT_US);
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
    static const int priorities[2] = { 0xA0, 0x70 };
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < CASE_COUNT; c++)
            run_case(c, priorities[p]);
    test_exit();
    return 0;
}

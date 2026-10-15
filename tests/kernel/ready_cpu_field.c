// What the current CPU field of thread info reports for a thread that can run but doesn't. T
// runs on core 0, behind a gate thread of the highest priority that spins there without syscalls
// until the main thread, on core 1, lifts it. T is made ready in one of these ways, and the main
// thread prints T's status and current CPU before the gate lifts:
// - started from core 1 at 0x70, which makes it STANDBY
// - woken by its own 100 ms timeout at 0x70, which makes it READY in the queue of core 0
// - started from core 1 at 0xA0, which makes it READY in the queue the cores share
//
// On a PS Vita TV on firmware 3.65 the STANDBY T and the READY T in the queue of core 0 report
// core 0, while the READY T in the shared queue reports 0x7FFF.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000

static SceUID sema, gate;
static volatile int gate_up, gate_open;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

// T, which waits with a 100 ms timeout first when started with an argument
static int t_entry(SceSize args, void *argp) {
    (void)argp;
    if (args) {
        SceUInt32 timeout = 100000;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    return 0;
}

static void run_case(const char *name, int priority, int timer) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    int wait = 1;
    if (timer) {
        sceKernelStartThread(t, sizeof(wait), &wait);
        test_wait_until_waiting(t);
    }
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (timer) {
        // T's timeout ends its wait while the gate holds core 0
        for (int i = 0; i < 5000; i++) {
            sceKernelGetThreadInfo(t, &info);
            if (info.status != SCE_THREAD_WAITING)
                break;
            sceKernelDelayThread(1000);
        }
    } else {
        sceKernelStartThread(t, 0, NULL);
        sceKernelGetThreadInfo(t, &info);
    }
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    test_join_thread(t);
    PRINTF(info.status, "%s: T's status", name);
    PRINTF(info.currentCpuId, "%s: T's current CPU", name);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("started from core 1 at 0x70", 0x70, 0);
    run_case("woken by its timeout at 0x70", 0x70, 1);
    run_case("started from core 1 at 0xA0", 0xA0, 0);
    test_exit();
    return 0;
}

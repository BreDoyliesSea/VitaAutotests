// Where the priorities whose threads wait in the queue of one core end, and those that use the
// queue the cores share begin. T runs on core 0, behind a gate thread of the highest priority
// that spins there without syscalls until the main thread, on core 1, lifts it. The main thread
// starts T from core 1 at 0x40, 0x7F, 0x80 and 0xBF, the ends of both ranges, and each case
// prints T's status before the gate lifts.
//
// On a PS Vita TV on firmware 3.65 T is STANDBY at 0x40 and 0x7F, and READY at 0x80 and 0xBF.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000

static SceUID gate;
static volatile int gate_up, gate_open, t_ran;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    t_ran = 1;
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(int priority) {
    gate_up = gate_open = t_ran = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    sceKernelDelayThread(10000);
    const SceUInt32 status = status_of(t);
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    test_join_thread(t);
    PRINTF(status, "T at 0x%X: its status before the gate lifts", priority);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[4] = { 0x40, 0x7F, 0x80, 0xBF };
    for (int p = 0; p < 4; p++)
        run_case(priorities[p]);
    test_exit();
    return 0;
}

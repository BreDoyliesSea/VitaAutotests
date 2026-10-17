// Where a thread that may run on cores 0 and 1 goes when a thread on one of those cores wakes it
// while both are busy. T last ran on core 0 and waits on a semaphore, then its mask becomes cores
// 0 and 1. Gate threads of the highest priority spin on both cores without syscalls, and the main
// thread runs on core 2. The gate of one core signals T, either the gate of core 0, the core T
// last ran on, or the gate of core 1. The main thread prints T's status and current CPU 10 ms
// later, lifts the gate of core 1 and prints whether T ran within 200 ms, while core 0 is still
// busy, then lifts the gate of core 0 and prints the core T ran on. Each case runs at 0x70, in the
// queues of one core, and at 0xA0, in the queue the cores share. A signal from core 2, outside
// T's mask, is in timer_wake_cores.
//
// On a PS Vita TV on firmware 3.65 T at 0x70 waits for core 0 either way, READY when the gate of
// core 0 signals it and STANDBY when the gate of core 1 does. It doesn't take core 1 when that gate
// lifts, and runs on core 0 once core 0 is free. At 0xA0 T is READY with the current CPU 0x7FFF
// either way, and takes core 1 as soon as its gate lifts.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000
#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)

static SceUID sema, gates[2];
static volatile int gate_up[2], gate_open[2], gate_signal[2], gate_signaled[2], t_ran, t_cpu;

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    gate_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !gate_open[core]; n++) {
        if (gate_signal[core] && !gate_signaled[core]) {
            sceKernelSignalSema(sema, 1);
            gate_signaled[core] = 1;
        }
    }
    return 0;
}

static void gate_close(int core) {
    gate_up[core] = gate_open[core] = gate_signal[core] = gate_signaled[core] = 0;
    const SceUInt32 mask = core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    gates[core] = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, mask, NULL);
    sceKernelStartThread(gates[core], sizeof(core), &core);
    TEST_WAIT_FOR(gate_up[core]);
}

static void gate_lift(int core) {
    gate_open[core] = 1;
    sceKernelWaitThreadEnd(gates[core], NULL, NULL);
    sceKernelDeleteThread(gates[core]);
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_ran = 1;
    return 0;
}

static void run_case(int waker, int priority) {
    t_ran = 0;
    t_cpu = -2;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    sceKernelChangeThreadCpuAffinityMask(t, TWO_CORES);
    gate_close(0);
    gate_close(1);

    gate_signal[waker] = 1;
    TEST_WAIT_FOR(gate_signaled[waker]);
    sceKernelDelayThread(10000);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    gate_lift(1);
    for (int i = 0; i < 200 && !t_ran; i++)
        sceKernelDelayThread(1000);
    const int ran_on_core_1 = t_ran;
    gate_lift(0);
    TEST_WAIT_FOR(t_ran);
    // Joined first, so every record the thread made is complete
    test_join_thread(t);
    sceKernelDeleteSema(sema);

    const char *const name = waker ? "the gate of core 1 signals" : "the gate of core 0 signals";
    PRINTF(info.status, "%s, priority 0x%X: T's status", name, priority);
    PRINTF(info.currentCpuId, "%s, priority 0x%X: T's current CPU", name, priority);
    PRINTF(ran_on_core_1, "%s, priority 0x%X: T ran while core 0 was still busy", name, priority);
    PRINTF(t_cpu, "%s, priority 0x%X: the core T ran on", name, priority);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        run_case(0, priorities[p]);
        run_case(1, priorities[p]);
    }
    test_exit();
    return 0;
}

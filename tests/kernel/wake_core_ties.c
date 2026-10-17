// Which core a woken thread that may run on cores 0 and 1 takes when neither runs a thread of a
// lower priority than the other. T first waits on one core, then its mask becomes cores 0 and 1.
// Spinner threads loop without syscalls on both cores, and the main thread signals T from core 2.
// The cases:
// - two equal lower ones: spinners at 0xB0 on both cores, T at 0x70 last ran on core 0, then on
//   core 1. Each prints the core T ran on. With T at 0xA0, in the queue the cores share, which
//   core takes it varies between runs, so that case isn't run.
// - one at T's priority, one higher: a spinner at T's priority on core 0 and one at 0x48 on core
//   1, T last ran on core 1. It prints T's status 10 ms after the signal, whether T ran within
//   200 ms of the spinner of core 0 ending, and the core T ran on once both ended. It runs with T
//   at 0x70 and at 0xA0.
// core_choice has the cases where one core runs a lower priority than the other.
//
// On a PS Vita TV on firmware 3.65 T at 0x70 goes to the core it last ran on when both cores run
// 0xB0. With core 0 at T's priority and core 1 higher, T waits STANDBY for core 1, its last core,
// doesn't take core 0 once it is free, and runs on core 1. At 0xA0 T is READY in the queue the
// cores share and runs on core 0 as soon as it is free.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

static SceUID sema, spinners[2];
static volatile int spinner_up[2], spinner_stop[2], t_cpu, t_ran;

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++)
        ;
    return 0;
}

static void spinner_start(int core, int priority) {
    spinner_up[core] = spinner_stop[core] = 0;
    const SceUInt32 mask = core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0, mask,
                                           NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

static void spinner_end(int core) {
    spinner_stop[core] = 1;
    sceKernelWaitThreadEnd(spinners[core], NULL, NULL);
    sceKernelDeleteThread(spinners[core]);
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

// Starts T on its last core, where it waits, then gives it both cores
static SceUID start_t(int priority, int last_core) {
    t_ran = 0;
    t_cpu = -2;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUInt32 mask = last_core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, mask, NULL);
    sceKernelStartThread(t, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema));
    sceKernelChangeThreadCpuAffinityMask(t, TWO_CORES);
    return t;
}

static void end_t(SceUID t) {
    TEST_WAIT_FOR(t_ran);
    // Joined first, so every record the thread made is complete
    test_join_thread(t);
    sceKernelDeleteSema(sema);
}

static void both_equal(int priority, int last_core) {
    const SceUID t = start_t(priority, last_core);
    spinner_start(0, 0xB0);
    spinner_start(1, 0xB0);
    sceKernelSignalSema(sema, 1);
    end_t(t);
    spinner_end(0);
    spinner_end(1);
    PRINTF(t_cpu, "T at 0x%X, both cores at 0xB0, last on core %d: the core T ran on", priority,
           last_core);
}

static void equal_and_higher(int priority) {
    const SceUID t = start_t(priority, 1);
    spinner_start(0, priority);
    spinner_start(1, 0x48);
    sceKernelSignalSema(sema, 1);
    sceKernelDelayThread(10000);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    spinner_end(0);
    for (int i = 0; i < 200 && !t_ran; i++)
        sceKernelDelayThread(1000);
    const int ran_on_core_0 = t_ran;
    spinner_end(1);
    end_t(t);
    char name[64];
    sceClibSnprintf(name, sizeof(name), "T at 0x%X, core 0 at its priority, core 1 higher",
                    priority);
    PRINTF(info.status, "%s: T's status", name);
    PRINTF(ran_on_core_0, "%s: T ran once core 0 was free", name);
    PRINTF(t_cpu, "%s: the core T ran on", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    both_equal(0x70, 0);
    both_equal(0x70, 1);
    for (int p = 0; p < 2; p++)
        equal_and_higher(priorities[p]);
    test_exit();
    return 0;
}

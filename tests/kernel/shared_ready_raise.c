// What a priority raise from another core does to a thread READY in the queue the cores share. T,
// at 0xA0 and on core 0 alone, is started while a spinner thread holds core 0 without syscalls,
// so it waits READY in that queue (ready_cpu_field). The main thread, on core 1 at 0x50, above
// every test thread, then raises T, and prints what the change returns, whether T ran before the
// spinner stopped, the core T ran on and the priority it ran at. Where the spinner is above T's
// new priority, so T can't take core 0, it also prints T's status and current CPU 10 ms after the
// change, while the main thread keeps core 1. The cases:
// - the spinner at 0x40 and T raised to 0x90, still in the queue the cores share
// - the spinner at 0x98, below T's new priority, and T raised to 0x90
// - the spinner at 0x98 and T raised to 0x60, a priority of the queues of one core
// - the spinner at 0x40 and T raised to 0x60, last, since a raise to a priority of the queues of
//   one core from core 1 may put T in the queue of core 1, which its mask leaves out
//
// On a PS Vita TV on firmware 3.65 a raise to 0x90 leaves T READY with the current CPU 0x7FFF, and
// it runs on core 0 at 0x90, after the spinner at 0x40 or ahead of the one at 0x98. A raise to 0x60
// puts T in the queue of core 1, the core the change came from, although its mask holds only core
// 0. It reports READY on core 1 while the main thread holds core 1, then runs there at 0x60 as soon
// as the main thread waits, even when core 0 runs only the spinner at 0x98 below it.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x50
#define T_PRIORITY 0xA0
#define SPINS 400000000

static volatile int spinner_up, spinner_stop, t_ran, t_cpu, t_priority;

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    spinner_up = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop; n++)
        ;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_priority = sceKernelGetThreadCurrentPriority();
    t_ran = 1;
    return 0;
}

// Waits about us microseconds without blocking, so the main thread keeps its core
static void busy_wait(SceUInt64 us) {
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    while (sceKernelGetProcessTimeWide() - start < us)
        ;
}

// Whether T runs within 200 ms, which tells a T that runs at once from one that waits
static int t_runs_soon(void) {
    for (int i = 0; i < 200 && !t_ran; i++)
        sceKernelDelayThread(1000);
    return t_ran;
}

static void run_case(int spinner_priority, int new_priority) {
    t_ran = 0;
    t_cpu = t_priority = -1;
    spinner_up = spinner_stop = 0;
    const SceUID spinner = sceKernelCreateThread("spinner", spinner_entry, spinner_priority, 0x4000,
                                                 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(spinner, 0, NULL);
    TEST_WAIT_FOR(spinner_up);

    const SceUID t = sceKernelCreateThread("t", t_entry, T_PRIORITY, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    const int result = sceKernelChangeThreadPriority(t, new_priority);
    const int spinner_above = spinner_priority < new_priority;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (spinner_above) {
        busy_wait(10000);
        sceKernelGetThreadInfo(t, &info);
    }
    const int ran_early = t_runs_soon();

    spinner_stop = 1;
    sceKernelWaitThreadEnd(spinner, NULL, NULL);
    sceKernelDeleteThread(spinner);
    TEST_WAIT_FOR(t_ran);
    // A T left in a queue that no core takes it from gets a mask and priority any core takes
    const int rescued = !t_ran;
    if (rescued) {
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_ALL);
        sceKernelChangeThreadPriority(t, T_PRIORITY);
        TEST_WAIT_FOR(t_ran);
    }
    SceUInt timeout = TEST_WAIT_US;
    if (sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0)
        sceKernelDeleteThread(t);

    char name[64];
    sceClibSnprintf(name, sizeof(name), "the spinner at 0x%X, T raised to 0x%X", spinner_priority,
                    new_priority);
    PRINTF(result, "%s: the change", name);
    if (spinner_above) {
        PRINTF(info.status, "%s: T's status after the change", name);
        PRINTF(info.currentCpuId, "%s: T's current CPU after the change", name);
    }
    PRINTF(ran_early, "%s: T ran before the spinner stopped", name);
    PRINTF(rescued, "%s: T only ran once given every core and its priority back", name);
    PRINTF(t_cpu, "%s: the core T ran on", name);
    PRINTF(t_priority, "%s: the priority T ran at", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case(0x40, 0x90);
    run_case(0x98, 0x90);
    run_case(0x98, 0x60);
    run_case(0x40, 0x60);
    test_exit();
    return 0;
}

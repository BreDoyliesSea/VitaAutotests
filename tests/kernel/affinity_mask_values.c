// Which CPU mask values a thread takes, and what a mask of 0 means for where it runs. The main
// thread, on core 2, creates threads with masks of 0, 0x1, 0x8000, 0x100000 and the user cores
// with 0x100000, and prints each create's result and the mask each thread reports. Then it
// changes the mask of a thread that runs on core 0 to 0 and prints what the change returns, the
// mask the thread reports after, and whether it then runs and on which core, while spinners of
// the highest priority keep cores 0 and 1 busy until it has run.
//
// On a PS Vita TV on firmware 3.65 a mask of 0 is taken and reported as 0, every mask with a bit
// outside the user cores is refused with 0x80028025, and a thread whose mask is 0 runs on core 2
// while cores 0 and 1 are busy, so 0 means any user core.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// How long a spinner keeps its core at most, far past the wait for T
#define SPIN_US 20000000

static SceUID sema;
static volatile int spinner_up[2], spinner_stop, t_ran, t_cpu;

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    spinner_up[*(const int *)argp] = 1;
    const SceUInt64 end = sceKernelGetProcessTimeWide() + SPIN_US;
    while (!spinner_stop && sceKernelGetProcessTimeWide() < end)
        ;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)argp;
    // Started with an argument, T waits for the semaphore first
    if (args) {
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_ran = 1;
    return 0;
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);

    static const SceUInt32 masks[5] = { 0, 0x1, 0x8000, 0x100000, SCE_KERNEL_CPU_MASK_USER_ALL | 0x100000 };
    for (int i = 0; i < 5; i++) {
        const SceUID t = sceKernelCreateThread("t", t_entry, 0x70, 0x4000, 0, masks[i], NULL);
        PRINTF(t < 0 ? t : 0, "create with mask 0x%X", (unsigned)masks[i]);
        if (t >= 0) {
            PRINTF(sceKernelGetThreadCpuAffinityMask(t), "created with mask 0x%X: the mask it reports", (unsigned)masks[i]);
            sceKernelDeleteThread(t);
        }
    }

    // A waiting T on core 0 gets a mask of 0, then cores 0 and 1 are kept busy and T is woken
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    int wait = 1;
    sceKernelStartThread(t, sizeof(wait), &wait);
    test_wait_until_waiting(t);
    PRINT("change a waiting thread's mask to 0", sceKernelChangeThreadCpuAffinityMask(t, 0));
    PRINT("the mask it reports after", sceKernelGetThreadCpuAffinityMask(t));
    SceUID spinners[2];
    for (int core = 0; core < 2; core++) {
        spinners[core] = sceKernelCreateThread("spinner", spinner_entry, SCE_KERNEL_HIGHEST_PRIORITY_USER, 0x4000, 0, core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(spinners[core], sizeof(core), &core);
        TEST_WAIT_FOR(spinner_up[core]);
    }
    sceKernelSignalSema(sema, 1);
    // The spinners keep cores 0 and 1 until T has run or the wait gave up
    TEST_WAIT_FOR(t_ran);
    const int ran = t_ran;
    spinner_stop = 1;
    for (int core = 0; core < 2; core++)
        test_join_thread(spinners[core]);
    test_join_thread(t);
    PRINT("with mask 0 and cores 0 and 1 busy, it ran", ran);
    PRINT("the core it ran on", t_cpu);
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}

// Whether a thread that lowers itself below a thread READY on its core moves to another core of
// its mask that is free. L runs on core 0, and the main thread runs on core 2. L gives itself the
// mask of cores 0 and 1, starts R on core 0 alone from core 0, which makes R READY behind it
// (wake_core_origin), then lowers itself below R. R spins without syscalls until L ran again, or
// until the main thread tells it to stop 200 ms later. L records the core it runs on once its
// change returns. Each case prints the core L ran on before the change, whether L ran again
// within 200 ms, while R held core 0, and the core it ran on then:
// - L at 0x70 lowered to 0xA0, R at 0x78, core 1 idle: L goes from the queues of one core to the
//   queue the cores share
// - L at 0x70 lowered to 0x7C, R at 0x78, core 1 idle: L stays in the queues of one core
// - L at 0xA0 lowered to 0xAC, R at 0xA8, core 1 idle: both in the queue the cores share
// - L at 0x70 lowered to 0xA0, R at 0x78, core 1 running a spinner at 0xB0 without syscalls
//
// On a PS Vita TV on firmware 3.65 L runs again on core 1 within 200 ms in every case: from the
// queues of one core to the queue the cores share, within the queues of one core, within the queue
// the cores share, and with core 1 running the spinner at 0xB0, below L's new priority.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

static SceUID r;
static volatile int new_priority, l_ran, l_cpu_before, l_cpu, r_stop;
static volatile int spinner_up, spinner_stop;

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelChangeThreadCpuAffinityMask(0, TWO_CORES);
    sceKernelStartThread(r, 0, NULL);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    l_cpu_before = info.currentCpuId;
    sceKernelChangeThreadPriority(0, new_priority);
    sceKernelGetThreadInfo(0, &info);
    l_cpu = info.currentCpuId;
    l_ran = 1;
    return 0;
}

static int r_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    for (volatile int n = 0; n < SPINS && !l_ran && !r_stop; n++)
        ;
    return 0;
}

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    spinner_up = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop; n++)
        ;
    return 0;
}

static void run_case(int l_priority, int lowered, int r_priority, int busy_core_1) {
    l_ran = r_stop = 0;
    l_cpu_before = l_cpu = -2;
    new_priority = lowered;
    SceUID spinner = -1;
    if (busy_core_1) {
        spinner_up = spinner_stop = 0;
        spinner = sceKernelCreateThread("spinner", spinner_entry, 0xB0, 0x4000, 0,
                                        SCE_KERNEL_CPU_MASK_USER_1, NULL);
        sceKernelStartThread(spinner, 0, NULL);
        TEST_WAIT_FOR(spinner_up);
    }
    r = sceKernelCreateThread("r", r_entry, r_priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0,
                              NULL);
    const SceUID l = sceKernelCreateThread("l", l_entry, l_priority, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    for (int i = 0; i < 200 && !l_ran; i++)
        sceKernelDelayThread(1000);
    const int ran_early = l_ran;
    r_stop = 1;
    TEST_WAIT_FOR(l_ran);
    // Joined first, so every record the threads made is complete
    test_join_thread(l);
    test_join_thread(r);
    if (busy_core_1) {
        spinner_stop = 1;
        test_join_thread(spinner);
    }
    char name[96];
    sceClibSnprintf(name, sizeof(name), "L at 0x%X lowered to 0x%X, R at 0x%X, core 1 %s",
                    l_priority, lowered, r_priority, busy_core_1 ? "busy at 0xB0" : "idle");
    PRINTF(l_cpu_before, "%s: the core L ran on before the change", name);
    PRINTF(ran_early, "%s: L ran again within 200 ms", name);
    PRINTF(l_cpu, "%s: the core L ran on", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case(0x70, 0xA0, 0x78, 0);
    run_case(0x70, 0x7C, 0x78, 0);
    run_case(0xA0, 0xAC, 0xA8, 0);
    run_case(0x70, 0xA0, 0x78, 1);
    test_exit();
    return 0;
}

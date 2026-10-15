// Whether a thread running on one core moves to another as soon as another core changes its
// mask. L runs on core 0 in a loop that notes the core it runs on, read from its own thread
// info, and counts. The main thread, on core 1, changes L's mask to core 2 alone, waits 20 ms,
// and prints the core L noted then and whether its count went on, with L at 0x70 and at 0xB0.
//
// On a PS Vita TV on firmware 3.65 L runs on core 2 within the 20 ms at both priorities, and
// keeps running.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static volatile int l_up, l_stop, l_cpu;
static volatile unsigned int l_count;

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    l_up = 1;
    while (!l_stop) {
        SceKernelThreadInfo info = { .size = sizeof(info) };
        sceKernelGetThreadInfo(0, &info);
        l_cpu = info.currentCpuId;
        l_count++;
    }
    return 0;
}

static void run_case(int priority) {
    l_up = l_stop = 0;
    l_cpu = -2;
    l_count = 0;
    const SceUID l = sceKernelCreateThread("l", l_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_up && l_count > 0);
    const int before = l_cpu;
    const int r = sceKernelChangeThreadCpuAffinityMask(l, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelDelayThread(20000);
    const int after = l_cpu;
    const unsigned int count = l_count;
    sceKernelDelayThread(10000);
    const int counting = l_count != count;
    l_stop = 1;
    test_join_thread(l);
    PRINTF(before, "L at 0x%X: its core before the change", priority);
    PRINTF(r >= 0, "L at 0x%X: the change", priority);
    PRINTF(after, "L at 0x%X: its core 20 ms after the change", priority);
    PRINTF(counting, "L at 0x%X: it still runs", priority);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case(0x70);
    run_case(0xB0);
    test_exit();
    return 0;
}

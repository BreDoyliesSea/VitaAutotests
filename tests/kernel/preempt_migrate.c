// Whether a thread that may run on cores 0 and 1 moves to core 1 when a thread pinned to core 0
// takes core 0 from it, while core 1 runs a thread of its own. S spins on core 1 at 0xB0, below
// T, or at T's priority. T, with the mask of cores 0 and 1, is started while core 0 is idle, so
// it runs there. Both count in loops without syscalls. The main thread, on core 2 at 0x40, then
// starts B at 0x48 on core 0 alone, which spins there. 10 ms later it notes T's status and current
// CPU, then whether T and S counted on over 50 ms, then lets them all end. Each case runs with T
// at 0x70, in the queues of one core, and at 0xA0, in the queue the cores share, and prints the
// core T ran on before B, and those. priority_queue_areas has the case with core 1 idle.
//
// On a PS Vita TV on firmware 3.65 when B takes core 0, a T that may run on cores 0 and 1 moves to
// core 1 and preempts S there when S runs 0xB0, at 0x70 and at 0xA0, so S stops counting. When S
// runs at T's priority, T waits READY, on core 0 at 0x70 and in the queue the cores share at 0xA0,
// and S keeps core 1.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define TWO_CORES (SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1)
#define SPINS 400000000

// The counters of S, T and B
static volatile unsigned int counts[3];
static volatile int stop;

static int count_entry(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    for (int n = 0; n < SPINS && !stop; n++)
        counts[i]++;
    return 0;
}

static SceUID start_counter(int i, int priority, SceUInt32 mask) {
    const SceUID thread = sceKernelCreateThread("counter", count_entry, priority, 0x4000, 0, mask,
                                                NULL);
    sceKernelStartThread(thread, sizeof(i), &i);
    TEST_WAIT_FOR(counts[i] != 0);
    return thread;
}

static void run_case(int t_priority, int s_priority) {
    stop = 0;
    for (int i = 0; i < 3; i++)
        counts[i] = 0;
    const SceUID s = start_counter(0, s_priority, SCE_KERNEL_CPU_MASK_USER_1);
    const SceUID t = start_counter(1, t_priority, TWO_CORES);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    const int cpu_before = info.currentCpuId;
    const SceUID b = start_counter(2, 0x48, SCE_KERNEL_CPU_MASK_USER_0);
    // B takes core 0 from T once its start reaches core 0
    sceKernelDelayThread(10000);
    sceKernelGetThreadInfo(t, &info);
    const unsigned int s_count = counts[0], t_count = counts[1];
    sceKernelDelayThread(50000);
    const int s_ran = counts[0] != s_count, t_ran = counts[1] != t_count;
    stop = 1;
    test_join_thread(b);
    test_join_thread(t);
    test_join_thread(s);

    char name[64];
    sceClibSnprintf(name, sizeof(name), "T at 0x%X, S at 0x%X", t_priority, s_priority);
    PRINTF(cpu_before, "%s: the core T ran on before B", name);
    PRINTF(info.status, "%s: T's status once B runs", name);
    PRINTF(info.currentCpuId, "%s: T's current CPU once B runs", name);
    PRINTF(t_ran, "%s: T counted on while B ran", name);
    PRINTF(s_ran, "%s: S counted on while B ran", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelChangeThreadPriority(0, SCE_KERNEL_HIGHEST_PRIORITY_USER);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++) {
        run_case(priorities[p], 0xB0);
        run_case(priorities[p], priorities[p]);
    }
    test_exit();
    return 0;
}

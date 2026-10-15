// Which core a thread that may run on cores 0 and 1 gets when a signal from core 2 ends its
// wait, depending on what runs on those cores and on the core it last ran on. T first waits on
// one core, then its mask becomes cores 0 and 1. Spinner threads that loop without syscalls
// until told to stop keep a core busy at a chosen priority. The main thread runs on core 2. Each
// case prints the core T ran on after the signal:
// - both idle: nothing runs on cores 0 and 1, T last ran on core 0, then on core 1
// - one lower, one idle: a spinner at 0xB0 runs on core 0, core 1 is idle, T last ran on core 0
// - one lower, one higher: a spinner at 0xB0 runs on core 0 and one at 0x50 on core 1, T last
//   ran on core 1
// - both lower: spinners at 0xB0 on core 0 and 0xB8 on core 1, T last ran on core 0, then on
//   core 1
// Each case runs with T at 0x70 and at 0xA0.
//
// On a PS Vita TV on firmware 3.65 both priorities give the same cores. T takes an idle core
// before the one it last ran on, and otherwise the core running the thread of the lowest
// priority, also over the core it last ran on. The core it last ran on only decides between two
// idle cores.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

static SceUID sema;
static volatile int spinner_up[2], spinner_stop[2], t_cpu, t_ran;
static SceUID spinners[2];

static int spinner_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    spinner_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !spinner_stop[core]; n++)
        ;
    return 0;
}

// Keeps core busy with a thread of priority until spinner_end, or does nothing for priority 0
static void spinner_start(int core, int priority) {
    spinners[core] = -1;
    if (!priority)
        return;
    spinner_up[core] = spinner_stop[core] = 0;
    spinners[core] = sceKernelCreateThread("spinner", spinner_entry, priority, 0x4000, 0, core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(spinners[core], sizeof(core), &core);
    TEST_WAIT_FOR(spinner_up[core]);
}

static void spinner_end(int core) {
    if (spinners[core] < 0)
        return;
    spinner_stop[core] = 1;
    sceKernelWaitThreadEnd(spinners[core], NULL, NULL);
    sceKernelDeleteThread(spinners[core]);
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    t_cpu = info.currentCpuId;
    t_ran = 1;
    return 0;
}

struct Case {
    const char *name;
    int last_core, priority_0, priority_1;
};

static const struct Case cases[] = {
    { "both idle, last on core 0", 0, 0, 0 },
    { "both idle, last on core 1", 1, 0, 0 },
    { "one lower, one idle, last on core 0", 0, 0xB0, 0 },
    { "one lower, one higher, last on core 1", 1, 0xB0, 0x50 },
    { "both lower, last on core 0", 0, 0xB0, 0xB8 },
    { "both lower, last on core 1", 1, 0xB0, 0xB8 },
};

static void run_case(const struct Case *c, int priority) {
    t_ran = 0;
    t_cpu = -2;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, c->last_core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    test_wait_until_waiting(t);
    sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_0 | SCE_KERNEL_CPU_MASK_USER_1);
    spinner_start(0, c->priority_0);
    spinner_start(1, c->priority_1);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(t_ran);
    // Joined first, so every record the threads made is complete
    test_join_thread(t);
    PRINTF(t_cpu, "%s, T at 0x%X: the core T ran on", c->name, priority);
    spinner_end(0);
    spinner_end(1);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++)
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            run_case(&cases[i], priorities[p]);
    test_exit();
    return 0;
}

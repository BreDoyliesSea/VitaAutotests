// Where a running thread that lowers its priority goes among the threads waiting at its new
// priority on its core. Every test thread runs on core 0 and the main thread on core 1. L runs at
// priority P and starts A at P + 8 and B at P + 0xC from core 0, which makes them READY behind it
// (wake_core_origin). L records 0, then lowers itself to B's priority, so A takes the core. Once
// A ends, L or B runs next. L records 1 once it runs again, A records 2 and B records 3. The
// cases:
// - P at 0x70, in the queue of core 0
// - P at 0xA0, in the queue the cores share
// - P at 0x70, with B started from core 1 instead, which makes it STANDBY behind the running L
// - P at 0xA0, with L lowered by the main thread from core 1 while L spins without syscalls
//   until A ran
//
// On a PS Vita TV on firmware 3.65 every case runs L, A, L, B, so 0 2 1 3. L goes ahead of B, to
// the head of its new priority, in the queues of one core and in the queue the cores share, ahead
// of a STANDBY B, and when the main thread lowers it from core 1.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_STEPS 4
#define SPINS 400000000

enum Mode { SELF, B_STANDBY, REMOTE };

static SceUID a, b;
static volatile int mode, l_priority, l_ready, b_started, a_ran, l_stop;
static int step_count, steps[MAX_STEPS];

static void record(int step) {
    const int i = __atomic_fetch_add(&step_count, 1, __ATOMIC_SEQ_CST);
    if (i < MAX_STEPS)
        steps[i] = step;
}

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    sceKernelStartThread(a, 0, NULL);
    if (mode != B_STANDBY)
        sceKernelStartThread(b, 0, NULL);
    record(0);
    l_ready = 1;
    if (mode == B_STANDBY) {
        for (volatile int n = 0; n < SPINS && !b_started; n++)
            ;
    }
    if (mode == REMOTE) {
        for (volatile int n = 0; n < SPINS && !a_ran && !l_stop; n++)
            ;
    } else {
        sceKernelChangeThreadPriority(0, l_priority + 0xC);
    }
    record(1);
    return 0;
}

static int a_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    a_ran = 1;
    record(2);
    return 0;
}

static int b_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(3);
    return 0;
}

static SceUID create(const char *name, SceKernelThreadEntry entry, int priority) {
    return sceKernelCreateThread(name, entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0,
                                 NULL);
}

static void run_case(const char *name, int priority, int case_mode) {
    mode = case_mode;
    l_priority = priority;
    l_ready = b_started = a_ran = l_stop = 0;
    step_count = 0;
    for (int i = 0; i < MAX_STEPS; i++)
        steps[i] = -1;
    a = create("a", a_entry, priority + 8);
    b = create("b", b_entry, priority + 0xC);
    const SceUID l = create("l", l_entry, priority);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_ready);
    int result = 0;
    if (case_mode == B_STANDBY) {
        sceKernelStartThread(b, 0, NULL);
        // The start hands B over to core 0, which L keeps
        sceKernelDelayThread(10000);
        b_started = 1;
    } else if (case_mode == REMOTE) {
        result = sceKernelChangeThreadPriority(l, priority + 0xC);
        // Lets L go on if the change leaves it the core, so it records that it kept it
        for (int i = 0; i < 500 && !a_ran; i++)
            sceKernelDelayThread(1000);
        l_stop = 1;
    }
    TEST_WAIT_FOR(step_count == 4);
    // Joined first, so every record the threads made is complete
    test_join_thread(l);
    test_join_thread(a);
    test_join_thread(b);
    if (case_mode == REMOTE)
        PRINTF(result, "%s: the change", name);
    for (int i = 0; i < MAX_STEPS; i++)
        PRINTF(steps[i], "%s: step %d", name, i);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("L at 0x70 lowers itself", 0x70, SELF);
    run_case("L at 0xA0 lowers itself", 0xA0, SELF);
    run_case("L at 0x70 lowers itself, B STANDBY", 0x70, B_STANDBY);
    run_case("L at 0xA0 lowered from core 1", 0xA0, REMOTE);
    test_exit();
    return 0;
}

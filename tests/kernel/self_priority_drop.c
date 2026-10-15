// Whether a thread that lowers its own priority below one waiting on its core gives the core up
// at once. L runs on core 0, and R, of a lower priority, waits behind it there: either started
// from core 1, which makes it STANDBY at 0x40 to 0x7F, or woken from core 0 by L itself, which
// makes it READY (wake_core_origin). L then sets its own priority below R's, and records that
// its call returned. Each case prints which of the two recorded first:
// - R started from core 1, L at 0x70 and R at 0x78, L lowered to 0x7C
// - R woken by L, the same priorities
// - R started from core 1, L at 0xA0 and R at 0xA8, L lowered to 0xAC
//
// On a PS Vita TV on firmware 3.65 R gets the core at once when it is READY, in both queues, but
// a STANDBY R doesn't: L keeps the core after lowering itself, so the STANDBY threads of a core
// don't count until it switches threads for another reason.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

static SceUID sema;
static volatile int r_waits_behind, ran, l_priority;
static volatile int order[2];

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

// R, which waits for the semaphore first when started with an argument
static int r_entry(SceSize args, void *argp) {
    (void)argp;
    if (args) {
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitSema(sema, 1, &timeout);
    }
    record(2);
    return 0;
}

// L, which wakes R itself when started with an argument, then lowers itself
static int l_entry(SceSize args, void *argp) {
    (void)argp;
    if (args)
        sceKernelSignalSema(sema, 1);
    else
        for (volatile int n = 0; n < SPINS && !r_waits_behind; n++)
            ;
    sceKernelChangeThreadPriority(0, l_priority + 12);
    record(1);
    return 0;
}

static void run_case(const char *name, int priority, int l_wakes_r) {
    ran = r_waits_behind = 0;
    order[0] = order[1] = 0;
    l_priority = priority;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID r = sceKernelCreateThread("r", r_entry, priority + 8, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const SceUID l = sceKernelCreateThread("l", l_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    int arg = 1;
    if (l_wakes_r) {
        sceKernelStartThread(r, sizeof(arg), &arg);
        test_wait_until_waiting(r);
        sceKernelStartThread(l, sizeof(arg), &arg);
    } else {
        sceKernelStartThread(l, 0, NULL);
        sceKernelDelayThread(10000);
        sceKernelStartThread(r, 0, NULL);
        sceKernelDelayThread(10000);
        r_waits_behind = 1;
    }
    TEST_WAIT_FOR(ran == 2);
    // Joined first, so every record the threads made is complete
    test_join_thread(l);
    test_join_thread(r);
    PRINTF(order[0], "%s: the first to record, 1 for L and 2 for R", name);
    sceKernelDeleteSema(sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("R started from core 1, at 0x70", 0x70, 0);
    run_case("R woken by L, at 0x70", 0x70, 1);
    run_case("R started from core 1, at 0xA0", 0xA0, 0);
    test_exit();
    return 0;
}

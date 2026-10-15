// Whether an interrupt on a core that doesn't switch threads takes the core's STANDBY threads
// into its queue. L spins at 0x70 on core 0 without syscalls. S, at 0x70, is started from core
// 1, so it waits as STANDBY behind L. W, at 0x7E, waits on core 0 with a 100 ms timeout, so its
// timer wakes it on core 0 while L keeps the core, being of a higher priority. The main thread,
// on core 1, prints S's and W's statuses before W's timeout and S's after it, then stops L, and
// prints which of S and W runs first after that. For W after its timeout it only prints whether
// W left its wait, since on a Vita W is READY on most runs and STANDBY on some.
//
// On a PS Vita TV on firmware 3.65 S stays STANDBY after W's timer woke W on core 0, so an
// interrupt that doesn't switch threads doesn't take S into the queue, and once L stops S runs
// before W, so its priority counts when the core does switch.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

static SceUID sema;
static volatile int l_up, l_stop, ran;
static volatile int order[2];

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

static int l_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    l_up = 1;
    for (volatile int n = 0; n < SPINS && !l_stop; n++)
        ;
    return 0;
}

static int s_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(1);
    return 0;
}

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 100000;
    sceKernelWaitSema(sema, 1, &timeout);
    record(2);
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const SceUID w = sceKernelCreateThread("w", w_entry, 0x7E, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(w, 0, NULL);
    test_wait_until_waiting(w);
    const SceUID l = sceKernelCreateThread("l", l_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(l, 0, NULL);
    TEST_WAIT_FOR(l_up);
    const SceUID s = sceKernelCreateThread("s", s_entry, 0x70, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(s, 0, NULL);
    sceKernelDelayThread(10000);
    const SceUInt32 s_before = status_of(s), w_before = status_of(w);
    sceKernelDelayThread(300000);
    const SceUInt32 s_after = status_of(s), w_after = status_of(w);
    l_stop = 1;
    test_join_thread(l);
    test_join_thread(s);
    test_join_thread(w);
    PRINTF(s_before, "S's status before W's timeout");
    PRINTF(w_before, "W's status before its timeout");
    PRINTF(s_after, "S's status after W's timeout");
    PRINTF(w_after == SCE_THREAD_READY || w_after == SCE_THREAD_STANDBY, "W left its wait, READY or STANDBY, before L stopped");
    PRINTF(order[0], "the first to run once L stops, 1 for S and 2 for W");
    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}

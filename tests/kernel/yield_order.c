// Where a thread that gives up its core goes among the threads of its priority waiting on that
// core. On core 0, R waits first, then Y starts and runs. The main thread, on core 1, starts S,
// which waits behind the running Y. Then Y signals R from core 0, and gives up the core in one
// of these ways:
// - a delay of 1 microsecond
// - setting its own priority to the value it has
// - a wait that the main thread ends from core 1 once Y waits
// Each case prints the statuses of R and S that Y saw just before it gave up the core, and the
// order R, S and Y run in after it, at priority 0x70, in the queue of core 0, and at 0xA0, in
// the queue the cores share.
//
// On a PS Vita TV on firmware 3.65 a delay or a wait puts Y behind both R and S, while setting
// its own priority to the value it has doesn't give up the core at all. At 0xA0 R and S run in
// the order they became ready, S first. At 0x70 S, STANDBY, runs after R, READY since Y's
// signal, even though S became ready first: a STANDBY thread takes its place in the queue of its
// core when that core next schedules, behind the threads made READY on it meanwhile.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define SPINS 400000000

enum Case { DELAY, SAME_PRIORITY, WAIT, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = { "a delay of 1", "its own priority", "a wait ended from core 1" };

static SceUID r_sema, y_sema, r, s;
static volatile int s_started, y_waits, ran, priority;
static volatile int order[3];
static volatile SceUInt32 r_status, s_status;

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static int r_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelWaitSema(r_sema, 1, &timeout);
    record(1);
    return 0;
}

static int s_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

static int y_entry(SceSize args, void *argp) {
    (void)args;
    const int c = *(const int *)argp;
    // Runs without syscalls until S waits behind it
    for (volatile int n = 0; n < SPINS && !s_started; n++)
        ;
    sceKernelSignalSema(r_sema, 1);
    r_status = status_of(r);
    s_status = status_of(s);
    if (c == DELAY) {
        sceKernelDelayThread(1);
    } else if (c == SAME_PRIORITY) {
        sceKernelChangeThreadPriority(0, priority);
    } else {
        SceUInt32 timeout = TEST_WAIT_US;
        y_waits = 1;
        sceKernelWaitSema(y_sema, 1, &timeout);
    }
    record(3);
    return 0;
}

static void run_case(int c, int p) {
    ran = s_started = y_waits = 0;
    priority = p;
    for (int i = 0; i < 3; i++)
        order[i] = 0;
    r_sema = sceKernelCreateSema("r", 0, 0, 1, NULL);
    y_sema = sceKernelCreateSema("y", 0, 0, 1, NULL);
    r = sceKernelCreateThread("r", r_entry, p, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(r, 0, NULL);
    test_wait_until_waiting(r);
    s = sceKernelCreateThread("s", s_entry, p, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const SceUID y = sceKernelCreateThread("y", y_entry, p, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(y, sizeof(c), &c);
    sceKernelDelayThread(10000);
    sceKernelStartThread(s, 0, NULL);
    sceKernelDelayThread(10000);
    s_started = 1;
    if (c == WAIT) {
        TEST_WAIT_FOR(y_waits && test_in_wait(y, WAIT_TYPE_SEMA, y_sema));
        sceKernelSignalSema(y_sema, 1);
    }
    TEST_WAIT_FOR(ran == 3);
    // Joined first, so every record the threads made is complete
    test_join_thread(y);
    test_join_thread(s);
    test_join_thread(r);
    PRINTF(r_status, "%s, priority 0x%X: R's status before Y gave up the core", case_names[c], p);
    PRINTF(s_status, "%s, priority 0x%X: S's status before Y gave up the core", case_names[c], p);
    for (int i = 0; i < 3; i++)
        PRINTF(order[i], "%s, priority 0x%X: run %d, 1 for R, 2 for S and 3 for Y", case_names[c], p, i + 1);
    sceKernelDeleteSema(r_sema);
    sceKernelDeleteSema(y_sema);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const int priorities[2] = { 0x70, 0xA0 };
    for (int p = 0; p < 2; p++)
        for (int c = 0; c < CASE_COUNT; c++)
            run_case(c, priorities[p]);
    test_exit();
    return 0;
}

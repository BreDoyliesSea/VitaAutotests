// The order threads handed over to a core from other cores run in. S1, S2 and S3 run on core 0,
// behind a gate thread of the highest priority that spins there without syscalls until the main
// thread, on core 1, lifts it. S1 is started from core 1, S2 from core 2 by a helper thread,
// then S3 from core 1, so all three wait as STANDBY. Each case prints the order they run in
// after the gate lifts:
// - all at 0x70
// - S2 at 0x6F, the others at 0x70
//
// On a PS Vita TV on firmware 3.65 they run in the order they were handed over, S1, S2, S3,
// whichever core handed them over, and S2 runs first at the higher priority.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define SPINS 400000000

static SceUID gate, s2;
static volatile int gate_up, gate_open, helper_go, helper_done, ran;
static volatile int order[3];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

static int s_entry(SceSize args, void *argp) {
    (void)args;
    record(*(const int *)argp);
    return 0;
}

// Starts S2 from core 2 when told to
static int helper_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    TEST_WAIT_FOR(helper_go);
    int id = 2;
    sceKernelStartThread(s2, sizeof(id), &id);
    helper_done = 1;
    return 0;
}

static SceUID create(int priority) {
    return sceKernelCreateThread("s", s_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
}

static void run_case(const char *name, int s2_priority) {
    ran = helper_go = helper_done = gate_up = gate_open = 0;
    for (int i = 0; i < 3; i++)
        order[i] = 0;
    const SceUID s1 = create(0x70), s3 = create(0x70);
    s2 = create(s2_priority);
    const SceUID helper = sceKernelCreateThread("helper", helper_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    sceKernelStartThread(helper, 0, NULL);
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);

    int id = 1;
    sceKernelStartThread(s1, sizeof(id), &id);
    sceKernelDelayThread(1000);
    helper_go = 1;
    TEST_WAIT_FOR(helper_done);
    sceKernelDelayThread(1000);
    id = 3;
    sceKernelStartThread(s3, sizeof(id), &id);
    sceKernelDelayThread(10000);

    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    TEST_WAIT_FOR(ran == 3);
    // Joined first, so every record the threads made is complete
    test_join_thread(s1);
    test_join_thread(s2);
    test_join_thread(s3);
    test_join_thread(helper);
    for (int i = 0; i < 3; i++)
        PRINTF(order[i], "%s: run %d, by the S number", name, i + 1);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("all at 0x70", 0x70);
    run_case("S2 at 0x6F", 0x6F);
    test_exit();
    return 0;
}

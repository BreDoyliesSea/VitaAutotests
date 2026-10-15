// Whether a thread woken by the vblank interrupt is READY or STANDBY on its core, depending on the
// core it waits on. T and U run at 0x70 on one core, behind a gate thread of the highest priority
// that spins there without syscalls until the main thread, on core 2, lifts it. T waits for the
// next vblank, U is started from core 2, which makes it STANDBY, and the main thread waits 50 ms,
// so vblanks pass. Each case prints both statuses just before the gate lifts, and which of the two
// runs first after it, with T and U on core 0 and on core 1.
//
// On a PS Vita TV on firmware 3.65 the vblank makes T READY on either core, and it runs before U,
// like a wake from its own core (wake_core_origin).
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define PRIORITY 0x70
#define SPINS 400000000

static SceUID gate;
static volatile int gate_up, gate_open, t_waiting, ran;
static volatile int order[2];

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

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    // Waits for one vblank first, so the next wait starts right after one and lasts a whole frame
    sceDisplayWaitVblankStart();
    t_waiting = 1;
    sceDisplayWaitVblankStart();
    record(1);
    return 0;
}

static int u_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    record(2);
    return 0;
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(int core) {
    const SceUInt32 mask = core ? SCE_KERNEL_CPU_MASK_USER_1 : SCE_KERNEL_CPU_MASK_USER_0;
    ran = t_waiting = 0;
    order[0] = order[1] = 0;
    const SceUID t = sceKernelCreateThread("t", t_entry, PRIORITY, 0x4000, 0, mask, NULL);
    sceKernelStartThread(t, 0, NULL);
    // The flag comes just before the wait, so the gate starts only once T is in it
    TEST_WAIT_FOR(t_waiting);
    test_wait_until_waiting(t);
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, mask, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
    const SceUID u = sceKernelCreateThread("u", u_entry, PRIORITY, 0x4000, 0, mask, NULL);
    sceKernelStartThread(u, 0, NULL);
    sceKernelDelayThread(50000);

    const SceUInt32 t_status = status_of(t), u_status = status_of(u);
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    TEST_WAIT_FOR(ran == 2);
    // Joined first, so every record the threads made is complete
    test_join_thread(t);
    test_join_thread(u);
    PRINTF(u_status, "on core %d: U's status before the gate lifts", core);
    PRINTF(t_status, "on core %d: T's status before the gate lifts", core);
    PRINTF(order[0], "on core %d: the first to run, 1 for T and 2 for U", core);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    // The main thread only moves to core 2 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case(0);
    run_case(1);
    test_exit();
    return 0;
}

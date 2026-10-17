// What priority and mask changes from another core do to a thread STANDBY on core 0, and where it
// then runs among the threads that wait there. A gate thread of the highest priority spins on core
// 0 without syscalls until the main thread, on core 1, lifts it, and the threads the main thread
// starts wait behind it, STANDBY at 0x40 to 0x7F (wake_core_origin). T, at 0x70 on core 0, is
// started first, then U, then the main thread changes T. The cases:
// - U at 0x78, and T lowered to 0x78
// - U at 0x68, and T raised to 0x68
// - U at 0x70, and T lowered to 0x78 and raised back to 0x70
// - U at 0xA0, which makes it READY in the queue the cores share, and T lowered to 0xA0
// - U at 0x70 on core 2 alone, behind a gate there too, and T's mask changed to core 2 alone.
//   The gate of core 2 lifts first, and the case prints whether T ran within 200 ms of U, while
//   the gate of core 0 still held it. It goes last, since T may be left in the queues of core 0,
//   which its mask leaves out. Such a T gets every core once the gates lifted.
// 10 ms after the change, each case prints both statuses and current CPUs, then which of the two
// runs first once the gates lift, and the core T ran on. local_ready_requeue has a change to the
// priority T has, and ready_changes a raise to 0x6F and a move to an idle core.
//
// On a PS Vita TV on firmware 3.65 a STANDBY T keeps its place ahead of a STANDBY U through every
// change between priorities of the queues of one core, lowered to U's, raised to U's, or lowered
// and back, and T runs first on core 0. Lowered to 0xA0, T becomes READY in the queue the cores
// share, current CPU 0x7FFF, behind the READY U, and U runs first. Its mask moved to core 2 makes
// it STANDBY on core 2 behind U, and it runs there after U, while the gate still holds core 0. The
// mask change returns the old mask, 0x10000.
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define T_PRIORITY 0x70
#define SPINS 400000000

static SceUID gates[3];
static volatile int gate_up[3], gate_open[3], ran;
static volatile int order[2], cpus[2];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    const int core = *(const int *)argp;
    gate_up[core] = 1;
    for (volatile int n = 0; n < SPINS && !gate_open[core]; n++)
        ;
    return 0;
}

static SceUInt32 mask_of(int core) {
    return core ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_0;
}

static void gate_close(int core) {
    gate_up[core] = gate_open[core] = 0;
    gates[core] = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0,
                                        mask_of(core), NULL);
    sceKernelStartThread(gates[core], sizeof(core), &core);
    TEST_WAIT_FOR(gate_up[core]);
}

static void gate_lift(int core) {
    gate_open[core] = 1;
    test_join_thread(gates[core]);
}

// Records that it ran, 1 for T and 2 for U, and the core it ran on
static int thread_entry(SceSize args, void *argp) {
    (void)args;
    const int id = *(const int *)argp;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(0, &info);
    cpus[id - 1] = info.currentCpuId;
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = id;
    return 0;
}

static void start(SceUID thread, int id) {
    sceKernelStartThread(thread, sizeof(id), &id);
}

// Runs one case, where first and second are the priorities T gets, 0 for none, and to_core_2
// moves T to core 2 instead
static void run_case(const char *name, int u_priority, int first, int second, int to_core_2) {
    ran = 0;
    order[0] = order[1] = 0;
    cpus[0] = cpus[1] = -1;
    gate_close(0);
    if (to_core_2)
        gate_close(2);
    const SceUID t = sceKernelCreateThread("t", thread_entry, T_PRIORITY, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    const SceUID u = sceKernelCreateThread("u", thread_entry, u_priority, 0x4000, 0,
                                           mask_of(to_core_2 ? 2 : 0), NULL);
    start(t, 1);
    start(u, 2);
    // The starts hand T and U over to the cores the gates keep
    sceKernelDelayThread(10000);
    int result;
    if (to_core_2) {
        result = sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_2);
    } else {
        result = sceKernelChangeThreadPriority(t, first);
        if (second)
            result |= sceKernelChangeThreadPriority(t, second);
    }
    // The change reaches T on core 0 from core 1
    sceKernelDelayThread(10000);
    SceKernelThreadInfo t_info = { .size = sizeof(t_info) };
    SceKernelThreadInfo u_info = { .size = sizeof(u_info) };
    sceKernelGetThreadInfo(t, &t_info);
    sceKernelGetThreadInfo(u, &u_info);
    int t_early = 0;
    if (to_core_2) {
        gate_lift(2);
        TEST_WAIT_FOR(cpus[1] != -1);
        for (int i = 0; i < 200 && cpus[0] == -1; i++)
            sceKernelDelayThread(1000);
        t_early = cpus[0] != -1;
    }
    gate_lift(0);
    TEST_WAIT_FOR(ran == 2);
    // A T left in a queue that no core takes it from gets every core
    const int rescued = ran != 2;
    if (rescued) {
        sceKernelChangeThreadCpuAffinityMask(t, SCE_KERNEL_CPU_MASK_USER_ALL);
        TEST_WAIT_FOR(ran == 2);
    }
    // Joined first, so every record the threads made is complete
    SceUInt timeout = TEST_WAIT_US;
    if (sceKernelWaitThreadEnd(t, NULL, &timeout) >= 0)
        sceKernelDeleteThread(t);
    test_join_thread(u);

    PRINTF(result, "%s: the change", name);
    PRINTF(t_info.status, "%s: T's status", name);
    PRINTF(t_info.currentCpuId, "%s: T's current CPU", name);
    PRINTF(u_info.status, "%s: U's status", name);
    PRINTF(u_info.currentCpuId, "%s: U's current CPU", name);
    if (to_core_2) {
        PRINTF(t_early, "%s: T ran while the gate of core 0 held it", name);
        PRINTF(rescued, "%s: T only ran once given every core", name);
    }
    PRINTF(order[0], "%s: the first to run, 1 for T and 2 for U", name);
    PRINTF(cpus[0], "%s: the core T ran on", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    run_case("U at 0x78, T lowered to 0x78", 0x78, 0x78, 0, 0);
    run_case("U at 0x68, T raised to 0x68", 0x68, 0x68, 0, 0);
    run_case("U at 0x70, T lowered to 0x78 and back", 0x70, 0x78, T_PRIORITY, 0);
    run_case("U at 0xA0, T lowered to 0xA0", 0xA0, 0xA0, 0, 0);
    run_case("U at 0x70 on core 2, T moved to core 2", 0x70, 0, 0, 1);
    test_exit();
    return 0;
}

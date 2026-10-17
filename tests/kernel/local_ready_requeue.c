// Where a priority change made on a thread's own core puts it among the threads of its priority
// on that core. T and U run at 0x70 on core 0, behind a gate thread of the highest priority that
// spins there without syscalls until the main thread, on core 1, lifts it. T and U are made ready
// in that order, either by signals from the gate, on core 0, which makes them READY, or by starts
// from core 1, which makes them STANDBY (wake_core_origin). Then the gate itself changes T's
// priority, from core 0:
// - no change
// - to the 0x70 it has
// - raised to 0x6F and back to 0x70
// - lowered to 0x71 and back to 0x70
// Each case prints what the changes return, both statuses just before the gate lifts, and which
// of the two runs first after it. ready_changes and ready_queue_order make such changes from
// another core.
//
// On a PS Vita TV on firmware 3.65 T runs first when nothing changes, and U runs first after each
// change of a READY T, even to the priority it has, so a change puts a READY thread behind the
// others of its priority. A STANDBY T still runs first after every change, so a change leaves a
// STANDBY thread's place as it was.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define PRIORITY 0x70
#define SPINS 400000000

enum Action { NONE, SIGNAL_T, SIGNAL_U, CHANGE };

static SceUID sema[2], gate, t;
static volatile int gate_up, gate_open, gate_action, gate_acted, ran, change_result;
static volatile int first_change, second_change;
static volatile int order[2];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++) {
        const int action = gate_action;
        if (action != NONE && gate_acted != action) {
            if (action == SIGNAL_T) {
                sceKernelSignalSema(sema[0], 1);
            } else if (action == SIGNAL_U) {
                sceKernelSignalSema(sema[1], 1);
            } else {
                change_result = sceKernelChangeThreadPriority(t, first_change);
                if (second_change)
                    change_result |= sceKernelChangeThreadPriority(t, second_change);
            }
            gate_acted = action;
        }
    }
    return 0;
}

// Has the gate do action from core 0 and waits until it did
static void gate_do(int action) {
    gate_action = action;
    TEST_WAIT_FOR(gate_acted == action);
}

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

struct Arg {
    int id, wait;
};

// A thread that waits for its semaphore first when told to, then records that it ran
static int thread_entry(SceSize args, void *argp) {
    (void)args;
    const struct Arg arg = *(const struct Arg *)argp;
    if (arg.wait) {
        SceUInt32 timeout = 2 * TEST_WAIT_US;
        sceKernelWaitSema(sema[arg.id - 1], 1, &timeout);
    }
    record(arg.id);
    return 0;
}

static void start(SceUID thread, int id, int wait) {
    struct Arg arg = { id, wait };
    sceKernelStartThread(thread, sizeof(arg), &arg);
}

static SceUInt32 status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

static void run_case(const char *name, int ready, int first, int second) {
    ran = 0;
    order[0] = order[1] = 0;
    change_result = 0;
    first_change = first;
    second_change = second;
    sema[0] = sceKernelCreateSema("t", 0, 0, 1, NULL);
    sema[1] = sceKernelCreateSema("u", 0, 0, 1, NULL);
    t = sceKernelCreateThread("t", thread_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0,
                              NULL);
    const SceUID u = sceKernelCreateThread("u", thread_entry, PRIORITY, 0x4000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_0, NULL);
    if (ready) {
        start(t, 1, 1);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_SEMA, sema[0]));
        start(u, 2, 1);
        TEST_WAIT_FOR(test_in_wait(u, WAIT_TYPE_SEMA, sema[1]));
    }
    gate_up = gate_open = 0;
    gate_action = gate_acted = NONE;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0,
                                 SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
    if (ready) {
        gate_do(SIGNAL_T);
        gate_do(SIGNAL_U);
    } else {
        start(t, 1, 0);
        start(u, 2, 0);
        // The starts hand T and U over to core 0, which the gate keeps
        sceKernelDelayThread(10000);
    }
    if (first)
        gate_do(CHANGE);
    sceKernelDelayThread(10000);
    const SceUInt32 t_status = status_of(t), u_status = status_of(u);
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
    TEST_WAIT_FOR(ran == 2);
    // Joined first, so every record the threads made is complete
    test_join_thread(t);
    test_join_thread(u);
    sceKernelDeleteSema(sema[0]);
    sceKernelDeleteSema(sema[1]);

    const char *const how = ready ? "READY" : "STANDBY";
    if (first)
        PRINTF(change_result, "%s, %s: the changes", how, name);
    PRINTF(t_status, "%s, %s: T's status before the gate lifts", how, name);
    PRINTF(u_status, "%s, %s: U's status before the gate lifts", how, name);
    PRINTF(order[0], "%s, %s: the first to run, 1 for T and 2 for U", how, name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (int ready = 1; ready >= 0; ready--) {
        run_case("no change", ready, 0, 0);
        run_case("T set to the 0x70 it has", ready, PRIORITY, 0);
        run_case("T raised to 0x6F and back", ready, PRIORITY - 1, PRIORITY);
        run_case("T lowered to 0x71 and back", ready, PRIORITY + 1, PRIORITY);
    }
    test_exit();
    return 0;
}

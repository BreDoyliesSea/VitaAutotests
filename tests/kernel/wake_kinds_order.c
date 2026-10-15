// Which kinds of wake make a thread at priority 0x70 STANDBY in the queue of its core, like a
// start or a signal, and which make it READY, like its own timeout (wake_ready_areas). T and U
// run at 0x70 on core 0, behind a gate thread of the highest priority that spins there without
// syscalls until the main thread, on core 1, lifts it. T waits first, U is started, then the
// main thread ends T's wait in one of these ways: a semaphore signal, a semaphore cancel, a
// semaphore delete, an event flag set, a thread signal, a notify of a callback T waits with, the
// end of a thread T waits for, or the unlock of a lightweight mutex T waits for. Each case
// prints both statuses just before the gate lifts, and which of the two runs first after it.
//
// On a PS Vita TV on firmware 3.65 every one of these makes T STANDBY behind U. Only a timer,
// its wait's timeout or the end of its delay, makes a thread at that priority READY.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GATE_PRIORITY SCE_KERNEL_HIGHEST_PRIORITY_USER
#define PRIORITY 0x70
#define SPINS 400000000

static SceUID sema, flag, callback, ender, ender_sema, gate;
static SceKernelLwMutexWork lw_mutex;
static volatile int gate_up, gate_open, ran;
static volatile int order[2];

static int gate_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    gate_up = 1;
    for (volatile int n = 0; n < SPINS && !gate_open; n++)
        ;
    return 0;
}

static void gate_close(void) {
    gate_up = gate_open = 0;
    gate = sceKernelCreateThread("gate", gate_entry, GATE_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(gate, 0, NULL);
    TEST_WAIT_FOR(gate_up);
}

static void gate_lift(void) {
    gate_open = 1;
    sceKernelWaitThreadEnd(gate, NULL, NULL);
    sceKernelDeleteThread(gate);
}

static void record(int who) {
    order[__atomic_fetch_add(&ran, 1, __ATOMIC_SEQ_CST)] = who;
}

enum Case { SEMA_SIGNAL, SEMA_CANCEL, SEMA_DELETE, FLAG_SET, THREAD_SIGNAL, CALLBACK_NOTIFY, THREAD_END, LW_MUTEX_UNLOCK, CASE_COUNT };
static const char *const case_names[CASE_COUNT] = {
    "semaphore signal", "semaphore cancel", "semaphore delete", "event flag set",
    "thread signal", "callback notify", "thread end", "lightweight mutex unlock",
};

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    return 0;
}

// The thread T waits for in the thread end case, on core 2, until the main thread lets it end
static int ender_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelWaitSema(ender_sema, 1, &timeout);
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    const int c = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    SceUInt32 bits;
    switch (c) {
    case SEMA_SIGNAL:
    case SEMA_CANCEL:
    case SEMA_DELETE:
        sceKernelWaitSema(sema, 1, &timeout);
        break;
    case FLAG_SET:
        sceKernelWaitEventFlag(flag, 1, SCE_EVENT_WAITOR, &bits, &timeout);
        break;
    case THREAD_SIGNAL:
        sceKernelWaitSignal(0, 0, &timeout);
        break;
    case CALLBACK_NOTIFY:
        callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        sceKernelDelayThreadCB(TEST_WAIT_US);
        break;
    case THREAD_END:
        sceKernelWaitThreadEnd(ender, NULL, &timeout);
        break;
    case LW_MUTEX_UNLOCK:
        sceKernelLockLwMutex(&lw_mutex, 1, &timeout);
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    }
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

static void run_case(int c) {
    ran = 0;
    order[0] = order[1] = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    ender_sema = sceKernelCreateSema("ender", 0, 0, 1, NULL);
    sceKernelCreateLwMutex(&lw_mutex, "lw", 0, 0, NULL);
    if (c == THREAD_END) {
        ender = sceKernelCreateThread("ender", ender_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
        sceKernelStartThread(ender, 0, NULL);
        test_wait_until_waiting(ender);
    }
    if (c == LW_MUTEX_UNLOCK)
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);

    const SceUID t = sceKernelCreateThread("t", t_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, sizeof(c), &c);
    test_wait_until_waiting(t);
    gate_close();
    const SceUID u = sceKernelCreateThread("u", u_entry, PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(u, 0, NULL);

    switch (c) {
    case SEMA_SIGNAL:
        sceKernelSignalSema(sema, 1);
        break;
    case SEMA_CANCEL:
        sceKernelCancelSema(sema, -1, NULL);
        break;
    case SEMA_DELETE:
        sceKernelDeleteSema(sema);
        sema = -1;
        break;
    case FLAG_SET:
        sceKernelSetEventFlag(flag, 1);
        break;
    case THREAD_SIGNAL:
        sceKernelSendSignal(t);
        break;
    case CALLBACK_NOTIFY:
        sceKernelNotifyCallback(callback, 1);
        break;
    case THREAD_END:
        sceKernelSignalSema(ender_sema, 1);
        break;
    case LW_MUTEX_UNLOCK:
        sceKernelUnlockLwMutex(&lw_mutex, 1);
        break;
    }
    sceKernelDelayThread(10000);

    const SceUInt32 t_status = status_of(t), u_status = status_of(u);
    gate_lift();
    TEST_WAIT_FOR(ran == 2);
    PRINTF(u_status, "%s: U's status before the gate lifts", case_names[c]);
    PRINTF(t_status, "%s: T's status before the gate lifts", case_names[c]);
    PRINTF(order[0], "%s: the first to run, 1 for T and 2 for U", case_names[c]);
    test_join_thread(t);
    test_join_thread(u);
    if (c == THREAD_END)
        test_join_thread(ender);
    if (c == CALLBACK_NOTIFY)
        sceKernelDeleteCallback(callback);
    if (sema >= 0)
        sceKernelDeleteSema(sema);
    sceKernelDeleteEventFlag(flag);
    sceKernelDeleteSema(ender_sema);
    sceKernelDeleteLwMutex(&lw_mutex);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    for (int c = 0; c < CASE_COUNT; c++)
        run_case(c);
    test_exit();
    return 0;
}

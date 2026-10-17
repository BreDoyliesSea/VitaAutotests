// When an event flag without SCE_EVENT_WAITMULTIPLE counts its one waiter A as there, so that a
// second wait fails. Every thread is pinned to core 0. A waits at 0xB0, below the main thread at
// 0x60, so a wait the main thread ends leaves A ready but not running. A probe thread P, at 0x50,
// then waits for the same bit with a timeout of 1 ms, while the main thread spins without syscalls
// until P has its result, so A can't run in between. The cases:
// - A waits
// - A was woken by a set of its bit, which it clears, but hasn't run yet
// - A was woken by a cancel, but hasn't run yet
// - A waits and sceKernelSuspendThreadForVM holds it
// - A waits with callbacks and is in its callback, which waits on a semaphore
// - the main thread polls the flag while A waits for another bit
// Each case prints P's result and A's result once its wait ended. A last case lets a second
// waiter B wait for a long time while A is in its callback, then lets A's callback return, and
// prints what A and B report once A is back, and the results of both after a set of their bit and
// a cancel, 0 for the ones the set woke.
//
// On a PS Vita TV on firmware 3.65 a flag without SCE_EVENT_WAITMULTIPLE refuses a second waiter
// with 0x800280E2 while A waits, while A is suspended for VM and while A runs a callback, and so it
// refuses the main thread's poll for another bit. A woken by a set or a cancel frees it at once, so
// P's wait then times out (0x80028005). B, which tries to wait while A runs its callback, is
// refused too, and A waits again after its callback and gets the set.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define PROBE_PRIORITY 0x50
#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define SPINS 400000000

static SceUID flag, gate, callback;
static volatile int a_result, b_result, probe_result, callback_ready;

static int gate_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = 3 * TEST_WAIT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

// Waits for bit 0x1, with callbacks if the argument is 1, clearing it if it is 2
static int a_entry(SceSize args, void *argp) {
    (void)args;
    const int mode = *(const int *)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    SceUInt32 bits;
    if (mode == 1) {
        callback = sceKernelCreateCallback("gate", 0, gate_callback, NULL);
        callback_ready = 1;
        a_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
        sceKernelDeleteCallback(callback);
    } else {
        const SceUInt32 clear = mode == 2 ? SCE_EVENT_WAITCLEAR : 0;
        a_result = sceKernelWaitEventFlag(
            flag, mode == 3 ? 0x2 : 0x1, SCE_EVENT_WAITOR | clear, &bits, &timeout);
    }
    return 0;
}

static int b_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    SceUInt32 bits;
    b_result = sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    return 0;
}

static int probe_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 1000;
    SceUInt32 bits;
    probe_result = sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    return 0;
}

static SceUID start(SceKernelThreadEntry entry, int priority, SceUInt attr, int arg) {
    const SceUID thread =
        sceKernelCreateThread("t", entry, priority, 0x4000, attr, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(arg), &arg);
    return thread;
}

// Runs P, which takes the core at once, and spins until it has its result
static int probe(void) {
    probe_result = 0xDEAD;
    const SceUID p = start(probe_entry, PROBE_PRIORITY, 0, 0);
    for (volatile int n = 0; n < SPINS && probe_result == (int)0xDEAD; n++)
        ;
    const int r = probe_result;
    test_join_thread(p);
    return r;
}

typedef enum Case { WAITING, SET, CANCEL, SUSPENDED, IN_CALLBACK, POLL } Case;

static void run_case(const char *name, Case c) {
    a_result = 0xDEAD;
    callback_ready = 0;
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    const int mode = c == IN_CALLBACK ? 1 : c == SET ? 2 : c == POLL ? 3 : 0;
    const SceUID a = start(a_entry, WAITER_PRIORITY, c == SUSPENDED ? THREAD_ATTR_VM : 0, mode);
    TEST_WAIT_FOR(test_in_wait(
        a, c == IN_CALLBACK ? WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB : WAIT_TYPE_EVENT_FLAG, flag));
    int step = 0;
    if (c == SET)
        step = sceKernelSetEventFlag(flag, 0x1);
    if (c == CANCEL)
        step = sceKernelCancelEventFlag(flag, 0, NULL);
    if (c == SUSPENDED)
        sceKernelSuspendThreadForVM(a);
    if (c == IN_CALLBACK) {
        sceKernelNotifyCallback(callback, 1);
        TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, gate));
    }
    if (c == POLL) {
        sceKernelSetEventFlag(flag, 0x1);
        SceUInt32 bits = 0;
        step = sceKernelPollEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits);
    }
    const int p = c == POLL ? step : probe();
    if (c == SUSPENDED)
        sceKernelResumeThreadForVM(a);
    if (c == IN_CALLBACK) {
        sceKernelSignalSema(gate, 1);
        TEST_WAIT_FOR(
            test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag) || a_result != (int)0xDEAD);
    }
    sceKernelCancelEventFlag(flag, 0, NULL);
    test_join_thread(a);
    if (c == SET || c == CANCEL)
        PRINTF(step, "%s: the wake", name);
    PRINTF(p, c == POLL ? "%s: the main thread's poll" : "%s: P's wait", name);
    PRINTF(a_result, "%s: A's result", name);
    sceKernelDeleteEventFlag(flag);
}

// B waits while A is in its callback, then A's callback returns
static void back_case(void) {
    const char *name = "B waits while A is in its callback";
    a_result = b_result = 0xDEAD;
    callback_ready = 0;
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    const SceUID a = start(a_entry, WAITER_PRIORITY, 0, 1);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag));
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, gate));
    const SceUID b = start(b_entry, WAITER_PRIORITY, 0, 0);
    TEST_WAIT_FOR(test_in_wait(b, WAIT_TYPE_EVENT_FLAG, flag) || b_result != (int)0xDEAD);
    const int b_waits = test_in_wait(b, WAIT_TYPE_EVENT_FLAG, flag);
    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(
        test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag) || a_result != (int)0xDEAD);
    const int a_back = test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag);
    const int b_still = test_in_wait(b, WAIT_TYPE_EVENT_FLAG, flag);
    const int set = sceKernelSetEventFlag(flag, 0x1);
    sceKernelCancelEventFlag(flag, 0, NULL);
    test_join_thread(a);
    test_join_thread(b);
    PRINTF(b_waits, "%s: B waits", name);
    PRINTF(a_back, "%s: A waits on the flag again after its callback", name);
    PRINTF(b_still, "%s: B still waits then", name);
    PRINTF(set, "%s: the set", name);
    PRINTF(a_result, "%s: A's result", name);
    PRINTF(b_result, "%s: B's result", name);
    sceKernelDeleteEventFlag(flag);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);

    run_case("A waits", WAITING);
    run_case("A woken by a set", SET);
    run_case("A woken by a cancel", CANCEL);
    run_case("A suspended", SUSPENDED);
    run_case("A in its callback", IN_CALLBACK);
    run_case("A waits for another bit", POLL);
    back_case();

    sceKernelDeleteSema(gate);
    test_exit();
    return 0;
}

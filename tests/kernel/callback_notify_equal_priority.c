// Whether sceKernelNotifyCallback switches to a waiting thread of the same priority as the notifier
// on the same core before it returns, the way callback_notify_preempt shows it does for a higher
// priority one. Same priority threads on one core run one after another, so the waiter's callback
// may only run once the notifier blocks. A thread T creates a callback and waits with callbacks,
// the main thread notifies that callback. Every thread is pinned to core 0, and the main thread is
// at 0xA0. The cases:
// - T waits on a semaphore with a timeout, at 0xA0, 0xB0 (below the main thread) and 0x80 (above)
// - T delays, at 0xA0
// Right after the notify, before anything blocks, the main thread reads how often T's callback ran
// and T's status (2 is ready, 8 is waiting). Then it blocks until the callback ran and T is back in
// its wait, signals the semaphore if there is one, and joins T. Values are printed once the case is
// over, since printing lets T run. Each case prints that T waited with callbacks before the notify,
// the notify's result, the two values read after it, the runs of the callback at the end and the
// result of T's wait.
//
// On a PS Vita TV on firmware 3.65 the notify switches to T only when T is above the main thread,
// and then T's callback ran and T waits again (status 8) when the notify returns. At the same
// priority or below, T is ready (status 2) when the notify returns and its callback runs once the
// main thread blocks, for the semaphore and for the delay.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0xA0
#define ABOVE_PRIORITY 0x80
#define BELOW_PRIORITY 0xB0
#define NOT_DONE 0xDEAD

typedef enum Kind { SEMA, DELAY } Kind;

static Kind kind;
static SceUID sema, callback;
static volatile int callback_ready, callback_runs, wait_result;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

// Creates its callback, then waits with callbacks for the wait of the case
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 3 * TEST_WAIT_US;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    if (kind == SEMA)
        wait_result = sceKernelWaitSemaCB(sema, 1, &timeout);
    else
        wait_result = sceKernelDelayThreadCB(TEST_WAIT_US);
    sceKernelDeleteCallback(callback);
    return 0;
}

// Returns whether T is in the wait of the case, with callbacks. A delay has no UID to compare.
static int t_waits(SceUID t) {
    if (kind == SEMA)
        return test_in_wait(t, WAIT_TYPE_SEMA | WAIT_TYPE_CB, sema);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(t, &info) == 0 && info.waitType == (WAIT_TYPE_DELAY | WAIT_TYPE_CB);
}

// Waits for a thread to end, for a few seconds at most, and deletes it
static void join(SceUID thread) {
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitThreadEnd(thread, NULL, &timeout);
    sceKernelDeleteThread(thread);
}

static void run_case(const char *name, Kind k, int priority) {
    kind = k;
    wait_result = NOT_DONE;
    callback_ready = callback_runs = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    const SceUID t = sceKernelCreateThread("t", t_entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(t, 0, NULL);
    // A T that isn't above the main thread starts when the main thread blocks here
    TEST_WAIT_FOR(callback_ready && t_waits(t));
    const int waits_before = t_waits(t);

    const int notify = sceKernelNotifyCallback(callback, 1);
    const int runs_at_notify = callback_runs;
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(t, &info);
    const int status = info.status;

    TEST_WAIT_FOR(callback_runs != 0);
    TEST_WAIT_FOR(t_waits(t));
    if (k == SEMA)
        sceKernelSignalSema(sema, 1);
    join(t);
    sceKernelDeleteSema(sema);

    PRINTF(waits_before, "%s: T waits with callbacks before the notify", name);
    PRINTF(notify, "%s: the notify", name);
    PRINTF(runs_at_notify, "%s: runs of the callback when the notify returned", name);
    PRINTF(status, "%s: T's status right after the notify", name);
    PRINTF(callback_runs, "%s: runs of the callback at the end", name);
    PRINTF(wait_result, "%s: T's wait", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("semaphore, T at the priority of the main thread", SEMA, MAIN_PRIORITY);
    run_case("semaphore, T below the main thread", SEMA, BELOW_PRIORITY);
    run_case("semaphore, T above the main thread", SEMA, ABOVE_PRIORITY);
    run_case("delay, T at the priority of the main thread", DELAY, MAIN_PRIORITY);

    test_exit();
    return 0;
}

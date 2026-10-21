// Whether a callback that is notified after a wait with callbacks was woken, but before the waiting
// thread ran again, runs before that wait returns or stays pending. Every thread is pinned to core 0.
// The waiter T waits at 0xB0, below the main thread at 0x60, so a wait the main thread ends leaves T
// ready but not running. The main thread wakes T, notifies T's callback and then blocks, which lets
// T run. T records how often its callback ran when the wait returned, calls sceKernelCheckCallback
// and records its result and how often the callback ran then. The cases:
// - T waits on a semaphore with callbacks and the main thread signals the semaphore
// - T waits for a thread signal with callbacks and the main thread sends it a signal
// - T waits for a thread to end with callbacks and the main thread releases that thread, which runs
//   at 0x50 and so ends before the main thread goes on
// Each case prints whether T was in the wait when the main thread woke it, the wake and the notify,
// T's status after each (ready, so it hadn't run yet), the wait's result, and the two run counts
// and the check's result from T.
//
// On a PS Vita TV on firmware 3.65 T is ready, with status 0x2, after the wake and after the
// notify. The semaphore wait runs the callback before it returns, and the check that follows has
// nothing to run and returns 0. The signal wait and the thread end wait return without running it,
// and the check runs it and returns 1.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define ENDER_PRIORITY 0x50
#define MAIN_PRIORITY 0x60
#define T_PRIORITY 0xB0
#define NOT_DONE 0xDEAD

typedef enum Kind { SEMA, SIGNAL, THREAD_END } Kind;

static Kind kind;
static SceUID sema, callback, ender;
static volatile int callback_ready, callback_runs, done;
static volatile int wait_result, runs_in_wait, check_result, runs_after_check;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    SceInt32 status = 0;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    callback_ready = 1;
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(sema, 1, &timeout); break;
    case SIGNAL: wait_result = sceKernelWaitSignalCB(0, 0, &timeout); break;
    case THREAD_END: wait_result = sceKernelWaitThreadEndCB(ender, &status, &timeout); break;
    }
    runs_in_wait = callback_runs;
    check_result = sceKernelCheckCallback();
    runs_after_check = callback_runs;
    sceKernelDeleteCallback(callback);
    done = 1;
    return 0;
}

// Waits on the semaphore, so that the main thread can end it by a signal
static int ender_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

static SceUID start(SceKernelThreadEntry entry, int priority) {
    const SceUID thread = sceKernelCreateThread("t", entry, priority, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    return thread;
}

static int status_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.status;
}

// Returns whether T is in the wait of the current case, with callbacks
static int t_waits(SceUID t) {
    switch (kind) {
    case SEMA: return test_in_wait(t, WAIT_TYPE_SEMA | WAIT_TYPE_CB, sema);
    case SIGNAL: {
        // The signal wait's UID is the same for every thread, so only its type is checked
        SceKernelThreadInfo info = { .size = sizeof(info) };
        return sceKernelGetThreadInfo(t, &info) == 0 && info.waitType == (WAIT_TYPE_SIGNAL | WAIT_TYPE_CB);
    }
    case THREAD_END: return test_in_wait(t, WAIT_TYPE_THREAD_END | WAIT_TYPE_CB, ender);
    }
    return 0;
}

static void run_case(const char *name, Kind k) {
    kind = k;
    wait_result = runs_in_wait = check_result = runs_after_check = NOT_DONE;
    callback_ready = callback_runs = done = 0;
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    if (k == THREAD_END) {
        ender = start(ender_entry, ENDER_PRIORITY);
        TEST_WAIT_FOR(test_in_wait(ender, WAIT_TYPE_SEMA, sema));
    }
    const SceUID t = start(t_entry, T_PRIORITY);
    TEST_WAIT_FOR(callback_ready && t_waits(t));
    const int waiting = t_waits(t);
    const int wake = k == SIGNAL ? sceKernelSendSignal(t) : sceKernelSignalSema(sema, 1);
    const int status_wake = status_of(t);
    const int notify = sceKernelNotifyCallback(callback, 1);
    const int status_notify = status_of(t);
    TEST_WAIT_FOR(done);
    test_join_thread(t);
    if (k == THREAD_END)
        test_join_thread(ender);
    sceKernelDeleteSema(sema);

    PRINTF(waiting, "%s: T waits when the main thread wakes it", name);
    PRINTF(wake, "%s: the wake", name);
    PRINTF(status_wake, "%s: T's status after the wake", name);
    PRINTF(notify, "%s: the notify", name);
    PRINTF(status_notify, "%s: T's status after the notify", name);
    PRINTF(wait_result, "%s: the wait's result", name);
    PRINTF(runs_in_wait, "%s: runs of the callback when the wait returned", name);
    PRINTF(check_result, "%s: the check's result", name);
    PRINTF(runs_after_check, "%s: runs of the callback after the check", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("semaphore wait", SEMA);
    run_case("signal wait", SIGNAL);
    run_case("thread end wait", THREAD_END);

    test_exit();
    return 0;
}

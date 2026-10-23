// Whether a callback notified after a condition variable waiter's wait was ended, but before the
// waiter ran again, runs before that wait returns or stays pending. Every thread is pinned to core
// 0. The waiter T runs at 0xB0, below the main thread at 0x60, so a wait the main thread ends
// leaves T ready but not running. T locks a heavy mutex M and waits on a heavy condition C with
// callbacks and a timeout of 4 s, which releases M. Every case has fresh objects. The cases:
// - the main thread signals C, notifies T's callback and then blocks, so T takes the free M back
// - the main thread locks M and signals C, blocks until T waits for M, then unlocks M, which hands
//   M over to T, notifies T's callback and then blocks
// - the main thread deletes C, notifies T's callback and then blocks
// Each case prints T's status after the main thread ended the wait and after the notify, the
// results of the calls that ended it, whether T waited for M in the second case, T's wait result,
// how often the callback ran when the wait returned, T's unlock of M afterwards, and the result of
// the sceKernelCheckCallback T calls next with how often the callback ran after it.
//
// On a PS Vita TV on firmware 3.65 T is ready, with status 0x2, after the main thread ended the
// wait and after the notify. After a signal T's wait returns 0 with M and the callback ran before
// it returned, so the check returns 0. When the mutex was handed over to T, the wait returns 0 with
// the callback still pending, and the check runs it and returns 1. After the delete of C T's wait
// returns 0x800281A4 without M, its unlock fails with 0x80028146, the callback ran before the wait
// returned and the check returns 0.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define T_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define WAIT_US (2 * TEST_WAIT_US)

typedef enum Kind { SIGNAL, HANDOVER, DELETE } Kind;

static SceUID mutex, cond, callback;
static volatile int callback_ready, callback_runs, done;
static volatile int wait_result, runs_in_wait, unlock_result, check_result, runs_after_check;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    return 0;
}

// Locks M and waits on C with callbacks
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    sceKernelLockMutex(mutex, 1, NULL);
    callback_ready = 1;
    SceUInt timeout = WAIT_US;
    wait_result = sceKernelWaitCondCB(cond, &timeout);
    runs_in_wait = callback_runs;
    unlock_result = sceKernelUnlockMutex(mutex, 1);
    check_result = sceKernelCheckCallback();
    runs_after_check = callback_runs;
    sceKernelDeleteCallback(callback);
    done = 1;
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

static void run_case(const char *name, Kind kind) {
    callback_ready = callback_runs = done = 0;
    wait_result = runs_in_wait = unlock_result = check_result = runs_after_check = NOT_DONE;
    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    cond = sceKernelCreateCond("cond", 0, mutex, NULL);

    const SceUID t = start(t_entry, T_PRIORITY);
    TEST_WAIT_FOR(callback_ready && test_in_wait(t, WAIT_TYPE_COND | WAIT_TYPE_CB, cond));
    int end = NOT_DONE, waits_for_mutex = NOT_DONE;
    switch (kind) {
    case SIGNAL: end = sceKernelSignalCond(cond); break;
    case HANDOVER:
        sceKernelLockMutex(mutex, 1, NULL);
        sceKernelSignalCond(cond);
        TEST_WAIT_FOR(test_in_wait(t, WAIT_TYPE_COND_MUTEX | WAIT_TYPE_CB, mutex));
        waits_for_mutex = test_in_wait(t, WAIT_TYPE_COND_MUTEX | WAIT_TYPE_CB, mutex);
        end = sceKernelUnlockMutex(mutex, 1);
        break;
    case DELETE: end = sceKernelDeleteCond(cond); break;
    }
    const int status_end = status_of(t);
    const int notify = sceKernelNotifyCallback(callback, 1);
    const int status_notify = status_of(t);
    TEST_WAIT_FOR(done);
    test_join_thread(t);
    if (kind != DELETE)
        sceKernelDeleteCond(cond);
    sceKernelDeleteMutex(mutex);

    if (kind == HANDOVER)
        PRINTF(waits_for_mutex, "%s: T waits for the mutex after the signal", name);
    PRINTF(end, "%s: the call that ended the wait", name);
    PRINTF(status_end, "%s: T's status after it", name);
    PRINTF(notify, "%s: the notify", name);
    PRINTF(status_notify, "%s: T's status after the notify", name);
    PRINTF(wait_result, "%s: T's wait", name);
    PRINTF(runs_in_wait, "%s: runs of the callback when the wait returned", name);
    PRINTF(unlock_result, "%s: T's unlock", name);
    PRINTF(check_result, "%s: T's check", name);
    PRINTF(runs_after_check, "%s: runs of the callback after the check", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    run_case("signal, then notify", SIGNAL);
    run_case("mutex handed over, then notify", HANDOVER);
    run_case("condition deleted, then notify", DELETE);

    test_exit();
    return 0;
}

// What a callback notified while a condition variable waiter waits for the signal does to the
// wait. A thread T locks a heavy mutex M and waits on a heavy condition C with callbacks and a
// timeout of 4 s, which releases M. The main thread notifies T's callback while T waits for the
// signal, waits until the callback ran and T waits on C again, and then signals C. Every case has
// fresh objects. The cases:
// - the callback does nothing
// - the callback locks M, which is free since T's wait released it, so T waits on C again owning M.
//   After the signal the main thread waits up to 5 s for T to wait for M, records whether it does,
//   and deletes M, which ends any wait for it.
// Each case prints whether T waited on C again after the callback ran, the callback's lock result
// in the second case, the signal's result, whether T then waited for the mutex it holds and the
// delete's result in the second case, T's wait result, how often the callback ran when the wait
// returned, T's unlock of M afterwards, and the result of the sceKernelCheckCallback T calls next.
//
// On a PS Vita TV on firmware 3.65 T waits on C again after its callback in both cases. When the
// callback did nothing, T's wait returns 0 with M, and its unlock returns 0. When the callback
// locked M, T doesn't wait for the mutex it holds: its wait returns 0x80028142 at once, the
// callback ran once, and T still holds M from the callback, so its unlock returns 0.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define WAIT_US (2 * TEST_WAIT_US)

static int locks_mutex;
static SceUID mutex, cond, callback;
static volatile int callback_ready, callback_runs, lock_in_callback, done;
static volatile int wait_result, runs_in_wait, unlock_result, check_result;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    if (locks_mutex)
        lock_in_callback = sceKernelLockMutex(mutex, 1, NULL);
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
    sceKernelDeleteCallback(callback);
    done = 1;
    return 0;
}

static void run_case(const char *name, int locks) {
    locks_mutex = locks;
    callback_ready = callback_runs = done = 0;
    lock_in_callback = wait_result = runs_in_wait = unlock_result = check_result = NOT_DONE;
    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    cond = sceKernelCreateCond("cond", 0, mutex, NULL);

    const SceUID t = test_start_thread(t_entry);
    TEST_WAIT_FOR(callback_ready && test_in_wait(t, WAIT_TYPE_COND | WAIT_TYPE_CB, cond));
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(callback_runs == 1 && test_in_wait(t, WAIT_TYPE_COND | WAIT_TYPE_CB, cond));
    const int waits_again = test_in_wait(t, WAIT_TYPE_COND | WAIT_TYPE_CB, cond);
    const int signal = sceKernelSignalCond(cond);
    int waits_for_own_mutex = NOT_DONE, delete_result = NOT_DONE;
    if (locks) {
        TEST_WAIT_FOR(done || test_in_wait(t, WAIT_TYPE_COND_MUTEX | WAIT_TYPE_CB, mutex));
        waits_for_own_mutex = test_in_wait(t, WAIT_TYPE_COND_MUTEX | WAIT_TYPE_CB, mutex);
        delete_result = sceKernelDeleteMutex(mutex);
    }
    TEST_WAIT_FOR(done);
    test_join_thread(t);
    sceKernelDeleteCond(cond);
    if (!locks)
        sceKernelDeleteMutex(mutex);

    PRINTF(waits_again, "%s: T waits on the condition again after the callback", name);
    if (locks)
        PRINTF(lock_in_callback, "%s: the callback's lock", name);
    PRINTF(signal, "%s: the signal", name);
    if (locks) {
        PRINTF(waits_for_own_mutex, "%s: T then waits for the mutex it holds", name);
        PRINTF(delete_result, "%s: the delete of the mutex", name);
    }
    PRINTF(wait_result, "%s: T's wait", name);
    PRINTF(runs_in_wait, "%s: runs of the callback when the wait returned", name);
    PRINTF(unlock_result, "%s: T's unlock", name);
    PRINTF(check_result, "%s: T's check", name);
}

int main(void) {
    run_case("callback does nothing", 0);
    run_case("callback locks the mutex", 1);

    test_exit();
    return 0;
}

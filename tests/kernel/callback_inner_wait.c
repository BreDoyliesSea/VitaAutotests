// A thread waits with callbacks, and the callback it runs blocks in a wait of its own. What the
// thread and the objects report then, and what happens when that wait is on the same object as the
// one the callback runs in. Where a wake could go to either wait, the callback's wait has a short
// timeout, so it ends either way and the test waits for its result instead of guessing a delay.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { OTHER, SAME_SEMA, SAME_MUTEX, SINGLE_FLAG, INNER_TIMEOUT };

// Far longer than the test needs to wake a wait, far shorter than the outer wait's timeout
#define INNER_TIMEOUT_US 300000

static SceUID outer, inner, mutex, flag, cb;
static volatile int kind, inner_result, outer_result;

static int inner_wait_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = 2 * TEST_WAIT_US;
    unsigned int bits;
    switch (kind) {
    case OTHER: inner_result = sceKernelWaitSema(inner, 1, &timeout); break;
    case SAME_SEMA:
        timeout = INNER_TIMEOUT_US;
        inner_result = sceKernelWaitSema(outer, 1, &timeout);
        break;
    case SAME_MUTEX:
        timeout = INNER_TIMEOUT_US;
        inner_result = sceKernelLockMutex(mutex, 1, &timeout);
        if (inner_result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case SINGLE_FLAG:
        timeout = 1000;
        inner_result = sceKernelWaitEventFlag(flag, 0x2, EVF_WAITMODE_AND, &bits, &timeout);
        break;
    case INNER_TIMEOUT:
        timeout = 1000;
        inner_result = sceKernelWaitSema(inner, 1, &timeout);
        break;
    }
    return 0;
}

static int outer_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("inner", 0, inner_wait_callback, NULL);
    SceUInt timeout = 4 * TEST_WAIT_US;
    unsigned int bits;
    switch (kind) {
    case SAME_MUTEX:
        outer_result = sceKernelLockMutexCB(mutex, 1, &timeout);
        if (outer_result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case SINGLE_FLAG: outer_result = sceKernelWaitEventFlagCB(flag, 0x1, EVF_WAITMODE_AND, &bits, &timeout); break;
    default: outer_result = sceKernelWaitSemaCB(outer, 1, &timeout); break;
    }
    sceKernelDeleteCallback(cb);
    return 0;
}

// Starts a thread that waits with callbacks, on new semaphores so nothing is left from another case
static SceUID start(int new_kind) {
    outer = sceKernelCreateSema("outer", 0, 0, 2, NULL);
    inner = sceKernelCreateSema("inner", 0, 0, 1, NULL);
    kind = new_kind;
    inner_result = outer_result = 0xDEAD;
    const SceUID thread = test_start_thread(outer_thread);
    test_wait_until_waiting(thread);
    return thread;
}

static int sema_count(SceUID sema);

// Waits for the thread to end, deletes the semaphores of its case and returns the count the outer
// one had left
static int end(SceUID thread) {
    test_join_thread(thread);
    const int count = sema_count(outer);
    sceKernelDeleteSema(outer);
    sceKernelDeleteSema(inner);
    return count;
}

static void print_thread(const char *name, SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINTF(info.status, "%s: the thread's status", name);
    PRINTF(info.waitType, "%s: its wait type", name);
    PRINTF(info.waitId == outer, "%s: its wait UID is the outer semaphore", name);
    PRINTF(info.waitId == inner, "%s: its wait UID is the inner semaphore", name);
}

static int sema_waiters(SceUID sema) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    return info.numWaitThreads;
}

static int sema_count(SceUID sema) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    return info.currentCount;
}

int main(void) {
    // A wait on another object. The thread is blocked in one wait or the other at each read.
    SceUID thread = start(OTHER);
    print_thread("before the callback", thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, inner));
    print_thread("in the callback's wait", thread);
    PRINT("in the callback's wait: the outer semaphore's waiters", sema_waiters(outer));
    PRINT("in the callback's wait: the inner semaphore's waiters", sema_waiters(inner));
    PRINT("in the callback's wait: the callback's notifications", sceKernelGetCallbackCount(cb));
    sceKernelSignalSema(inner, 1);
    TEST_WAIT_FOR(inner_result != 0xDEAD);
    PRINT("the callback's wait", inner_result);
    sceKernelSignalSema(outer, 1);
    end(thread);
    PRINT("the outer wait after the callback", outer_result);

    // A wait on the same semaphore. If the signal goes to the outer wait, the callback's wait times
    // out and the second signal is left over. If it goes to the callback's wait, the outer one takes
    // the second signal.
    thread = start(SAME_SEMA);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, outer));
    // Nothing prints before the signal, so it comes long before the callback's wait times out
    const int sema_waiting = sema_waiters(outer);
    sceKernelSignalSema(outer, 1);
    TEST_WAIT_FOR(inner_result != 0xDEAD);
    PRINT("same semaphore: its waiters", sema_waiting);
    PRINT("same semaphore: the callback's wait", inner_result);
    sceKernelSignalSema(outer, 1);
    const int count_left = end(thread);
    PRINT("same semaphore: the outer wait", outer_result);
    PRINT("same semaphore: the count at the end", count_left);

    // A lock of the same mutex, which this thread holds. If the unlock hands it to the outer lock,
    // the thread owns it and the callback's lock times out.
    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    sceKernelLockMutex(mutex, 1, NULL);
    thread = start(SAME_MUTEX);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_MUTEX, mutex));
    MutexInfo mutex_info = { .size = sizeof(mutex_info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&mutex_info);
    sceKernelUnlockMutex(mutex, 1);
    TEST_WAIT_FOR(inner_result != 0xDEAD);
    PRINT("same mutex: its waiters", mutex_info.numWaitThreads);
    PRINT("same mutex: the callback's lock", inner_result);
    end(thread);
    PRINT("same mutex: the outer lock", outer_result);
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&mutex_info);
    PRINT("same mutex: its count at the end", mutex_info.currentCount);
    sceKernelDeleteMutex(mutex);

    // A second wait on an event flag that only lets one thread wait
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    thread = start(SINGLE_FLAG);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(inner_result != 0xDEAD);
    PRINT("single waiter event flag: the callback's wait", inner_result);
    sceKernelSetEventFlag(flag, 0x1);
    end(thread);
    PRINT("single waiter event flag: the outer wait", outer_result);
    sceKernelDeleteEventFlag(flag);

    // The callback's wait times out and the outer wait goes on
    thread = start(INNER_TIMEOUT);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(inner_result != 0xDEAD);
    PRINT("inner timeout: the callback's wait", inner_result);
    sceKernelSignalSema(outer, 1);
    end(thread);
    PRINT("inner timeout: the outer wait", outer_result);

    test_exit();
    return 0;
}

// A thread A waits with callbacks on an object, its callback is notified, and the callback waits on
// the same object for something the object can give. Whether that wait is stuck behind A's own wait
// on the object, what the object reports from inside the callback, and how A's wait goes on once
// the callback is back. The callback records the object's state before its wait, the wait's result
// and the state after it. Every wait in a callback has a timeout of 500 ms, so it ends either way.
// A's own wait is long, and the main thread ends it with a cancel once the callback recorded what it
// saw, so the test always ends. Each case uses new objects. The cases:
// - a FIFO semaphore with a count of 1, A waits for 2, the callback waits for 1
// - the same with a priority semaphore
// - a FIFO semaphore with a count of 1, A waits for 2 and a thread B without callbacks waits for 1
//   behind it, the callback waits for 1 after the notify. The test prints whether B got the count.
// - a reader/writer lock the main thread holds for reading, A waits for the write lock, the callback
//   takes a read lock. Then the main thread gives up its read lock, and A's write lock is checked
//
// On a PS Vita TV on firmware 3.65 the notify takes A's wait out of the object, so inside the
// callback the object counts no waiter, and the callback's wait gets the count, or the read lock
// next to the main thread's, at once. With B waiting behind A, A's leaving gives B the count, so
// the callback's wait times out. Once the callback is back, A waits again and is counted, until the
// cancel or the main thread's unlock ends its wait.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
// The timeout of every wait the callback makes
#define INNER_TIMEOUT_US 500000
// The timeout of the waits the main thread ends
#define OUTER_TIMEOUT_US (3 * TEST_WAIT_US)

enum Kind { SEMA, SEMA_BEHIND, RWLOCK };

static SceUID object, callback;
static volatile int kind, callback_done, a_result, a_unlock, b_result, inner_result, inner_unlock;
static volatile int before[3], after[3];

// Records the object's state before and after a wait on the same object, which times out at 500 ms
static int inner_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = INNER_TIMEOUT_US;
    switch (kind) {
    case SEMA:
    case SEMA_BEHIND: {
        SceKernelSemaInfo info = { .size = sizeof(info) };
        sceKernelGetSemaInfo(object, &info);
        before[0] = info.numWaitThreads;
        before[1] = info.currentCount;
        inner_result = sceKernelWaitSema(object, 1, &timeout);
        sceKernelGetSemaInfo(object, &info);
        after[0] = info.numWaitThreads;
        after[1] = info.currentCount;
        break;
    }
    case RWLOCK: {
        SceKernelRWLockInfo info = { .size = sizeof(info) };
        sceKernelGetRWLockInfo(object, &info);
        before[0] = info.numReadWaitThreads;
        before[1] = info.numWriteWaitThreads;
        before[2] = info.lockCount;
        inner_result = sceKernelLockReadRWLock(object, &timeout);
        sceKernelGetRWLockInfo(object, &info);
        after[0] = info.numReadWaitThreads;
        after[1] = info.numWriteWaitThreads;
        after[2] = info.lockCount;
        if (inner_result == 0)
            inner_unlock = sceKernelUnlockReadRWLock(object);
        break;
    }
    }
    callback_done = 1;
    return 0;
}

// Waits with callbacks on the object, for more than the callback's wait can give
static int a_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("inner", 0, inner_callback, NULL);
    SceUInt timeout = OUTER_TIMEOUT_US;
    switch (kind) {
    case SEMA:
    case SEMA_BEHIND: a_result = sceKernelWaitSemaCB(object, 2, &timeout); break;
    case RWLOCK:
        a_result = sceKernelLockWriteRWLockCB(object, &timeout);
        if (a_result == 0)
            a_unlock = sceKernelUnlockWriteRWLock(object);
        break;
    }
    sceKernelDeleteCallback(callback);
    return 0;
}

// Waits for 1 on the semaphore without callbacks, behind A
static int b_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = OUTER_TIMEOUT_US;
    b_result = sceKernelWaitSema(object, 1, &timeout);
    return 0;
}

// Starts A on the new kind of case and waits until it waits on the object with callbacks
static SceUID start_a(int new_kind, SceUInt32 wait_type) {
    kind = new_kind;
    callback_done = 0;
    a_result = a_unlock = b_result = inner_result = inner_unlock = NOT_DONE;
    for (int i = 0; i < 3; i++)
        before[i] = after[i] = NOT_DONE;
    const SceUID a = test_start_thread(a_thread);
    TEST_WAIT_FOR(test_in_wait(a, wait_type | WAIT_TYPE_CB, object));
    return a;
}

// Notifies A's callback and waits until it recorded what it saw
static void notify_and_wait(void) {
    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(callback_done);
}

// Waits until A waits on the object again after its callback, or has ended
static int wait_for_a_again(SceUID a, SceUInt32 wait_type) {
    TEST_WAIT_FOR(a_result != NOT_DONE || test_in_wait(a, wait_type | WAIT_TYPE_CB, object));
    return test_in_wait(a, wait_type | WAIT_TYPE_CB, object);
}

static void sema_case(const char *name, int new_kind, SceUInt32 attr) {
    object = sceKernelCreateSema("sema", attr, 1, 10, NULL);
    const SceUID a = start_a(new_kind, WAIT_TYPE_SEMA);
    SceUID b = 0;
    if (new_kind == SEMA_BEHIND) {
        b = test_start_thread(b_thread);
        // B waits behind A, or takes the count at once and ends
        TEST_WAIT_FOR(b_result != NOT_DONE || test_in_wait(b, WAIT_TYPE_SEMA, object));
    }
    notify_and_wait();
    PRINTF(before[0], "%s: in the callback, the waiting threads before its wait", name);
    PRINTF(before[1], "%s: in the callback, the count before its wait", name);
    PRINTF(inner_result, "%s: the callback's wait", name);
    PRINTF(after[0], "%s: in the callback, the waiting threads after its wait", name);
    PRINTF(after[1], "%s: in the callback, the count after its wait", name);
    if (b) {
        // B ended when A left the queue, or it still waits
        TEST_WAIT_FOR(b_result != NOT_DONE || test_in_wait(b, WAIT_TYPE_SEMA, object));
        PRINTF(b_result, "%s: B's result once the callback ran", name);
        PRINTF(test_in_wait(b, WAIT_TYPE_SEMA, object), "%s: B still waits then", name);
    }

    PRINTF(wait_for_a_again(a, WAIT_TYPE_SEMA), "%s: A waits again after the callback", name);
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(object, &info);
    PRINTF(info.currentCount, "%s: the count then", name);
    PRINTF(info.numWaitThreads, "%s: the waiting threads then", name);
    int canceled = NOT_DONE;
    PRINTF(sceKernelCancelSema(object, 0, &canceled), "%s: cancel the semaphore", name);
    PRINTF(canceled, "%s: the waiting threads it ended", name);
    test_join_thread(a);
    PRINTF(a_result, "%s: A's wait", name);
    if (b) {
        test_join_thread(b);
        PRINTF(b_result, "%s: B's wait at the end", name);
    }
    sceKernelDeleteSema(object);
}

static void rwlock_case(const char *name) {
    object = sceKernelCreateRWLock("rwlock", 0, NULL);
    SceUInt lock_timeout = TEST_WAIT_US;
    PRINTF(sceKernelLockReadRWLock(object, &lock_timeout), "%s: the main thread takes a read lock", name);
    const SceUID a = start_a(RWLOCK, WAIT_TYPE_RW_LOCK);
    notify_and_wait();
    PRINTF(before[0], "%s: in the callback, the read waiters before its wait", name);
    PRINTF(before[1], "%s: in the callback, the write waiters before its wait", name);
    PRINTF(before[2], "%s: in the callback, the lock count before its wait", name);
    PRINTF(inner_result, "%s: the callback's read lock", name);
    PRINTF(after[0], "%s: in the callback, the read waiters after its wait", name);
    PRINTF(after[1], "%s: in the callback, the write waiters after its wait", name);
    PRINTF(after[2], "%s: in the callback, the lock count after its wait", name);
    PRINTF(inner_unlock, "%s: the callback's unlock", name);

    PRINTF(wait_for_a_again(a, WAIT_TYPE_RW_LOCK), "%s: A waits again after the callback", name);
    SceKernelRWLockInfo info = { .size = sizeof(info) };
    sceKernelGetRWLockInfo(object, &info);
    PRINTF(info.lockCount, "%s: the lock count then", name);
    PRINTF(info.numReadWaitThreads, "%s: the read waiters then", name);
    PRINTF(info.numWriteWaitThreads, "%s: the write waiters then", name);
    PRINTF(sceKernelUnlockReadRWLock(object), "%s: the main thread gives up its read lock", name);
    // A gets the write lock when nothing holds the lock, so it ends. If it still waits, cancel it.
    TEST_WAIT_FOR(a_result != NOT_DONE);
    const int still_waits = test_in_wait(a, WAIT_TYPE_RW_LOCK | WAIT_TYPE_CB, object);
    PRINTF(still_waits, "%s: A still waits after the unlock", name);
    if (still_waits)
        sceKernelCancelRWLock(object, NULL, NULL, 0);
    test_join_thread(a);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(a_unlock, "%s: A's unlock after its wait", name);
    sceKernelGetRWLockInfo(object, &info);
    PRINTF(info.lockCount, "%s: the lock count at the end", name);
    sceKernelDeleteRWLock(object);
}

int main(void) {
    sema_case("FIFO semaphore", SEMA, 0);
    sema_case("priority semaphore", SEMA, SCE_KERNEL_ATTR_THREAD_PRIO);
    sema_case("FIFO semaphore with a waiter behind A", SEMA_BEHIND, 0);
    rwlock_case("reader/writer lock");

    test_exit();
    return 0;
}

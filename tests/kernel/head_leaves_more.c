// What happens to a reader queued behind a waiting writer on a rwlock once that writer stops
// waiting. The main thread holds a read lock, writer W waits for the write lock, and reader R
// waits behind it for a read lock. Then W leaves the head: its timeout passes, its priority drops
// below R's in a priority queue, or it runs a callback during a wait with callbacks. Each case
// prints whether R got its read lock while W was gone.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define HIGH_PRIORITY 0x80
#define LOW_PRIORITY 0x90

static SceUID rwlock, callback;
static volatile int result[2], done[2], in_callback, callback_done, use_cb, release_reader, reader_ended;
static volatile SceUInt w_timeout;

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    in_callback = 1;
    sceKernelDelayThread(300000);
    callback_done = 1;
    return 0;
}

static int writer(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = w_timeout;
    if (use_cb) {
        callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        result[0] = sceKernelLockWriteRWLockCB(rwlock, &timeout);
        sceKernelDeleteCallback(callback);
    } else {
        result[0] = sceKernelLockWriteRWLock(rwlock, &timeout);
    }
    if (result[0] == 0)
        sceKernelUnlockWriteRWLock(rwlock);
    done[0] = 1;
    return 0;
}

static int reader(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    result[1] = sceKernelLockReadRWLock(rwlock, &timeout);
    done[1] = 1;
    // Keeps the read lock until the test ends the case
    TEST_WAIT_FOR(release_reader);
    if (result[1] == 0)
        sceKernelUnlockReadRWLock(rwlock);
    reader_ended = 1;
    return 0;
}

static SceUID threads[2];

static void start(SceUInt32 attr, int w_priority, SceUInt timeout, int cb) {
    rwlock = sceKernelCreateRWLock("rw", attr, NULL);
    sceKernelLockReadRWLock(rwlock, NULL);
    use_cb = cb;
    in_callback = callback_done = release_reader = reader_ended = 0;
    w_timeout = timeout;
    for (int i = 0; i < 2; i++) {
        result[i] = 0xDEAD;
        done[i] = 0;
    }
    threads[0] = sceKernelCreateThread("writer", writer, w_priority, 0x4000, 0, 0, NULL);
    sceKernelStartThread(threads[0], 0, NULL);
    test_wait_until_waiting(threads[0]);
    threads[1] = sceKernelCreateThread("reader", reader, LOW_PRIORITY, 0x4000, 0, 0, NULL);
    sceKernelStartThread(threads[1], 0, NULL);
    test_wait_until_waiting(threads[1]);
}

// Waits up to 500 ms for R to be done, for an outcome that would come at once
static int reader_done_soon(void) {
    for (int i = 0; i < 500 && !done[1]; i++)
        sceKernelDelayThread(1000);
    return done[1];
}

static void finish(void) {
    sceKernelUnlockReadRWLock(rwlock);
    release_reader = 1;
    TEST_WAIT_FOR(done[0] && reader_ended);
    test_join_thread(threads[1]);
    test_join_thread(threads[0]);
    sceKernelDeleteRWLock(rwlock);
}

int main(void) {
    start(0, HIGH_PRIORITY, TEST_WAIT_US, 0);
    const int baseline = reader_done_soon();
    PRINT("a reader behind a waiting writer while a read lock is held: it got the lock", baseline);
    finish();
    PRINT("a reader behind a waiting writer: the writer's result after the read unlock", result[0]);

    start(0, HIGH_PRIORITY, 200000, 0);
    TEST_WAIT_FOR(done[0]);
    PRINT("the writer times out: its result", result[0]);
    PRINT("the writer times out: the reader got its lock after it", reader_done_soon());
    finish();
    PRINT("the writer times out: the reader's result", result[1]);

    start(SCE_KERNEL_ATTR_THREAD_PRIO, HIGH_PRIORITY, TEST_WAIT_US, 0);
    PRINT("priority queue, the writer drops below the reader: change its priority", sceKernelChangeThreadPriority(threads[0], LOW_PRIORITY + 0x10));
    PRINT("priority queue, the writer drops below the reader: the reader got its lock", reader_done_soon());
    PRINT("priority queue, the writer drops below the reader: the writer is done", done[0]);
    finish();
    PRINT("priority queue, the writer drops below the reader: the writer's result", result[0]);
    PRINT("priority queue, the writer drops below the reader: the reader's result", result[1]);

    start(0, LOW_PRIORITY, TEST_WAIT_US, 1);
    PRINT("the writer runs a callback: notify it", sceKernelNotifyCallback(callback, 1));
    TEST_WAIT_FOR(in_callback);
    const int during = reader_done_soon() && !callback_done;
    TEST_WAIT_FOR(callback_done);
    PRINT("the writer runs a callback: it ran", in_callback);
    PRINT("while the writer runs a callback: the reader got its lock", during);
    PRINT("after the callback: the reader is done", done[1]);
    finish();
    PRINT("the writer runs a callback: the writer's result", result[0]);
    PRINT("the writer runs a callback: the reader's result", result[1]);

    test_exit();
    return 0;
}

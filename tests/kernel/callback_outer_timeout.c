// How the timeout of a wait with callbacks counts the time a callback takes, and whether the wait
// can still be woken once its timeout passed while a callback ran. The callback blocks until the
// test releases it, and each time the test checks is at least 250 ms away from every outcome it
// tells apart.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID sema, release, cb;
static volatile int is_delay, held, wait_result;
static volatile SceUInt wait_timeout_us, wait_left;

// Blocks until the test releases it
static int held_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    held = 1;
    SceUInt timeout = 10 * TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("held", 0, held_callback, NULL);
    SceUInt timeout = wait_timeout_us;
    wait_result = is_delay ? sceKernelDelayThreadCB(wait_timeout_us) : sceKernelWaitSemaCB(sema, 1, &timeout);
    wait_left = timeout;
    sceKernelDeleteCallback(cb);
    return 0;
}

// Starts a thread that waits for timeout_us, or delays for it, and whose callback runs after
// notify_after_us of it and blocks for hold_us. Returns whether the thread then ends within
// within_us of the callback's release.
static int ends_within(int delay, SceUInt timeout_us, SceUInt notify_after_us, SceUInt hold_us, SceUInt within_us) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    is_delay = delay;
    wait_timeout_us = timeout_us;
    held = 0;
    wait_result = 0xDEAD;
    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    if (notify_after_us)
        sceKernelDelayThread(notify_after_us);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(held);
    sceKernelDelayThread(hold_us);
    sceKernelSignalSema(release, 1);
    SceUInt timeout = within_us;
    const int ended = sceKernelWaitThreadEnd(thread, NULL, &timeout) == 0;
    test_join_thread(thread);
    sceKernelDeleteSema(sema);
    sceKernelDeleteSema(release);
    return ended;
}

int main(void) {
    // A wait of 500 ms whose callback runs at once and takes 1000 ms. If the callback's time counts,
    // the timeout passed during it and the wait ends right after it. If not, about 500 ms are left.
    PRINT("a wait of 500 ms with a callback of 1000 ms ends within 250 ms of it", ends_within(0, 500000, 0, 1000000, 250000));
    PRINT("the wait", wait_result);
    PRINT("a delay of 500 ms with a callback of 1000 ms ends within 250 ms of it", ends_within(1, 500000, 0, 1000000, 250000));
    PRINT("the delay", wait_result);

    // A wait of 1000 ms whose callback runs after 600 ms and takes 1000 ms. If the time before the
    // callback still counts after it, about 400 ms or less are left. If the timeout starts over,
    // 1000 ms are.
    PRINT("a wait of 1000 ms with a callback after 600 ms ends within 700 ms of it", ends_within(0, 1000000, 600000, 1000000, 700000));
    PRINT("a delay of 1000 ms with a callback after 600 ms ends within 700 ms of it", ends_within(1, 1000000, 600000, 1000000, 700000));

    // A wait of 100 ms whose callback runs at once. 500 ms into the callback, long after the
    // timeout, the semaphore is signaled.
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    is_delay = 0;
    wait_timeout_us = 100000;
    held = 0;
    wait_result = 0xDEAD;
    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(held);
    sceKernelDelayThread(500000);
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("past the timeout in the callback: the semaphore's waiters", sema_info.numWaitThreads);
    PRINT("past the timeout in the callback: the signal", sceKernelSignalSema(sema, 1));
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("past the timeout in the callback: the count after the signal", sema_info.currentCount);
    sceKernelSignalSema(release, 1);
    test_join_thread(thread);
    PRINT("past the timeout in the callback: the wait", wait_result);
    PRINT("past the timeout in the callback: its time left is 0", wait_left == 0);

    test_exit();
    return 0;
}

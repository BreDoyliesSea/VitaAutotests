// Where a waiter goes back in a priority ordered semaphore queue after a callback during which its
// priority changed. Waiter A waits on the semaphore with callbacks and queues first, then B waits
// on it without callbacks. A's callback is notified, and either waits on a FIFO gate semaphore
// until the main thread lets it go, or changes its own priority and returns. Once A waits on the
// semaphore again, one signal goes to the first waiter in the queue, and a cancel ends the other
// wait. Cases:
// - A at 0x80 and B at 0x88, no change
// - A at 0x80 and B at 0x88, the main thread drops A to 0x88 while the callback waits on the gate
// - A and B at 0x88, the main thread raises A to 0x80 while the callback waits on the gate
// - A and B at 0x88, A's callback raises its own thread to 0x80
// Each case prints A's priority at the end of the callback, and each waiter's result: 0 for the
// one the signal went to, the cancel's error for the other.
//
// On a PS Vita TV on firmware 3.65 A goes back into the queue by the priority it has at the end of
// the callback. The signal goes to A in every case but the one where the main thread dropped A to
// B's priority, where it goes to B, so A went back behind B. Raised by the main thread or by its
// own callback, A gets the signal ahead of B.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID sema, gate, callback;
static SceUID threads[2];
static volatile int self_priority, in_callback, callback_done, callback_priority;
static volatile int result[2];

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    in_callback = 1;
    if (self_priority) {
        sceKernelChangeThreadPriority(0, self_priority);
    } else {
        SceUInt timeout = 2 * TEST_WAIT_US;
        sceKernelWaitSema(gate, 1, &timeout);
    }
    callback_priority = sceKernelGetThreadCurrentPriority();
    callback_done = 1;
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    if (i == 0) {
        callback = sceKernelCreateCallback("cb", 0, callback_entry, NULL);
        result[0] = sceKernelWaitSemaCB(sema, 1, &timeout);
        sceKernelDeleteCallback(callback);
    } else {
        result[1] = sceKernelWaitSema(sema, 1, &timeout);
    }
    return 0;
}

// Returns whether thread waits on id
static int waits_on(SceUID thread, SceUID id) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && (info.status & SCE_THREAD_WAITING) &&
           info.waitId == id;
}

static int sema_waiters(void) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(sema, &info);
    return info.numWaitThreads;
}

// change is the priority the main thread gives A while its callback waits, or 0 for none
static void run(const char *name, int a_priority, int b_priority, int change, int self) {
    sema = sceKernelCreateSema("sema", SCE_KERNEL_ATTR_THREAD_PRIO, 0, 2, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    self_priority = self;
    in_callback = callback_done = callback_priority = 0;
    const int priorities[2] = { a_priority, b_priority };
    for (int i = 0; i < 2; i++) {
        result[i] = 0xDEAD;
        threads[i] = sceKernelCreateThread("waiter", waiter, priorities[i], 0x4000, 0, 0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        test_wait_until_waiting(threads[i]);
    }

    sceKernelNotifyCallback(callback, 1);
    TEST_WAIT_FOR(in_callback);
    int changed = 0;
    if (!self) {
        TEST_WAIT_FOR(waits_on(threads[0], gate));
        if (change)
            changed = sceKernelChangeThreadPriority(threads[0], change);
        sceKernelSignalSema(gate, 1);
    }
    // Until the callback ran and both waits are queued again
    TEST_WAIT_FOR(callback_done && sema_waiters() == 2 && waits_on(threads[0], sema));
    const int back = waits_on(threads[0], sema);
    sceKernelSignalSema(sema, 1);
    int canceled = 0xDEAD;
    sceKernelCancelSema(sema, 0, &canceled);
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);

    if (change)
        PRINTF(changed, "%s: the change", name);
    PRINTF(callback_priority, "%s: A's priority at the end of the callback", name);
    PRINTF(back, "%s: A waits on the semaphore again", name);
    PRINTF(canceled, "%s: waits the cancel ended", name);
    PRINTF(result[0], "%s: A's result", name);
    PRINTF(result[1], "%s: B's result", name);
    sceKernelDeleteSema(gate);
    sceKernelDeleteSema(sema);
}

int main(void) {
    run("A above B, no change", 0x80, 0x88, 0, 0);
    run("A above B, A dropped to B's priority", 0x80, 0x88, 0x88, 0);
    run("A and B level, A raised", 0x88, 0x88, 0x80, 0);
    run("A and B level, A raises itself", 0x88, 0x88, 0, 0x80);

    test_exit();
    return 0;
}

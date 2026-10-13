// Where a wait goes back in its queue after its thread ran a callback: at its old place, or behind
// the waiters that came after it. One thread waits first with callbacks, another waits after it,
// the first runs a callback, and then one wake goes to whoever is first and a cancel ends the other.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA_FIFO, SEMA_PRIO, EVENT_FLAG };

static SceUID object, cb;
static volatile int kind, callback_done, result[2];

static int quick_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_done = 1;
    return 0;
}

// Waits on the object, with callbacks as waiter 0
static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    if (i == 0)
        cb = sceKernelCreateCallback("quick", 0, quick_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    unsigned int bits;
    switch (kind) {
    case SEMA_FIFO:
    case SEMA_PRIO: result[i] = i == 0 ? sceKernelWaitSemaCB(object, 1, &timeout) : sceKernelWaitSema(object, 1, &timeout); break;
    case EVENT_FLAG:
        result[i] = i == 0 ? sceKernelWaitEventFlagCB(object, 0x1, EVF_WAITMODE_AND | EVF_WAITMODE_CLEAR_ALL, &bits, &timeout)
                           : sceKernelWaitEventFlag(object, 0x1, EVF_WAITMODE_AND | EVF_WAITMODE_CLEAR_ALL, &bits, &timeout);
        break;
    }
    if (i == 0)
        sceKernelDeleteCallback(cb);
    return 0;
}

static int waiters(void) {
    if (kind == EVENT_FLAG) {
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        sceKernelGetEventFlagInfo(object, &info);
        return info.numWaitThreads;
    }
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(object, &info);
    return info.numWaitThreads;
}

static int waits_on(SceUID thread, SceUID id) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType != 0 && info.waitId == id;
}

static void run(const char *name, int new_kind) {
    kind = new_kind;
    switch (new_kind) {
    case SEMA_FIFO: object = sceKernelCreateSema("fifo", 0, 0, 1, NULL); break;
    case SEMA_PRIO: object = sceKernelCreateSema("prio", ATTR_TH_PRIO, 0, 1, NULL); break;
    case EVENT_FLAG: object = sceKernelCreateEventFlag("flag", EVF_ATTR_MULTI, 0, NULL); break;
    }
    callback_done = 0;
    result[0] = result[1] = 0xDEAD;
    const SceUID first = test_start_thread_arg(waiter, 0);
    test_wait_until_waiting(first);
    const SceUID second = test_start_thread_arg(waiter, 1);
    test_wait_until_waiting(second);
    sceKernelNotifyCallback(cb, 1);
    // Until the callback ran and both waits are queued again
    TEST_WAIT_FOR(callback_done && waiters() == 2 && waits_on(first, object));
    const int back_in_wait = waits_on(first, object);
    int canceled = 0xDEAD;
    switch (new_kind) {
    case SEMA_FIFO:
    case SEMA_PRIO:
        sceKernelSignalSema(object, 1);
        sceKernelCancelSema(object, 0, &canceled);
        break;
    case EVENT_FLAG:
        sceKernelSetEventFlag(object, 0x1);
        sceKernelCancelEventFlag(object, 0, (SceUInt32 *)&canceled);
        break;
    }
    test_join_thread(first);
    test_join_thread(second);
    PRINTF(back_in_wait, "%s: the first waiter waits again after its callback", name);
    PRINTF(canceled, "%s: waits the cancel ended", name);
    PRINTF(result[0], "%s: the waiter that ran a callback", name);
    PRINTF(result[1], "%s: the waiter that came after it", name);
    if (new_kind == EVENT_FLAG)
        sceKernelDeleteEventFlag(object);
    else
        sceKernelDeleteSema(object);
}

int main(void) {
    run("FIFO semaphore", SEMA_FIFO);
    run("priority semaphore, same priorities", SEMA_PRIO);
    run("event flag that clears", EVENT_FLAG);

    test_exit();
    return 0;
}

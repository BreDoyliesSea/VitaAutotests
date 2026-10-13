// Callbacks that delete, cancel or notify themselves or each other while they run, the order
// pending callbacks run in, and deleting a pending callback of another thread
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Mode { DELETE_SELF, CANCEL_SELF, NOTIFY_SELF, NOTIFY_SELF_RETURN_1, RETURN_NEGATIVE, DELETE_OTHER, NOTIFY_OTHER };

static SceUID self_cb, other_cb;
static volatile int mode, self_calls, other_calls, inner_result;

static int other_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    other_calls++;
    return 0;
}

static int self_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    self_calls++;
    switch (mode) {
    case DELETE_SELF: inner_result = sceKernelDeleteCallback(self_cb); break;
    case CANCEL_SELF:
        sceKernelNotifyCallback(self_cb, 2);
        inner_result = sceKernelCancelCallback(self_cb);
        break;
    case NOTIFY_SELF:
    case NOTIFY_SELF_RETURN_1:
        if (self_calls == 1)
            inner_result = sceKernelNotifyCallback(self_cb, 2);
        break;
    case DELETE_OTHER: inner_result = sceKernelDeleteCallback(other_cb); break;
    case NOTIFY_OTHER: inner_result = sceKernelNotifyCallback(other_cb, 2); break;
    }
    if (mode == NOTIFY_SELF_RETURN_1)
        return 1;
    if (mode == RETURN_NEGATIVE)
        return -1;
    return 0;
}

static void run(const char *name, int new_mode) {
    mode = new_mode;
    self_calls = other_calls = 0;
    inner_result = 0xDEAD;
    self_cb = sceKernelCreateCallback("self", 0, self_callback, NULL);
    other_cb = sceKernelCreateCallback("other", 0, other_callback, NULL);
    sceKernelNotifyCallback(self_cb, 1);
    if (new_mode == DELETE_OTHER)
        sceKernelNotifyCallback(other_cb, 1);
    PRINTF(sceKernelCheckCallback(), "%s: the check", name);
    if (new_mode != RETURN_NEGATIVE)
        PRINTF(inner_result, "%s: the call inside the callback", name);
    PRINTF(self_calls, "%s: runs of the callback", name);
    PRINTF(other_calls, "%s: runs of the other callback", name);
    PRINTF(sceKernelGetCallbackCount(self_cb), "%s: the callback's notifications after", name);
    PRINTF(sceKernelGetCallbackCount(other_cb), "%s: the other callback's notifications after", name);
    PRINTF(sceKernelCheckCallback(), "%s: the next check", name);
    PRINTF(self_calls, "%s: runs of the callback after the next check", name);
    PRINTF(sceKernelNotifyCallback(self_cb, 3), "%s: notify the callback after", name);
    sceKernelDeleteCallback(self_cb);
    sceKernelDeleteCallback(other_cb);
}

// Callbacks that log the order they run in
static volatile int order[3], order_count;

static int ordered_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)common;
    if (order_count < 3)
        order[order_count++] = arg;
    return 0;
}

// A thread with a callback pending in a wait without callbacks, which checks once the wait ends
static SceUID pending_sema, pending_cb;
static volatile int pending_calls, pending_check;

static int pending_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    pending_calls++;
    return 0;
}

static int pending_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    pending_cb = sceKernelCreateCallback("pending", 0, pending_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(pending_sema, 1, &timeout);
    pending_check = sceKernelCheckCallback();
    return 0;
}

int main(void) {
    run("delete itself", DELETE_SELF);
    run("cancel itself", CANCEL_SELF);
    run("notify itself", NOTIFY_SELF);
    run("notify itself and return 1", NOTIFY_SELF_RETURN_1);
    run("return -1", RETURN_NEGATIVE);
    run("delete another pending callback", DELETE_OTHER);
    run("notify another callback", NOTIFY_OTHER);

    // Three callbacks created as 1, 2, 3 and notified as 3, 1, 2
    SceUID ordered[3];
    for (int i = 0; i < 3; i++)
        ordered[i] = sceKernelCreateCallback("ordered", 0, ordered_callback, NULL);
    sceKernelNotifyCallback(ordered[2], 3);
    sceKernelNotifyCallback(ordered[0], 1);
    sceKernelNotifyCallback(ordered[1], 2);
    PRINT("three pending callbacks: the check", sceKernelCheckCallback());
    for (int i = 0; i < order_count; i++)
        PRINTF(order[i], "three pending callbacks: run %d is the one created as", i + 1);
    for (int i = 0; i < 3; i++)
        sceKernelDeleteCallback(ordered[i]);

    // Another thread's pending callback, deleted before that thread checks
    pending_sema = sceKernelCreateSema("pending", 0, 0, 1, NULL);
    pending_check = 0xDEAD;
    const SceUID thread = test_start_thread(pending_thread);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(pending_cb, 1);
    PRINT("another thread's pending callback: delete it", sceKernelDeleteCallback(pending_cb));
    sceKernelSignalSema(pending_sema, 1);
    test_join_thread(thread);
    PRINT("another thread's pending callback: that thread's check", pending_check);
    PRINT("another thread's pending callback: its runs", pending_calls);

    test_exit();
    return 0;
}

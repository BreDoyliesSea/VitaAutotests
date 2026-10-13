// Callbacks that check callbacks or wait with callbacks while another callback, or the same one,
// is pending: whether it runs inside, and how deep callbacks nest
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Mode { CHECK_OTHER, CHECK_SELF, DELAY_OTHER, DELAY_SELF, WAIT_OTHER, WAIT_SELF };

static SceUID self_cb, other_cb, never;
static volatile int mode, depth, max_depth, self_calls, other_calls, other_depth;
static volatile int inner_result, self_calls_inside, other_calls_inside;

static int other_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    other_calls++;
    other_depth = depth;
    return 0;
}

// On its first run, makes a callback pending and then checks or waits with callbacks
static int self_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    depth++;
    if (depth > max_depth)
        max_depth = depth;
    self_calls++;
    if (self_calls == 1) {
        const int self = mode == CHECK_SELF || mode == DELAY_SELF || mode == WAIT_SELF;
        sceKernelNotifyCallback(self ? self_cb : other_cb, 1);
        SceUInt timeout = 20000;
        switch (mode) {
        case CHECK_OTHER:
        case CHECK_SELF: inner_result = sceKernelCheckCallback(); break;
        case DELAY_OTHER:
        case DELAY_SELF: inner_result = sceKernelDelayThreadCB(20000); break;
        case WAIT_OTHER:
        case WAIT_SELF: inner_result = sceKernelWaitSemaCB(never, 1, &timeout); break;
        }
        self_calls_inside = self_calls;
        other_calls_inside = other_calls;
    }
    depth--;
    return 0;
}

static void run(const char *name, int new_mode) {
    mode = new_mode;
    depth = max_depth = self_calls = other_calls = 0;
    other_depth = inner_result = self_calls_inside = other_calls_inside = 0xDEAD;
    sceKernelNotifyCallback(self_cb, 1);
    PRINTF(sceKernelCheckCallback(), "%s: the check", name);
    PRINTF(inner_result, "%s: the call inside the callback", name);
    PRINTF(self_calls_inside, "%s: runs of the callback when that call returned", name);
    PRINTF(other_calls_inside, "%s: runs of the other callback when it returned", name);
    PRINTF(self_calls, "%s: runs of the callback after the check", name);
    PRINTF(other_calls, "%s: runs of the other callback after the check", name);
    PRINTF(max_depth, "%s: the deepest nesting", name);
    if (other_calls)
        PRINTF(other_depth, "%s: callbacks the other one ran inside", name);
    PRINTF(sceKernelGetCallbackCount(self_cb), "%s: the callback's notifications left", name);
    PRINTF(sceKernelGetCallbackCount(other_cb), "%s: the other callback's notifications left", name);
    PRINTF(sceKernelCheckCallback(), "%s: the next check", name);
}

// A thread whose callback waits with callbacks while the thread's own wait with callbacks goes on.
// The callback's wait times out by itself, so a callback notified during it runs either inside it or
// after it, and the test waits until each one ran.
#define INNER_TIMEOUT_US 300000

static SceUID outer, inner, thread_self_cb, thread_other_cb;
static volatile int thread_self_calls, thread_other_calls, thread_depth, thread_other_depth, thread_self_again_depth;
static volatile int thread_inner_result, thread_outer_result;

static int thread_other_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    thread_other_depth = thread_depth;
    thread_other_calls++;
    return 0;
}

// Waits with callbacks on its first run, and records how deep it runs on the next one
static int thread_self_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    const int depth = thread_depth++;
    if (thread_self_calls == 0) {
        thread_self_calls = 1;
        SceUInt timeout = INNER_TIMEOUT_US;
        thread_inner_result = sceKernelWaitSemaCB(inner, 1, &timeout);
    } else {
        thread_self_again_depth = depth;
        thread_self_calls++;
    }
    thread_depth--;
    return 0;
}

static int outer_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    thread_self_cb = sceKernelCreateCallback("self", 0, thread_self_callback, NULL);
    thread_other_cb = sceKernelCreateCallback("other", 0, thread_other_callback, NULL);
    SceUInt timeout = 4 * TEST_WAIT_US;
    thread_outer_result = sceKernelWaitSemaCB(outer, 1, &timeout);
    sceKernelDeleteCallback(thread_self_cb);
    sceKernelDeleteCallback(thread_other_cb);
    return 0;
}

int main(void) {
    never = sceKernelCreateSema("never", 0, 0, 1, NULL);
    self_cb = sceKernelCreateCallback("self", 0, self_callback, NULL);
    other_cb = sceKernelCreateCallback("other", 0, other_callback, NULL);

    run("check with another callback pending", CHECK_OTHER);
    run("check with itself pending", CHECK_SELF);
    run("delay with another callback pending", DELAY_OTHER);
    run("delay with itself pending", DELAY_SELF);
    run("wait with another callback pending", WAIT_OTHER);
    run("wait with itself pending", WAIT_SELF);

    // Notifications from another thread while the callback waits with callbacks: one for another
    // callback and one for the same callback
    outer = sceKernelCreateSema("outer", 0, 0, 1, NULL);
    inner = sceKernelCreateSema("inner", 0, 0, 1, NULL);
    thread_other_depth = thread_self_again_depth = thread_inner_result = thread_outer_result = 0xDEAD;
    const SceUID thread = test_start_thread(outer_thread);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(thread_self_cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA | WAIT_TYPE_CB, inner));
    const int in_inner_wait = test_in_wait(thread, WAIT_TYPE_SEMA | WAIT_TYPE_CB, inner);
    sceKernelNotifyCallback(thread_other_cb, 1);
    sceKernelNotifyCallback(thread_self_cb, 2);
    TEST_WAIT_FOR(thread_other_calls == 1 && thread_self_calls == 2 && thread_inner_result != 0xDEAD);
    PRINT("thread: it reports the callback's wait", in_inner_wait);
    PRINT("thread: callbacks the other callback ran inside", thread_other_depth);
    PRINT("thread: callbacks the callback ran inside when notified again", thread_self_again_depth);
    PRINT("thread: the callback's wait", thread_inner_result);
    sceKernelSignalSema(outer, 1);
    test_join_thread(thread);
    PRINT("thread: the outer wait", thread_outer_result);

    test_exit();
    return 0;
}

// Waiting for a thread to end in less common cases: the thread exits and deletes itself, the wait
// runs callbacks, the waits have different timeouts, and the thread is in a callback
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID target, go, inner, waiter_cb, target_cb;
static volatile int end_result[2], end_status[2], waiter_calls, end_mode;
static volatile SceUInt end_timeout[2];

// Waits for go, then ends in the way end_mode says
static int target_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    if (end_mode == 1)
        sceKernelExitDeleteThread(0x33);
    if (end_mode == 2)
        sceKernelExitThread(0x44);
    return 0x22;
}

static int counting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    waiter_calls++;
    return 0;
}

// Waits for the target's end, with callbacks if the argument says so
static int end_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp & 1;
    const int callbacks = *(const int *)argp & 2;
    int status = 0xDEAD;
    SceUInt timeout = end_timeout[i];
    if (callbacks) {
        waiter_cb = sceKernelCreateCallback("waiter", 0, counting_callback, NULL);
        end_result[i] = sceKernelWaitThreadEndCB(target, &status, &timeout);
    } else {
        end_result[i] = sceKernelWaitThreadEnd(target, &status, &timeout);
    }
    end_status[i] = status;
    return 0;
}

static void reset(void) {
    end_result[0] = end_result[1] = end_status[0] = end_status[1] = 0xDEAD;
    end_timeout[0] = end_timeout[1] = TEST_WAIT_US;
}

// A target whose callback blocks until inner is signaled
static int blocking_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(inner, 1, &timeout);
    return 0;
}

static int callback_target(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    target_cb = sceKernelCreateCallback("target", 0, blocking_callback, NULL);
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSemaCB(go, 1, &timeout);
    return 0x55;
}

static int main_waiter_result;

static int main_waiter(SceSize args, void *argp) {
    (void)args;
    const SceUID main_thread = *(const SceUID *)argp;
    SceUInt timeout = 1000;
    main_waiter_result = sceKernelWaitThreadEnd(main_thread, NULL, &timeout);
    return 0;
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 1, NULL);
    inner = sceKernelCreateSema("inner", 0, 0, 1, NULL);

    // The target exits and deletes itself while a thread waits for its end
    reset();
    end_mode = 1;
    target = test_start_thread(target_thread);
    test_wait_until_waiting(target);
    SceUID waiter = test_start_thread_arg(end_waiter, 0);
    test_wait_until_waiting(waiter);
    sceKernelSignalSema(go, 1);
    test_join_thread(waiter);
    PRINT("exit and delete: the wait for its end", end_result[0]);
    PRINT("exit and delete: the status", end_status[0]);

    // The target exits while a thread waits for its end with callbacks, which run first
    reset();
    end_mode = 2;
    waiter_calls = 0;
    target = test_start_thread(target_thread);
    test_wait_until_waiting(target);
    waiter = test_start_thread_arg(end_waiter, 2);
    test_wait_until_waiting(waiter);
    sceKernelNotifyCallback(waiter_cb, 1);
    TEST_WAIT_FOR(waiter_calls != 0);
    PRINT("with callbacks: runs of the callback during the wait", waiter_calls);
    sceKernelSignalSema(go, 1);
    test_join_thread(waiter);
    PRINT("with callbacks: the wait for its end", end_result[0]);
    PRINT("with callbacks: the status", end_status[0]);
    sceKernelDeleteCallback(waiter_cb);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    sceKernelDeleteThread(target);

    // Two waiters, one of which times out before the end. The target only ends once that one did.
    reset();
    end_mode = 0;
    end_timeout[0] = 20000;
    target = test_start_thread(target_thread);
    test_wait_until_waiting(target);
    const SceUID first = test_start_thread_arg(end_waiter, 0);
    test_wait_until_waiting(first);
    const SceUID second = test_start_thread_arg(end_waiter, 1);
    test_wait_until_waiting(second);
    test_join_thread(first);
    sceKernelSignalSema(go, 1);
    test_join_thread(second);
    PRINT("two waiters: the one that timed out", end_result[0]);
    PRINT("two waiters: its status", end_status[0]);
    PRINT("two waiters: the other one", end_result[1]);
    PRINT("two waiters: its status", end_status[1]);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    sceKernelDeleteThread(target);

    // The target is in a callback that blocks, inside its own wait
    target = test_start_thread(callback_target);
    test_wait_until_waiting(target);
    sceKernelNotifyCallback(target_cb, 1);
    TEST_WAIT_FOR(test_in_wait(target, WAIT_TYPE_SEMA, inner));
    int status = 0xDEAD;
    SceUInt timeout = 20000;
    PRINT("target in a callback: a wait for its end", sceKernelWaitThreadEnd(target, &status, &timeout));
    PRINT("target in a callback: the status written", status);
    sceKernelSignalSema(inner, 1);
    sceKernelSignalSema(go, 1);
    status = 0xDEAD;
    PRINT("target in a callback: the wait once it ends", sceKernelWaitThreadEnd(target, &status, NULL));
    PRINT("target in a callback: the status", status);
    sceKernelDeleteThread(target);

    // Another thread waits for the main thread's end
    const SceUID main_thread = sceKernelGetThreadId();
    const SceUID thread = sceKernelCreateThread("main waiter", main_waiter, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(thread, sizeof(main_thread), (void *)&main_thread);
    test_join_thread(thread);
    PRINT("wait for the main thread's end", main_waiter_result);

    test_exit();
    return 0;
}

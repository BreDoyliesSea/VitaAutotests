// A thread's exit status for each way it can end: returning, sceKernelExitThread and
// sceKernelExitDeleteThread, with statuses that are negative, have the top bit set or look like
// thread errors. Also end waits that time out, with and without callbacks, a waiter with callbacks
// next to one without, and waits for a thread that already ended.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define GIVEN 20000

enum { BY_RETURN, BY_EXIT, BY_EXIT_DELETE };

static SceUID go, target;
static volatile int status_value, end_way;

// Waits for go, then ends in the way end_way says with status_value
static int ending_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = 2 * TEST_WAIT_US;
    sceKernelWaitSema(go, 1, &timeout);
    if (end_way == BY_EXIT)
        sceKernelExitThread(status_value);
    if (end_way == BY_EXIT_DELETE)
        sceKernelExitDeleteThread(status_value);
    return status_value;
}

static volatile int waiter_result[2], waiter_status[2];

// Waits for the target's end, with callbacks if the argument is 1
static int end_waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    int status = 0xDEAD;
    SceUInt timeout = TEST_WAIT_US;
    if (i == 1)
        waiter_result[i] = sceKernelWaitThreadEndCB(target, &status, &timeout);
    else
        waiter_result[i] = sceKernelWaitThreadEnd(target, &status, &timeout);
    waiter_status[i] = status;
    return 0;
}

// 0 if nothing is left, 1 if some is, 2 if all of it is
static int left(SceUInt value) {
    return value == 0 ? 0 : value >= GIVEN ? 2 : 1;
}

// Starts a target that ends in the given way with the given status, with an end waiter on it
static void start_target(int way, int value) {
    end_way = way;
    status_value = value;
    waiter_result[0] = waiter_status[0] = 0xDEAD;
    target = test_start_thread(ending_thread);
    TEST_WAIT_FOR(test_in_wait(target, WAIT_TYPE_SEMA, go));
}

int main(void) {
    go = sceKernelCreateSema("go", 0, 0, 2, NULL);

    static const int values[] = { 0, -1, 0x7FFFFFFF, (int)0x80000000, (int)0x80028027, (int)0x80028028 };
    static const char *const ways[] = { "return", "exit" };
    for (int way = BY_RETURN; way <= BY_EXIT; way++) {
        for (unsigned int i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            const unsigned int v = (unsigned)values[i];
            start_target(way, values[i]);
            sceKernelSignalSema(go, 1);
            int status = 0xDEAD;
            SceUInt timeout = TEST_WAIT_US;
            PRINTF(sceKernelWaitThreadEnd(target, &status, &timeout), "%s 0x%08X: the end wait", ways[way], v);
            PRINTF(status, "%s 0x%08X: the status it gets", ways[way], v);
            status = 0xDEAD;
            PRINTF(sceKernelGetThreadExitStatus(target, &status), "%s 0x%08X: get the exit status", ways[way], v);
            PRINTF(status, "%s 0x%08X: the exit status", ways[way], v);
            SceKernelThreadInfo info = { .size = sizeof(info) };
            sceKernelGetThreadInfo(target, &info);
            PRINTF(info.exitStatus, "%s 0x%08X: the exit status in its info", ways[way], v);
            sceKernelDeleteThread(target);
        }
    }

    // ExitDeleteThread, seen by a thread waiting for the end
    static const int delete_values[] = { -1, (int)0x80000000, (int)0x80028027 };
    for (unsigned int i = 0; i < sizeof(delete_values) / sizeof(delete_values[0]); i++) {
        const unsigned int v = (unsigned)delete_values[i];
        start_target(BY_EXIT_DELETE, delete_values[i]);
        const SceUID waiter = test_start_thread_arg(end_waiter, 0);
        TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_THREAD_END, target));
        sceKernelSignalSema(go, 1);
        test_join_thread(waiter);
        PRINTF(waiter_result[0], "exit and delete 0x%08X: the end wait", v);
        PRINTF(waiter_status[0], "exit and delete 0x%08X: the status it gets", v);
    }

    // End waits that time out
    start_target(BY_RETURN, 5);
    int status = 0xDEAD;
    SceUInt timeout = GIVEN;
    PRINT("an end wait that times out", sceKernelWaitThreadEnd(target, &status, &timeout));
    PRINT("its time left", left(timeout));
    PRINT("its status is untouched", status == 0xDEAD);
    timeout = GIVEN;
    PRINT("an end wait with callbacks that times out", sceKernelWaitThreadEndCB(target, &status, &timeout));
    PRINT("its time left", left(timeout));
    PRINT("its status is untouched", status == 0xDEAD);

    // One waiter with callbacks and one without, on the same end
    waiter_result[0] = waiter_result[1] = waiter_status[0] = waiter_status[1] = 0xDEAD;
    const SceUID plain = test_start_thread_arg(end_waiter, 0);
    test_wait_until_waiting(plain);
    const SceUID with_cb = test_start_thread_arg(end_waiter, 1);
    test_wait_until_waiting(with_cb);
    sceKernelSignalSema(go, 1);
    test_join_thread(plain);
    test_join_thread(with_cb);
    PRINT("two waiters: the one without callbacks", waiter_result[0]);
    PRINT("two waiters: its status", waiter_status[0]);
    PRINT("two waiters: the one with callbacks", waiter_result[1]);
    PRINT("two waiters: its status", waiter_status[1]);

    // Waits for a thread that already ended
    sceKernelWaitThreadEnd(target, NULL, NULL);
    status = 0xDEAD;
    timeout = GIVEN;
    PRINT("an end wait for an ended thread", sceKernelWaitThreadEnd(target, &status, &timeout));
    PRINT("its status", status);
    PRINT("its time left", left(timeout));
    status = 0xDEAD;
    timeout = GIVEN;
    PRINT("an end wait with callbacks for an ended thread", sceKernelWaitThreadEndCB(target, &status, &timeout));
    PRINT("its status", status);
    PRINT("its time left", left(timeout));
    PRINT("an end wait for an ended thread with no status pointer", sceKernelWaitThreadEnd(target, NULL, NULL));
    sceKernelDeleteThread(target);

    sceKernelDeleteSema(go);
    test_exit();
    return 0;
}

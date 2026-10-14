// What happens to a thread's callbacks when it ends, starts again, or is deleted
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID made_cb, sema;
static volatile int calls, runs, check_result, exit_delete;

static int counting_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    return 0;
}

// Creates a callback on its first run, and checks callbacks on the later ones
static int owner_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    runs++;
    if (runs == 1)
        made_cb = sceKernelCreateCallback("owner", 0, counting_callback, NULL);
    else
        check_result = sceKernelCheckCallback();
    return 0;
}

// Creates a callback and waits without checking callbacks, then ends with it pending
static int pending_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    made_cb = sceKernelCreateCallback("pending", 0, counting_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    if (exit_delete)
        sceKernelExitDeleteThread(0);
    return 0;
}

static int thread_gone(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) < 0;
}

static void print_callback(const char *name, SceUID thread) {
    CallbackInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetCallbackInfo(made_cb, (SceKernelCallbackInfo *)&info), "%s: the callback's info", name);
    PRINTF(info.threadId == thread, "%s: its thread is the one that made it", name);
    PRINTF(sceKernelGetCallbackCount(made_cb), "%s: its notifications", name);
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);

    // A thread that ended
    const SceUID owner = sceKernelCreateThread("owner", owner_thread, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(owner, 0, NULL);
    sceKernelWaitThreadEnd(owner, NULL, NULL);
    print_callback("ended owner", owner);
    PRINT("ended owner: notify the callback", sceKernelNotifyCallback(made_cb, 1));
    PRINT("ended owner: its notifications after", sceKernelGetCallbackCount(made_cb));
    sceKernelCheckCallback();
    PRINT("ended owner: a check on this thread runs it", calls);

    // The same thread started again
    check_result = 0xDEAD;
    sceKernelStartThread(owner, 0, NULL);
    sceKernelWaitThreadEnd(owner, NULL, NULL);
    PRINT("restarted owner: its check", check_result);
    PRINT("restarted owner: runs of the callback", calls);
    print_callback("restarted owner", owner);

    // The thread deleted
    sceKernelNotifyCallback(made_cb, 2);
    PRINT("deleted owner: delete the thread", sceKernelDeleteThread(owner));
    print_callback("deleted owner", owner);
    PRINT("deleted owner: notify the callback", sceKernelNotifyCallback(made_cb, 3));
    PRINT("deleted owner: delete the callback", sceKernelDeleteCallback(made_cb));

    // A thread that ends with a notification pending
    calls = 0;
    SceUID thread = test_start_thread(pending_thread);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(made_cb, 4);
    sceKernelSignalSema(sema, 1);
    sceKernelWaitThreadEnd(thread, NULL, NULL);
    PRINT("ended with one pending: runs of the callback", calls);
    print_callback("ended with one pending", thread);
    sceKernelDeleteThread(thread);
    print_callback("ended with one pending, then deleted", thread);
    sceKernelDeleteCallback(made_cb);

    // A thread that exits and deletes itself
    exit_delete = 1;
    thread = test_start_thread(pending_thread);
    test_wait_until_waiting(thread);
    sceKernelSignalSema(sema, 1);
    TEST_WAIT_FOR(thread_gone(thread));
    print_callback("deleted itself", thread);
    sceKernelDeleteCallback(made_cb);

    test_exit();
    return 0;
}

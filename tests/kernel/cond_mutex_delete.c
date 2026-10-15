// What a condition variable wait returns when its mutex is deleted, for a heavy and a
// lightweight condition. W locks the mutex and waits on the condition. Then the main thread
// either signals the condition while it holds the mutex, so W waits to lock the mutex again, or
// leaves W waiting on the condition, and deletes the mutex. Each case prints W's wait type while
// it waits, the delete's result, W's result once it returns, which may only be at its 2 s
// timeout, and the result of the unlock W makes after it.
//
// On a PS Vita TV on firmware 3.65 the wait returns SCE_KERNEL_ERROR_WAIT_DELETE_MUTEX for the
// heavy condition and SCE_KERNEL_ERROR_WAIT_DELETE_LW_MUTEX for the lightweight one, whether W
// was signaled or still waited on the condition. The unlock after it finds the heavy mutex
// unknown, but says the lightweight one isn't owned.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID mutex, cond;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int lightweight, w_result, w_unlock, w_done;

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    if (lightweight) {
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        w_result = sceKernelWaitLwCond(&lw_cond, &timeout);
        w_unlock = sceKernelUnlockLwMutex(&lw_mutex, 1);
    } else {
        sceKernelLockMutex(mutex, 1, NULL);
        w_result = sceKernelWaitCond(cond, &timeout);
        w_unlock = sceKernelUnlockMutex(mutex, 1);
    }
    w_done = 1;
    return 0;
}

static SceUInt32 wait_type_of(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    return info.waitType;
}

static void run_case(int lw, int signaled) {
    lightweight = lw;
    w_result = w_unlock = -1;
    w_done = 0;
    if (lw) {
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_mutex, NULL);
    } else {
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        cond = sceKernelCreateCond("cond", 0, mutex, NULL);
    }
    const SceUID w = test_start_thread(w_entry);
    test_wait_until_waiting(w);
    if (signaled) {
        if (lw) {
            sceKernelLockLwMutex(&lw_mutex, 1, NULL);
            sceKernelSignalLwCond(&lw_cond);
        } else {
            sceKernelLockMutex(mutex, 1, NULL);
            sceKernelSignalCond(cond);
        }
        sceKernelDelayThread(10000);
    }
    const SceUInt32 wait_type = wait_type_of(w);
    const int deleted = lw ? sceKernelDeleteLwMutex(&lw_mutex) : sceKernelDeleteMutex(mutex);
    test_join_thread(w);
    const char *const kind = lw ? "lightweight" : "heavy";
    const char *const state = signaled ? "signaled, waiting for the mutex" : "waiting on the condition";
    PRINTF(wait_type, "%s, %s: W's wait type", kind, state);
    PRINTF(deleted, "%s, %s: delete the mutex", kind, state);
    PRINTF(w_result, "%s, %s: W's wait", kind, state);
    PRINTF(w_unlock, "%s, %s: W's unlock after it", kind, state);
    if (lw)
        sceKernelDeleteLwCond(&lw_cond);
    else
        sceKernelDeleteCond(cond);
}

int main(void) {
    for (int lw = 0; lw < 2; lw++) {
        run_case(lw, 1);
        run_case(lw, 0);
    }
    test_exit();
    return 0;
}

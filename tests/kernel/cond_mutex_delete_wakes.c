// Whether deleting a mutex ends the waits of its conditions at once. cond_mutex_delete prints what
// the waits return, which may only be at their timeout. This test records when the wait returned.
// For a heavy and a lightweight condition, each with a plain and a callback wait: thread W locks
// the mutex and waits on the condition with a timeout of 4 s, the main thread waits until W is in
// that wait, takes the process time and deletes the mutex, then joins W, which took the process
// time when its wait returned. Each case prints the delete's result, W's result, and whether W's
// wait returned within 1 s of the delete.
//
// On a PS Vita TV on firmware 3.65 the delete succeeds and W's wait returns
// SCE_KERNEL_ERROR_WAIT_DELETE_MUTEX for the heavy condition and
// SCE_KERNEL_ERROR_WAIT_DELETE_LW_MUTEX for the lightweight one, within 1 s of the delete, with
// plain and callback waits alike.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// How long W's wait lasts, and how soon after the delete it must return
#define WAIT_TIMEOUT_US 4000000
#define PROMPT_US 1000000

static SceUID mutex, cond;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static volatile int lightweight, with_callbacks, w_result;
static volatile SceUInt64 w_returned;

static int w_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = WAIT_TIMEOUT_US;
    if (lightweight) {
        sceKernelLockLwMutex(&lw_mutex, 1, NULL);
        w_result = with_callbacks ? sceKernelWaitLwCondCB(&lw_cond, &timeout) : sceKernelWaitLwCond(&lw_cond, &timeout);
    } else {
        sceKernelLockMutex(mutex, 1, NULL);
        w_result = with_callbacks ? sceKernelWaitCondCB(cond, &timeout) : sceKernelWaitCond(cond, &timeout);
    }
    w_returned = sceKernelGetProcessTimeWide();
    return 0;
}

// Returns the UID that W's wait on the lightweight condition reports
static SceUID lw_cond_uid(void) {
    LwCondInfo info = { .size = sizeof(info) };
    return sceKernelGetLwCondInfo(&lw_cond, &info) == 0 ? info.uid : -1;
}

static void run_case(int lw, int cb) {
    lightweight = lw;
    with_callbacks = cb;
    w_result = -1;
    w_returned = 0;
    SceUID wait_id;
    if (lw) {
        sceKernelCreateLwMutex(&lw_mutex, "lw mutex", 0, 0, NULL);
        sceKernelCreateLwCond(&lw_cond, "lw cond", 0, &lw_mutex, NULL);
        wait_id = lw_cond_uid();
    } else {
        mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
        cond = sceKernelCreateCond("cond", 0, mutex, NULL);
        wait_id = cond;
    }
    const SceUInt32 wait_type = (lw ? WAIT_TYPE_LW_COND : WAIT_TYPE_COND) | (cb ? WAIT_TYPE_CB : 0);
    const SceUID w = test_start_thread(w_entry);
    TEST_WAIT_FOR(test_in_wait(w, wait_type, wait_id));
    const SceUInt64 deleted_at = sceKernelGetProcessTimeWide();
    const int deleted = lw ? sceKernelDeleteLwMutex(&lw_mutex) : sceKernelDeleteMutex(mutex);
    test_join_thread(w);
    const char *const kind = lw ? "lightweight" : "heavy";
    const char *const wait = cb ? "callback wait" : "plain wait";
    PRINTF(deleted, "%s, %s: delete the mutex", kind, wait);
    PRINTF(w_result, "%s, %s: W's wait", kind, wait);
    PRINTF(w_returned - deleted_at < PROMPT_US, "%s, %s: W returned within 1 s of the delete", kind, wait);
    if (lw)
        sceKernelDeleteLwCond(&lw_cond);
    else
        sceKernelDeleteCond(cond);
}

int main(void) {
    for (int lw = 0; lw < 2; lw++) {
        run_case(lw, 0);
        run_case(lw, 1);
    }
    test_exit();
    return 0;
}

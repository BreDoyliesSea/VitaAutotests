// TLS slot 5 and the calls that lock a lightweight mutex. The slots of the main thread and of a new
// thread are in tls_start_slots. The cases:
// - whether slot 5 is 0 inside a callback, and inside the start and the end handler of a thread
//   event handler
// - the main thread sets its slot 5 to 1, then calls sceKernelLockLwMutex and
//   sceKernelLockLwMutexCB, each on its own free mutex, and checks from the mutex's info whether it
//   is still free. It also calls sceKernelTryLockLwMutex and unlocks if that took the mutex. The
//   results are printed once the slot has its old value again, since a call that prints could lock
//   a lightweight mutex itself.
// - a lock of a free mutex once the slot is back
//
// On a PS Vita TV on firmware 3.65 slot 5 is 0 in all three places. With the slot set to 1 both
// locks give 0x80028004 and leave the mutex free, the try lock takes the mutex and gives 0, and
// the lock works again once the slot is back.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_RUN (-1)

static volatile int callback_slot = NOT_RUN, start_slot = NOT_RUN, end_slot = NOT_RUN;

static int slot5_is_set(void) {
    return *(volatile SceUInt32 *)sceKernelGetTLSAddr(5) != 0;
}

static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_slot = slot5_is_set();
    return 0;
}

static int handler(SceInt32 type, SceUID thread_id, SceInt32 arg, void *common) {
    (void)thread_id;
    (void)arg;
    (void)common;
    if (type == SCE_KERNEL_THREAD_EVENT_TYPE_START)
        start_slot = slot5_is_set();
    else if (type == SCE_KERNEL_THREAD_EVENT_TYPE_EXIT)
        end_slot = slot5_is_set();
    return 0;
}

static int target_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}

// Returns whether the mutex has no owner and no count, from its info
static int is_free(SceKernelLwMutexWork *work) {
    LwMutexInfo info = { .size = sizeof(info) };
    return sceKernelGetLwMutexInfo(work, &info) == 0 && info.currentOwnerId == 0 && info.currentCount == 0;
}

int main(void) {
    // A callback runs on the thread that made it, so it sees that thread's slot 5
    const SceUID callback = sceKernelCreateCallback("callback", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 0);
    sceKernelCheckCallback();
    PRINT("slot 5 in a callback is not 0", callback_slot);
    sceKernelDeleteCallback(callback);

    // Fresh mutexes, one per call
    static SceKernelLwMutexWork locks[3], again;
    for (int i = 0; i < 3; i++)
        sceKernelCreateLwMutex(&locks[i], "mutex", 0, 0, NULL);
    sceKernelCreateLwMutex(&again, "again", 0, 0, NULL);

    volatile SceUInt32 *slot = sceKernelGetTLSAddr(5);
    const SceUInt32 old = *slot;
    *slot = 1;
    const int lock = sceKernelLockLwMutex(&locks[0], 1, NULL);
    const int lock_free = is_free(&locks[0]);
    const int lock_cb = sceKernelLockLwMutexCB(&locks[1], 1, NULL);
    const int lock_cb_free = is_free(&locks[1]);
    const int try_lock = sceKernelTryLockLwMutex(&locks[2], 1);
    if (try_lock == 0)
        sceKernelUnlockLwMutex(&locks[2], 1);
    *slot = old;
    PRINT("lock with slot 5 set to 1", lock);
    PRINT("the mutex is free after the lock", lock_free);
    PRINT("lock with callbacks with slot 5 set to 1", lock_cb);
    PRINT("the mutex is free after the lock with callbacks", lock_cb_free);
    PRINT("try lock with slot 5 set to 1", try_lock);
    PRINT("lock once slot 5 is back", sceKernelLockLwMutex(&again, 1, NULL));
    sceKernelUnlockLwMutex(&again, 1);

    // The handlers last, since they run in the thread's start and end
    static SceUID target;
    target = sceKernelCreateThread("target", target_entry, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    const SceUID registered = sceKernelRegisterThreadEventHandler("handler", target, SCE_KERNEL_THREAD_EVENT_TYPE_START | SCE_KERNEL_THREAD_EVENT_TYPE_EXIT, handler, NULL);
    PRINT("register the handler", test_created(registered));
    sceKernelStartThread(target, 0, NULL);
    sceKernelWaitThreadEnd(target, NULL, NULL);
    PRINT("slot 5 in the start handler is not 0", start_slot);
    PRINT("slot 5 in the end handler is not 0", end_slot);
    sceKernelUnregisterThreadEventHandler(registered);
    sceKernelDeleteThread(target);

    test_exit();
    return 0;
}

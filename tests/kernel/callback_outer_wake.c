// A thread waits with callbacks, and the callback it runs blocks in a wait of its own. What a wake
// for the first wait does while the callback still waits: whether it takes the count, the mutex,
// the bits or the message, and whether it ends the callback's wait. The thread stays blocked in the
// callback's wait until the test signals it, so what the test reads in between can't change.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { SEMA, MUTEX, EVENT_FLAG, SIMPLE_EVENT, MSG_PIPE, DELETE, CANCEL };

static SceUID sema, mutex, flag, event, pipe, inner, cb;
static volatile int inner_result, outer_result;
static SceUInt outer_left;
static unsigned int outer_bits;
static char outer_message[4];

// Blocks in a wait on another semaphore, until the test signals it
static int inner_wait_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = 2 * TEST_WAIT_US;
    inner_result = sceKernelWaitSema(inner, 1, &timeout);
    return 0;
}

static int outer_thread(SceSize args, void *argp) {
    (void)args;
    cb = sceKernelCreateCallback("inner", 0, inner_wait_callback, NULL);
    outer_left = 4 * TEST_WAIT_US;
    SceUInt32 result;
    SceUInt64 user_data;
    switch (*(const int *)argp) {
    case SEMA:
    case DELETE:
    case CANCEL: outer_result = sceKernelWaitSemaCB(sema, 1, &outer_left); break;
    case MUTEX:
        outer_result = sceKernelLockMutexCB(mutex, 1, &outer_left);
        if (outer_result == 0)
            sceKernelUnlockMutex(mutex, 1);
        break;
    case EVENT_FLAG: outer_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITAND | SCE_EVENT_WAITCLEAR, &outer_bits, &outer_left); break;
    case SIMPLE_EVENT: outer_result = sceKernelWaitEventCB(event, EVENT_BIT_A, &result, &user_data, &outer_left); break;
    case MSG_PIPE: outer_result = sceKernelReceiveMsgPipeCB(pipe, outer_message, sizeof(outer_message), MSG_PIPE_MODE_FULL, NULL, &outer_left); break;
    }
    sceKernelDeleteCallback(cb);
    return 0;
}

static int sema_count(SceUID id) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(id, &info);
    return info.currentCount;
}

// Starts a thread waiting in the given way and makes its callback block on the inner semaphore
static SceUID start_in_callback(int kind) {
    inner = sceKernelCreateSema("inner", 0, 0, 1, NULL);
    inner_result = outer_result = 0xDEAD;
    const SceUID thread = test_start_thread_arg(outer_thread, kind);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, inner));
    return thread;
}

// Ends the callback's wait, waits for the thread to end and prints how both waits ended. If the
// wake for the outer wait had ended the callback's wait too, the signal here is left over.
static void finish(const char *name, SceUID thread) {
    sceKernelSignalSema(inner, 1);
    test_join_thread(thread);
    PRINTF(inner_result, "%s: the callback's wait", name);
    PRINTF(sema_count(inner), "%s: the inner semaphore's count at the end", name);
    PRINTF(outer_result, "%s: the outer wait", name);
    PRINTF(outer_left > 0, "%s: the outer wait has time left", name);
    sceKernelDeleteSema(inner);
}

int main(void) {
    // A semaphore signal
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    SceUID thread = start_in_callback(SEMA);
    SceKernelSemaInfo sema_info = { .size = sizeof(sema_info) };
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("semaphore: its waiters while the callback waits", sema_info.numWaitThreads);
    PRINT("semaphore: signal it", sceKernelSignalSema(sema, 1));
    sceKernelGetSemaInfo(sema, &sema_info);
    PRINT("semaphore: its count after the signal", sema_info.currentCount);
    PRINT("semaphore: its waiters after the signal", sema_info.numWaitThreads);
    finish("semaphore", thread);
    PRINT("semaphore: its count at the end", sema_count(sema));
    sceKernelDeleteSema(sema);

    // A mutex unlock
    mutex = sceKernelCreateMutex("mutex", 0, 0, NULL);
    sceKernelLockMutex(mutex, 1, NULL);
    thread = start_in_callback(MUTEX);
    PRINT("mutex: unlock it", sceKernelUnlockMutex(mutex, 1));
    MutexInfo mutex_info = { .size = sizeof(mutex_info) };
    sceKernelGetMutexInfo(mutex, (SceKernelMutexInfo *)&mutex_info);
    PRINT("mutex: the waiting thread owns it after the unlock", mutex_info.currentOwnerId == thread);
    PRINT("mutex: its count after the unlock", mutex_info.currentCount);
    PRINT("mutex: its waiters after the unlock", mutex_info.numWaitThreads);
    const int try_result = sceKernelTryLockMutex(mutex, 1);
    PRINT("mutex: try to lock it", try_result);
    if (try_result == 0)
        sceKernelUnlockMutex(mutex, 1);
    finish("mutex", thread);
    sceKernelDeleteMutex(mutex);

    // Event flag bits for a wait that clears them
    flag = sceKernelCreateEventFlag("flag", 0, 0, NULL);
    thread = start_in_callback(EVENT_FLAG);
    PRINT("event flag: set the bit", sceKernelSetEventFlag(flag, 0x1));
    SceKernelEventFlagInfo flag_info = { .size = sizeof(flag_info) };
    sceKernelGetEventFlagInfo(flag, &flag_info);
    PRINT("event flag: its bits after the set", flag_info.currentPattern);
    PRINT("event flag: its waiters after the set", flag_info.numWaitThreads);
    finish("event flag", thread);
    PRINT("event flag: the bits the outer wait got", outer_bits);
    sceKernelGetEventFlagInfo(flag, &flag_info);
    PRINT("event flag: its bits at the end", flag_info.currentPattern);
    sceKernelDeleteEventFlag(flag);

    // A bit of an auto reset simple event
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL);
    thread = start_in_callback(SIMPLE_EVENT);
    PRINT("simple event: set the bit", sceKernelSetEvent(event, EVENT_BIT_A, 0));
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    PRINT("simple event: its bits after the set", pattern);
    finish("simple event", thread);
    pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    PRINT("simple event: its bits at the end", pattern);
    sceKernelDeleteSimpleEvent(event);

    // A message for a receive that waits for all of it
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 64, NULL);
    thread = start_in_callback(MSG_PIPE);
    char message[4] = { 1, 2, 3, 4 };
    PRINT("message pipe: send the message", sceKernelTrySendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL));
    SceKernelMppInfo pipe_info = { .size = sizeof(pipe_info) };
    sceKernelGetMsgPipeInfo(pipe, &pipe_info);
    PRINT("message pipe: the bytes in it after the send", pipe_info.bufSize - pipe_info.freeSize);
    PRINT("message pipe: its receivers after the send", pipe_info.numReceiveWaitThreads);
    finish("message pipe", thread);
    PRINT("message pipe: the outer wait got the message", sceClibMemcmp(outer_message, message, sizeof(message)) == 0);
    sceKernelDeleteMsgPipe(pipe);

    // Deleting the object of the outer wait
    sema = sceKernelCreateSema("delete", 0, 0, 1, NULL);
    thread = start_in_callback(DELETE);
    PRINT("delete: delete the semaphore", sceKernelDeleteSema(sema));
    finish("delete", thread);

    // Canceling the outer wait
    sema = sceKernelCreateSema("cancel", 0, 0, 1, NULL);
    thread = start_in_callback(CANCEL);
    int waiting = 0xDEAD;
    PRINT("cancel: cancel the semaphore", sceKernelCancelSema(sema, 0, &waiting));
    PRINT("cancel: the waiters it counts", waiting);
    finish("cancel", thread);
    sceKernelDeleteSema(sema);

    test_exit();
    return 0;
}

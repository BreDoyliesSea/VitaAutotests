// Whether a wait with callbacks that takes what it waits for at once runs a callback that was
// notified during its callback run before it returns. The main thread creates a callback C and
// notifies it, which only leaves it pending since the main thread isn't waiting, and then waits
// with callbacks on an object that already holds what it waits for. C runs at the start of the
// wait, and its first run notifies C again, so it is pending once more when the take succeeds. Its
// later runs do nothing. Every case has fresh objects. The waits:
// - semaphore: sceKernelWaitSemaCB for 1 on a semaphore with a count of 1
// - event flag: sceKernelWaitEventFlagCB for bit 0x1, which is set
// - simple event: sceKernelWaitEventCB for a user bit, which is set
// - mutex: sceKernelLockMutexCB of a free mutex
// - rwlock: sceKernelLockReadRWLockCB and sceKernelLockWriteRWLockCB of a free rwlock
// - message pipe: sceKernelReceiveMsgPipeCB of 4 bytes the pipe holds, and sceKernelSendMsgPipeCB
//   of 4 bytes into an empty pipe
// Each case prints the wait's result and how often C ran when the wait returned, then calls
// sceKernelCheckCallback and prints its result and how often C ran after it.
//
// On a PS Vita TV on firmware 3.65 every wait returns 0. The semaphore, event flag, simple event,
// mutex and rwlock waits return with C run once and pending again, and the check runs it and
// returns 1. The message pipe receive and send run C a second time before they return, and the
// check has nothing to run and returns 0.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define MESSAGE_SIZE 4

typedef enum Kind { SEMA, FLAG, EVENT, MUTEX, READ, WRITE, RECEIVE, SEND } Kind;

static SceUID callback;
static volatile int callback_runs;

// The first run notifies the callback again, the later ones do nothing
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    if (callback_runs == 1)
        sceKernelNotifyCallback(callback, 1);
    return 0;
}

static void run_case(const char *name, Kind kind) {
    SceUInt timeout = TEST_WAIT_US;
    SceUID object = -1;
    switch (kind) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 1, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0x1, NULL); break;
    case EVENT:
        object = sceKernelCreateSimpleEvent("event", 0, 0, NULL);
        sceKernelSetEvent(object, EVENT_BIT_A, 0);
        break;
    case MUTEX: object = sceKernelCreateMutex("mutex", 0, 0, NULL); break;
    case READ:
    case WRITE: object = sceKernelCreateRWLock("rwlock", 0, NULL); break;
    case RECEIVE:
        object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);
        sceKernelSendMsgPipe(object, "ABCD", MESSAGE_SIZE, MSG_PIPE_MODE_FULL, NULL, &timeout);
        break;
    case SEND: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    }

    callback_runs = 0;
    callback = sceKernelCreateCallback("again", 0, callback_entry, NULL);
    sceKernelNotifyCallback(callback, 1);
    timeout = TEST_WAIT_US;
    SceUInt32 bits = 0;
    SceUInt64 user_data = 0;
    char buffer[MESSAGE_SIZE] = { 'W', 'X', 'Y', 'Z' };
    int result = NOT_DONE;
    switch (kind) {
    case SEMA: result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: result = sceKernelWaitEventCB(object, EVENT_BIT_A, &bits, &user_data, &timeout); break;
    case MUTEX: result = sceKernelLockMutexCB(object, 1, &timeout); break;
    case READ: result = sceKernelLockReadRWLockCB(object, &timeout); break;
    case WRITE: result = sceKernelLockWriteRWLockCB(object, &timeout); break;
    case RECEIVE: result = sceKernelReceiveMsgPipeCB(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case SEND: result = sceKernelSendMsgPipeCB(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
    const int runs_in_wait = callback_runs;
    const int check = sceKernelCheckCallback();
    const int runs_after_check = callback_runs;
    sceKernelDeleteCallback(callback);

    PRINTF(result, "%s: the wait", name);
    PRINTF(runs_in_wait, "%s: runs of the callback when the wait returned", name);
    PRINTF(check, "%s: the check", name);
    PRINTF(runs_after_check, "%s: runs of the callback after the check", name);

    switch (kind) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case MUTEX:
        sceKernelUnlockMutex(object, 1);
        sceKernelDeleteMutex(object);
        break;
    case READ:
        sceKernelUnlockReadRWLock(object);
        sceKernelDeleteRWLock(object);
        break;
    case WRITE:
        sceKernelUnlockWriteRWLock(object);
        sceKernelDeleteRWLock(object);
        break;
    case RECEIVE:
    case SEND: sceKernelDeleteMsgPipe(object); break;
    }
}

int main(void) {
    run_case("semaphore", SEMA);
    run_case("event flag", FLAG);
    run_case("simple event", EVENT);
    run_case("mutex", MUTEX);
    run_case("rwlock read", READ);
    run_case("rwlock write", WRITE);
    run_case("message pipe receive", RECEIVE);
    run_case("message pipe send", SEND);

    test_exit();
    return 0;
}

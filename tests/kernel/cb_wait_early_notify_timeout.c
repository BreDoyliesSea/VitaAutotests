// What a wait with callbacks leaves in the caller's timeout variable when a callback is already
// pending as the wait is about to sleep, and the callbacks end up giving it what it waits for. A
// thread T creates a callback C, and before its wait it notifies C, which only leaves it pending
// since T isn't waiting. Then T waits with callbacks with a timeout of 1 s in a variable. C runs
// twice. Its first run notifies C again, so a callback is pending once more when the wait is about
// to sleep, after its take failed. Its second run gives T's object what T waits for, so the take
// that follows succeeds. The wait never sleeps, and the cases show whether the timeout variable
// still holds what was passed. Every case has fresh objects and runs T alone, the main thread
// waits for T to end. Each wait takes what it waits for:
// - semaphore: sceKernelWaitSemaCB for 1, which C's second run signals
// - event flag: sceKernelWaitEventFlagCB for bit 0x1, which C's second run sets
// - simple event: sceKernelWaitEventCB for a user bit, which C's second run sets
// - message pipe: sceKernelReceiveMsgPipeCB of 4 bytes in FULL mode, which C's second run sends to
//   the same pipe
// Each case prints the wait's result, how often C ran, and whether the timeout is exactly the one
// passed. After the four cases come the same four waits without anything pending. T waits, and once
// the main thread sees it waiting, the main thread gives it the object. These print the wait's
// result and whether the time left is below the timeout passed.
//
// On a PS Vita TV on firmware 3.65 the wait with the pending callback returns without sleeping and
// without writing the time left, so the timeout comes back as it was passed, 1000000, in all four
// cases. C runs twice in each. The semaphore, the event flag and the message pipe wait return 0.
// The simple event wait returns 0x800280A2 with the bits taken, as it does when its bits come while
// its callback runs. The waits that sleep and are woken report a time left below the timeout.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define WAIT_US 1000000
#define MESSAGE_SIZE 4

typedef enum Kind { SEMA, FLAG, EVENT, PIPE } Kind;

static Kind kind;
static int pending_first;
static SceUID object;
static volatile SceUID callback;
static volatile int callback_runs, wait_result;
static volatile SceUInt time_after;

// Gives T's wait what it waits for
static void give(void) {
    SceUInt timeout = TEST_WAIT_US;
    switch (kind) {
    case SEMA: sceKernelSignalSema(object, 1); break;
    case FLAG: sceKernelSetEventFlag(object, 0x1); break;
    case EVENT: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    case PIPE: sceKernelSendMsgPipe(object, "ABCD", MESSAGE_SIZE, MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    }
}

// With a callback pending first, the first run notifies the callback again and the second gives T's
// wait what it waits for. Without, it does nothing.
static int callback_entry(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_runs++;
    if (pending_first) {
        if (callback_runs == 1)
            sceKernelNotifyCallback(callback, 1);
        else if (callback_runs == 2)
            give();
    }
    return 0;
}

// Creates a callback, notifies it if the case says so, and waits with callbacks for 1 s
static int t_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("early", 0, callback_entry, NULL);
    if (pending_first)
        sceKernelNotifyCallback(callback, 1);
    SceUInt timeout = WAIT_US;
    SceUInt32 bits = 0;
    SceUInt64 user_data = 0;
    SceSize moved = 0;
    char buffer[MESSAGE_SIZE];
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITOR, &bits, &timeout); break;
    case EVENT: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &bits, &user_data, &timeout); break;
    case PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buffer, sizeof(buffer), MSG_PIPE_MODE_FULL, &moved, &timeout); break;
    }
    time_after = timeout;
    sceKernelDeleteCallback(callback);
    return 0;
}

static SceUInt32 wait_type(Kind k) {
    switch (k) {
    case SEMA: return WAIT_TYPE_SEMA | WAIT_TYPE_CB;
    case FLAG: return WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB;
    case EVENT: return WAIT_TYPE_EVENT | WAIT_TYPE_CB;
    case PIPE: return WAIT_TYPE_MSG_PIPE | WAIT_TYPE_CB;
    }
    return 0;
}

static void run_case(const char *name, Kind k, int pending) {
    kind = k;
    pending_first = pending;
    callback = 0;
    callback_runs = 0;
    wait_result = NOT_DONE;
    time_after = NOT_DONE;
    switch (k) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case EVENT: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    case PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL); break;
    }

    const SceUID t = test_start_thread(t_entry);
    if (!pending) {
        TEST_WAIT_FOR(test_in_wait(t, wait_type(k), object));
        give();
    }
    test_join_thread(t);

    PRINTF(wait_result, "%s: the wait", name);
    if (pending) {
        PRINTF(callback_runs, "%s: runs of the callback", name);
        PRINTF(time_after == WAIT_US, "%s: the timeout is exactly the one passed", name);
    } else {
        PRINTF(time_after < WAIT_US, "%s: the time left is below the timeout passed", name);
    }

    switch (k) {
    case SEMA: sceKernelDeleteSema(object); break;
    case FLAG: sceKernelDeleteEventFlag(object); break;
    case EVENT: sceKernelDeleteSimpleEvent(object); break;
    case PIPE: sceKernelDeleteMsgPipe(object); break;
    }
}

int main(void) {
    run_case("semaphore, nothing pending", SEMA, 0);
    run_case("event flag, nothing pending", FLAG, 0);
    run_case("simple event, nothing pending", EVENT, 0);
    run_case("message pipe, nothing pending", PIPE, 0);

    run_case("semaphore, callback pending", SEMA, 1);
    run_case("event flag, callback pending", FLAG, 1);
    run_case("simple event, callback pending", EVENT, 1);
    // The callback sends to the pipe, so a wait that holds the pipe while it runs callbacks hangs
    run_case("message pipe, callback pending", PIPE, 1);

    test_exit();
    return 0;
}

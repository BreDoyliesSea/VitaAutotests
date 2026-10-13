// What a wait on an event object returns when its event comes while its thread runs a callback:
// a timer event that fires, a pulse, and the end of a message pipe receive that didn't wait. A
// simple event set during the callback ends the wait with NOTIFY_CALLBACK (callback_outer_wake).
// The callback blocks until the test releases it, long after the event came.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

enum Kind { TIMER, PULSE, PIPE_EVENT, SET_FOR_REFERENCE };

static SceUID object, release, cb;
static volatile int kind, held, wait_result;
static volatile SceUInt32 wait_pattern;
static volatile SceUInt time_left;

static int held_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    held = 1;
    SceUInt timeout = 5 * TEST_WAIT_US;
    sceKernelWaitSema(release, 1, &timeout);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("held", 0, held_callback, NULL);
    SceUInt timeout = TEST_WAIT_US;
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data;
    const SceUInt32 bit = kind == TIMER ? TIMER_EVENT_BIT : kind == PIPE_EVENT ? EVENT_OUT : EVENT_BIT_A;
    wait_result = sceKernelWaitEventCB(object, bit, &pattern, &user_data, &timeout);
    wait_pattern = pattern;
    time_left = timeout;
    sceKernelDeleteCallback(cb);
    return 0;
}

static void run(const char *name, int new_kind) {
    kind = new_kind;
    static char buf[4];
    switch (new_kind) {
    case TIMER: object = sceKernelCreateTimer("timer", 0, NULL); break;
    case PIPE_EVENT:
        object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 64, NULL);
        sceKernelReceiveMsgPipe(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, NULL, NULL);
        break;
    default: object = sceKernelCreateSimpleEvent("event", 0, 0, NULL); break;
    }
    release = sceKernelCreateSema("release", 0, 0, 1, NULL);
    held = 0;
    wait_result = 0xDEAD;
    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(held);

    // The event, while the callback runs
    SceUInt64 interval = 20000;
    char message[4] = { 1, 2, 3, 4 };
    switch (new_kind) {
    case TIMER:
        sceKernelSetTimerEvent(object, 0, &interval, 0);
        sceKernelStartTimer(object);
        break;
    case PULSE: sceKernelPulseEvent(object, EVENT_BIT_A, 0); break;
    case PIPE_EVENT: sceKernelTrySendMsgPipe(object, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL); break;
    case SET_FOR_REFERENCE: sceKernelSetEvent(object, EVENT_BIT_A, 0); break;
    }
    // Ten times the timer's interval
    sceKernelDelayThread(200000);
    sceKernelSignalSema(release, 1);
    test_join_thread(thread);
    PRINTF(wait_result, "%s: the wait", name);
    PRINTF(wait_pattern, "%s: the bits it got", name);
    PRINTF(time_left > 0, "%s: it ended before its timeout", name);

    if (new_kind == TIMER) {
        sceKernelStopTimer(object);
        sceKernelDeleteTimer(object);
    } else if (new_kind == PIPE_EVENT) {
        sceKernelDeleteMsgPipe(object);
    } else {
        sceKernelDeleteSimpleEvent(object);
    }
    sceKernelDeleteSema(release);
}

int main(void) {
    run("simple event set, for reference", SET_FOR_REFERENCE);
    run("timer event", TIMER);
    run("simple event pulsed", PULSE);
    run("message pipe receive that didn't wait", PIPE_EVENT);

    test_exit();
    return 0;
}

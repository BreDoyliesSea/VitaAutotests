// What a wait with callbacks returns when a callback is notified while it waits and nothing else
// happens: whether it goes on until its timeout or ends after the callback. Each wait has a
// timeout of 1 s, so it ends either way. A wait on several events isn't here: a Vita (FW 3.65)
// hangs the whole app when a callback is notified while a thread is in sceKernelWaitMultipleEventsCB.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define WAIT_US 1000000

enum Kind { SEMA, EVENT_FLAG, SIMPLE_EVENT, SET_IN_CALLBACK, TIMER, MSG_PIPE, DELAY };

static SceUID object, cb;
static volatile int kind, calls, wait_result;
static volatile SceUInt time_left;
static volatile SceUInt64 elapsed;

static int notified_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    calls++;
    if (kind == SET_IN_CALLBACK)
        sceKernelSetEvent(object, EVENT_BIT_A, 0);
    return 0;
}

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("notified", 0, notified_callback, NULL);
    SceUInt timeout = WAIT_US;
    SceUInt32 pattern;
    SceUInt64 user_data;
    unsigned int bits;
    char buf[4];
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    switch (kind) {
    case SEMA: wait_result = sceKernelWaitSemaCB(object, 1, &timeout); break;
    case EVENT_FLAG: wait_result = sceKernelWaitEventFlagCB(object, 0x1, SCE_EVENT_WAITAND, &bits, &timeout); break;
    case SIMPLE_EVENT:
    case SET_IN_CALLBACK: wait_result = sceKernelWaitEventCB(object, EVENT_BIT_A, &pattern, &user_data, &timeout); break;
    case TIMER: wait_result = sceKernelWaitEventCB(object, EVENT_TIMER, &pattern, &user_data, &timeout); break;
    case MSG_PIPE: wait_result = sceKernelReceiveMsgPipeCB(object, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL, &timeout); break;
    case DELAY: wait_result = sceKernelDelayThreadCB(WAIT_US); break;
    }
    elapsed = sceKernelGetProcessTimeWide() - start;
    time_left = timeout;
    sceKernelDeleteCallback(cb);
    return 0;
}

static void run(const char *name, int new_kind) {
    kind = new_kind;
    switch (new_kind) {
    case SEMA: object = sceKernelCreateSema("sema", 0, 0, 1, NULL); break;
    case EVENT_FLAG: object = sceKernelCreateEventFlag("flag", 0, 0, NULL); break;
    case TIMER: object = sceKernelCreateTimer("timer", 0, NULL); break;
    case MSG_PIPE: object = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 64, NULL); break;
    default: object = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL); break;
    }
    calls = 0;
    wait_result = 0xDEAD;
    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    test_join_thread(thread);
    PRINTF(calls, "%s: runs of the callback", name);
    PRINTF(wait_result, "%s: the wait", name);
    if (new_kind == DELAY)
        PRINTF(elapsed < WAIT_US / 2, "%s: it ended long before its time", name);
    else
        PRINTF(time_left > 0, "%s: it ended before its timeout", name);
    if (new_kind == SET_IN_CALLBACK) {
        SceUInt32 pattern = 0xDEAD;
        sceKernelGetEventPattern(object, &pattern);
        PRINTF(pattern, "%s: the event's bits after it", name);
    }
    switch (new_kind) {
    case SEMA: sceKernelDeleteSema(object); break;
    case EVENT_FLAG: sceKernelDeleteEventFlag(object); break;
    case TIMER: sceKernelDeleteTimer(object); break;
    case MSG_PIPE: sceKernelDeleteMsgPipe(object); break;
    default: sceKernelDeleteSimpleEvent(object); break;
    }
}

int main(void) {
    run("semaphore", SEMA);
    run("event flag", EVENT_FLAG);
    run("simple event", SIMPLE_EVENT);
    run("simple event set by the callback", SET_IN_CALLBACK);
    run("timer", TIMER);
    run("message pipe", MSG_PIPE);
    run("delay", DELAY);

    test_exit();
    return 0;
}

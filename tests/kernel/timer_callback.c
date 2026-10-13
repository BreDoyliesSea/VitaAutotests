// Callbacks registered to a timer: whether its events notify them with no thread waiting, what the
// callback gets, repeating events, and a stopped timer. Each event that must have fired gets at
// least ten times its interval, and the count of a repeating event is compared with the time the
// timer itself counted.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID timer;
static volatile int calls, got_notify_id, got_count, got_arg;

static int recording_callback(int notify_id, int count, int arg, void *common) {
    (void)common;
    calls++;
    got_notify_id = notify_id;
    got_count = count;
    got_arg = arg;
    return 0;
}

int main(void) {
    const SceUID cb = sceKernelCreateCallback("timer", 0, recording_callback, NULL);

    // One event 20 ms after the start, then a delay that checks callbacks
    timer = sceKernelCreateTimer("once", EVENT_ATTR_NOTIFY_CB_ALL, NULL);
    PRINT("register", sceKernelRegisterCallbackToEvent(timer, cb));
    SceUInt64 interval = 20000;
    PRINT("set the event", sceKernelSetTimerEvent(timer, 0, &interval, 0));
    PRINT("start", sceKernelStartTimer(timer));
    sceKernelDelayThreadCB(200000);
    PRINT("one event: runs of the callback", calls);
    if (calls) {
        PRINT("one event: the notify ID is the timer", got_notify_id == timer);
        PRINT("one event: the count", got_count);
        PRINT("one event: the argument", got_arg);
    }
    SceUInt32 result = 0;
    SceUInt64 user_data = 0;
    PRINT("one event: poll the timer's event", sceKernelPollEvent(timer, TIMER_EVENT_BIT, &result, &user_data));
    sceKernelStopTimer(timer);

    // A repeating event every 20 ms for 300 ms, with callbacks only checked once the timer stopped
    calls = 0;
    sceKernelSetTimerEvent(timer, 0, &interval, 1);
    const SceUInt64 timer_before = sceKernelGetTimerTimeWide(timer);
    sceKernelStartTimer(timer);
    sceKernelDelayThread(300000);
    sceKernelStopTimer(timer);
    const int events = (int)((sceKernelGetTimerTimeWide(timer) - timer_before) / interval);
    const int pending = sceKernelGetCallbackCount(cb);
    PRINT("repeating: more than one notification pending", pending > 1);
    PRINT("repeating: about one per event, give or take one", pending >= events - 1 && pending <= events + 1);
    sceKernelCheckCallback();
    PRINT("repeating: runs of the callback", calls);
    PRINT("repeating: the count it got is the notifications", got_count == pending);

    // Stopped, it notifies nothing more
    sceKernelCancelCallback(cb);
    calls = 0;
    sceKernelDelayThreadCB(200000);
    PRINT("stopped: runs of the callback", calls);
    PRINT("unregister", sceKernelUnregisterCallbackFromEvent(timer, cb));
    sceKernelDeleteTimer(timer);

    // Notify the woken only, with no thread waiting
    calls = 0;
    timer = sceKernelCreateTimer("woken", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY, NULL);
    sceKernelRegisterCallbackToEvent(timer, cb);
    sceKernelSetTimerEvent(timer, 0, &interval, 0);
    sceKernelStartTimer(timer);
    sceKernelDelayThreadCB(200000);
    PRINT("notify the woken only: runs of the callback", calls);
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);

    test_exit();
    return 0;
}

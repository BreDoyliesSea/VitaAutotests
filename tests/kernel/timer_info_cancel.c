// The fields of a timer's info after create, after an event is set and after it starts, and what
// sceKernelCancelTimer does with 2 waiters and with a waiter and no count pointer. The event is far
// in the future, so it never fires during the test. Kept short and on its own, so a hang in it
// loses nothing else.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define INTERVAL 10000000

static SceUID timer;
static volatile int results[2];

static int wait_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US, result;
    SceUInt64 user_data;
    results[i] = sceKernelWaitEvent(timer, EVENT_TIMER, &result, &user_data, &timeout);
    return 0;
}

static void start_waiters(SceUID *threads, int count) {
    for (int i = 0; i < count; i++) {
        results[i] = -1;
        threads[i] = test_start_thread_arg(wait_thread, i);
        test_wait_until_waiting(threads[i]);
    }
}

static TimerInfo info_of(void) {
    TimerInfo info = { .size = sizeof(info) };
    sceKernelGetTimerInfo(timer, &info);
    return info;
}

int main(void) {
    SceUID threads[2];
    timer = sceKernelCreateTimer("timer", EVENT_ATTR_AUTO_RESET, NULL);
    TimerInfo info = { .size = sizeof(info) };
    PRINT("info", sceKernelGetTimerInfo(timer, &info));
    PRINT("the attribute", info.attr);
    PRINT("it is active", info.active);
    PRINT("the base time is 0", info.baseTime == 0);
    PRINT("the current time is 0", info.currentTime == 0);
    PRINT("the schedule is 0", info.schedule == 0);
    PRINT("the interval is 0", info.interval == 0);
    PRINT("the type", info.type);
    PRINT("the repeat setting", info.repeat);
    PRINT("the waiting threads", info.numWaitThreads);

    SceUInt64 interval = INTERVAL;
    PRINT("set a repeating pulse event", sceKernelSetTimerEvent(timer, TIMER_TYPE_PULSE_EVENT, &interval, 1));
    info = info_of();
    PRINT("the type after it", info.type);
    PRINT("the repeat setting after it", info.repeat);
    PRINT("the interval after it is the one set", info.interval == INTERVAL);
    PRINT("the schedule after it is the interval", info.schedule == INTERVAL);
    PRINT("it is active after it", info.active);

    sceKernelStartTimer(timer);
    info = info_of();
    PRINT("it is active after starting", info.active);
    PRINT("the base time is set after starting", info.baseTime != 0);
    PRINT("the current time is below the interval", info.currentTime < INTERVAL);
    PRINT("the schedule is still the interval", info.schedule == INTERVAL);

    start_waiters(threads, 2);
    PRINT("the waiting threads with 2 waiters", info_of().numWaitThreads);
    SceUInt32 waiting = 0xDEAD;
    PRINT("cancel with 2 waiters", sceKernelCancelTimer(timer, &waiting));
    for (int i = 0; i < 2; i++)
        test_join_thread(threads[i]);
    PRINT("the waiting count", waiting);
    PRINT("the first waiter", results[0]);
    PRINT("the second waiter", results[1]);
    info = info_of();
    PRINT("it is active after the cancel", info.active);
    PRINT("the interval after the cancel is the one set", info.interval == INTERVAL);

    start_waiters(threads, 1);
    PRINT("cancel without a count pointer with a waiter", sceKernelCancelTimer(timer, NULL));
    test_join_thread(threads[0]);
    PRINT("the waiter", results[0]);

    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
    test_exit();
    return 0;
}

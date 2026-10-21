// Whether a one shot timer event that already fired fires again. A one shot event is set with a
// repeat of 0 and an interval of 1 s, the timer starts, and the event fires once. Each case runs on
// a fresh timer, for a set event (manual reset, cleared with sceKernelClearEvent after it fired) and
// for a pulse event, and a waiter thread waits for the timer's bit with an explicit timeout.
//
// Cases:
// - stop and start: once the event fired and the bit is clear, a waiter starts waiting with a
//   timeout of 300 ms, the main thread stops the timer and starts it again, and the waiter's result
//   is printed. A fire right after the start gets the event, a fire after a full interval or never
//   times out. It runs twice in a row on the same timer.
// - no stop and start: the same, with the timer left running, as the reference.
// - set time: once the event fired and the bit is clear, a waiter starts waiting with a timeout of
//   3 s, and the main thread sets the timer's time to 0 while it runs, which is below the event's
//   time of 1 s. The waiter's result is printed, with whether it returned within 500 ms of the set
//   (at once) and within 1500 ms (after the interval, which is the time the set left).
//
// Every time is only printed as a comparison with thresholds at least 250 ms from each outcome.
//
// On a PS Vita TV on firmware 3.65 a one shot event that already fired fires again right after a
// stop and a start, each time, for a set event and for a pulse event. With the timer left running
// it doesn't fire again. Setting the time to 0 makes it fire again after the interval, between 500
// ms and 1500 ms after the set.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define INTERVAL_US 1000000
#define STOP_START_WAIT_US 300000
#define SET_TIME_WAIT_US 3000000
#define AT_ONCE_US 500000
#define AFTER_INTERVAL_US 1500000

// What the waiter thread reports, read once the thread ended
static struct {
    SceUID timer;
    SceUInt32 timeout_us;
    int result;
    SceUInt64 end_time;
} waiter;

static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = waiter.timeout_us;
    const int result = sceKernelWaitEvent(waiter.timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    waiter.end_time = sceKernelGetProcessTimeWide();
    waiter.result = result;
    return 0;
}

// Starts a thread that waits for the timer's bit, and returns once it waits
static SceUID start_waiter(SceUID timer, SceUInt32 timeout_us) {
    waiter.timer = timer;
    waiter.timeout_us = timeout_us;
    waiter.result = 0xDEAD;
    waiter.end_time = 0;
    const SceUID thread = test_start_thread(waiter_entry);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_EVENT, timer));
    return thread;
}

static int poll_event(SceUID timer) {
    SceUInt32 result = 0;
    SceUInt64 user_data = 0;
    return sceKernelPollEvent(timer, EVENT_TIMER, &result, &user_data);
}

// Creates a timer with a one shot event of the type
static SceUID create_timer(int type) {
    const SceUID timer = sceKernelCreateTimer("refire", 0, NULL);
    SceUInt64 interval = INTERVAL_US;
    sceKernelSetTimerEvent(timer, type, &interval, 0);
    return timer;
}

// Starts the timer, waits until its event fired, and clears the bit. Returns the wait's result.
static int fire_and_clear(SceUID timer) {
    SceUInt32 result = 0;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    sceKernelStartTimer(timer);
    const int wait = sceKernelWaitEvent(timer, EVENT_TIMER, &result, &user_data, &timeout);
    sceKernelClearEvent(timer, 0);
    return wait;
}

static void stop_and_start_case(const char *name, int type) {
    const SceUID timer = create_timer(type);
    PRINTF(fire_and_clear(timer), "%s, stop and start: the first wait", name);
    PRINTF(poll_event(timer), "%s, stop and start: a poll after the clear", name);
    for (int round = 1; round <= 2; round++) {
        const SceUID thread = start_waiter(timer, STOP_START_WAIT_US);
        const int stop = sceKernelStopTimer(timer);
        const int start = sceKernelStartTimer(timer);
        test_join_thread(thread);
        PRINTF(stop, "%s, stop and start: the stop %d", name, round);
        PRINTF(start, "%s, stop and start: the start %d", name, round);
        PRINTF(waiter.result, "%s, stop and start: the wait after it %d", name, round);
        sceKernelClearEvent(timer, 0);
    }
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

static void no_stop_and_start_case(const char *name, int type) {
    const SceUID timer = create_timer(type);
    PRINTF(fire_and_clear(timer), "%s, no stop and start: the first wait", name);
    const SceUID thread = start_waiter(timer, STOP_START_WAIT_US);
    test_join_thread(thread);
    PRINTF(waiter.result, "%s, no stop and start: the wait after the first", name);
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

static void set_time_case(const char *name, int type) {
    const SceUID timer = create_timer(type);
    PRINTF(fire_and_clear(timer), "%s, set time: the first wait", name);
    const SceUID thread = start_waiter(timer, SET_TIME_WAIT_US);
    SceKernelSysClock clock = 0;
    const SceUInt64 begin = sceKernelGetProcessTimeWide();
    const int set = sceKernelSetTimerTime(timer, &clock);
    test_join_thread(thread);
    const SceUInt64 elapsed = waiter.end_time - begin;
    const int got = waiter.result == 0;
    PRINTF(set, "%s, set time: the set to 0", name);
    PRINTF(waiter.result, "%s, set time: the wait after it", name);
    PRINTF(got && elapsed < AT_ONCE_US, "%s, set time: it fired within 500 ms of the set", name);
    PRINTF(got && elapsed < AFTER_INTERVAL_US, "%s, set time: it fired within 1500 ms of the set", name);
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

int main(void) {
    static const struct {
        const char *name;
        int type;
    } columns[] = {
        { "set event", TIMER_TYPE_SET_EVENT },
        { "pulse event", TIMER_TYPE_PULSE_EVENT },
    };

    for (unsigned i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        stop_and_start_case(columns[i].name, columns[i].type);
        no_stop_and_start_case(columns[i].name, columns[i].type);
        set_time_case(columns[i].name, columns[i].type);
    }

    test_exit();
    return 0;
}

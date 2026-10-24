// What sceKernelGetTimerEventRemainingTime returns once a one shot timer event's time has passed.
// Each case runs on a fresh timer with a one shot event of 100 ms, for a set event and for a pulse
// event. The timer starts, the main thread waits for the timer's bit with a timeout of 2 s, and
// right after the wait returns it reads the remaining time, then stops the timer and reads it again.
// The values are only read in between, and printed once both reads are done. For each read it
// prints the call's result and three comparisons on the value it wrote: whether it is above
// 0x8000000000000000, whether it is 0, and whether it is within 1 s below 2^64.
//
// On a PS Vita TV on firmware 3.65 the fired one shot event stays armed, so the call succeeds, both
// while the timer runs and after the stop. The value is the time left as an unsigned 64 bit number
// without a clamp, so it wrapped past 0: above 0x8000000000000000, not 0, and within 1 s of 2^64,
// for the set event and for the pulse event.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define INTERVAL_US 100000
#define WRAP_LIMIT_US 1000000
#define NOT_WRITTEN 0x1234

// One read of the remaining time
typedef struct Remaining {
    int result;
    SceKernelSysClock value;
} Remaining;

static Remaining read_remaining(SceUID timer) {
    Remaining remaining = { .value = NOT_WRITTEN };
    remaining.result = sceKernelGetTimerEventRemainingTime(timer, &remaining.value);
    return remaining;
}

static void print_remaining(const char *name, const char *when, const Remaining *remaining) {
    const SceKernelSysClock value = remaining->value;
    PRINTF(remaining->result, "%s, %s: the remaining time", name, when);
    PRINTF(value > 0x8000000000000000ULL, "%s, %s: the value is above 0x8000000000000000", name, when);
    PRINTF(value == 0, "%s, %s: the value is 0", name, when);
    PRINTF(value > 0x8000000000000000ULL && 0 - value < WRAP_LIMIT_US, "%s, %s: the value is within 1 s below 2^64", name, when);
}

static void passed_case(const char *name, int type) {
    const SceUID timer = sceKernelCreateTimer("passed", 0, NULL);
    SceUInt64 interval = INTERVAL_US;
    sceKernelSetTimerEvent(timer, type, &interval, 0);
    sceKernelStartTimer(timer);

    SceUInt32 pattern = 0;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = TEST_WAIT_US;
    const int wait = sceKernelWaitEvent(timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    const Remaining running = read_remaining(timer);
    const int stop = sceKernelStopTimer(timer);
    const Remaining stopped = read_remaining(timer);

    PRINTF(wait, "%s: the wait", name);
    print_remaining(name, "running", &running);
    PRINTF(stop, "%s: the stop", name);
    print_remaining(name, "stopped", &stopped);
    sceKernelDeleteTimer(timer);
}

int main(void) {
    passed_case("set event", TIMER_TYPE_SET_EVENT);
    passed_case("pulse event", TIMER_TYPE_PULSE_EVENT);

    test_exit();
    return 0;
}

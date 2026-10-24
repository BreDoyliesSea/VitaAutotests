// What sceKernelGetTimerBase and sceKernelGetTimerEventRemainingTime read in each state of a timer.
// The timer gets a one shot set event of 10 s, which never fires during the test. The steps, each
// printing the calls' results and comparisons, never the times themselves:
// - a fresh timer: the base, and the remaining time with no event set
// - the event set while the timer is stopped: the remaining time is the interval
// - started: the base is the process time it started at, and the remaining time plus the timer's
//   time is the interval, within 50 ms
// - stopped: the base, and the remaining time plus the timer's time is the interval exactly
// - started again: the base isn't 0, and the remaining time plus the timer's time is the interval,
//   within 50 ms
// - canceled: the remaining time
//
// On a PS Vita TV on firmware 3.65 the remaining time with no event armed, also after the cancel,
// is 0x80028242. Otherwise it plus the timer's time is the interval, exactly while the timer is
// stopped. The base reads 0 while the timer is stopped, and the process time the count runs from
// while it runs.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define INTERVAL 10000000
#define CLOSE_US 50000

static SceUID timer;

// Prints the remaining time's result, and whether it plus the timer's time is the interval, exactly
// or within 50 ms
static void print_remaining(const char *name, int exact) {
    const SceUInt64 time = sceKernelGetTimerTimeWide(timer);
    SceKernelSysClock left = 0;
    PRINTF(sceKernelGetTimerEventRemainingTime(timer, &left), "%s: the remaining time", name);
    const SceInt64 off = (SceInt64)(left + time) - INTERVAL;
    if (exact)
        PRINTF(off == 0, "%s: the remaining time plus the timer's time is the interval", name);
    else
        PRINTF(off > -CLOSE_US && off < CLOSE_US, "%s: the remaining time plus the timer's time is the interval, within 50 ms", name);
}

static void print_base(const char *name, const char *what, SceUInt64 low, SceUInt64 high) {
    SceKernelSysClock base = 0xDEAD;
    PRINTF(sceKernelGetTimerBase(timer, &base), "%s: the base", name);
    PRINTF(base >= low && base <= high, "%s: %s", name, what);
}

int main(void) {
    timer = sceKernelCreateTimer("base", 0, NULL);
    print_base("a fresh timer", "the base is 0", 0, 0);
    SceKernelSysClock left = 0;
    PRINT("a fresh timer: the remaining time", sceKernelGetTimerEventRemainingTime(timer, &left));

    SceUInt64 interval = INTERVAL;
    sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0);
    PRINT("set while stopped: the remaining time", sceKernelGetTimerEventRemainingTime(timer, &left));
    PRINT("set while stopped: the remaining time is the interval", left == INTERVAL);

    const SceUInt64 before = sceKernelGetProcessTimeWide();
    sceKernelStartTimer(timer);
    print_base("started", "the base is the process time it started at", before, sceKernelGetProcessTimeWide());
    print_remaining("started", 0);

    sceKernelStopTimer(timer);
    print_base("stopped", "the base is 0", 0, 0);
    print_remaining("stopped", 1);

    sceKernelStartTimer(timer);
    print_base("started again", "the base isn't 0", 1, ~0ULL);
    print_remaining("started again", 0);

    sceKernelCancelTimer(timer, NULL);
    PRINT("canceled: the remaining time", sceKernelGetTimerEventRemainingTime(timer, &left));

    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
    test_exit();
    return 0;
}

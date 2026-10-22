// That a timer's event reaches every waiter it should. Waiters 0, 1 and 2, at 0x90, 0x70 and 0x80,
// wait for EVENT_TIMER on core 0 in that order, each once the one before waits and each with a
// timeout of 5 seconds, and the main thread, on core 1, starts the timer once all three wait. Then
// it waits until every waiter's wait returned, so nothing cancels a wait, and prints each waiter's
// result and the pattern it got. The cases, all with a timer interval of 50 ms:
// - manual reset, a one shot set event, and the timer's bit after
// - manual reset, a one shot pulse event, and the timer's bit after
// - auto reset, a set event that repeats, FIFO and with SCE_KERNEL_ATTR_THREAD_PRIO
// - auto reset, a one shot set event, where one waiter gets it and the other two time out
//
// On a PS Vita TV on firmware 3.65 a manual reset timer's set event and its pulse event both reach
// all three waiters, and the set leaves the bit set while the pulse clears it. A repeating auto
// reset event gives each waiter one event, FIFO and by priority. A one shot auto reset event
// reaches waiter 0 and the other two time out.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// A hundred times the timer's interval, so a waiter that gets an event never times out first
#define WAIT_TIMEOUT_US 5000000

#define WAITERS 3

static SceUID timer;
static volatile int results[WAITERS], patterns[WAITERS];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data;
    SceUInt timeout = WAIT_TIMEOUT_US;
    results[i] = sceKernelWaitEvent(timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    patterns[i] = pattern;
    return 0;
}

// Runs one case, and prints the timer's bit after every wait returned if print_bit is set
static void timer_case(const char *name, SceUInt32 attr, int type, int repeat, int print_bit) {
    static const int priorities[WAITERS] = { 0x90, 0x70, 0x80 };
    SceUID threads[WAITERS];
    timer = sceKernelCreateTimer("timer", attr, NULL);
    SceUInt64 interval = 50000;
    sceKernelSetTimerEvent(timer, type, &interval, repeat);
    for (int i = 0; i < WAITERS; i++) {
        results[i] = patterns[i] = 0xDEAD;
        threads[i] = sceKernelCreateThread(
            "waiter", waiter, priorities[i], 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        TEST_WAIT_FOR(test_in_wait(threads[i], WAIT_TYPE_EVENT, timer));
    }
    sceKernelStartTimer(timer);
    // Every wait has a timeout, so the joins end
    for (int i = 0; i < WAITERS; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < WAITERS; i++) {
        PRINTF(results[i], "%s: waiter %d's result", name, i);
        if (results[i] == 0)
            PRINTF(patterns[i], "%s: the pattern waiter %d got", name, i);
    }
    if (print_bit) {
        EventInfo info = { .size = sizeof(info) };
        sceKernelGetEventInfo(timer, &info);
        PRINTF(info.eventPattern & EVENT_TIMER, "%s: the timer's bit after", name);
    }
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    timer_case("manual reset, set event", EVENT_ATTR_MANUAL_RESET, TIMER_TYPE_SET_EVENT, 0, 1);
    timer_case("manual reset, pulse event", EVENT_ATTR_MANUAL_RESET, TIMER_TYPE_PULSE_EVENT, 0, 1);
    timer_case("auto reset, repeating set event, FIFO", EVENT_ATTR_AUTO_RESET,
        TIMER_TYPE_SET_EVENT, 1, 0);
    timer_case("auto reset, repeating set event, priority",
        EVENT_ATTR_AUTO_RESET | SCE_KERNEL_ATTR_THREAD_PRIO, TIMER_TYPE_SET_EVENT, 1, 0);
    timer_case("auto reset, one shot set event", EVENT_ATTR_AUTO_RESET, TIMER_TYPE_SET_EVENT, 0, 0);

    test_exit();
    return 0;
}

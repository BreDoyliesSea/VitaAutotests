// Which of several threads waiting on one timer its event wakes, for each reset attribute, event
// type and queue order. Waiters 0, 1 and 2, at 0x90, 0x70 and 0x80, wait for EVENT_TIMER on core
// 0 in that order, each once the one before waits, and the main thread, on core 1, starts the
// timer once all three wait, so its first event fires 50 ms later. The one shot cases:
// - manual reset, a set event
// - auto reset, a set event, FIFO and with SCE_KERNEL_ATTR_THREAD_PRIO
// - manual reset, a pulse event
// - auto reset, a pulse event
// Once a waiter's wait returned, the main thread reads the timer's bit and cancels the timer,
// which ends the waits left. Each prints the bit, the waiters the cancel counted, and each
// waiter's result and the pattern it got. Then repeating cases: an auto reset timer whose set
// event repeats every 50 ms, FIFO and with SCE_KERNEL_ATTR_THREAD_PRIO. The main thread waits until
// every waiter's wait returned, and the order they returned in prints as one hex digit per
// waiter, first first, so 0x012 is the order they queued in.
//
// On a PS Vita TV on firmware 3.65 a manual reset timer's set event wakes all three waiters and its
// bit stays set, and its pulse event wakes all three too and leaves the bit clear. An auto reset
// timer's set or pulse event wakes one waiter, the first in the queue, waiter 0 in FIFO order and
// waiter 1, at 0x70, by priority, and the cancel then ends the other two. A repeating auto reset
// event wakes one waiter per event, in queue order: 0x012 in FIFO order and 0x120 by priority.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define WAITERS 3

static SceUID timer;
static volatile int results[WAITERS], patterns[WAITERS], done_count;
static int order[WAITERS];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data;
    SceUInt timeout = 3 * TEST_WAIT_US;
    results[i] = sceKernelWaitEvent(timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    patterns[i] = pattern;
    order[__atomic_fetch_add(&done_count, 1, __ATOMIC_SEQ_CST)] = i;
    return 0;
}

static SceUID threads[WAITERS];

// Creates the timer with its event set, and starts the waiters, each once the one before waits
static void begin(SceUInt32 attr, int type, int repeat) {
    static const int priorities[WAITERS] = { 0x90, 0x70, 0x80 };
    done_count = 0;
    timer = sceKernelCreateTimer("timer", attr, NULL);
    SceUInt64 interval = 50000;
    sceKernelSetTimerEvent(timer, type, &interval, repeat);
    for (int i = 0; i < WAITERS; i++) {
        results[i] = patterns[i] = 0xDEAD;
        order[i] = 0xF;
        threads[i] = sceKernelCreateThread(
            "waiter", waiter, priorities[i], 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
        sceKernelStartThread(threads[i], sizeof(i), &i);
        TEST_WAIT_FOR(test_in_wait(threads[i], WAIT_TYPE_EVENT, timer));
    }
    sceKernelStartTimer(timer);
}

static void one_shot_case(const char *name, SceUInt32 attr, int type) {
    begin(attr, type, 0);
    TEST_WAIT_FOR(done_count != 0);
    EventInfo info = { .size = sizeof(info) };
    sceKernelGetEventInfo(timer, &info);
    SceUInt32 waiting = 0xDEAD;
    const int cancel = sceKernelCancelTimer(timer, &waiting);
    for (int i = 0; i < WAITERS; i++)
        test_join_thread(threads[i]);
    PRINTF(info.eventPattern & EVENT_TIMER, "%s: the timer's bit after a wait returned", name);
    PRINTF(cancel, "%s: the cancel", name);
    PRINTF(waiting, "%s: the waiters it counted", name);
    for (int i = 0; i < WAITERS; i++) {
        PRINTF(results[i], "%s: waiter %d's result", name, i);
        if (results[i] == 0)
            PRINTF(patterns[i], "%s: the pattern waiter %d got", name, i);
    }
    sceKernelDeleteTimer(timer);
}

static void repeating_case(const char *name, SceUInt32 attr) {
    begin(attr, TIMER_TYPE_SET_EVENT, 1);
    TEST_WAIT_FOR(done_count == WAITERS);
    sceKernelStopTimer(timer);
    // Ends any wait left, so the joins don't wait for its timeout
    sceKernelCancelTimer(timer, NULL);
    for (int i = 0; i < WAITERS; i++)
        test_join_thread(threads[i]);
    PRINTF(done_count, "%s: the waits that returned", name);
    PRINTF(order[0] << 8 | order[1] << 4 | order[2], "%s: the order they returned in", name);
    PRINTF(results[0] | results[1] | results[2], "%s: their results", name);
    sceKernelDeleteTimer(timer);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    one_shot_case("manual reset, set event", EVENT_ATTR_MANUAL_RESET, TIMER_TYPE_SET_EVENT);
    one_shot_case("auto reset, set event, FIFO", EVENT_ATTR_AUTO_RESET, TIMER_TYPE_SET_EVENT);
    one_shot_case("auto reset, set event, priority",
        EVENT_ATTR_AUTO_RESET | SCE_KERNEL_ATTR_THREAD_PRIO, TIMER_TYPE_SET_EVENT);
    one_shot_case("manual reset, pulse event", EVENT_ATTR_MANUAL_RESET, TIMER_TYPE_PULSE_EVENT);
    one_shot_case("auto reset, pulse event", EVENT_ATTR_AUTO_RESET, TIMER_TYPE_PULSE_EVENT);
    repeating_case("auto reset, repeating set event, FIFO", EVENT_ATTR_AUTO_RESET);
    repeating_case("auto reset, repeating set event, priority",
        EVENT_ATTR_AUTO_RESET | SCE_KERNEL_ATTR_THREAD_PRIO);

    test_exit();
    return 0;
}

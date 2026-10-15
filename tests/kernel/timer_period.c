// How evenly a repeating timer event fires, and what a period under the delay floor does. A
// timer with an auto reset event repeats with a period of 100, 1000 and 10000 microseconds. The
// main thread, on core 1 where nothing else runs, waits for 20 of its events in a row and times
// them with the process time. Each case prints whether the 20 took within 0.5 ms of 20 periods,
// which a timer on a fixed schedule does and one restarted from each event's wake doesn't, and
// whether every single wait took less than 2 periods plus 1 ms. For the period of 100 it also
// prints whether the 20 took at least 4 ms, which the delay floor would make them
// (delay_precision).
//
// On a PS Vita TV on firmware 3.65 the timer keeps a fixed schedule at 1000 and 10000, and at
// 100 the 20 events take at least 4 ms instead of 2, held back by the floor.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define EVENTS 20

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);
    static const SceUInt64 periods[3] = { 100, 1000, 10000 };
    for (int p = 0; p < 3; p++) {
        const SceUID timer = sceKernelCreateTimer("timer", EVENT_ATTR_AUTO_RESET, NULL);
        SceUInt64 interval = periods[p];
        sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 1);
        sceKernelStartTimer(timer);
        // The first event starts the count, so the 20 timed ones follow it
        SceUInt32 result;
        SceUInt64 user_data;
        SceUInt32 timeout = TEST_WAIT_US;
        sceKernelWaitEvent(timer, EVENT_TIMER, &result, &user_data, &timeout);
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        SceUInt64 last = start;
        int each_in_time = 1, waits_ok = 1;
        for (int i = 0; i < EVENTS; i++) {
            timeout = TEST_WAIT_US;
            waits_ok &= sceKernelWaitEvent(timer, EVENT_TIMER, &result, &user_data, &timeout) == 0;
            const SceUInt64 now = sceKernelGetProcessTimeWide();
            each_in_time &= now - last < 2 * periods[p] + 1000;
            last = now;
        }
        const SceUInt64 took = last - start;
        PRINTF(waits_ok, "a period of %u: every wait got its event", (unsigned)periods[p]);
        const SceUInt64 expected = EVENTS * periods[p];
        const SceUInt64 off = took > expected ? took - expected : expected - took;
        PRINTF(off < 500, "a period of %u: the %d events took within 0.5 ms of %d periods", (unsigned)periods[p], EVENTS, EVENTS);
        if (periods[p] < 200)
            PRINTF(took >= 4000, "a period of %u: they took at least 4 ms", (unsigned)periods[p]);
        PRINTF(each_in_time, "a period of %u: every wait took less than 2 periods and 1 ms", (unsigned)periods[p]);
        sceKernelStopTimer(timer);
        sceKernelDeleteTimer(timer);
    }
    test_exit();
    return 0;
}

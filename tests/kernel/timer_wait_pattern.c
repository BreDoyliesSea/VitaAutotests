// Which of a timer's bits its waits and polls get. A timer with a manual reset set event of 50 ms
// starts, and the main thread waits for the timer bit 0x8000 until the event fired, so the bit
// stays set. Then, each printing its result and the bits it got, which start as 0xDEAD:
// - a poll for the user bit 0x01000000
// - a wait for that bit with a timeout of 100 ms
// - a poll for 0x8000 and that bit
// - a wait for 0x8000 and that bit with a timeout of 100 ms
// - a poll for the create bit 0x4
// - a poll for 0x8000 | 0x4
//
// On a PS Vita TV on firmware 3.65 the waits and polls get only the bits asked for that the timer
// has. A poll or a wait for the user bit alone fails, with 0x80028202 or 0x80028005, and gets no
// bits. One that also asks for 0x8000 gets 0x8000, and the timer has the create bit 0x4 set too.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD

static SceUID timer;

static void poll_case(const char *name, SceUInt32 pattern) {
    SceUInt32 bits = NOT_DONE;
    SceUInt64 user_data = 0;
    PRINTF(sceKernelPollEvent(timer, pattern, &bits, &user_data), "%s: the poll", name);
    PRINTF(bits, "%s: the bits it got", name);
}

static void wait_case(const char *name, SceUInt32 pattern) {
    SceUInt32 bits = NOT_DONE;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = 100000;
    PRINTF(sceKernelWaitEvent(timer, pattern, &bits, &user_data, &timeout), "%s: the wait", name);
    PRINTF(bits, "%s: the bits it got", name);
}

int main(void) {
    timer = sceKernelCreateTimer("pattern", 0, NULL);
    SceUInt64 interval = 50000;
    sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0);
    sceKernelStartTimer(timer);
    SceUInt32 bits = 0;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = TEST_WAIT_US;
    PRINT("the event fired", sceKernelWaitEvent(timer, EVENT_TIMER, &bits, &user_data, &timeout));

    poll_case("a poll for a user bit", EVENT_BIT_A);
    wait_case("a wait for a user bit", EVENT_BIT_A);
    poll_case("a poll for the timer bit and a user bit", EVENT_TIMER | EVENT_BIT_A);
    wait_case("a wait for the timer bit and a user bit", EVENT_TIMER | EVENT_BIT_A);
    poll_case("a poll for the create bit", 0x4);
    poll_case("a poll for the timer bit and the create bit", EVENT_TIMER | 0x4);

    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
    test_exit();
    return 0;
}

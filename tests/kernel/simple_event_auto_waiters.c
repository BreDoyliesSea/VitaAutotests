// How one set or pulse of an auto reset simple event is shared out among waiters for different
// bits, which the auto reset clears. Every thread is pinned to core 0 and the waiters, at 0xB0,
// run below the main thread, at 0x60, so the threads a call wakes can't run before the main
// thread reads what it left. Waiters 0 and 1 wait in that order, each once the one before waits.
// The cases:
// - a pulse of A, both waiting for A
// - a set of A and B, waiter 0 waiting for A or B, waiter 1 for B
// - a set of A and B, waiter 0 waiting for A, waiter 1 for A or B
// - a set of A while B is set, waiter 0 waiting for A
// - a pulse of A and B, waiter 0 waiting for A, waiter 1 for B
// - two sets of A before any waiter, then two waiters for A
// Each case prints the bits left right after the call. Then the event is canceled, which ends the
// waits left, and the case prints the waiters the cancel counted, and each waiter's result, 0 for
// the ones the call woke, and the bits it got.
//
// On a PS Vita TV on firmware 3.65 an auto reset event's set or pulse goes to the waiters in queue
// order, and each takes the bits it waits for out of what's left. A pulse of A wakes one of two
// waiters for A, and a waiter for A or B takes both A and B, leaving nothing for a waiter for B.
// With waiter 0 for A first, it takes A and waiter 1, for A or B, takes B. A pulse of A and B wakes
// a waiter for each. A set of A leaves an earlier B set, and two sets of A before any waiter wake
// only one.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0

static SceUID event;
static volatile int results[2], patterns[2];
static volatile SceUInt32 wanted[2];

static int waiter(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data;
    SceUInt timeout = 3 * TEST_WAIT_US;
    results[i] = sceKernelWaitEvent(event, wanted[i], &pattern, &user_data, &timeout);
    patterns[i] = pattern;
    return 0;
}

static SceUID threads[2];

static void start_waiter(int i, SceUInt32 bits) {
    wanted[i] = bits;
    results[i] = patterns[i] = 0xDEAD;
    threads[i] = sceKernelCreateThread(
        "waiter", waiter, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    test_wait_until_waiting(threads[i]);
}

static SceUInt32 event_pattern(void) {
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    return pattern & (EVENT_BIT_A | EVENT_BIT_B);
}

// Reads what the call left, cancels the waits left, and prints it all
static void finish(const char *name, int call, int count) {
    const SceUInt32 left = event_pattern();
    SceUInt32 waiting = 0xDEAD;
    sceKernelCancelEvent(event, &waiting);
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
    PRINTF(call, "%s: the call", name);
    PRINTF(left, "%s: the bits left", name);
    PRINTF(waiting, "%s: the waiters the cancel counted", name);
    for (int i = 0; i < count; i++) {
        PRINTF(results[i], "%s: waiter %d's result", name, i);
        if (results[i] == 0)
            PRINTF(
                patterns[i] & (EVENT_BIT_A | EVENT_BIT_B), "%s: the bits waiter %d got", name, i);
    }
    sceKernelDeleteSimpleEvent(event);
}

static void two_waiter_case(const char *name, SceUInt32 bits0, SceUInt32 bits1, SceUInt32 preset,
    int pulse, SceUInt32 bits) {
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL);
    if (preset)
        sceKernelSetEvent(event, preset, 0);
    start_waiter(0, bits0);
    if (bits1)
        start_waiter(1, bits1);
    const int call =
        pulse ? sceKernelPulseEvent(event, bits, 0) : sceKernelSetEvent(event, bits, 0);
    finish(name, call, bits1 ? 2 : 1);
}

static void sets_before_case(void) {
    const char *name = "two sets of A before any waiter";
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_A, 0);
    const int call = sceKernelSetEvent(event, EVENT_BIT_A, 0);
    start_waiter(0, EVENT_BIT_A);
    start_waiter(1, EVENT_BIT_A);
    finish(name, call, 2);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    const SceUInt32 a = EVENT_BIT_A, b = EVENT_BIT_B;
    two_waiter_case("pulse of A, both waiting for A", a, a, 0, 1, a);
    two_waiter_case("set of A and B, waiter 0 for A or B, waiter 1 for B", a | b, b, 0, 0, a | b);
    two_waiter_case("set of A and B, waiter 0 for A, waiter 1 for A or B", a, a | b, 0, 0, a | b);
    two_waiter_case("set of A while B is set, a waiter for A", a, 0, b, 0, a);
    two_waiter_case("pulse of A and B, waiter 0 for A, waiter 1 for B", a, b, 0, 1, a | b);
    sets_before_case();

    test_exit();
    return 0;
}

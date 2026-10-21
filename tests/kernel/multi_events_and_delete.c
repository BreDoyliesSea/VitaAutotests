// What an AND sceKernelWaitMultipleEvents does when events it waits on are deleted, and what thread
// info reports while it waits. The events are auto reset simple events a, b and c, which wait for
// EVENT_BIT_A, EVENT_BIT_B and EVENT_BIT_C. A thread waits with a timeout far past the test, and
// the main thread then sets or deletes events one step at a time. After every step but the last,
// the test prints whether the thread is still in the wait. After the last step it joins the thread
// and prints the call's result, then each result entry as the event it is for (1 for a, 2 for b,
// 3 for c), its result, its bits and its user data, and whether the time left is over 1 s, which
// it isn't for a wait that timed out. Cases:
// - a is set, then b, the last event still pending, is deleted
// - b is deleted, which leaves a pending, then a is set
// - a is set before the call, so the call takes it, then b is deleted while the wait queues on it
// - a wait on a, b and c: the wait type and wait UID in thread info, before and after a is set,
//   then b and c are set. The wait UID is compared with 0x80000000 and with each event.
// - b is deleted, then a, so both deletes come while the wait is queued
//
// On a PS Vita TV on firmware 3.65 the AND wait ends with success once every event was set or
// deleted. Deleting the last pending event ends it, and the call returns the number of entries,
// with a deleted event's entry holding 0x80028006, bits 0x10 and user data 0. The entries come in
// the order the events finished. A set hands the event's bit to the wait at once, so it is gone
// from the event's pattern. Thread info reports the wait type 0x20000 and the wait UID 0x80000000,
// before and after a is set.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define EVENT_COUNT 3

static SceUID ev[EVENT_COUNT];
static int deleted[EVENT_COUNT];
static WaitEvent events[EVENT_COUNT];
static ResultEvent results[EVENT_COUNT];
static SceUInt32 wait_count;
static SceUInt32 wait_timeout;
static volatile int wait_result;

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    wait_timeout = TEST_WAIT_US;
    wait_result = sceKernelWaitMultipleEvents(events, wait_count, EVENT_WAIT_MODE_AND, results, &wait_timeout);
    return 0;
}

// Creates count auto reset events, event i waits for the bit EVENT_BIT_A << i, and marks the results
static void setup(int count) {
    for (int i = 0; i < EVENT_COUNT; i++) {
        ev[i] = i < count ? sceKernelCreateSimpleEvent("event", EVENT_ATTR_AUTO_RESET, 0, NULL) : 0;
        deleted[i] = 0;
        events[i] = (WaitEvent){ ev[i], EVENT_BIT_A << i };
    }
    sceClibMemset(results, 0xAA, sizeof(results));
    wait_count = count;
    wait_result = 0xDEAD;
    wait_timeout = 0;
}

static void teardown(void) {
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (ev[i] && !deleted[i])
            sceKernelDeleteSimpleEvent(ev[i]);
    }
}

static void delete_event(int i) {
    sceKernelDeleteSimpleEvent(ev[i]);
    deleted[i] = 1;
}

// Starts the waiting thread and returns once it is in its wait
static SceUID start_wait(void) {
    const SceUID thread = test_start_thread(waiter);
    test_wait_until_waiting(thread);
    return thread;
}

static SceUInt32 bits_of(SceUID event) {
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    return pattern;
}

// Returns whether thread is in a wait for multiple events
static int in_multiple_wait(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 && info.waitType == WAIT_TYPE_MP_EVENTS;
}

// Returns 1 for a, 2 for b and 3 for c, or 0 for another UID
static int which_event(SceUID id) {
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (ev[i] && id == ev[i])
            return i + 1;
    }
    return 0;
}

// Prints the call's result and the entries it filled
static void print_wait(const char *name) {
    PRINTF(wait_result, "%s: the wait", name);
    for (int i = 0; i < EVENT_COUNT && i < wait_result; i++) {
        PRINTF(which_event(results[i].eventId), "%s: result %d is for event 1, 2 or 3", name, i);
        PRINTF(results[i].result, "%s: result %d", name, i);
        PRINTF(results[i].resultPattern, "%s: result %d bits", name, i);
        PRINTF((int)results[i].userData, "%s: result %d user data", name, i);
    }
    PRINTF(wait_timeout > 1000000, "%s: the time left is over 1 s", name);
}

// Runs an AND wait on a and b. Each character of steps is one step, 's' sets a, 'd' deletes b and
// 'D' deletes a. With a_before, a is set before the call.
static void run_case(const char *name, int a_before, const char *steps) {
    setup(2);
    if (a_before)
        sceKernelSetEvent(ev[0], EVENT_BIT_A, 0x11);
    const SceUID thread = start_wait();
    if (a_before)
        PRINTF(bits_of(ev[0]), "%s: a's bits while waiting", name);
    for (const char *step = steps; *step; step++) {
        if (*step == 's') {
            PRINTF(sceKernelSetEvent(ev[0], EVENT_BIT_A, 0x11), "%s: set a", name);
            PRINTF(bits_of(ev[0]), "%s: a's bits after the set", name);
        } else if (*step == 'd') {
            delete_event(1);
        } else {
            delete_event(0);
        }
        if (step[1])
            PRINTF(in_multiple_wait(thread), "%s: still waiting after step %d", name, (int)(step - steps) + 1);
    }
    test_join_thread(thread);
    print_wait(name);
    teardown();
}

int main(void) {
    run_case("AND, set a then delete b", 0, "sd");
    run_case("AND, delete b then set a", 0, "ds");
    run_case("AND, a set before the call then delete b", 1, "d");

    // What thread info reports for the wait on three events
    setup(3);
    SceUID thread = start_wait();
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINT("three events: the wait type", info.waitType);
    PRINT("three events: the wait UID is 0x80000000", (SceUInt32)info.waitId == 0x80000000u);
    PRINT("three events: the wait UID is a", info.waitId == ev[0]);
    PRINT("three events: the wait UID is b", info.waitId == ev[1]);
    PRINT("three events: the wait UID is c", info.waitId == ev[2]);

    // Once a is handed over, the wait goes on
    sceKernelSetEvent(ev[0], EVENT_BIT_A, 0x11);
    info = (SceKernelThreadInfo){ .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINT("three events, a set: the wait type", info.waitType);
    PRINT("three events, a set: the wait UID is 0x80000000", (SceUInt32)info.waitId == 0x80000000u);
    PRINT("three events, a set: the wait UID is a", info.waitId == ev[0]);
    PRINT("three events, a set: the wait UID is b", info.waitId == ev[1]);
    PRINT("three events, a set: the wait UID is c", info.waitId == ev[2]);
    sceKernelSetEvent(ev[1], EVENT_BIT_B, 0x22);
    PRINT("three events, a and b set: still waiting", in_multiple_wait(thread));
    sceKernelSetEvent(ev[2], EVENT_BIT_C, 0x33);
    test_join_thread(thread);
    print_wait("three events");
    teardown();

    // The last case, since a wrong implementation may handle it worse than the others: both events deleted
    run_case("AND, delete b then delete a", 0, "dD");

    test_exit();
    return 0;
}

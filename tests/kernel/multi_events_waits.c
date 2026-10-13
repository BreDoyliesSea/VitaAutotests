// sceKernelWaitMultipleEvents while it waits and when it ends: what thread info reports, how many
// events an OR wait reports, whether waits consume auto reset bits, a wait competing with a single
// event wait, and events deleted or pulsed during the wait
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID a, b;
// Whether each event got its bits at create, which gives it user data that isn't the test's
static int a_set_at_create, b_set_at_create;
static WaitEvent events[2];
static ResultEvent results[2];
static volatile int wait_result, single_result, calls;
static volatile SceUInt32 wait_mode;

static int multi_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    wait_result = sceKernelWaitMultipleEvents(events, 2, wait_mode, results, &timeout);
    return 0;
}

static int single_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 pattern;
    SceUInt64 user_data;
    SceUInt32 timeout = 500000;
    single_result = sceKernelWaitEvent(a, EVENT_BIT_A, &pattern, &user_data, &timeout);
    return 0;
}

// Creates the two events and fills the results with a marker
static void setup(SceUInt32 attr, SceUInt32 a_bits, SceUInt32 b_bits, SceUInt32 mode) {
    a_set_at_create = a_bits != 0;
    b_set_at_create = b_bits != 0;
    a = sceKernelCreateSimpleEvent("a", attr, a_bits, NULL);
    b = sceKernelCreateSimpleEvent("b", attr, b_bits, NULL);
    events[0] = (WaitEvent){ a, EVENT_BIT_A };
    events[1] = (WaitEvent){ b, EVENT_BIT_A };
    sceClibMemset(results, 0xAA, sizeof(results));
    wait_mode = mode;
    wait_result = single_result = 0xDEAD;
}

static void teardown(void) {
    sceKernelDeleteSimpleEvent(a);
    sceKernelDeleteSimpleEvent(b);
}

// Prints the result entries the wait filled, and whether it left the others untouched
static void print_results(const char *name, int count) {
    for (int i = 0; i < 2; i++) {
        if (i >= count) {
            PRINTF(results[i].eventId == (SceUID)0xAAAAAAAA, "%s: result %d is untouched", name, i);
            continue;
        }
        PRINTF(results[i].eventId == a ? 1 : results[i].eventId == b ? 2 : 0, "%s: result %d is for event 1 or 2", name, i);
        PRINTF(results[i].result, "%s: result %d", name, i);
        PRINTF(results[i].resultPattern, "%s: result %d bits", name, i);
        const int set_at_create = results[i].eventId == a ? a_set_at_create : results[i].eventId == b ? b_set_at_create : 0;
        if (!set_at_create)
            PRINTF((int)results[i].userData, "%s: result %d user data", name, i);
    }
}

static SceUInt32 bits_of(SceUID event) {
    SceUInt32 pattern = 0xDEAD;
    sceKernelGetEventPattern(event, &pattern);
    return pattern;
}

int main(void) {
    SceUInt32 timeout;

    // OR with both events set before the call
    setup(0, EVENT_BIT_A, EVENT_BIT_A, EVENT_WAIT_MODE_OR);
    timeout = 1000;
    int count = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    PRINT("OR, both set before: the wait", count);
    print_results("OR, both set before", count);
    teardown();

    // What thread info reports for a thread in the wait
    setup(0, 0, 0, EVENT_WAIT_MODE_OR);
    SceUID thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    PRINT("waiting: the wait type", info.waitType);
    PRINT("waiting: the wait UID is the first event", info.waitId == a);
    PRINT("waiting: the wait UID is the second event", info.waitId == b);
    PRINT("waiting: the wait UID is 0", info.waitId == 0);
    PRINT("waiting: the wait UID is -1", info.waitId == -1);

    // A set of the second event ends it, with user data
    PRINT("OR, a set while waiting", sceKernelSetEvent(b, EVENT_BIT_A, 0x77));
    test_join_thread(thread);
    PRINT("OR, a set while waiting: the wait", wait_result);
    print_results("OR, a set while waiting", wait_result);
    teardown();

    // OR on auto reset events: whether the bit it reports is consumed
    setup(EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, 0, EVENT_WAIT_MODE_OR);
    timeout = 1000;
    count = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    PRINT("OR, auto reset: the wait", count);
    PRINT("OR, auto reset: the first event's bits after", bits_of(a));
    teardown();

    // AND on auto reset events, both set before
    setup(EVENT_ATTR_AUTO_RESET, EVENT_BIT_A, EVENT_BIT_A, EVENT_WAIT_MODE_AND);
    timeout = 1000;
    count = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_AND, results, &timeout);
    PRINT("AND, auto reset, both set: the wait", count);
    print_results("AND, auto reset, both set", count);
    PRINT("AND, auto reset, both set: the first event's bits after", bits_of(a));
    PRINT("AND, auto reset, both set: the second event's bits after", bits_of(b));
    teardown();

    // AND with one set before, the other set while waiting
    setup(0, EVENT_BIT_A, 0, EVENT_WAIT_MODE_AND);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    sceKernelSetEvent(b, EVENT_BIT_A, 0);
    test_join_thread(thread);
    PRINT("AND, one set while waiting: the wait", wait_result);
    print_results("AND, one set while waiting", wait_result);
    teardown();

    // AND on auto reset events while another thread waits for the first one alone. The first is
    // set, then the second. A wait that doesn't get its bits times out, so each one ends either way.
    setup(EVENT_ATTR_AUTO_RESET, 0, 0, EVENT_WAIT_MODE_AND);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    SceUID single = test_start_thread(single_waiter);
    test_wait_until_waiting(single);
    sceKernelSetEvent(a, EVENT_BIT_A, 0);
    test_join_thread(single);
    PRINT("AND against a single wait: the single wait", single_result);
    PRINT("AND against a single wait: the first event's bits after it", bits_of(a));
    sceKernelSetEvent(b, EVENT_BIT_A, 0);
    test_join_thread(thread);
    PRINT("AND against a single wait: the multiple wait after the second set", wait_result);
    PRINT("AND against a single wait: the first event's bits at the end", bits_of(a));
    PRINT("AND against a single wait: the second event's bits at the end", bits_of(b));
    teardown();

    // OR on an auto reset event while another thread waits for it alone, the single wait first. Then
    // the second event is set, which ends the multiple wait if the first set didn't.
    setup(EVENT_ATTR_AUTO_RESET, 0, 0, EVENT_WAIT_MODE_OR);
    single = test_start_thread(single_waiter);
    test_wait_until_waiting(single);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    sceKernelSetEvent(a, EVENT_BIT_A, 0);
    test_join_thread(single);
    PRINT("OR against a single wait: the single wait", single_result);
    sceKernelSetEvent(b, EVENT_BIT_A, 0);
    test_join_thread(thread);
    PRINT("OR against a single wait: the multiple wait", wait_result);
    print_results("OR against a single wait", wait_result);
    teardown();

    // One of the events deleted while waiting
    setup(0, 0, 0, EVENT_WAIT_MODE_OR);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    sceKernelDeleteSimpleEvent(a);
    test_join_thread(thread);
    PRINT("OR, an event deleted while waiting: the wait", wait_result);
    print_results("OR, an event deleted while waiting", wait_result);
    sceKernelDeleteSimpleEvent(b);

    setup(0, 0, 0, EVENT_WAIT_MODE_AND);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    sceKernelDeleteSimpleEvent(a);
    test_join_thread(thread);
    PRINT("AND, an event deleted while waiting: the wait", wait_result);
    print_results("AND, an event deleted while waiting", wait_result);
    sceKernelDeleteSimpleEvent(b);

    // A pulse while waiting
    setup(0, 0, 0, EVENT_WAIT_MODE_OR);
    thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    sceKernelPulseEvent(a, EVENT_BIT_A, 0x88);
    test_join_thread(thread);
    PRINT("OR, a pulse while waiting: the wait", wait_result);
    print_results("OR, a pulse while waiting", wait_result);
    PRINT("OR, a pulse while waiting: the bits after", bits_of(a));
    teardown();

    // A timer among the events
    const SceUID timer = sceKernelCreateTimer("timer", 0, NULL);
    a = sceKernelCreateSimpleEvent("a", 0, 0, NULL);
    b = timer;
    a_set_at_create = b_set_at_create = 0;
    events[0] = (WaitEvent){ a, EVENT_BIT_A };
    events[1] = (WaitEvent){ timer, TIMER_EVENT_BIT };
    sceClibMemset(results, 0xAA, sizeof(results));
    SceUInt64 interval = 20000;
    sceKernelSetTimerEvent(timer, 0, &interval, 0);
    sceKernelStartTimer(timer);
    timeout = 500000;
    count = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    PRINT("OR with a timer: the wait", count);
    print_results("OR with a timer", count);
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
    sceKernelDeleteSimpleEvent(a);

    test_exit();
    return 0;
}

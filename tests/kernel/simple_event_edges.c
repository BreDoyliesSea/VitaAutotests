// Simple event cases the other simple event tests leave out. Every out value starts as a marker, so
// a value a call wrote shows apart from one it left alone. The cases:
// - a wait for pattern 0 on an event with every user bit set, with a timeout of 600 ms and with a
//   timeout of 0, and a poll for pattern 0 on the same event. Each prints its result and what it
//   wrote to the out values, and the 600 ms wait also whether it took at least 300 ms
// - a thread waiting for pattern 0 while every user bit is set and then pulsed. It prints whether
//   the thread still waits after each, then a cancel ends the wait and it prints the count the
//   cancel wrote and what the wait got
// - the same wait for pattern 0, ended by deleting the event
// - the user data a poll that fails and a wait that times out write, on an event that holds other
//   user data
// - a delete with a waiter for the delete bit and one for a user bit. It prints the result, the
//   result pattern and the user data of each. For the delete bit waiter it prints whether the
//   user data is not 0, whether its high word is the thread that deleted the event, and whether
//   its low word is the process ID
// - the user data a poll gets after a set with user data, after a pulse of another bit with other
//   user data and no waiter, after a set of pattern 0 with other user data and after a pulse of
//   pattern 0 with other user data
// - creating an event with attribute 0x800, and 0x900, which prints the result, the attribute the
//   info call reports, and whether a poll of a bit set on it keeps the bit
//
// On a PS Vita TV on firmware 3.65 a wait for pattern 0 blocks until its timeout, a cancel or a
// delete, and a set or pulse of every user bit doesn't end it. A poll for pattern 0 fails with
// 0x80020005. Every wait or poll that fails writes a pattern of 0 and user data 0, except that a
// cancel or a delete writes the pattern 0x10. The waiter for the delete bit gets 0 with the pattern
// 0x8, and the user data holds the deleting thread's UID in its high word and the process ID in its
// low word. A pulse with no waiter still replaces the user data, and a set or pulse of pattern 0
// leaves it. Attribute 0x800 is accepted and reported, and 0x900 is an auto reset event too.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define OUT_MARKER 0xDEADBEEF
#define USER_DATA_MARKER 0xCAFEF00DBAADF00Dull
// Every bit a game may set on a simple event
#define EVENT_ALL_BITS 0xFF000000

static SceUID event;
static SceUInt32 wanted[2];
static volatile int results[2];
static SceUInt32 patterns[2];
static SceUInt64 user_datas[2];

// Waits for wanted[i] into slot i of the results
static int wait_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    results[i] = sceKernelWaitEvent(event, wanted[i], &patterns[i], &user_datas[i], &timeout);
    return 0;
}

// Starts a thread waiting for pattern in slot i, with its out values set to markers
static SceUID start_waiter(int i, SceUInt32 pattern) {
    wanted[i] = pattern;
    results[i] = (int)OUT_MARKER;
    patterns[i] = OUT_MARKER;
    user_datas[i] = USER_DATA_MARKER;
    const SceUID thread = test_start_thread_arg(wait_thread, i);
    test_wait_until_waiting(thread);
    return thread;
}

static void print_user_data(const char *what, SceUInt64 user_data) {
    PRINTF((SceUInt32)user_data, "%s: the user data, low word", what);
    PRINTF((SceUInt32)(user_data >> 32), "%s: the user data, high word", what);
}

static void print_out(const char *what, SceUInt32 pattern, SceUInt64 user_data) {
    PRINTF(pattern, "%s: the result pattern", what);
    print_user_data(what, user_data);
}

// Joins the waiter in slot i and prints what its wait returned and wrote
static void join_and_print(const char *what, SceUID thread, int i) {
    test_join_thread(thread);
    PRINTF(results[i], "%s: the wait", what);
    print_out(what, patterns[i], user_datas[i]);
}

// Polls for bit and returns the user data it got
static SceUInt64 poll_user_data(SceUInt32 bit) {
    SceUInt32 result = OUT_MARKER;
    SceUInt64 user_data = USER_DATA_MARKER;
    sceKernelPollEvent(event, bit, &result, &user_data);
    return user_data;
}

static void wait_for_zero_cases(void) {
    SceUInt32 result = OUT_MARKER;
    SceUInt64 user_data = USER_DATA_MARKER;
    SceUInt32 timeout = 600000;

    // An event with every user bit set, so a wait that took any set bit would end at once
    event = sceKernelCreateSimpleEvent("zero", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_ALL_BITS, 0x1111);
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    const int waited = sceKernelWaitEvent(event, 0, &result, &user_data, &timeout);
    const SceUInt64 elapsed = sceKernelGetProcessTimeWide() - start;
    PRINT("wait for pattern 0 with a timeout", waited);
    PRINT("wait for pattern 0 with a timeout: it took at least 300 ms", elapsed >= 300000);
    print_out("wait for pattern 0 with a timeout", result, user_data);

    result = OUT_MARKER;
    user_data = USER_DATA_MARKER;
    timeout = 0;
    PRINT("wait for pattern 0 with a timeout of 0", sceKernelWaitEvent(event, 0, &result, &user_data, &timeout));
    print_out("wait for pattern 0 with a timeout of 0", result, user_data);

    result = OUT_MARKER;
    user_data = USER_DATA_MARKER;
    PRINT("poll for pattern 0", sceKernelPollEvent(event, 0, &result, &user_data));
    print_out("poll for pattern 0", result, user_data);
    sceKernelDeleteSimpleEvent(event);

    // A thread that waits for pattern 0 keeps waiting after a set and a pulse of every user bit
    event = sceKernelCreateSimpleEvent("zero", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    SceUID thread = start_waiter(0, 0);
    PRINT("wait for pattern 0 and a cancel: the thread waits for the event", test_in_wait(thread, WAIT_TYPE_EVENT, event));
    PRINT("wait for pattern 0 and a cancel: set of every user bit", sceKernelSetEvent(event, EVENT_ALL_BITS, 0x2222));
    PRINT("wait for pattern 0 and a cancel: the thread still waits after the set", test_in_wait(thread, WAIT_TYPE_EVENT, event));
    PRINT("wait for pattern 0 and a cancel: pulse of every user bit", sceKernelPulseEvent(event, EVENT_ALL_BITS, 0x3333));
    PRINT("wait for pattern 0 and a cancel: the thread still waits after the pulse", test_in_wait(thread, WAIT_TYPE_EVENT, event));
    SceUInt32 waiting = OUT_MARKER;
    PRINT("wait for pattern 0 and a cancel: the cancel", sceKernelCancelEvent(event, &waiting));
    PRINT("wait for pattern 0 and a cancel: the count the cancel wrote", waiting);
    join_and_print("wait for pattern 0 and a cancel", thread, 0);
    sceKernelDeleteSimpleEvent(event);

    // A delete ends it too
    event = sceKernelCreateSimpleEvent("zero", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_B, 0x4444);
    thread = start_waiter(0, 0);
    PRINT("wait for pattern 0 and a delete: the thread waits for the event", test_in_wait(thread, WAIT_TYPE_EVENT, event));
    PRINT("wait for pattern 0 and a delete: the delete", sceKernelDeleteSimpleEvent(event));
    join_and_print("wait for pattern 0 and a delete", thread, 0);
}

static void failed_call_cases(void) {
    SceUInt32 result = OUT_MARKER;
    SceUInt64 user_data = USER_DATA_MARKER;
    SceUInt32 timeout = 1000;

    // The event holds user data that the calls could write if they wrote the event's
    event = sceKernelCreateSimpleEvent("fails", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_B, 0x5555);
    PRINT("a poll that fails", sceKernelPollEvent(event, EVENT_BIT_A, &result, &user_data));
    print_user_data("a poll that fails", user_data);

    result = OUT_MARKER;
    user_data = USER_DATA_MARKER;
    PRINT("a wait that times out", sceKernelWaitEvent(event, EVENT_BIT_A, &result, &user_data, &timeout));
    print_user_data("a wait that times out", user_data);
    sceKernelDeleteSimpleEvent(event);
}

static void delete_cases(void) {
    event = sceKernelCreateSimpleEvent("deleted", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_B, 0x6666);
    const SceUID user_bit_waiter = start_waiter(0, EVENT_BIT_A);
    const SceUID delete_waiter = start_waiter(1, EVENT_DELETE);
    PRINT("delete: the delete", sceKernelDeleteSimpleEvent(event));

    test_join_thread(user_bit_waiter);
    PRINT("delete: the wait for a user bit", results[0]);
    print_out("delete: the wait for a user bit", patterns[0], user_datas[0]);

    test_join_thread(delete_waiter);
    PRINT("delete: the wait for the delete bit", results[1]);
    PRINT("delete: the wait for the delete bit: the result pattern", patterns[1]);
    PRINT("delete: the wait for the delete bit: the user data is not 0",
        user_datas[1] != 0 && user_datas[1] != USER_DATA_MARKER);
    PRINT("delete: the wait for the delete bit: the high word is the thread that deleted",
        (SceUInt32)(user_datas[1] >> 32) == (SceUInt32)sceKernelGetThreadId());
    PRINT("delete: the wait for the delete bit: the low word is the process ID",
        (SceUInt32)user_datas[1] == (SceUInt32)sceKernelGetProcessId());
}

static void user_data_cases(void) {
    event = sceKernelCreateSimpleEvent("data", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    PRINT("user data: set of bit A", sceKernelSetEvent(event, EVENT_BIT_A, 0x1111));
    print_user_data("user data: a poll after the set", poll_user_data(EVENT_BIT_A));
    PRINT("user data: pulse of bit B with no waiter", sceKernelPulseEvent(event, EVENT_BIT_B, 0x2222));
    print_user_data("user data: a poll of bit A after the pulse", poll_user_data(EVENT_BIT_A));
    PRINT("user data: set of pattern 0", sceKernelSetEvent(event, 0, 0x3333));
    print_user_data("user data: a poll after the set of pattern 0", poll_user_data(EVENT_BIT_A));
    PRINT("user data: pulse of pattern 0", sceKernelPulseEvent(event, 0, 0x4444));
    print_user_data("user data: a poll after the pulse of pattern 0", poll_user_data(EVENT_BIT_A));
    sceKernelDeleteSimpleEvent(event);
}

// Creates an event with attr, and prints the result, the attribute it reports and whether a poll
// of a bit set on it keeps the bit
static void create_case(const char *what, SceUInt32 attr) {
    const SceUID id = sceKernelCreateSimpleEvent("attr", attr, 0, NULL);
    PRINTF(test_created(id), "%s: the create", what);
    if (id < 0)
        return;
    EventInfo info = { .size = sizeof(info) };
    sceKernelGetEventInfo(id, &info);
    PRINTF(info.attr, "%s: the attribute it reports", what);
    sceKernelSetEvent(id, EVENT_BIT_A, 0);
    SceUInt32 result = OUT_MARKER;
    SceUInt64 user_data = USER_DATA_MARKER;
    PRINTF(sceKernelPollEvent(id, EVENT_BIT_A, &result, &user_data), "%s: the first poll", what);
    PRINTF(sceKernelPollEvent(id, EVENT_BIT_A, &result, &user_data), "%s: the second poll", what);
    PRINTF(sceKernelDeleteSimpleEvent(id), "%s: the delete", what);
}

int main(void) {
    wait_for_zero_cases();
    failed_call_cases();
    delete_cases();
    user_data_cases();
    create_case("create with attribute 0x800", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY);
    create_case("create with attribute 0x900", EVENT_ATTR_NOTIFY_CB_WAKEUP_ONLY | EVENT_ATTR_AUTO_RESET);

    test_exit();
    return 0;
}

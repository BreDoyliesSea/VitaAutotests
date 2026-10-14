// What sceKernelCancelEvent and sceKernelCancelEventWithSetPattern do on a simple event: what its 2
// waiters get back, the waiting thread count each writes, the pattern and user data after them, and
// patterns a game may not set. Kept short and on its own, so a hang in it loses nothing else.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static SceUID event;
static volatile int results[2];
static SceUInt32 patterns[2];
static SceUInt64 user_datas[2];

// Waits for EVENT_BIT_A into slot i
static int wait_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    results[i] = sceKernelWaitEvent(event, EVENT_BIT_A, &patterns[i], &user_datas[i], &timeout);
    return 0;
}

static void start_waiters(SceUID *threads, int count) {
    for (int i = 0; i < count; i++) {
        results[i] = -1;
        patterns[i] = 0xDEAD;
        user_datas[i] = 0xDEAD;
        threads[i] = test_start_thread_arg(wait_thread, i);
        test_wait_until_waiting(threads[i]);
    }
}

static void join_and_print(SceUID *threads, int count, const char *what) {
    for (int i = 0; i < count; i++) {
        test_join_thread(threads[i]);
        PRINTF(results[i], "%s: waiter %d", what, i);
        PRINTF(patterns[i], "%s: the pattern waiter %d got", what, i);
        PRINTF(user_datas[i] == 0xDEAD ? 0xDEAD : (SceUInt32)user_datas[i], "%s: the user data waiter %d got", what, i);
    }
}

static void print_state(const char *what) {
    EventInfo info = { .size = sizeof(info) };
    sceKernelGetEventInfo(event, &info);
    PRINTF(info.eventPattern, "%s: the pattern after", what);
    PRINTF((SceUInt32)info.userData, "%s: the user data after", what);
}

int main(void) {
    SceUID threads[2];
    SceUInt32 waiting;
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    sceKernelSetEvent(event, EVENT_BIT_B, 0x77);

    start_waiters(threads, 2);
    waiting = 0xDEAD;
    PRINT("cancel with 2 waiters", sceKernelCancelEvent(event, &waiting));
    PRINT("the waiting count", waiting);
    join_and_print(threads, 2, "cancel");
    print_state("cancel");

    start_waiters(threads, 2);
    waiting = 0xDEAD;
    PRINT("cancel and set with 2 waiters", sceKernelCancelEventWithSetPattern(event, EVENT_BIT_C, 0x99, &waiting));
    PRINT("the waiting count", waiting);
    join_and_print(threads, 2, "cancel and set");
    print_state("cancel and set");

    // A set pattern with the bit the waiters wait for
    start_waiters(threads, 1);
    waiting = 0xDEAD;
    PRINT("cancel and set the waited bit", sceKernelCancelEventWithSetPattern(event, EVENT_BIT_A, 0x55, &waiting));
    PRINT("the waiting count", waiting);
    join_and_print(threads, 1, "cancel and set the waited bit");
    print_state("cancel and set the waited bit");

    // Patterns and pointers it may reject, with no waiters
    waiting = 0xDEAD;
    PRINT("cancel and set with no waiters", sceKernelCancelEventWithSetPattern(event, 0, 0, &waiting));
    PRINT("the waiting count", waiting);
    waiting = 0xDEAD;
    PRINT("cancel and set a system bit", sceKernelCancelEventWithSetPattern(event, 0x1, 0, &waiting));
    PRINT("the waiting count", waiting);
    print_state("cancel and set a system bit");
    PRINT("cancel and set without a count pointer", sceKernelCancelEventWithSetPattern(event, EVENT_BIT_D, 0, NULL));

    // No count pointer, with a waiter
    sceKernelClearEvent(event, 0);
    start_waiters(threads, 1);
    PRINT("cancel without a count pointer with a waiter", sceKernelCancelEvent(event, NULL));
    join_and_print(threads, 1, "cancel without a count pointer");

    sceKernelDeleteSimpleEvent(event);
    test_exit();
    return 0;
}

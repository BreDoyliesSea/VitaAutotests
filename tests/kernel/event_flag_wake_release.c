// What an event flag without SCE_EVENT_WAITMULTIPLE looks like right after something ends the wait
// of its one waiting thread A, before A runs again. The flag starts with bit 0x2 set. A waits for
// bit 0x1 with a long timeout and an out pointer to a global that starts with a marker. Every
// thread is pinned to core 0. The main thread runs at 0x60 and A at 0xB0, so A, once woken, stays
// ready until the main thread blocks. Right after the call that ends A's wait, and before any call
// that can block, the main thread reads four things. A poll of the flag for any of the bits 0x1 to
// 0x8, which succeeds with 0 if nothing holds the flag and fails with 0x800280E2 if A still does.
// The flag's info, its number of waiting threads and its bits. A's out global. And A's thread
// status, where 2 is ready. After that it clears the flag with sceKernelClearEventFlag(flag, 0),
// still before it blocks, so a value A got from the wake is told apart from one written later when
// A runs. Then it joins A and prints A's wait result and out global. Every case has a fresh flag
// and a fresh A:
// - a set of bit 0x1 ends A's wait
// - a set of bits 0x1 and 0x4 ends A's wait
// - sceKernelCancelEventFlag with the set pattern 0x8 ends A's wait
// - sceKernelNotifyCallback of a callback A created, with A in sceKernelWaitEventFlagCB. The main
//   thread reads before it blocks, then once A ran its callback and waits again, and then a set of
//   bit 0x1 ends A's wait. The flag is not cleared in this case.
// - a delete of the flag, last, with only A's status read before the main thread blocks
// Each case first prints whether A waits, and the result of the call that ends the wait.
//
// On a PS Vita TV on firmware 3.65 a set or a cancel frees the flag before A runs. The poll right
// after it succeeds, the flag counts no waiter, and A is ready (0x2). A's out bits still hold the
// marker then, and once A returns they hold the bits from the wake, not the 0 of the later clear. A
// notified callback also takes A out of the count, but A keeps the flag, so the poll fails with
// 0x800280E2 before A runs and again once A waits again. A delete ends A's wait with 0x80028006 and
// the bits from before it.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define MARKER 0xBEEF
#define LONG_TIMEOUT (3 * TEST_WAIT_US)

typedef enum Wake { SET, CANCEL } Wake;

// What the main thread reads while A is ready or waiting
typedef struct Snapshot {
    int poll;
    SceUInt32 waiters, bits, out_bits;
    int status;
} Snapshot;

static SceUID flag, callback;
static volatile SceUInt32 a_bits;
static volatile int a_result, callback_ran;

static int notified_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_ran = 1;
    return 0;
}

// Waits for bit 0x1, with callbacks if the argument is 1
static int a_entry(SceSize args, void *argp) {
    (void)args;
    const int with_callbacks = *(const int *)argp;
    SceUInt timeout = LONG_TIMEOUT;
    if (with_callbacks) {
        callback = sceKernelCreateCallback("notified", 0, notified_callback, NULL);
        a_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITOR, (SceUInt32 *)&a_bits, &timeout);
        sceKernelDeleteCallback(callback);
    } else {
        a_result = sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, (SceUInt32 *)&a_bits, &timeout);
    }
    return 0;
}

// Creates the flag and A, which is pinned to core 0, and waits until A waits on the flag
static SceUID begin(int with_callbacks) {
    flag = sceKernelCreateEventFlag("flag", 0, 0x2, NULL);
    a_bits = MARKER;
    a_result = NOT_DONE;
    callback_ran = 0;
    const SceUID thread =
        sceKernelCreateThread("a", a_entry, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(with_callbacks), &with_callbacks);
    TEST_WAIT_FOR(test_in_wait(
        thread, with_callbacks ? WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB : WAIT_TYPE_EVENT_FLAG, flag));
    return thread;
}

// Returns the status of thread, or NOT_DONE if the call fails
static int thread_status(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    return sceKernelGetThreadInfo(thread, &info) == 0 ? (int)info.status : NOT_DONE;
}

// Reads the poll, the flag's info, A's out global and A's status, in that order
static Snapshot take_snapshot(SceUID a) {
    Snapshot s = { .poll = NOT_DONE, .waiters = NOT_DONE, .bits = NOT_DONE };
    SceUInt32 poll_bits = 0;
    s.poll = sceKernelPollEventFlag(flag, 0xF, SCE_EVENT_WAITOR, &poll_bits);
    SceKernelEventFlagInfo info = { .size = sizeof(info) };
    if (sceKernelGetEventFlagInfo(flag, &info) == 0) {
        s.waiters = info.numWaitThreads;
        s.bits = info.currentPattern;
    }
    s.out_bits = a_bits;
    s.status = thread_status(a);
    return s;
}

// Prints what take_snapshot read, for the moment named when
static void print_snapshot(const char *name, const char *when, const Snapshot *s) {
    PRINTF(s->poll, "%s: the poll %s", name, when);
    PRINTF(s->waiters, "%s: the flag's waiting threads %s", name, when);
    PRINTF(s->bits, "%s: the flag's bits %s", name, when);
    PRINTF(s->out_bits, "%s: A's out bits %s", name, when);
    PRINTF(s->status, "%s: A's status %s", name, when);
}

// A set or a cancel ends A's wait
static void wake_case(const char *name, Wake how, SceUInt32 pattern) {
    const SceUID a = begin(0);
    const int waits = test_in_wait(a, WAIT_TYPE_EVENT_FLAG, flag);
    SceUInt32 canceled = NOT_DONE;
    const int step = how == SET ? sceKernelSetEventFlag(flag, pattern) : sceKernelCancelEventFlag(flag, pattern, &canceled);
    const Snapshot s = take_snapshot(a);
    sceKernelClearEventFlag(flag, 0);
    test_join_thread(a);

    PRINTF(waits, "%s: A waits", name);
    PRINTF(step, "%s: the %s", name, how == SET ? "set" : "cancel");
    if (how == CANCEL)
        PRINTF(canceled, "%s: the waiting threads the cancel counts", name);
    print_snapshot(name, "before the main thread blocks", &s);
    PRINTF(a_result, "%s: A's result", name);
    PRINTF(a_bits, "%s: A's out bits after it ran", name);
    sceKernelDeleteEventFlag(flag);
}

// A notified callback takes A out of its wait with callbacks, then A waits again
static void notify_case(void) {
    const char *name = "a notified callback";
    const SceUID a = begin(1);
    const int waits = test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag);
    const int step = sceKernelNotifyCallback(callback, 1);
    const Snapshot before = take_snapshot(a);
    TEST_WAIT_FOR(callback_ran && test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag));
    const int ran = callback_ran;
    const Snapshot after = take_snapshot(a);
    const int set = sceKernelSetEventFlag(flag, 0x1);
    test_join_thread(a);

    PRINTF(waits, "%s: A waits", name);
    PRINTF(step, "%s: the notify", name);
    print_snapshot(name, "before the main thread blocks", &before);
    PRINTF(ran, "%s: A's callback ran", name);
    print_snapshot(name, "when A waits again", &after);
    PRINTF(set, "%s: the set", name);
    PRINTF(a_result, "%s: A's result", name);
    PRINTF(a_bits, "%s: A's out bits after it ran", name);
    sceKernelDeleteEventFlag(flag);
}

// The delete of the flag ends A's wait
static void delete_case(void) {
    const char *name = "a delete of the flag";
    const SceUID a = begin(0);
    const int waits = test_in_wait(a, WAIT_TYPE_EVENT_FLAG, flag);
    const int step = sceKernelDeleteEventFlag(flag);
    const int status = thread_status(a);
    test_join_thread(a);

    PRINTF(waits, "%s: A waits", name);
    PRINTF(step, "%s: the delete", name);
    PRINTF(status, "%s: A's status before the main thread blocks", name);
    PRINTF(a_result, "%s: A's result", name);
    PRINTF(a_bits, "%s: A's out bits after it ran", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    wake_case("a set of 0x1", SET, 0x1);
    wake_case("a set of 0x1 and 0x4", SET, 0x1 | 0x4);
    wake_case("a cancel with 0x8", CANCEL, 0x8);
    notify_case();
    delete_case();

    test_exit();
    return 0;
}

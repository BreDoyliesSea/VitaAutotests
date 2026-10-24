// What a timer's event does when the first of its waiting threads stops waiting before the event,
// or when the event moves earlier while a thread waits. Waiters run on core 0 and the main thread
// on core 1. Each case uses a fresh auto reset timer with a FIFO queue and a one shot set event,
// and the waits are for EVENT_TIMER. Waiter A waits first and waiter B second, and B's wait has a
// timeout of 3 s. The cases:
// - A's wait times out: A's timeout is 200 ms, the main thread starts the timer once both wait,
//   and the event comes 600 ms after the start.
// - A leaves for a callback: A waits with callbacks, with a timeout of 2 s, and the main thread
//   starts the timer once both wait, then notifies A's callback, which blocks on a gate semaphore
//   until B's wait returned. The event comes 300 ms after the start.
// Both print A's and B's results, and whether B returned within 1500 ms of the start, long before
// its own timeout.
// - the event moves earlier: A alone waits, with a timeout of 4 s, on a timer that the main thread
//   starts with an event 3 s away, then sets again with an interval of 200 ms. It prints A's result
//   and whether A returned within 1500 ms of the set.
//
// On a PS Vita TV on firmware 3.65 B gets the event when it comes, both after A's wait timed out
// and while A's callback runs, and A's wait times out. A lone waiter gets an event that was moved
// earlier at its new time.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define B_TIMEOUT_US 3000000
#define SOON_US 1500000
// Longer than anything the callback waits for, so a wrong outcome can't hang the test
#define GATE_TIMEOUT_US 5000000

typedef struct Waiter {
    SceUInt32 timeout_us;
    int callbacks;
    volatile int result;
    volatile SceUInt64 end;
} Waiter;

static SceUID timer, gate;
static Waiter a, b;
static volatile SceUID a_callback;

// Blocks on the gate until the main thread signals it
static int gate_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = GATE_TIMEOUT_US;
    sceKernelWaitSema(gate, 1, &timeout);
    return 0;
}

// Waits for the timer's bit as the Waiter its argument points to says, and records the result
// and the time it returned
static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    Waiter *const w = *(Waiter **)argp;
    if (w->callbacks)
        a_callback = sceKernelCreateCallback("gate", 0, gate_callback, NULL);
    SceUInt32 pattern = 0;
    SceUInt64 user_data = 0;
    SceUInt32 timeout = w->timeout_us;
    const int result = w->callbacks ? sceKernelWaitEventCB(timer, EVENT_TIMER, &pattern, &user_data, &timeout)
                                    : sceKernelWaitEvent(timer, EVENT_TIMER, &pattern, &user_data, &timeout);
    w->end = sceKernelGetProcessTimeWide();
    w->result = result;
    if (w->callbacks)
        sceKernelDeleteCallback(a_callback);
    return 0;
}

// Starts a waiter on core 0 and returns once it waits on the timer
static SceUID start_waiter(Waiter *w, SceUInt32 timeout_us, int callbacks) {
    w->timeout_us = timeout_us;
    w->callbacks = callbacks;
    w->result = NOT_DONE;
    w->end = 0;
    const SceUID thread = sceKernelCreateThread("waiter", waiter_entry, 0x80, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(thread, sizeof(w), &w);
    const SceUInt32 type = callbacks ? WAIT_TYPE_EVENT | WAIT_TYPE_CB : WAIT_TYPE_EVENT;
    TEST_WAIT_FOR(test_in_wait(thread, type, timer));
    return thread;
}

static void create_timer(SceUInt64 interval) {
    timer = sceKernelCreateTimer("timer", EVENT_ATTR_AUTO_RESET, NULL);
    sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0);
}

static void delete_timer(void) {
    sceKernelStopTimer(timer);
    sceKernelDeleteTimer(timer);
}

static void print_both(const char *name, SceUInt64 start) {
    PRINTF(a.result, "%s: A's wait", name);
    PRINTF(b.result, "%s: B's wait", name);
    PRINTF(b.result == 0 && b.end - start < SOON_US, "%s: B returned within 1500 ms of the start", name);
}

static void timeout_case(void) {
    create_timer(600000);
    const SceUID ta = start_waiter(&a, 200000, 0);
    const SceUID tb = start_waiter(&b, B_TIMEOUT_US, 0);
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    sceKernelStartTimer(timer);
    test_join_thread(ta);
    test_join_thread(tb);
    print_both("A's wait times out", start);
    delete_timer();
}

static void callback_case(void) {
    create_timer(300000);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    a_callback = 0;
    const SceUID ta = start_waiter(&a, 2000000, 1);
    const SceUID tb = start_waiter(&b, B_TIMEOUT_US, 0);
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    sceKernelStartTimer(timer);
    sceKernelNotifyCallback(a_callback, 1);
    TEST_WAIT_FOR(test_in_wait(ta, WAIT_TYPE_SEMA, gate));
    const int in_gate = test_in_wait(ta, WAIT_TYPE_SEMA, gate);
    TEST_WAIT_FOR(b.result != NOT_DONE);
    sceKernelSignalSema(gate, 1);
    test_join_thread(ta);
    test_join_thread(tb);
    PRINT("A leaves for a callback: A's callback waits on the gate", in_gate);
    print_both("A leaves for a callback", start);
    sceKernelDeleteSema(gate);
    delete_timer();
}

static void earlier_case(void) {
    create_timer(3000000);
    const SceUID ta = start_waiter(&a, 4000000, 0);
    sceKernelStartTimer(timer);
    const SceUInt64 set = sceKernelGetProcessTimeWide();
    SceUInt64 interval = 200000;
    const int set_result = sceKernelSetTimerEvent(timer, TIMER_TYPE_SET_EVENT, &interval, 0);
    test_join_thread(ta);
    PRINT("the event moves earlier: the set", set_result);
    PRINT("the event moves earlier: A's wait", a.result);
    PRINT("the event moves earlier: A returned within 1500 ms of the set", a.result == 0 && a.end - set < SOON_US);
    delete_timer();
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    // The main thread only moves to core 1 when it is next scheduled
    sceKernelDelayThread(1000);

    timeout_case();
    callback_case();
    earlier_case();

    test_exit();
    return 0;
}

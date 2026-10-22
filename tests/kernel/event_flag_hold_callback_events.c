// What other events do to the single waiter hold of an event flag without SCE_EVENT_WAITMULTIPLE,
// and to its waiting thread A, while A is out of the flag's queue running the callback of a wait
// with callbacks. While A's wait holds the flag, another thread's wait or poll on it fails with
// 0x800280E2. The flag starts with bit 0x2 set. A waits for bit 0x1, so a poll of the main thread
// for bit 0x2 succeeds with 0 unless the flag is held, and 0x800280E2 means it is held. A wait with
// callbacks leaves the flag's queue while its callback runs, keeps its hold, and waits again
// afterwards.
//
// A creates its callback in its own thread. The main thread notifies it once A waits. The callback
// blocks on a gate semaphore, with a finite timeout, until the main thread signals it, so the main
// thread can act while the callback runs, except in the cases that say otherwise. After its wait A
// records its result and its time left, rests on another semaphore so that its end is a step of its
// own, then ends. Every wait has a finite timeout. Every thread is pinned to core 0. The main
// thread runs at 0x60 and A at 0xB0, so A only runs while the main thread blocks. Every case has a
// fresh flag and a fresh A, and the main thread prints after A ended, since a print lets A run.
// Every poll prints 0 if the flag was free and 0x800280E2 if it was held. Every time left prints 1
// if A's timeout variable is above 0 after its wait, else 0. Cases:
// - a set during the callback: the main thread sets bit 0x1 and polls while the callback runs, then
//   signals the gate. Prints the set, the poll during the callback, A's wait, A's time left, the
//   poll after A's wait returned, and the poll after A ended.
// - a set during the callback past the deadline: A's timeout is 300 ms and the main thread holds
//   the callback for 600 ms before it sets bit 0x1 and signals the gate. Prints A's wait, A's time
//   left, the poll after A's wait returned, and the poll after A ended.
// - a cancel during the callback: the main thread cancels the flag with the pattern 0x2 and polls
//   while the callback runs, then signals the gate. Then it waits until A waits again, polls, and
//   sets bit 0x1. Prints the cancel, the number of waiters it woke, the poll during the callback,
//   whether A waits again after its callback, the poll after the callback, A's wait, the poll after
//   A's wait returned, and the poll after A ended.
// - a delete during the callback: A's timeout is 1500 ms. The main thread deletes the flag while
//   the callback runs and signals the gate. Prints the delete, A's wait, whether A's wait returned
//   within 500 ms of the signal, and A's time left. The flag is not polled once deleted.
// - a semaphore deleted during the callback: the same with a semaphore at 0 that A waits on with
//   sceKernelWaitSemaCB, as a reference.
// - a second notify during the callback: the main thread notifies A's callback again while it runs,
//   then signals the gate. The callback only blocks on its first run. Then the main thread waits
//   until A waits again, polls, and sets bit 0x1. Prints the second notify, how often the callback
//   ran when A's wait returned, whether A waits again, the poll after the callbacks, A's wait, the
//   poll after A's wait returned, and the poll after A ended.
// - a timeout while waiting again: A's timeout is 600 ms and its callback returns at once. A waits
//   again and its deadline passes while it waits in the queue. Prints whether A waits again, A's
//   wait, the poll after A's wait returned, and the poll after A ended.
// - a timeout of 1 that misses: A waits with sceKernelWaitEventFlag, without callbacks, for bit 0x1
//   with a timeout of 1 microsecond. Prints A's wait, the poll after it returned, and the poll
//   after A ended.
//
// On a PS Vita TV on firmware 3.65 the flag stays held while A runs its callback in every case. A
// set during the callback lets A take the bits after it, also past A's deadline, with time left
// from before, and frees the flag. A cancel during the callback wakes nobody, and A waits again,
// still holding the flag, until the set. A delete of the flag or of a semaphore during the callback
// ends A's wait with 0x80028006 at once, with time left. A second notify runs the callback twice
// before A waits again. A timeout while A waits again frees the flag. A wait with a timeout of 1
// that misses keeps the flag held until A ends, like one with a timeout of 0.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAIN_PRIORITY 0x60
#define A_PRIORITY 0xB0
#define NOT_DONE 0xDEAD
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define DEADLINE_US 300000
#define CALLBACK_TIME_US 600000
#define DELETE_TIMEOUT_US 1500000
#define AGAIN_TIMEOUT_US 600000
#define SOON_US 500000

typedef enum Case {
    SET_DURING,
    SET_PAST_DEADLINE,
    CANCEL_DURING,
    DELETE_FLAG,
    DELETE_SEMA,
    SECOND_NOTIFY,
    TIMEOUT_AGAIN,
    TIMEOUT_ONE
} Case;

static SceUID flag, sema, gate, rest_sema;
static volatile SceUID callback;
static Case current_case;
static SceUInt a_timeout;
static volatile int callback_done, callback_runs, step, a_result, runs_at_return;
static volatile SceUInt a_left;
static volatile SceUInt64 callback_start, a_end;

// Polls for bit 0x2, which is set
static int poll_set_bit(void) {
    SceUInt32 bits = 0;
    return sceKernelPollEventFlag(flag, 0x2, SCE_EVENT_WAITOR, &bits);
}

// Runs inside A's wait. It blocks on the gate semaphore, except in the case where it returns at
// once, and in the second run of the case that notifies it a second time.
static int hold_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    const int run = ++callback_runs;
    callback_start = sceKernelGetProcessTimeWide();
    if (current_case != TIMEOUT_AGAIN && (current_case != SECOND_NOTIFY || run == 1)) {
        SceUInt timeout = LONG_TIMEOUT;
        sceKernelWaitSema(gate, 1, &timeout);
    }
    callback_done = 1;
    return 0;
}

// Waits with callbacks for bit 0x1 of the flag, or for the semaphore, or without callbacks in the
// last case. Then it records the result and rests until the main thread lets it end.
static int a_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 bits;
    SceUInt timeout = a_timeout;
    if (current_case == TIMEOUT_ONE) {
        a_result = sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    } else {
        callback = sceKernelCreateCallback("hold", 0, hold_callback, NULL);
        if (current_case == DELETE_SEMA)
            a_result = sceKernelWaitSemaCB(sema, 1, &timeout);
        else
            a_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    }
    a_end = sceKernelGetProcessTimeWide();
    a_left = timeout;
    runs_at_return = callback_runs;
    step = 1;
    timeout = LONG_TIMEOUT;
    sceKernelWaitSema(rest_sema, 1, &timeout);
    if (callback)
        sceKernelDeleteCallback(callback);
    return 0;
}

// Creates the objects and starts A on core 0
static SceUID begin(Case c, SceUInt timeout) {
    flag = c == DELETE_SEMA ? 0 : sceKernelCreateEventFlag("flag", 0, 0x2, NULL);
    sema = c == DELETE_SEMA ? sceKernelCreateSema("sema", 0, 0, 1, NULL) : 0;
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    rest_sema = sceKernelCreateSema("rest", 0, 0, 1, NULL);
    current_case = c;
    a_timeout = timeout;
    callback = 0;
    callback_done = callback_runs = step = 0;
    a_result = runs_at_return = NOT_DONE;
    a_left = 0;
    callback_start = a_end = 0;
    const SceUID a = sceKernelCreateThread("a", a_entry, A_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(a, 0, NULL);
    return a;
}

// Lets A end
static void end_a(SceUID a) {
    sceKernelSignalSema(rest_sema, 1);
    test_join_thread(a);
}

// Deletes what the case still has
static void clean_up(void) {
    if (flag > 0)
        sceKernelDeleteEventFlag(flag);
    if (sema > 0)
        sceKernelDeleteSema(sema);
    sceKernelDeleteSema(gate);
    sceKernelDeleteSema(rest_sema);
}

// Notifies A's callback once A waits with callbacks on the object
static int notify_callback(SceUID a, SceUInt32 type, SceUID object) {
    TEST_WAIT_FOR(test_in_wait(a, type | WAIT_TYPE_CB, object));
    return sceKernelNotifyCallback(callback, 1);
}

// Waits until A's callback blocks on the gate
static void wait_for_gate(SceUID a) {
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, gate));
}

// Returns whether A's callback returned and A waits on the flag again
static int waits_again(SceUID a) {
    return step < 1 && callback_done && test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag);
}

// Waits until A waits on the flag again after its callback, or its wait ended, and returns
// whether A waits again
static int wait_for_second_wait(SceUID a) {
    int again = 0;
    TEST_WAIT_FOR((again = waits_again(a)) || step >= 1);
    return again;
}

static void set_case(void) {
    const char *name = "a set during the callback";
    const SceUID a = begin(SET_DURING, LONG_TIMEOUT);
    notify_callback(a, WAIT_TYPE_EVENT_FLAG, flag);
    wait_for_gate(a);
    const int set_result = sceKernelSetEventFlag(flag, 0x1);
    const int poll_during = poll_set_bit();
    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(set_result, "%s: the set", name);
    PRINTF(poll_during, "%s: the poll during the callback", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(a_left > 0, "%s: A's time left is above 0", name);
    PRINTF(poll_after_wait, "%s: the poll after A's wait returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

static void set_past_deadline_case(void) {
    const char *name = "a set during the callback past the deadline";
    const SceUID a = begin(SET_PAST_DEADLINE, DEADLINE_US);
    notify_callback(a, WAIT_TYPE_EVENT_FLAG, flag);
    wait_for_gate(a);
    // A's callback blocks until the main thread signals, so it keeps running past A's deadline
    TEST_WAIT_FOR(sceKernelGetProcessTimeWide() - callback_start >= CALLBACK_TIME_US);
    sceKernelSetEventFlag(flag, 0x1);
    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(a_left > 0, "%s: A's time left is above 0", name);
    PRINTF(poll_after_wait, "%s: the poll after A's wait returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

static void cancel_case(void) {
    const char *name = "a cancel during the callback";
    const SceUID a = begin(CANCEL_DURING, LONG_TIMEOUT);
    notify_callback(a, WAIT_TYPE_EVENT_FLAG, flag);
    wait_for_gate(a);
    SceUInt32 woken = NOT_DONE;
    const int cancel_result = sceKernelCancelEventFlag(flag, 0x2, &woken);
    const int poll_during = poll_set_bit();
    sceKernelSignalSema(gate, 1);
    const int again = wait_for_second_wait(a);
    const int poll_after_callback = poll_set_bit();
    sceKernelSetEventFlag(flag, 0x1);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(cancel_result, "%s: the cancel", name);
    PRINTF(woken, "%s: the waiters the cancel woke", name);
    PRINTF(poll_during, "%s: the poll during the callback", name);
    PRINTF(again, "%s: A waits again after its callback", name);
    PRINTF(poll_after_callback, "%s: the poll after the callback", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_after_wait, "%s: the poll after A's wait returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

// Deletes the flag or the semaphore of A's wait while its callback runs, then waits for A's wait to
// return. Its timeout is long enough that a wait that goes on after the delete ends much later.
static void delete_case(Case c, const char *name) {
    const int is_sema = c == DELETE_SEMA;
    const SceUID a = begin(c, DELETE_TIMEOUT_US);
    notify_callback(a, is_sema ? WAIT_TYPE_SEMA : WAIT_TYPE_EVENT_FLAG, is_sema ? sema : flag);
    wait_for_gate(a);
    int delete_result;
    if (is_sema) {
        delete_result = sceKernelDeleteSema(sema);
        sema = 0;
    } else {
        delete_result = sceKernelDeleteEventFlag(flag);
        flag = 0;
    }
    const SceUInt64 signal_time = sceKernelGetProcessTimeWide();
    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(step >= 1);
    end_a(a);
    clean_up();
    PRINTF(delete_result, "%s: the delete", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(a_end >= signal_time && a_end - signal_time < SOON_US, "%s: A's wait returned within 500 ms of the signal", name);
    PRINTF(a_left > 0, "%s: A's time left is above 0", name);
}

static void second_notify_case(void) {
    const char *name = "a second notify during the callback";
    const SceUID a = begin(SECOND_NOTIFY, LONG_TIMEOUT);
    notify_callback(a, WAIT_TYPE_EVENT_FLAG, flag);
    wait_for_gate(a);
    const int second_notify = sceKernelNotifyCallback(callback, 1);
    sceKernelSignalSema(gate, 1);
    const int again = wait_for_second_wait(a);
    const int poll_after_callbacks = poll_set_bit();
    sceKernelSetEventFlag(flag, 0x1);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(second_notify, "%s: the second notify", name);
    PRINTF(runs_at_return, "%s: runs of the callback when A's wait returned", name);
    PRINTF(again, "%s: A waits again after its callbacks", name);
    PRINTF(poll_after_callbacks, "%s: the poll after the callbacks", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_after_wait, "%s: the poll after A's wait returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

static void timeout_again_case(void) {
    const char *name = "a timeout while waiting again";
    const SceUID a = begin(TIMEOUT_AGAIN, AGAIN_TIMEOUT_US);
    notify_callback(a, WAIT_TYPE_EVENT_FLAG, flag);
    const int again = wait_for_second_wait(a);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(again, "%s: A waits again after its callback", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_after_wait, "%s: the poll after A's wait returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

static void timeout_one_case(void) {
    const char *name = "a timeout of 1 that misses";
    const SceUID a = begin(TIMEOUT_ONE, 1);
    TEST_WAIT_FOR(step >= 1);
    const int poll_after_wait = poll_set_bit();
    end_a(a);
    const int poll_after_end = poll_set_bit();
    clean_up();
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_after_wait, "%s: the poll after it returned", name);
    PRINTF(poll_after_end, "%s: the poll after A ended", name);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);

    set_case();
    set_past_deadline_case();
    cancel_case();
    delete_case(DELETE_FLAG, "a delete during the callback");
    delete_case(DELETE_SEMA, "a semaphore deleted during the callback");
    second_notify_case();
    timeout_again_case();
    timeout_one_case();

    test_exit();
    return 0;
}

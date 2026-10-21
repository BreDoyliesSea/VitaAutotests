// What an event flag without SCE_EVENT_WAITMULTIPLE does with the hold of its one waiting thread A
// while A runs the callbacks of a wait with callbacks. While another thread holds the flag, a wait
// or a poll of that thread fails with 0x800280E2. The flag starts with bit 0x2 set. A waits for bit
// 0x1 with sceKernelWaitEventFlagCB, so a poll of the main thread for bit 0x2 succeeds with 0 unless
// the flag is held. A's callback blocks on a semaphore that the main thread signals, so the main
// thread can act while it runs. After its wait, A rests on another semaphore, so its end is a step
// of its own. Every case has a fresh flag and a fresh A:
// - hold across a callback: the main thread's poll while A's callback runs, then A's own poll for
//   bit 0x2 inside the callback, then the callback returns and A waits again, the main thread's poll
//   then, a set of bit 0x1 ends A's wait, with the poll after it and after A ended
// - deadline passes during a callback: A's wait has a timeout of 300 ms and its callback runs for
//   600 ms, then A's wait result, the poll after A's wait returned, and the poll after A ended
// - nested waits in a callback: the callback waits for bit 0x1 on the same flag with a timeout of 0
//   and then with a timeout of 300 ms, then it returns and A waits again, a set of bit 0x1 ends A's
//   wait, then the results of both nested waits, A's wait, and the poll after A's wait and after
//   A ended
//
// On a PS Vita TV on firmware 3.65 the flag keeps counting A as its waiting thread while A's
// callback runs and once A waits again, so the main thread's polls fail with 0x800280E2, while A's
// own poll succeeds. A set that ends A's wait frees the flag. When the deadline passes during the
// callback, A's wait ends with 0x80028005 and the flag stays held until A ends. A nested wait with
// a timeout of 0 inside the callback returns 0x80028005 and leaves the flag held after the set
// ended A's wait, until A ends. The nested wait with a timeout of 300 ms times out too.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define SHORT_TIMEOUT 300000
#define CALLBACK_TIME_US 600000

typedef enum Case { HOLD_AROUND, DEADLINE_PASSES, NESTED_WAITS } Case;

static SceUID flag, gate, rest_sema, callback;
static Case current_case;
static SceUInt a_timeout;
static volatile int callback_done, step, a_result, own_poll, nested_zero, nested_short;
static volatile SceUInt64 callback_start;

// Polls for bit 0x2, which is set
static int poll_set_bit(void) {
    SceUInt32 bits = 0;
    return sceKernelPollEventFlag(flag, 0x2, SCE_EVENT_WAITOR, &bits);
}

// Waits for bit 0x1 on the flag without callbacks
static int wait_for_bit(SceUInt timeout) {
    SceUInt32 bits;
    return sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
}

// Runs inside A's wait. Depending on the case it blocks on the gate semaphore and polls, or waits
// for bit 0x1 on the flag itself.
static int hold_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    callback_start = sceKernelGetProcessTimeWide();
    if (current_case == NESTED_WAITS) {
        nested_zero = wait_for_bit(0);
        nested_short = wait_for_bit(SHORT_TIMEOUT);
    } else {
        SceUInt timeout = LONG_TIMEOUT;
        sceKernelWaitSema(gate, 1, &timeout);
        if (current_case == HOLD_AROUND)
            own_poll = poll_set_bit();
    }
    callback_done = 1;
    return 0;
}

// Waits for bit 0x1 with callbacks, then rests until the main thread lets it end
static int a_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    callback = sceKernelCreateCallback("hold", 0, hold_callback, NULL);
    SceUInt32 bits;
    SceUInt timeout = a_timeout;
    a_result = sceKernelWaitEventFlagCB(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
    step = 1;
    timeout = LONG_TIMEOUT;
    sceKernelWaitSema(rest_sema, 1, &timeout);
    sceKernelDeleteCallback(callback);
    return 0;
}

// Creates the flag and the semaphores and starts A
static SceUID begin(Case c, SceUInt timeout) {
    flag = sceKernelCreateEventFlag("flag", 0, 0x2, NULL);
    gate = sceKernelCreateSema("gate", 0, 0, 1, NULL);
    rest_sema = sceKernelCreateSema("rest", 0, 0, 1, NULL);
    current_case = c;
    a_timeout = timeout;
    callback_done = step = 0;
    a_result = own_poll = nested_zero = nested_short = NOT_DONE;
    return test_start_thread(a_entry);
}

// Lets A end
static void end_a(SceUID a) {
    sceKernelSignalSema(rest_sema, 1);
    test_join_thread(a);
}

static void clean_up(void) {
    sceKernelDeleteEventFlag(flag);
    sceKernelDeleteSema(gate);
    sceKernelDeleteSema(rest_sema);
}

// Notifies A's callback once A waits
static void start_callback(SceUID a) {
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag));
    sceKernelNotifyCallback(callback, 1);
}

// Waits until A's callback returned and A waits on the flag again
static void wait_for_second_wait(SceUID a) {
    TEST_WAIT_FOR(callback_done && test_in_wait(a, WAIT_TYPE_EVENT_FLAG | WAIT_TYPE_CB, flag));
}

static void hold_around_case(void) {
    const char *name = "hold across a callback";
    const SceUID a = begin(HOLD_AROUND, LONG_TIMEOUT);
    start_callback(a);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, gate));
    PRINTF(poll_set_bit(), "%s: the main thread's poll while A's callback runs", name);
    sceKernelSignalSema(gate, 1);
    wait_for_second_wait(a);
    PRINTF(own_poll, "%s: A's own poll inside its callback", name);
    PRINTF(poll_set_bit(), "%s: the main thread's poll while A waits again", name);
    sceKernelSetEventFlag(flag, 0x1);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after A's wait", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

static void deadline_passes_case(void) {
    const char *name = "deadline passes during a callback";
    const SceUID a = begin(DEADLINE_PASSES, SHORT_TIMEOUT);
    start_callback(a);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_SEMA, gate));
    // A's callback blocks until the main thread signals, so it keeps running past A's deadline
    TEST_WAIT_FOR(sceKernelGetProcessTimeWide() - callback_start >= CALLBACK_TIME_US);
    sceKernelSignalSema(gate, 1);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after A's wait returned", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

static void nested_waits_case(void) {
    const char *name = "nested waits in a callback";
    const SceUID a = begin(NESTED_WAITS, LONG_TIMEOUT);
    start_callback(a);
    wait_for_second_wait(a);
    sceKernelSetEventFlag(flag, 0x1);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(nested_zero, "%s: the nested wait with a timeout of 0", name);
    PRINTF(nested_short, "%s: the nested wait with a timeout of 300 ms", name);
    PRINTF(a_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after A's wait", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

int main(void) {
    hold_around_case();
    deadline_passes_case();
    nested_waits_case();

    test_exit();
    return 0;
}

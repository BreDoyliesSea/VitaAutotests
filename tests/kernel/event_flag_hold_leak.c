// How long an event flag without SCE_EVENT_WAITMULTIPLE counts one waiting thread A as there, so
// that another thread's wait or poll on the flag fails with 0x800280E2. The flag starts with bit
// 0x2 set. A waits for bit 0x1, which is never set, except in the cases that say so. The main
// thread then polls for bit 0x2, which is set, so a poll that gets no error succeeds with 0. Between
// its steps A waits on a semaphore that the main thread signals, so A's end is a step of its own.
// Each wait and each poll prints its result. Every case has a fresh flag and a fresh A:
// - A waits with a timeout of 0 and returns at once, then the poll, then A ends and the poll again
// - the same on a flag with SCE_EVENT_WAITMULTIPLE, as a reference
// - A waits with a timeout of 300 ms that passes while A sleeps, then the poll, then A ends and the
//   poll again
// - A waits and a set of its bit ends the wait, with a poll while A waits and one after the set, then
//   A ends and the poll again
// - A waits with a timeout of 0, then waits again and a set ends that wait, with a poll after each,
//   then A ends and the poll again
// - A waits with a timeout of 0, then the main thread deletes the flag and polls it
// The case with the delete goes last, since an implementation may keep a pointer to the deleted flag.
//
// On a PS Vita TV on firmware 3.65 the flag keeps counting A as its waiting thread after a wait
// with a timeout of 0, so the poll fails with 0x800280E2 until A ends or the flag is deleted. A
// later wait of A that a set ends doesn't change that. After a wait whose timeout passed while A
// slept, or that a set ended, the poll succeeds. The flag with the multi attribute never refuses
// the poll.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define LONG_TIMEOUT (3 * TEST_WAIT_US)
#define SHORT_TIMEOUT 300000

static SceUID flag, go;
static SceUInt first_timeout;
static int second_wait;
static volatile int step, first_result, second_result;

// Waits for bit 0x1
static int wait_for_bit(SceUInt timeout) {
    SceUInt32 bits;
    return sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
}

// Polls for bit 0x2, which is set
static int poll_set_bit(void) {
    SceUInt32 bits = 0;
    return sceKernelPollEventFlag(flag, 0x2, SCE_EVENT_WAITOR, &bits);
}

// Waits until the main thread signals the semaphore
static void rest(void) {
    SceUInt timeout = LONG_TIMEOUT;
    sceKernelWaitSema(go, 1, &timeout);
}

// Waits on the flag, rests, and does it again if the case wants a second wait
static int a_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    first_result = wait_for_bit(first_timeout);
    step = 1;
    rest();
    if (second_wait) {
        second_result = wait_for_bit(LONG_TIMEOUT);
        step = 2;
        rest();
    }
    return 0;
}

// Creates the flag and the semaphore and starts A
static SceUID begin(SceUInt32 attr, SceUInt timeout, int second) {
    flag = sceKernelCreateEventFlag("flag", attr, 0x2, NULL);
    go = sceKernelCreateSema("go", 0, 0, 2, NULL);
    first_timeout = timeout;
    second_wait = second;
    step = 0;
    first_result = second_result = NOT_DONE;
    return test_start_thread(a_entry);
}

// Lets A end
static void end_a(SceUID a) {
    sceKernelSignalSema(go, 1);
    test_join_thread(a);
}

static void clean_up(void) {
    sceKernelDeleteEventFlag(flag);
    sceKernelDeleteSema(go);
}

// A's wait ends by itself, then A ends
static void timeout_case(const char *name, SceUInt32 attr, SceUInt timeout) {
    const SceUID a = begin(attr, timeout, 0);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(first_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

// A's wait ends by a set
static void set_case(void) {
    const char *name = "a wait ended by a set";
    const SceUID a = begin(0, LONG_TIMEOUT, 0);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_EVENT_FLAG, flag));
    PRINTF(poll_set_bit(), "%s: the poll while A waits", name);
    PRINTF(sceKernelSetEventFlag(flag, 0x1), "%s: the set", name);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(first_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

// A's first wait has a timeout of 0, its second wait ends by a set
static void zero_then_set_case(void) {
    const char *name = "a timeout of 0, then a wait ended by a set";
    const SceUID a = begin(0, 0, 1);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(first_result, "%s: A's first wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after it", name);
    sceKernelSignalSema(go, 1);
    TEST_WAIT_FOR(test_in_wait(a, WAIT_TYPE_EVENT_FLAG, flag));
    PRINTF(sceKernelSetEventFlag(flag, 0x1), "%s: the set", name);
    TEST_WAIT_FOR(step >= 2);
    PRINTF(second_result, "%s: A's second wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after it", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

// A's wait has a timeout of 0, then the main thread deletes the flag
static void delete_case(void) {
    const char *name = "a timeout of 0, then the flag is deleted";
    const SceUID a = begin(0, 0, 0);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(first_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll", name);
    PRINTF(sceKernelDeleteEventFlag(flag), "%s: the delete", name);
    PRINTF(poll_set_bit(), "%s: the poll on the deleted flag", name);
    end_a(a);
    sceKernelDeleteSema(go);
}

int main(void) {
    timeout_case("a timeout of 0 on a flag with the multi attribute", SCE_EVENT_WAITMULTIPLE, 0);
    timeout_case("a timeout of 0", 0, 0);
    timeout_case("a timeout that passes while A sleeps", 0, SHORT_TIMEOUT);
    set_case();
    zero_then_set_case();
    delete_case();

    test_exit();
    return 0;
}

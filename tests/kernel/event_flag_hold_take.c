// Whether a wait that takes its bits at once leaves a hold on an event flag without
// SCE_EVENT_WAITMULTIPLE. Another thread's poll fails with 0x800280E2 while A retains a hold.
// The flag starts with bit 0x2 set. A's waits that take the bits wait for 0x2 without clearing it,
// and the main thread then polls for 0x2, so a poll that gets no error succeeds with 0. Between its
// steps A waits on a semaphore that the main thread signals, so A remains alive during every poll.
// Each wait and each poll prints its result. Every case has a fresh flag and a fresh A:
// - A waits with a long timeout and takes the bits at once, then the poll, then A ends and the
//   poll again
// - the same with a timeout of 0
// - A waits with a timeout of 0 for bit 0x1, which is never set, then waits and takes the bits at
//   once, with a poll after each, then A ends and the poll again
//
// On a PS Vita TV on firmware 3.65 a wait that takes the bits at once leaves the flag free, also
// with a timeout of 0, so the poll succeeds while A is still there. After a wait with a timeout of
// 0 that missed, the poll fails with 0x800280E2, and a later wait of A that takes the bits at once
// doesn't change that. Once A ends the poll succeeds.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define NOT_DONE 0xDEAD
#define LONG_TIMEOUT (3 * TEST_WAIT_US)

static SceUID flag, go;
static SceUInt take_timeout;
static int miss_first;
static volatile int step, miss_result, take_result;

// Waits with a timeout of 0 for bit 0x1, which is never set
static int miss_bit(void) {
    SceUInt32 bits;
    SceUInt timeout = 0;
    return sceKernelWaitEventFlag(flag, 0x1, SCE_EVENT_WAITOR, &bits, &timeout);
}

// Waits for bit 0x2, which is set, so the wait takes it at once
static int take_bit(SceUInt timeout) {
    SceUInt32 bits;
    return sceKernelWaitEventFlag(flag, 0x2, SCE_EVENT_WAITOR, &bits, &timeout);
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

// Misses the bit first if the case wants it, rests, then takes the bits and rests again
static int a_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (miss_first) {
        miss_result = miss_bit();
        step = 1;
        rest();
    }
    take_result = take_bit(take_timeout);
    step = 2;
    rest();
    return 0;
}

// Creates the flag and the semaphore and starts A
static SceUID begin(SceUInt timeout, int miss) {
    flag = sceKernelCreateEventFlag("flag", 0, 0x2, NULL);
    go = sceKernelCreateSema("go", 0, 0, 2, NULL);
    take_timeout = timeout;
    miss_first = miss;
    step = 0;
    miss_result = take_result = NOT_DONE;
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

// A's only wait takes the bits at once
static void take_case(const char *name, SceUInt timeout) {
    const SceUID a = begin(timeout, 0);
    TEST_WAIT_FOR(step >= 2);
    PRINTF(take_result, "%s: A's wait", name);
    PRINTF(poll_set_bit(), "%s: the poll", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

// A's first wait misses the bit with a timeout of 0, its second takes the bits at once
static void miss_then_take_case(void) {
    const char *name = "a timeout of 0 that misses, then a wait that takes the bits at once";
    const SceUID a = begin(LONG_TIMEOUT, 1);
    TEST_WAIT_FOR(step >= 1);
    PRINTF(miss_result, "%s: A's first wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after it", name);
    sceKernelSignalSema(go, 1);
    TEST_WAIT_FOR(step >= 2);
    PRINTF(take_result, "%s: A's second wait", name);
    PRINTF(poll_set_bit(), "%s: the poll after it", name);
    end_a(a);
    PRINTF(poll_set_bit(), "%s: the poll after A ended", name);
    clean_up();
}

int main(void) {
    take_case("a wait that takes the bits at once", LONG_TIMEOUT);
    take_case("a wait with a timeout of 0 that takes the bits at once", 0);
    miss_then_take_case();

    test_exit();
    return 0;
}

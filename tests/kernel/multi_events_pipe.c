// A message pipe in the list of sceKernelWaitMultipleEvents, next to a simple event. A message pipe
// posts the event bits 0x1 for a send and 0x2 for a receive that moved bytes. Each case uses a fresh
// empty pipe of 0x1000 bytes and a fresh simple event, and every wait has a timeout of 2 s. For each
// wait it prints the call's result, and for each result entry whether it is the pipe or the event,
// its result, its bits and both halves of its user data. Cases:
// - OR on the pipe's bit 0x1 and a bit of the event, from a waiter thread, ended by a try send of 4
//   bytes
// - OR on the pipe's bit 0x2 and a bit of the event, from a waiter thread, which a try send of 4
//   bytes doesn't end and a blocking receive of 4 bytes does
// - AND on the pipe's bit 0x1 and a bit of the event, from a waiter thread, with a try send of 4
//   bytes and then a set of the event
// - OR on the pipe's bit 0x1 and a bit of the event after a try send already set the bit
// - sceKernelWaitEvent on the pipe for the bit 0x1 after a try send
//
// On a PS Vita TV on firmware 3.65 the pipe takes its place in the list like an event, since the
// call looks every entry up as any kind of event. The send ends the OR wait with one entry for the
// pipe, with the result 0, the bit 0x1 and the user data (0, 4), the result of the send and the size
// it moved. A try send doesn't end the wait for the bit 0x2, and the receive does, with the bit 0x2
// and the user data (0, 4). The AND wait takes the pipe's entry at the send and ends at the set of
// the event with two entries. A bit set before the call is reported at once, and a plain wait takes
// it the same way.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 0x1000

static SceUID pipe, event;
static WaitEvent events[2];
static ResultEvent results[2];
static volatile int wait_result;
static volatile SceUInt32 wait_mode;
static char out[CAPACITY];
static char in[CAPACITY];

static int multi_waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    wait_result = sceKernelWaitMultipleEvents(events, 2, wait_mode, results, &timeout);
    return 0;
}

// Creates the pipe and the event, lists the pipe's bit and the event's bit A, and fills the results
// with a marker
static void setup(SceUInt32 pipe_bit, SceUInt32 mode) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    event = sceKernelCreateSimpleEvent("event", EVENT_ATTR_MANUAL_RESET, 0, NULL);
    events[0] = (WaitEvent){ pipe, pipe_bit };
    events[1] = (WaitEvent){ event, EVENT_BIT_A };
    sceClibMemset(results, 0xAA, sizeof(results));
    wait_mode = mode;
    wait_result = 0xDEAD;
}

static void teardown(void) {
    sceKernelDeleteMsgPipe(pipe);
    sceKernelDeleteSimpleEvent(event);
}

// Prints the first count result entries
static void print_results(const char *name, int count) {
    for (int i = 0; i < 2 && i < count; i++) {
        PRINTF(results[i].eventId == pipe ? 1 : results[i].eventId == event ? 2 : 0, "%s: entry %d is the pipe (1) or the event (2)", name, i);
        PRINTF(results[i].result, "%s: entry %d result", name, i);
        PRINTF(results[i].resultPattern, "%s: entry %d bits", name, i);
        PRINTF((SceUInt32)results[i].userData, "%s: entry %d low half of the user data", name, i);
        PRINTF((SceUInt32)(results[i].userData >> 32), "%s: entry %d high half of the user data", name, i);
    }
}

// Starts the waiter and returns once it is in its wait
static SceUID start_waiter(void) {
    const SceUID thread = test_start_thread(multi_waiter);
    test_wait_until_waiting(thread);
    return thread;
}

int main(void) {
    SceUInt32 timeout;
    SceSize size;
    SceUID thread;

    // OR, ended by a try send
    setup(EVENT_IN, EVENT_WAIT_MODE_OR);
    thread = start_waiter();
    PRINT("OR on the pipe's bit 0x1: the try send", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL));
    test_join_thread(thread);
    PRINT("OR on the pipe's bit 0x1: the wait", wait_result);
    print_results("OR on the pipe's bit 0x1", wait_result);
    teardown();

    // OR on the bit of a receive, which a send doesn't set
    setup(EVENT_OUT, EVENT_WAIT_MODE_OR);
    thread = start_waiter();
    PRINT("OR on the pipe's bit 0x2: the try send", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL));
    // Read right after the send, which should have done nothing to the wait
    PRINT("OR on the pipe's bit 0x2: the wait is still going after the send", wait_result == 0xDEAD);
    timeout = TEST_WAIT_US;
    size = 0xDEAD;
    PRINT("OR on the pipe's bit 0x2: the receive", sceKernelReceiveMsgPipe(pipe, in, 4, MSG_PIPE_MODE_FULL, &size, &timeout));
    test_join_thread(thread);
    PRINT("OR on the pipe's bit 0x2: the wait", wait_result);
    print_results("OR on the pipe's bit 0x2", wait_result);
    teardown();

    // AND, the pipe's bit and then the event's
    setup(EVENT_IN, EVENT_WAIT_MODE_AND);
    thread = start_waiter();
    PRINT("AND on the pipe: the try send", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL));
    PRINT("AND on the pipe: the wait is still going after the send", wait_result == 0xDEAD);
    PRINT("AND on the pipe: the set", sceKernelSetEvent(event, EVENT_BIT_A, 0x77));
    test_join_thread(thread);
    PRINT("AND on the pipe: the wait", wait_result);
    print_results("AND on the pipe", wait_result);
    teardown();

    // The pipe's bit set before the call
    setup(EVENT_IN, EVENT_WAIT_MODE_OR);
    PRINT("the bit set before: the try send", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL));
    timeout = TEST_WAIT_US;
    const int count = sceKernelWaitMultipleEvents(events, 2, EVENT_WAIT_MODE_OR, results, &timeout);
    PRINT("the bit set before: the wait", count);
    print_results("the bit set before", count);
    teardown();

    // A plain wait on the pipe
    setup(EVENT_IN, EVENT_WAIT_MODE_OR);
    PRINT("single wait: the try send", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL));
    SceUInt32 bits = 0xDEAD;
    SceUInt64 user_data = 0xDEADDEADDEADDEADull;
    timeout = TEST_WAIT_US;
    PRINT("single wait: the wait", sceKernelWaitEvent(pipe, EVENT_IN, &bits, &user_data, &timeout));
    PRINT("single wait: the bits", bits);
    PRINT("single wait: the low half of the user data", (SceUInt32)user_data);
    PRINT("single wait: the high half of the user data", (SceUInt32)(user_data >> 32));
    teardown();

    test_exit();
    return 0;
}

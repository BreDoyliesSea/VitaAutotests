// The event bits and user data that plain message pipe sends and receives leave on the pipe, read
// with sceKernelGetEventInfo on the pipe, or with sceKernelPollEvent for each bit if that call
// fails. First a fresh pipe: whether the info call takes it, its bits, and whether
// sceKernelClearEvent clears them. Then each case on a fresh pipe of 0x1000 bytes, printing the
// call's result, the bits and both halves of the user data after it, and the bits before it where
// the case sets up the pipe first:
// - a try send of 4 bytes, ASAP
// - a try send of 0x1000 bytes, FULL, into a pipe that holds 4, with the bits cleared before it
// - a try receive of 4 bytes, FULL, from a pipe that holds 4, which empties it
// - a try receive of 4 bytes, ASAP, from a pipe that holds 8, which doesn't
// - a blocking send of 4 bytes that ends at once
// - a blocking receive of 4 bytes that ends at once and empties the pipe
// - a blocking receive with a timeout of 0 on an empty pipe
//
// On a PS Vita TV on firmware 3.65 a fresh pipe holds the bit 0x4 like a fresh simple event, and
// clearing every bit works on it. Every call posts an event on the pipe, the bits 0x1 for a send,
// 0x2 for a receive and 0x10 for an error, with 0x10000 added when the pipe holds bytes after a
// send. A receive that empties the pipe clears 0x10000. The user data holds the result in its low
// half and the size moved in its high half, or 0xFFFFFFFF there for an error. A send that fails
// without moving anything posts only 0x10.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 0x1000

static char out[CAPACITY];
static char in[CAPACITY];

typedef struct PipeEvent {
    SceUInt32 bits;
    SceUInt64 user_data;
} PipeEvent;

// Reads the pipe's event bits and user data with the info call, or with a poll for each bit
static PipeEvent event_of(SceUID pipe) {
    PipeEvent event = { 0, 0 };
    EventInfo info = { .size = sizeof(info) };
    if (sceKernelGetEventInfo(pipe, &info) == 0) {
        event.bits = info.eventPattern;
        event.user_data = info.userData;
        return event;
    }
    static const SceUInt32 bits[] = { EVENT_IN, EVENT_OUT, 0x10, 0x10000 };
    for (unsigned i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        SceUInt32 got;
        SceUInt64 user_data;
        if (sceKernelPollEvent(pipe, bits[i], &got, &user_data) == 0) {
            event.bits |= got;
            event.user_data = user_data;
        }
    }
    return event;
}

// Creates a pipe and clears the bits of its creation
static SceUID new_pipe(void) {
    const SceUID pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    sceKernelClearEvent(pipe, 0);
    return pipe;
}

static void print_before(const char *name, SceUID pipe) {
    PRINTF(event_of(pipe).bits, "%s: the bits before", name);
}

// Prints the result of the call, and the event the pipe holds after it
static void print_after(const char *name, int result, SceUID pipe) {
    const PipeEvent event = event_of(pipe);
    PRINTF(result, "%s: the call", name);
    PRINTF(event.bits, "%s: the bits after", name);
    PRINTF((SceUInt32)event.user_data, "%s: the low half of the user data", name);
    PRINTF((SceUInt32)(event.user_data >> 32), "%s: the high half of the user data", name);
}

int main(void) {
    SceSize size;
    SceUInt32 timeout;

    // A fresh pipe
    SceUID pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    EventInfo info = { .size = sizeof(info) };
    PRINT("event info of a fresh pipe", sceKernelGetEventInfo(pipe, &info));
    PRINT("a fresh pipe: the bits", event_of(pipe).bits);
    PRINT("clear every bit", sceKernelClearEvent(pipe, 0));
    PRINT("the bits after the clear", event_of(pipe).bits);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    print_after("try send of 4 ASAP", sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_ASAP, NULL), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL);
    sceKernelClearEvent(pipe, 0);
    print_before("try send FULL into a pipe holding 4", pipe);
    print_after("try send FULL into a pipe holding 4", sceKernelTrySendMsgPipe(pipe, out, CAPACITY, MSG_PIPE_MODE_FULL, NULL), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL);
    print_before("try receive of 4 that empties the pipe", pipe);
    print_after("try receive of 4 that empties the pipe", sceKernelTryReceiveMsgPipe(pipe, in, 4, MSG_PIPE_MODE_FULL, NULL), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    sceKernelTrySendMsgPipe(pipe, out, 8, MSG_PIPE_MODE_FULL, NULL);
    print_before("try receive of 4 ASAP from a pipe holding 8", pipe);
    print_after("try receive of 4 ASAP from a pipe holding 8", sceKernelTryReceiveMsgPipe(pipe, in, 4, MSG_PIPE_MODE_ASAP, NULL), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    timeout = TEST_WAIT_US;
    size = 0xDEAD;
    print_after("blocking send of 4", sceKernelSendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, &size, &timeout), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    sceKernelTrySendMsgPipe(pipe, out, 4, MSG_PIPE_MODE_FULL, NULL);
    print_before("blocking receive of 4 that empties the pipe", pipe);
    timeout = TEST_WAIT_US;
    size = 0xDEAD;
    print_after("blocking receive of 4 that empties the pipe", sceKernelReceiveMsgPipe(pipe, in, 4, MSG_PIPE_MODE_FULL, &size, &timeout), pipe);
    sceKernelDeleteMsgPipe(pipe);

    pipe = new_pipe();
    timeout = 0;
    size = 0xDEAD;
    print_after("blocking receive with a timeout of 0", sceKernelReceiveMsgPipe(pipe, in, 4, MSG_PIPE_MODE_FULL, &size, &timeout), pipe);
    sceKernelDeleteMsgPipe(pipe);

    test_exit();
    return 0;
}

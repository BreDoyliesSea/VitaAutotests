// Message pipe sends and receives that don't wait: what they return, and how their end shows up
// as an event of the pipe, in its user data and to callbacks registered to it
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

static char fill[0x1000];
static volatile int calls, got_arg, got_notify_id;
static SceUID pipe;

static int recording_callback(int notify_id, int count, int arg, void *common) {
    (void)count;
    (void)common;
    calls++;
    got_notify_id = notify_id;
    got_arg = arg;
    return 0;
}

// Prints whether the pipe reports the event, with the user data split into its two halves
static void print_event(const char *name, SceUInt32 bit) {
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data = 0xDEADDEADDEADDEADull;
    SceUInt32 timeout = 100000;
    PRINTF(sceKernelWaitEvent(pipe, bit, &pattern, &user_data, &timeout), "%s: wait for the event", name);
    PRINTF(pattern, "%s: the event's bits", name);
    PRINTF((int)(user_data & 0xFFFFFFFF), "%s: the low half of its user data", name);
    PRINTF((int)(user_data >> 32), "%s: the high half of its user data", name);
}

int main(void) {
    char message[4] = { 1, 2, 3, 4 };
    char buf[4] = { 0 };
    SceSize size;

    // A receive from an empty pipe that doesn't wait, then a send
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
    size = 0xDEAD;
    PRINT("receive without waiting from an empty pipe", sceKernelReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, &size, NULL));
    PRINT("the size it wrote", size);
    SceUInt32 pattern = 0xDEAD;
    SceUInt64 user_data;
    PRINT("poll for the receive's event before a send", sceKernelPollEvent(pipe, EVENT_OUT, &pattern, &user_data));
    PRINT("a second receive without waiting", sceKernelReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, &size, NULL));
    PRINT("send", sceKernelTrySendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL));
    print_event("the receive", EVENT_OUT);
    PRINT("the receive got the message", sceClibMemcmp(buf, message, sizeof(message)) == 0);
    PRINT("receive what's left", sceKernelTryReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL));
    sceKernelDeleteMsgPipe(pipe);

    // A send to a full pipe that doesn't wait, then a receive
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
    sceKernelTrySendMsgPipe(pipe, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL);
    size = 0xDEAD;
    PRINT("send without waiting to a full pipe", sceKernelSendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, &size, NULL));
    PRINT("the size it wrote", size);
    PRINT("poll for the send's event before a receive", sceKernelPollEvent(pipe, EVENT_IN, &pattern, &user_data));
    PRINT("receive the pipe's contents", sceKernelTryReceiveMsgPipe(pipe, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL));
    print_event("the send", EVENT_IN);
    PRINT("receive the sent message", sceKernelTryReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL, NULL));
    sceKernelDeleteMsgPipe(pipe);

    // A send that doesn't wait to an empty pipe
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
    PRINT("send without waiting to an empty pipe", sceKernelSendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, &size, NULL));
    print_event("the send that could end at once", EVENT_IN);
    sceKernelDeleteMsgPipe(pipe);

    // A callback registered to the pipe
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, sizeof(fill), NULL);
    const SceUID cb = sceKernelCreateCallback("pipe", 0, recording_callback, NULL);
    PRINT("register a callback to the pipe", sceKernelRegisterCallbackToEvent(pipe, cb));
    sceKernelReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_WAIT, &size, NULL);
    sceKernelTrySendMsgPipe(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    PRINT("the callback's notifications after the receive ended", sceKernelGetCallbackCount(cb));
    sceKernelCheckCallback();
    PRINT("runs of the callback", calls);
    if (calls)
        PRINT("the notify ID is the pipe", got_notify_id == pipe);
    sceKernelDeleteMsgPipe(pipe);

    test_exit();
    return 0;
}

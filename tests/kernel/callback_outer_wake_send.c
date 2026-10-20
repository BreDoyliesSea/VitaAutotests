// A message pipe send waits with callbacks, the callback it runs blocks in a wait of its own, and
// the test empties the pipe in between. What the pipe holds then, and how the send ends after the
// callback. The receive takes everything, so no sender is left waiting once it is done.
//
// On a PS Vita TV on firmware 3.65 the pipe counts no sender while the callback runs, and the
// receive leaves the pipe empty without taking the waiting message. The send puts it in once the
// callback returns.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// One send of this many bytes fills the pipe
#define PIPE_SIZE 0x1000

static SceUID pipe, inner, cb;
static volatile int inner_result, outer_result;
static SceUInt outer_left;
static char message[4] = { 1, 2, 3, 4 };

// Blocks in a wait on another semaphore, until the test signals it
static int inner_wait_callback(int notify_id, int count, int arg, void *common) {
    (void)notify_id;
    (void)count;
    (void)arg;
    (void)common;
    SceUInt timeout = 2 * TEST_WAIT_US;
    inner_result = sceKernelWaitSema(inner, 1, &timeout);
    return 0;
}

static int sender_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    cb = sceKernelCreateCallback("inner", 0, inner_wait_callback, NULL);
    outer_left = 4 * TEST_WAIT_US;
    outer_result = sceKernelSendMsgPipeCB(pipe, message, sizeof(message), MSG_PIPE_MODE_FULL, NULL, &outer_left);
    sceKernelDeleteCallback(cb);
    return 0;
}

static int sema_count(SceUID id) {
    SceKernelSemaInfo info = { .size = sizeof(info) };
    sceKernelGetSemaInfo(id, &info);
    return info.currentCount;
}

int main(void) {
    static char fill[PIPE_SIZE];
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL);
    PRINT("fill the pipe", sceKernelTrySendMsgPipe(pipe, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL));
    inner = sceKernelCreateSema("inner", 0, 0, 1, NULL);
    inner_result = outer_result = 0xDEAD;
    const SceUID thread = test_start_thread(sender_thread);
    test_wait_until_waiting(thread);
    sceKernelNotifyCallback(cb, 1);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, inner));

    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("its senders while the callback waits", info.numSendWaitThreads);
    PRINT("empty the pipe", sceKernelTryReceiveMsgPipe(pipe, fill, sizeof(fill), MSG_PIPE_MODE_FULL, NULL));
    sceKernelGetMsgPipeInfo(pipe, &info);
    PRINT("the bytes in it after the receive", info.bufSize - info.freeSize);
    PRINT("its senders after the receive", info.numSendWaitThreads);

    // Ends the callback's wait. If the receive had ended it too, the signal here is left over.
    sceKernelSignalSema(inner, 1);
    test_join_thread(thread);
    PRINT("the callback's wait", inner_result);
    PRINT("the inner semaphore's count at the end", sema_count(inner));
    PRINT("the outer send", outer_result);
    PRINT("the outer send has time left", outer_left > 0);
    char got[4] = { 0 };
    PRINT("receive the message", sceKernelTryReceiveMsgPipe(pipe, got, sizeof(got), MSG_PIPE_MODE_FULL, NULL));
    PRINT("it is the sent message", sceClibMemcmp(got, message, sizeof(message)) == 0);
    sceKernelDeleteSema(inner);
    sceKernelDeleteMsgPipe(pipe);

    test_exit();
    return 0;
}

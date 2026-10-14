// What happens to bytes a message pipe already gave a waiting FULL receive of 8 when that receive
// ends without getting all 8: its timeout passes, the pipe is canceled, or the pipe is deleted. For
// each, the receive's result, the size it reports, what its buffer holds, and what the pipe holds
// after. Also an ASAP receive queued behind the FULL one, and whether it gets those bytes once the
// FULL receive times out. Only receives wait here, so the pipe is never full.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096

static SceUID pipe;
static volatile int result[2], done[2];
static SceSize moved[2];
static char buf[2][9];
static volatile SceUInt timeouts[2];
static volatile SceUInt32 modes[2];

static int receiver(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt timeout = timeouts[i];
    result[i] = sceKernelReceiveMsgPipe(pipe, buf[i], 8, modes[i], &moved[i], &timeout);
    done[i] = 1;
    return 0;
}

static SceUID threads[2];

static void start(int i, SceUInt32 mode, SceUInt timeout) {
    result[i] = 0xDEAD;
    done[i] = 0;
    moved[i] = 0xDEAD;
    sceClibMemset(buf[i], 0, sizeof(buf[i]));
    modes[i] = mode;
    timeouts[i] = timeout;
    threads[i] = sceKernelCreateThread("receiver", receiver, SCE_KERNEL_DEFAULT_PRIORITY_USER, 0x4000, 0, 0, NULL);
    sceKernelStartThread(threads[i], sizeof(i), &i);
    test_wait_until_waiting(threads[i]);
}

static int pipe_bytes(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return CAPACITY - info.freeSize;
}

// Prints what receive i ended with
static void print_receive(const char *name, int i) {
    PRINTF(result[i], "%s: the result", name);
    PRINTF(moved[i], "%s: the size it reports", name);
    PRINTF(sceClibStrnlen(buf[i], 8), "%s: its buffer holds \"%s\"", name, buf[i]);
}

// Receives what the pipe holds without waiting and prints it
static void print_pipe(const char *name) {
    char rest[9] = { 0 };
    SceSize size = 0;
    sceKernelTryReceiveMsgPipe(pipe, rest, 8, MSG_PIPE_MODE_ASAP, &size);
    PRINTF(size, "%s: the pipe still holds \"%s\"", name, rest);
}

int main(void) {
    SceSize size;

    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    start(0, MSG_PIPE_MODE_FULL, 200000);
    sceKernelTrySendMsgPipe(pipe, "abcd", 4, MSG_PIPE_MODE_FULL, &size);
    PRINT("timeout, after a send of 4: the bytes in the pipe", pipe_bytes());
    TEST_WAIT_FOR(done[0]);
    test_join_thread(threads[0]);
    print_receive("timeout", 0);
    PRINT("timeout, after it: the bytes in the pipe", pipe_bytes());
    print_pipe("timeout");
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    start(0, MSG_PIPE_MODE_FULL, TEST_WAIT_US);
    sceKernelTrySendMsgPipe(pipe, "efgh", 4, MSG_PIPE_MODE_FULL, &size);
    int senders = 0xDEAD, receivers = 0xDEAD;
    PRINT("cancel, after a send of 4", sceKernelCancelMsgPipe(pipe, &senders, &receivers));
    PRINT("cancel: the waiting receivers it counts", receivers);
    test_join_thread(threads[0]);
    print_receive("cancel", 0);
    PRINT("cancel, after it: the bytes in the pipe", pipe_bytes());
    print_pipe("cancel");
    sceKernelDeleteMsgPipe(pipe);

    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    start(0, MSG_PIPE_MODE_FULL, TEST_WAIT_US);
    sceKernelTrySendMsgPipe(pipe, "ijkl", 4, MSG_PIPE_MODE_FULL, &size);
    PRINT("delete, after a send of 4", sceKernelDeleteMsgPipe(pipe));
    test_join_thread(threads[0]);
    print_receive("delete", 0);

    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    start(0, MSG_PIPE_MODE_FULL, 200000);
    start(1, MSG_PIPE_MODE_ASAP, TEST_WAIT_US);
    sceKernelTrySendMsgPipe(pipe, "mnop", 4, MSG_PIPE_MODE_FULL, &size);
    const int asap_before = done[1];
    TEST_WAIT_FOR(done[0]);
    // Up to 500 ms for the ASAP receive to end, if the timeout passed it the bytes
    for (int i = 0; i < 500 && !done[1]; i++)
        sceKernelDelayThread(1000);
    const int asap_after = done[1];
    PRINT("an ASAP receive behind it: it was done before the timeout", asap_before);
    PRINT("an ASAP receive behind it: it was done after the timeout", asap_after);
    test_join_thread(threads[0]);
    print_receive("an ASAP receive behind it, the FULL receive", 0);
    if (!done[1])
        sceKernelCancelMsgPipe(pipe, NULL, NULL);
    test_join_thread(threads[1]);
    print_receive("an ASAP receive behind it, the ASAP receive", 1);
    print_pipe("an ASAP receive behind it");
    sceKernelDeleteMsgPipe(pipe);

    test_exit();
    return 0;
}

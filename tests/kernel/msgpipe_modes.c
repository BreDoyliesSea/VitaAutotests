// Message pipe sends and receives that wait, in ASAP and FULL mode: a waiting receive that gets less
// than it asked for, a send that wakes a receive that asked for less, a waiting ASAP send that only
// part of fits, the sizes timed out calls write back, a FULL receive of more than the pipe holds
// while a sender waits with the rest, a receive that doesn't remove the data with a receiver
// waiting, and a send of 0 bytes with a receive waiting. The pipe is filled with '.' bytes where
// it must be full, and the test prints the other bytes it gets back, in order. Left out: receives
// from a full pipe that leave a FULL sender waiting, a receive that doesn't remove the data
// included. On a PS Vita TV on firmware 3.65 such a receive never returned and the console stopped
// responding.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// A Vita rounds a pipe's buffer up to 4 KiB, so the pipe is created with that size
#define CAPACITY 4096

typedef struct Op {
    int send;
    SceUInt32 mode;
    int size;
    const char *data;
} Op;

static SceUID pipe;
static Op ops[2];
static SceUID threads[2];
static volatile int result[2], done[2];
static SceSize moved[2];
static char received[2][16];
static char fill[CAPACITY], drained[CAPACITY];
// The bytes other than '.' the test got back from the pipe
static char seen[64];
static int seen_len;

// The type of the wait thread is in, without the callback bit, or 0 if it doesn't wait
static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    return info.waitType & ~WAIT_TYPE_CB;
}

static int waits_in(SceUID thread, SceUInt32 type) {
    return wait_type(thread) == type;
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Runs ops[i], a send or receive that waits
static int op_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    if (ops[i].send)
        result[i] = sceKernelSendMsgPipe(pipe, (void *)ops[i].data, ops[i].size, ops[i].mode, &moved[i], &timeout);
    else
        result[i] = sceKernelReceiveMsgPipe(pipe, received[i], ops[i].size, ops[i].mode, &moved[i], &timeout);
    done[i] = 1;
    return 0;
}

static void start_op(int i, int send, SceUInt32 mode, int size, const char *data) {
    ops[i] = (Op){ send, mode, size, data };
    result[i] = 0xDEAD;
    done[i] = 0;
    moved[i] = 0xDEAD;
    sceClibMemset(received[i], 0, sizeof(received[i]));
    threads[i] = test_start_thread_arg(op_thread, i);
    test_wait_until_waiting(threads[i]);
}

static int settled(int count) {
    return (count < 1 || done[0] || waits_in(threads[0], WAIT_TYPE_MSG_PIPE)) && (count < 2 || done[1] || waits_in(threads[1], WAIT_TYPE_MSG_PIPE));
}

// Waits until each of the first count ops is done or waits on the pipe, on 20 polls in a row, at
// most 5 s. A thread that was just woken may show its wait until it runs, so one look isn't enough.
static void settle(int count) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        stable = settled(count) ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

static void new_pipe(int filled) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    if (filled)
        sceKernelTrySendMsgPipe(pipe, fill, filled, MSG_PIPE_MODE_FULL, NULL);
    seen_len = 0;
    seen[0] = '\0';
}

static void collect(const char *buf, int size) {
    for (int i = 0; i < size; i++) {
        if (buf[i] != '.' && seen_len < (int)sizeof(seen) - 1)
            seen[seen_len++] = buf[i];
    }
    seen[seen_len] = '\0';
}

// Receives what the pipe holds without waiting, and returns how much it got
static int drain(void) {
    SceSize size = 0;
    if (sceKernelTryReceiveMsgPipe(pipe, drained, CAPACITY, MSG_PIPE_MODE_ASAP, &size) < 0)
        return 0;
    collect(drained, size);
    return size;
}

// Deletes the pipe, which ends any wait still going, and joins the threads
static void end_pipe(int count) {
    sceKernelDeleteMsgPipe(pipe);
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
}

int main(void) {
    sceClibMemset(fill, '.', sizeof(fill));
    SceSize size;
    SceUInt32 timeout;
    SceKernelMppInfo info;

    // A FULL receive of 8 that the pipe gets 4 and then 4 more for
    new_pipe(0);
    start_op(0, 0, MSG_PIPE_MODE_FULL, 8, NULL);
    PRINT("FULL receive of 8: send 4", sceKernelTrySendMsgPipe(pipe, "abcd", 4, MSG_PIPE_MODE_FULL, NULL));
    info = pipe_info();
    PRINT("FULL receive of 8: it still waits after 4", waits_in(threads[0], WAIT_TYPE_MSG_PIPE));
    PRINT("FULL receive of 8: the free size after 4", info.freeSize);
    PRINT("FULL receive of 8: the waiting receivers after 4", info.numReceiveWaitThreads);
    PRINT("FULL receive of 8: send 4 more", sceKernelTrySendMsgPipe(pipe, "efgh", 4, MSG_PIPE_MODE_FULL, NULL));
    settle(1);
    PRINT("FULL receive of 8: its result", result[0]);
    PRINT("FULL receive of 8: the size it got", moved[0]);
    PRINTF(sceClibStrnlen(received[0], 16), "FULL receive of 8: it got \"%s\"", received[0]);
    PRINT("FULL receive of 8: the free size at the end", pipe_info().freeSize);
    end_pipe(1);

    // An ASAP receive of 8 that the pipe gets 3 for
    new_pipe(0);
    start_op(0, 0, MSG_PIPE_MODE_ASAP, 8, NULL);
    sceKernelTrySendMsgPipe(pipe, "xyz", 3, MSG_PIPE_MODE_FULL, NULL);
    settle(1);
    PRINT("ASAP receive of 8, send of 3: its result", result[0]);
    PRINT("ASAP receive of 8, send of 3: the size it got", moved[0]);
    PRINTF(sceClibStrnlen(received[0], 16), "ASAP receive of 8, send of 3: it got \"%s\"", received[0]);
    end_pipe(1);

    // A send of 5 to a waiting receive that asked for 2
    new_pipe(0);
    start_op(0, 0, MSG_PIPE_MODE_ASAP, 2, NULL);
    PRINT("ASAP receive of 2, send of 5: the send", sceKernelTrySendMsgPipe(pipe, "vwxyz", 5, MSG_PIPE_MODE_FULL, NULL));
    settle(1);
    PRINT("ASAP receive of 2, send of 5: the receive's result", result[0]);
    PRINT("ASAP receive of 2, send of 5: the size it got", moved[0]);
    PRINTF(sceClibStrnlen(received[0], 16), "ASAP receive of 2, send of 5: it got \"%s\"", received[0]);
    PRINT("ASAP receive of 2, send of 5: the free size after it", pipe_info().freeSize);
    PRINT("ASAP receive of 2, send of 5: what is left", drain());
    PRINTF(seen_len, "ASAP receive of 2, send of 5: the pipe holds \"%s\"", seen);
    end_pipe(1);

    // An ASAP send of 8 that waits on a full pipe, then gets room for 3
    new_pipe(CAPACITY);
    start_op(0, 1, MSG_PIPE_MODE_ASAP, 8, "ABCDEFGH");
    sceKernelTryReceiveMsgPipe(pipe, drained, 3, MSG_PIPE_MODE_FULL, &size);
    settle(1);
    PRINT("ASAP send of 8 with room for 3: it is done", done[0]);
    PRINT("ASAP send of 8 with room for 3: its result", result[0]);
    PRINT("ASAP send of 8 with room for 3: the size it sent", moved[0]);
    PRINT("ASAP send of 8 with room for 3: the free size after it", pipe_info().freeSize);
    drain();
    settle(1);
    drain();
    PRINTF(seen_len, "ASAP send of 8 with room for 3: the pipe gave back \"%s\"", seen);
    end_pipe(1);

    // Calls whose timeout passes, and the sizes they write back
    new_pipe(CAPACITY - 4);
    size = 0xDEAD;
    timeout = 1000;
    PRINT("timed out FULL send of 8 with room for 4", sceKernelSendMsgPipe(pipe, "ABCDEFGH", 8, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("its size", size);
    PRINT("the free size after it", pipe_info().freeSize);
    sceKernelTrySendMsgPipe(pipe, fill, 4, MSG_PIPE_MODE_FULL, NULL);
    size = 0xDEAD;
    timeout = 1000;
    PRINT("timed out ASAP send of 8 to a full pipe", sceKernelSendMsgPipe(pipe, "ABCDEFGH", 8, MSG_PIPE_MODE_ASAP, &size, &timeout));
    PRINT("its size", size);
    drain();
    sceKernelTrySendMsgPipe(pipe, "abc", 3, MSG_PIPE_MODE_FULL, NULL);
    size = 0xDEAD;
    timeout = 1000;
    char buf[8] = { 0 };
    PRINT("timed out FULL receive of 8 with 3 in the pipe", sceKernelReceiveMsgPipe(pipe, buf, 8, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("its size", size);
    PRINT("the free size after it", pipe_info().freeSize);
    seen_len = 0;
    drain();
    PRINTF(seen_len, "the pipe still holds \"%s\"", seen);
    end_pipe(0);

    // A FULL receive of the whole pipe, which holds 4092, while a FULL send of 8 waits for room
    new_pipe(CAPACITY - 4);
    start_op(0, 1, MSG_PIPE_MODE_FULL, 8, "ABCDEFGH");
    size = 0xDEAD;
    timeout = 200000;
    PRINT("FULL receive of 4096 with 4092 and a sender waiting", sceKernelReceiveMsgPipe(pipe, drained, CAPACITY, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("its size", size);
    if (size <= CAPACITY)
        collect(drained, size);
    settle(1);
    PRINT("the sender is done after it", done[0]);
    PRINT("the free size after it", pipe_info().freeSize);
    drain();
    settle(1);
    drain();
    PRINTF(seen_len, "the receive and the pipe gave back \"%s\"", seen);
    end_pipe(1);
    PRINT("the sender's result", result[0]);

    // A receive that doesn't remove the data, with another receive waiting behind it
    new_pipe(0);
    start_op(0, 0, MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_REMOVE, 4, NULL);
    start_op(1, 0, MSG_PIPE_MODE_FULL, 4, NULL);
    sceKernelTrySendMsgPipe(pipe, "pqrs", 4, MSG_PIPE_MODE_FULL, NULL);
    settle(2);
    PRINT("peek then receive: the peek is done", done[0]);
    PRINTF(sceClibStrnlen(received[0], 16), "peek then receive: the peek got \"%s\"", received[0]);
    PRINT("peek then receive: the receive is done", done[1]);
    PRINTF(sceClibStrnlen(received[1], 16), "peek then receive: the receive got \"%s\"", received[1]);
    PRINT("peek then receive: the free size after the send", pipe_info().freeSize);
    end_pipe(2);

    // A send of 0 bytes while a receive waits
    new_pipe(0);
    start_op(0, 0, MSG_PIPE_MODE_ASAP, 4, NULL);
    size = 0xDEAD;
    PRINT("send of 0 bytes to a waiting receive", sceKernelTrySendMsgPipe(pipe, "", 0, MSG_PIPE_MODE_FULL, &size));
    PRINT("its size", size);
    PRINT("the receive still waits", waits_in(threads[0], WAIT_TYPE_MSG_PIPE));
    sceKernelTrySendMsgPipe(pipe, "k", 1, MSG_PIPE_MODE_FULL, NULL);
    settle(1);
    PRINT("a send of 1 after it: the receive's size", moved[0]);
    PRINTF(sceClibStrnlen(received[0], 16), "a send of 1 after it: the receive got \"%s\"", received[0]);
    end_pipe(1);

    test_exit();
    return 0;
}

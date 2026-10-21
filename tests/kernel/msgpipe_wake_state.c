// What a message pipe and the out values of a waiting thread look like right after a send or a
// receive woke that thread, before the woken thread ran. The main thread and the waiter W are pinned
// to core 0, the main thread at 0x60 and W at 0xB0, so a wake leaves W ready but not running until
// the main thread blocks. W's buffer and its size out value are globals that the main thread sets to
// markers first. Right after the waking call, before any call that blocks, the main thread reads the
// pipe's info (free size and the numbers of waiting senders and receivers), W's size and buffer and
// W's status (2 is ready). Then it blocks to join W and prints what it read, followed by what W
// reports once it ran. Every case uses a fresh pipe of 4096 bytes. The cases:
// - W waits for 4 bytes with a FULL receive and the main thread sends "abcd" with a FULL send
// - W waits for 8 bytes with an ASAP receive and the main thread sends "xy" with a FULL send
// - the pipe holds 4094 bytes, W waits with a FULL send of "pqrs", which doesn't fit, and the main
//   thread receives 4 bytes with an ASAP receive. Then the main thread drains the pipe.
// Only calls with timeouts are used, no try calls. The prints wait until W ended, since printing
// blocks the main thread.
//
// On a PS Vita TV on firmware 3.65 the send copies the bytes straight into the waiting receive's
// buffer and writes its size before the receiver runs, so the pipe stays empty and the receiver is
// ready (0x2). A receive from the full pipe takes its bytes, lets the waiting sender's bytes in and
// writes the sender's size, all before the sender runs.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// The pipe holds one 4 KiB page
#define CAPACITY 4096
#define MAIN_PRIORITY 0x60
#define WAITER_PRIORITY 0xB0
#define NOT_SET 0xDEAD
// The byte W's buffer holds until something writes it
#define MARKER '-'

static SceUID pipe, waiter;
// What W does: a send or a receive, in this mode, of this many bytes
static int op_send;
static SceUInt32 op_mode;
static int op_size;
static const char *op_data;
// W's result and its size out value, which the pipe call writes
static int result;
static SceSize moved;
// What W receives into. 8 marker bytes, then zeros, so the whole buffer is a string.
static char buffer[16];
static char fill[CAPACITY], drained[CAPACITY];

// What the main thread reads right after a waking call
typedef struct Seen {
    int free_size;
    int senders;
    int receivers;
    int status;
    SceSize moved;
    char buffer[16];
} Seen;

// The type of the wait thread is in, without the callback bit, or 0 if it doesn't wait
static SceUInt32 wait_type(SceUID thread) {
    SceKernelThreadInfo info = { .size = sizeof(info) };
    if (sceKernelGetThreadInfo(thread, &info) < 0 || !(info.status & SCE_THREAD_WAITING))
        return 0;
    return info.waitType & ~WAIT_TYPE_CB;
}

static int waiter_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 3 * TEST_WAIT_US;
    if (op_send)
        result = sceKernelSendMsgPipe(pipe, (void *)op_data, op_size, op_mode, &moved, &timeout);
    else
        result = sceKernelReceiveMsgPipe(pipe, buffer, op_size, op_mode, &moved, &timeout);
    return 0;
}

// Starts W with a send or a receive and waits until it waits on the pipe
static void start_waiter(int send, SceUInt32 mode, int size, const char *data) {
    op_send = send;
    op_mode = mode;
    op_size = size;
    op_data = data;
    result = NOT_SET;
    moved = NOT_SET;
    sceClibMemset(buffer, 0, sizeof(buffer));
    sceClibMemset(buffer, MARKER, 8);
    waiter = sceKernelCreateThread("waiter", waiter_entry, WAITER_PRIORITY, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_0, NULL);
    sceKernelStartThread(waiter, 0, NULL);
    TEST_WAIT_FOR(wait_type(waiter) == WAIT_TYPE_MSG_PIPE);
}

// Reads the pipe and W without a call that blocks
static Seen look(void) {
    Seen seen = { 0 };
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    SceKernelThreadInfo thread_info = { .size = sizeof(thread_info) };
    sceKernelGetThreadInfo(waiter, &thread_info);
    seen.free_size = info.freeSize;
    seen.senders = info.numSendWaitThreads;
    seen.receivers = info.numReceiveWaitThreads;
    seen.status = thread_info.status;
    seen.moved = moved;
    sceClibMemcpy(seen.buffer, buffer, sizeof(buffer) - 1);
    return seen;
}

static void print_seen(const char *name, const Seen *seen, int show_buffer) {
    PRINTF(seen->free_size, "%s: right after the wake: the pipe's free size", name);
    PRINTF(seen->senders, "%s: right after the wake: the waiting senders", name);
    PRINTF(seen->receivers, "%s: right after the wake: the waiting receivers", name);
    PRINTF(seen->moved, "%s: right after the wake: W's size", name);
    if (show_buffer)
        PRINTF(sceClibStrnlen(seen->buffer, 16), "%s: right after the wake: W's buffer \"%s\"", name, seen->buffer);
    PRINTF(seen->status, "%s: right after the wake: W's status", name);
}

static void print_after(const char *name, int show_buffer) {
    PRINTF(result, "%s: after W ran: its result", name);
    PRINTF(moved, "%s: after W ran: its size", name);
    if (show_buffer)
        PRINTF(sceClibStrnlen(buffer, 16), "%s: after W ran: its buffer \"%s\"", name, buffer);
}

// W waits to receive, then the main thread sends it data with a FULL send
static void receiver_case(const char *name, SceUInt32 mode, int size, const char *data, int len) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    start_waiter(0, mode, size, NULL);
    const int waits = wait_type(waiter) == WAIT_TYPE_MSG_PIPE;
    SceUInt32 timeout = TEST_WAIT_US;
    SceSize sent = NOT_SET;
    const int wake = sceKernelSendMsgPipe(pipe, (void *)data, len, MSG_PIPE_MODE_FULL, &sent, &timeout);
    const Seen seen = look();
    test_join_thread(waiter);
    PRINTF(waits, "%s: W waits before the send", name);
    PRINTF(wake, "%s: the send", name);
    PRINTF(sent, "%s: the send's size", name);
    print_seen(name, &seen, 1);
    print_after(name, 1);
    sceKernelDeleteMsgPipe(pipe);
}

// The pipe holds 4094 bytes and W waits to send 4 more, which don't fit. The main thread receives 4
// bytes with an ASAP receive, which makes room for them.
static void sender_case(const char *name) {
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    SceUInt32 timeout = TEST_WAIT_US;
    SceSize got = NOT_SET;
    const int filled = sceKernelSendMsgPipe(pipe, fill, CAPACITY - 2, MSG_PIPE_MODE_FULL, &got, &timeout);
    start_waiter(1, MSG_PIPE_MODE_FULL, 4, "pqrs");
    const int waits = wait_type(waiter) == WAIT_TYPE_MSG_PIPE;
    const Seen before = look();
    timeout = TEST_WAIT_US;
    got = NOT_SET;
    const int wake = sceKernelReceiveMsgPipe(pipe, drained, 4, MSG_PIPE_MODE_ASAP, &got, &timeout);
    const SceSize received = got;
    const Seen seen = look();
    test_join_thread(waiter);
    PRINTF(filled, "%s: the fill of 4094", name);
    PRINTF(waits, "%s: W waits before the receive", name);
    PRINTF(before.free_size, "%s: while W waits: the pipe's free size", name);
    PRINTF(before.senders, "%s: while W waits: the waiting senders", name);
    PRINTF(wake, "%s: the receive", name);
    PRINTF(received, "%s: the receive's size", name);
    print_seen(name, &seen, 0);
    print_after(name, 0);

    // W's send is done, so no sender waits and the blocking receive that drains the pipe returns
    timeout = TEST_WAIT_US;
    got = NOT_SET;
    PRINTF(sceKernelReceiveMsgPipe(pipe, drained, CAPACITY - 2, MSG_PIPE_MODE_FULL, &got, &timeout), "%s: the drain", name);
    PRINTF(got, "%s: the drain's size", name);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(0, MAIN_PRIORITY);
    // The main thread only moves to core 0 when it is next scheduled
    sceKernelDelayThread(1000);
    sceClibMemset(fill, '.', sizeof(fill));

    receiver_case("receive FULL 4, send FULL 4", MSG_PIPE_MODE_FULL, 4, "abcd", 4);
    receiver_case("receive ASAP 8, send FULL 2", MSG_PIPE_MODE_ASAP, 8, "xy", 2);
    sender_case("send FULL 4 with 2 free, receive ASAP 4");

    test_exit();
    return 0;
}

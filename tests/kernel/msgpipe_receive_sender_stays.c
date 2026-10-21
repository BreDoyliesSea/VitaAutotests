// A blocking message pipe receive that leaves a sender waiting. Each case uses a fresh pipe of 4096
// bytes, filled with the letters a to z over and over up to the room the case wants. Then the
// senders queue one at a time, each in its own thread with a long timeout, and this thread receives
// with a timeout. It prints the receive's result, its size and its bytes as a string, which
// senders still wait right after it, the pipe's free size and waiting senders, and then, after the
// pipe is canceled, each sender's result and the size it reports, and the free size. The cases:
// - a full pipe, one FULL sender of 10, a receive of 1 in ASAP mode
// - a full pipe, FULL senders of 4 and 10, a receive of 8 in FULL mode
// - a full pipe, ASAP senders of 1, 1 and 1, a receive of 2 in ASAP mode
// - a pipe with 5 bytes of room, one FULL sender of 20 that doesn't fit, a receive of 1 in ASAP mode
// Only blocking calls send and receive here. No try send or try receive runs, since a try receive
// that leaves a sender waiting can fail to return. The senders left are ended by the cancel, and the
// pipe is deleted after it. This test goes last, since a receive that doesn't return would stop
// everything after it.
//
// On a PS Vita TV on firmware 3.65 every receive returns 0 at once with the bytes from the front of
// the pipe. Then the waiting senders are served in order into the room the receive made, until one
// doesn't fit. The FULL sender of 4 is served and the one of 10 still waits, two of the three ASAP
// senders of 1 are served, which fills the pipe again, and a FULL sender that doesn't fit still
// waits. The cancel ends the senders left with 0x80028007 and a size of 0, and empties the pipe.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// A Vita rounds a pipe's buffer up to 4 KiB, so the pipe is created with that size
#define CAPACITY 4096
#define MAX_SENDERS 3

typedef struct Sender {
    SceUInt32 mode;
    int size;
    const char *data;
} Sender;

typedef struct Case {
    const char *name;
    // The bytes of room the pipe has when the senders queue
    int room;
    int sender_count;
    Sender senders[MAX_SENDERS];
    SceUInt32 receive_mode;
    int receive_size;
} Case;

static const Case *current;
static SceUID pipe;
static SceUID threads[MAX_SENDERS];
static volatile int result[MAX_SENDERS], done[MAX_SENDERS];
static SceSize moved[MAX_SENDERS];
static char fill[CAPACITY];

static int sender_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    const Sender *sender = &current->senders[i];
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    result[i] = sceKernelSendMsgPipe(pipe, (void *)sender->data, sender->size, sender->mode, &moved[i], &timeout);
    done[i] = 1;
    return 0;
}

static int still_waits(int i) {
    return !done[i] && test_in_wait(threads[i], WAIT_TYPE_MSG_PIPE, pipe);
}

// Waits until every sender is done or waits on the pipe, on 20 polls in a row, at most 5 s. A thread
// that was just woken may show its wait until it runs, so one look isn't enough.
static void settle(int count) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        int all = 1;
        for (int i = 0; i < count; i++)
            all = all && (done[i] || still_waits(i));
        stable = all ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

static void run_case(const Case *c) {
    const char *name = c->name;
    current = c;
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);

    // Fills the pipe with a blocking send, which has room for all of it and returns at once
    SceSize filled = 0;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelSendMsgPipe(pipe, fill, CAPACITY - c->room, MSG_PIPE_MODE_FULL, &filled, &timeout);

    for (int i = 0; i < c->sender_count; i++) {
        result[i] = 0xDEAD;
        done[i] = 0;
        moved[i] = 0xDEAD;
        threads[i] = test_start_thread_arg(sender_thread, i);
        test_wait_until_waiting(threads[i]);
    }
    const SceKernelMppInfo before = pipe_info();

    char received[16] = { 0 };
    SceSize size = 0xDEAD;
    timeout = TEST_WAIT_US;
    const int received_result = sceKernelReceiveMsgPipe(pipe, received, c->receive_size, c->receive_mode, &size, &timeout);

    // Every other thread of the test is blocked or done after this, so the values can be read
    settle(c->sender_count);
    int waits[MAX_SENDERS] = { 0 };
    for (int i = 0; i < c->sender_count; i++)
        waits[i] = still_waits(i);
    const SceKernelMppInfo after = pipe_info();

    int canceled_senders = -1, canceled_receivers = -1;
    const int canceled = sceKernelCancelMsgPipe(pipe, &canceled_senders, &canceled_receivers);
    for (int i = 0; i < c->sender_count; i++)
        test_join_thread(threads[i]);
    const SceKernelMppInfo ended = pipe_info();

    PRINTF(before.freeSize, "%s: the free size before the receive", name);
    PRINTF(before.numSendWaitThreads, "%s: the waiting senders before the receive", name);
    PRINTF(received_result, "%s: the receive", name);
    PRINTF(size, "%s: the receive gets \"%s\", size", name, received);
    for (int i = 0; i < c->sender_count; i++)
        PRINTF(waits[i], "%s: sender %d still waits", name, i + 1);
    PRINTF(after.freeSize, "%s: the free size after the receive", name);
    PRINTF(after.numSendWaitThreads, "%s: the waiting senders after the receive", name);
    PRINTF(canceled, "%s: cancel the pipe", name);
    PRINTF(canceled_senders, "%s: the senders the cancel ended", name);
    for (int i = 0; i < c->sender_count; i++) {
        PRINTF(result[i], "%s: sender %d's result", name, i + 1);
        PRINTF(moved[i], "%s: sender %d's size sent", name, i + 1);
    }
    PRINTF(ended.freeSize, "%s: the free size after the cancel", name);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    for (int i = 0; i < CAPACITY; i++)
        fill[i] = 'a' + i % 26;

    static const Case cases[] = {
        { "full pipe, FULL sender of 10, receive ASAP 1", 0, 1,
          { { MSG_PIPE_MODE_FULL, 10, "ABCDEFGHIJ" } },
          MSG_PIPE_MODE_ASAP, 1 },
        { "full pipe, FULL senders of 4 and 10, receive FULL 8", 0, 2,
          { { MSG_PIPE_MODE_FULL, 4, "KLMN" }, { MSG_PIPE_MODE_FULL, 10, "OPQRSTUVWX" } },
          MSG_PIPE_MODE_FULL, 8 },
        { "full pipe, ASAP senders of 1, 1 and 1, receive ASAP 2", 0, 3,
          { { MSG_PIPE_MODE_ASAP, 1, "Y" }, { MSG_PIPE_MODE_ASAP, 1, "Z" }, { MSG_PIPE_MODE_ASAP, 1, "W" } },
          MSG_PIPE_MODE_ASAP, 2 },
        { "5 bytes of room, FULL sender of 20, receive ASAP 1", 5, 1,
          { { MSG_PIPE_MODE_FULL, 20, "ABCDEFGHIJKLMNOPQRST" } },
          MSG_PIPE_MODE_ASAP, 1 },
    };
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);

    test_exit();
    return 0;
}

// A FULL or ASAP try receive of a message pipe while a sender waits, which asks for more bytes than
// the pipe holds when the sender's bytes cover the rest. Each case uses a fresh pipe of 4096 bytes,
// filled with the letters a to z over and over up to the bytes the case wants it to hold. When the
// case has a sender, a FULL sender of 8 bytes "ABCDEFGH" then queues in its own thread and waits,
// since its 8 bytes don't fit in the room the pipe has. This thread then try receives. It prints
// the pipe's free size and waiting senders before, the call's result, the size it writes (preset to
// 0xDEAD), the first and last 4 bytes it received as strings, whether the sender is done, and the
// pipe's free size and waiting senders after. Then it ends the sender with a cancel if it still
// waits, prints the sender's result and the size it reports, and prints what the pipe still holds,
// taken with one more try receive of everything. The cases:
// - 4092 bytes held, a FULL sender of 8, a FULL try receive of 4096
// - 4096 bytes held, a FULL sender of 8, a FULL try receive of 4096
// - 4092 bytes held, a FULL sender of 8, an ASAP try receive of 16
// - 4000 bytes held, no sender, a FULL try receive of 4096, which asks for more than there is
//
// A try receive that leaves a sender waiting can freeze a console, so each case with a sender makes
// room for the sender's rest. The try receive only runs when the pipe is in the state a case
// expects and leaves_no_sender() holds for it. The room each case makes, where a try receive takes
// bytes from the pipe first and then from the sender, and then the sender puts its rest into the
// pipe:
// - case 1: takes 4092 from the pipe and 4 from the sender. The sender's rest of 4 goes into a pipe
//   that is empty after the take, with 4096 free.
// - case 2: takes 4096 from the pipe and nothing from the sender. The sender's 8 go into a pipe
//   that is empty after the take, with 4096 free.
// - case 3: takes 16 from the pipe and nothing from the sender. The sender's 8 go into a pipe with
//   4076 bytes left, which has 20 free.
// - case 4: no sender, so nobody can be left waiting.
// The pipe is deleted at the end of each case.
//
// On a PS Vita TV on firmware 3.65 a FULL try receive counts the bytes of waiting senders. With
// 4092 bytes held and a FULL sender of 8 waiting it takes the 4092 and the sender's first 4, and
// the sender then puts its last 4 into the pipe and is done. A FULL try receive that the pipe and
// its senders can't cover fails with 0x80028223 and writes no size.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

// A Vita rounds a pipe's buffer up to 4 KiB, so the pipe is created with that size
#define CAPACITY 4096
#define SENDER_SIZE 8

typedef struct Case {
    const char *name;
    // The bytes the pipe holds when the sender queues
    int held;
    // Whether a FULL sender of SENDER_SIZE bytes waits when the try receive runs
    int has_sender;
    SceUInt32 mode;
    int asked;
} Case;

static SceUID pipe;
static SceUID sender;
static volatile int send_result, send_done;
static SceSize send_moved;
static char sender_bytes[] = "ABCDEFGH";
static char fill[CAPACITY];
static char received[CAPACITY];
static char drained[CAPACITY];

static int sender_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt32 timeout = 2 * TEST_WAIT_US;
    send_result = sceKernelSendMsgPipe(pipe, sender_bytes, SENDER_SIZE, MSG_PIPE_MODE_FULL, &send_moved, &timeout);
    send_done = 1;
    return 0;
}

static int sender_waits(void) {
    return !send_done && test_in_wait(sender, WAIT_TYPE_MSG_PIPE, pipe);
}

static int min(int a, int b) {
    return a < b ? a : b;
}

// Returns whether the sender has room for the rest of its bytes after a try receive of the case,
// which takes from the pipe first, then from the sender, and then lets the sender put its rest into
// the pipe. A case without a sender leaves nobody waiting.
static int leaves_no_sender(const Case *c) {
    if (!c->has_sender)
        return 1;
    const int from_pipe = min(c->held, c->asked);
    const int from_sender = min(c->asked - from_pipe, SENDER_SIZE);
    return SENDER_SIZE - from_sender <= CAPACITY - (c->held - from_pipe);
}

// Waits until the sender is done or waits on the pipe, on 20 polls in a row, at most 5 s. A thread
// that was just woken may show its wait until it runs, so one look isn't enough.
static void settle(void) {
    int stable = 0;
    for (int poll = 0; poll < 5000 && stable < 20; poll++) {
        stable = send_done || sender_waits() ? stable + 1 : 0;
        if (stable < 20)
            sceKernelDelayThread(1000);
    }
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Copies the first 4 and the last 4 of the first size bytes of buf as strings, empty when size is
// not between 4 and the pipe's capacity
static void ends(const char *buf, int size, char *first, char *last) {
    sceClibMemset(first, 0, 5);
    sceClibMemset(last, 0, 5);
    if (size >= 4 && size <= CAPACITY) {
        sceClibMemcpy(first, buf, 4);
        sceClibMemcpy(last, buf + size - 4, 4);
    }
}

static void run_case(const Case *c) {
    const char *name = c->name;
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);

    // Fills the pipe with a try send, which has room for all of it and returns at once
    SceSize filled = 0;
    sceKernelTrySendMsgPipe(pipe, fill, c->held, MSG_PIPE_MODE_FULL, &filled);

    send_result = 0xDEAD;
    send_done = 0;
    send_moved = 0xDEAD;
    if (c->has_sender) {
        sender = test_start_thread(sender_thread);
        TEST_WAIT_FOR(send_done || test_in_wait(sender, WAIT_TYPE_MSG_PIPE, pipe));
    }
    // Every other thread of the test is blocked, so the pipe's state can be read. The try receive
    // only runs when the pipe is in the state the case's arithmetic is for.
    const SceKernelMppInfo before = pipe_info();
    const int ready = before.freeSize == CAPACITY - c->held && before.numSendWaitThreads == c->has_sender && (!c->has_sender || sender_waits()) && leaves_no_sender(c);

    sceClibMemset(received, 0, sizeof(received));
    SceSize size = 0xDEAD;
    int receive_result = 0xDEAD;
    if (ready)
        receive_result = sceKernelTryReceiveMsgPipe(pipe, received, c->asked, c->mode, &size);

    if (c->has_sender)
        settle();
    const int waits = c->has_sender && sender_waits();
    const int done = send_done;
    const SceKernelMppInfo after = pipe_info();

    // A sender that still waits is ended by a cancel, which also empties the pipe
    if (waits)
        sceKernelCancelMsgPipe(pipe, NULL, NULL);
    if (c->has_sender)
        test_join_thread(sender);

    // Nobody waits on the pipe now, so a try receive of all of it can't freeze
    sceClibMemset(drained, 0, sizeof(drained));
    SceSize left = 0xDEAD;
    sceKernelTryReceiveMsgPipe(pipe, drained, CAPACITY, MSG_PIPE_MODE_ASAP, &left);

    char first[5], last[5], left_first[5], left_last[5];
    ends(received, c->asked, first, last);
    ends(drained, left, left_first, left_last);

    PRINTF(ready, "%s: the pipe is ready for the try receive", name);
    PRINTF(before.freeSize, "%s: the free size before the try receive", name);
    PRINTF(before.numSendWaitThreads, "%s: the waiting senders before the try receive", name);
    PRINTF(receive_result, "%s: the try receive", name);
    PRINTF(size, "%s: the try receive gets \"%s\" to \"%s\", size", name, first, last);
    if (c->has_sender)
        PRINTF(done, "%s: the sender is done after the try receive", name);
    PRINTF(after.freeSize, "%s: the free size after the try receive", name);
    PRINTF(after.numSendWaitThreads, "%s: the waiting senders after the try receive", name);
    if (c->has_sender) {
        PRINTF(send_result, "%s: the sender's result", name);
        PRINTF(send_moved, "%s: the sender's size sent", name);
    }
    PRINTF(left, "%s: the pipe then holds \"%s\" to \"%s\", size", name, left_first, left_last);
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    for (int i = 0; i < CAPACITY; i++)
        fill[i] = 'a' + i % 26;

    static const Case cases[] = {
        { "FULL 4096 with 4092 held and a FULL sender of 8", CAPACITY - 4, 1, MSG_PIPE_MODE_FULL, CAPACITY },
        { "FULL 4096 with a full pipe and a FULL sender of 8", CAPACITY, 1, MSG_PIPE_MODE_FULL, CAPACITY },
        { "ASAP 16 with 4092 held and a FULL sender of 8", CAPACITY - 4, 1, MSG_PIPE_MODE_ASAP, 16 },
        { "FULL 4096 with 4000 held and no sender", 4000, 0, MSG_PIPE_MODE_FULL, CAPACITY },
    };
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run_case(&cases[i]);

    test_exit();
    return 0;
}

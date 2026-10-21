// What a message pipe holds after a send hands bytes to a waiting receive that doesn't remove them
// (DONT_REMOVE), and what the plain receives and sends after it see. In each case receivers wait on
// an empty pipe and a blocking FULL send serves them. Then the test sends short messages and
// receives them, one pair at a time, and prints each result, each size, the bytes received as a
// short string and the pipe's free size from its info. The bytes are letters, so a receive that
// gets bytes other than the ones just sent shows. The cases are:
// - peek and a receive: receiver 1 waits FULL with DONT_REMOVE for 4 bytes and receiver 2 waits
//   FULL for 4 bytes, then "pqrs" is sent. Then "tu", "vwx" and "yz" are each sent and received.
// - peek alone: only the DONT_REMOVE receiver waits, then "pqrs" is sent. The pipe is received
//   from at once, then the same three messages go through.
// - short peek: an ASAP receiver with DONT_REMOVE waits for 2 bytes, then "abcd" is sent. The pipe
//   is received from at once, then "ef", "ghi" and "jk" go through.
// Each case ends with a receive on the idle pipe that times out, a cancel, and two more pairs
// ("ab" and "cd") to show what the cancel changed. Only blocking calls with timeouts are used,
// and no sender ever waits on the pipe, so no call here runs while a sender waits. While
// receivers wait, the only calls on the pipe are the info call and the one send that serves them.
//
// On a PS Vita TV on firmware 3.65 the pipe is out of step after the send that served the
// DONT_REMOVE receiver. Each later receive gets bytes from before the last send, with the right
// size. In the first two cases "tu" comes back as "pq", "vwx" as "rst" and "yz" as "uv". In the
// short peek case the receive right after the send gets "abab", and the later ones lag in the same
// way. The free sizes stay right, and after the cancel the pipe is in step again.
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define CAPACITY 4096
#define NOT_WRITTEN 0xDEAD
// How long a receive on an idle pipe waits, nothing else can end it
#define IDLE_TIMEOUT_US 10000

typedef struct Receiver {
    SceUInt32 mode;
    int size;
} Receiver;

static SceUID pipe;
static Receiver receivers[2];
static volatile int result[2];
static SceSize moved[2];
static char received[2][16];

static int receiver_thread(SceSize args, void *argp) {
    (void)args;
    const int i = *(const int *)argp;
    SceUInt32 timeout = TEST_WAIT_US;
    result[i] = sceKernelReceiveMsgPipe(pipe, received[i], receivers[i].size, receivers[i].mode, &moved[i], &timeout);
    return 0;
}

static SceKernelMppInfo pipe_info(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info;
}

// Sends msg with a blocking FULL send and prints what it returns
static void send_message(const char *name, const char *msg) {
    SceSize size = NOT_WRITTEN;
    SceUInt32 timeout = TEST_WAIT_US;
    PRINTF(sceKernelSendMsgPipe(pipe, (void *)msg, sceClibStrnlen(msg, 16), MSG_PIPE_MODE_FULL, &size, &timeout), "%s: send \"%s\"", name, msg);
    PRINTF(size, "%s: send \"%s\", its size", name, msg);
}

// Receives up to 16 bytes with a blocking ASAP receive and prints what it returns, the bytes it
// got and the pipe's free size after it
static void receive_message(const char *name, const char *what, SceUInt32 timeout_us) {
    char buf[32] = { 0 };
    SceSize size = NOT_WRITTEN;
    SceUInt32 timeout = timeout_us;
    PRINTF(sceKernelReceiveMsgPipe(pipe, buf, 16, MSG_PIPE_MODE_ASAP, &size, &timeout), "%s: receive %s", name, what);
    PRINTF(size, "%s: receive %s, it got \"%s\", its size", name, what, buf);
    PRINTF(pipe_info().freeSize, "%s: receive %s, the free size after it", name, what);
}

// Sends one message and receives from the pipe right after
static void round_trip(const char *name, const char *msg) {
    char what[40];
    send_message(name, msg);
    PRINTF(pipe_info().freeSize, "%s: send \"%s\", the free size after it", name, msg);
    sceClibSnprintf(what, sizeof(what), "after \"%s\"", msg);
    receive_message(name, what, TEST_WAIT_US);
}

// Waits receivers on a new pipe, sends first, then runs the later messages through, then idles,
// cancels, and runs two more messages through
static void run_case(const char *name, const Receiver *list, int count, const char *first, int receive_first, const char *const *later, int later_count) {
    SceUID threads[2];
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, CAPACITY, NULL);
    for (int i = 0; i < count; i++) {
        receivers[i] = list[i];
        result[i] = NOT_WRITTEN;
        moved[i] = NOT_WRITTEN;
        sceClibMemset(received[i], 0, sizeof(received[i]));
        threads[i] = test_start_thread_arg(receiver_thread, i);
        test_wait_until_waiting(threads[i]);
        PRINTF(test_in_wait(threads[i], WAIT_TYPE_MSG_PIPE, pipe), "%s: receiver %d waits on the pipe", name, i + 1);
    }
    PRINTF(pipe_info().numReceiveWaitThreads, "%s: the waiting receivers before the send", name);
    send_message(name, first);
    for (int i = 0; i < count; i++)
        test_join_thread(threads[i]);
    for (int i = 0; i < count; i++) {
        PRINTF(result[i], "%s: receiver %d's result", name, i + 1);
        PRINTF(moved[i], "%s: receiver %d got \"%s\", its size", name, i + 1, received[i]);
    }
    PRINTF(pipe_info().numReceiveWaitThreads, "%s: the waiting receivers after the send", name);
    PRINTF(pipe_info().freeSize, "%s: the free size after the receivers ended", name);
    if (receive_first)
        receive_message(name, "right after it", TEST_WAIT_US);

    for (int i = 0; i < later_count; i++)
        round_trip(name, later[i]);

    // Nothing was sent, so this ends by its timeout
    receive_message(name, "on the idle pipe", IDLE_TIMEOUT_US);

    PRINTF(sceKernelCancelMsgPipe(pipe, NULL, NULL), "%s: cancel", name);
    PRINTF(pipe_info().freeSize, "%s: the free size after the cancel", name);
    round_trip(name, "ab");
    round_trip(name, "cd");
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    static const Receiver peek_and_receive[2] = { { MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_REMOVE, 4 }, { MSG_PIPE_MODE_FULL, 4 } };
    static const Receiver peek_alone[1] = { { MSG_PIPE_MODE_FULL | MSG_PIPE_MODE_DONT_REMOVE, 4 } };
    static const Receiver short_peek[1] = { { MSG_PIPE_MODE_ASAP | MSG_PIPE_MODE_DONT_REMOVE, 2 } };
    static const char *const messages[] = { "tu", "vwx", "yz" };
    static const char *const short_messages[] = { "ef", "ghi", "jk" };

    run_case("peek and a receive", peek_and_receive, 2, "pqrs", 0, messages, 3);
    run_case("peek alone", peek_alone, 1, "pqrs", 1, messages, 3);
    run_case("short peek", short_peek, 1, "abcd", 1, short_messages, 3);

    test_exit();
    return 0;
}

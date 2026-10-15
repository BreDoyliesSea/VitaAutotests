// The size the message pipe calls write when they can't move the bytes they ask for: the try calls
// and the waiting calls with a timeout of 0, plain and with a vector of two pieces, in ASAP and
// FULL mode. A receive of 8 bytes runs on an empty pipe, on one with 3 bytes, and on an empty one
// while another receiver waits. A send of 8 runs on a full pipe, on one with room for 4, and on a
// full one while another sender waits. Each case sets the size to 0xDEAD first, and prints the
// pipe's free size before the call, the call's result and the size after it.
//
// On a PS Vita TV on firmware 3.65 a waiting call whose timeout of 0 passes always writes the size
// it moved: 0, or for a FULL receive the bytes it took. A try call writes its size only in ASAP
// mode while no other thread waits on its side of the pipe. One that fails in FULL mode, or while
// another thread waits, writes nothing. A pipe created with 16 bytes holds 4096.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define PIPE_SIZE 4096
#define ASKED 8

static SceUID pipe;
static char fill[PIPE_SIZE];

static int receiver_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    char buf[ASKED];
    SceSize got;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelReceiveMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL, &got, &timeout);
    return 0;
}

static int sender_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    char buf[ASKED] = { 0 };
    SceSize sent;
    SceUInt32 timeout = TEST_WAIT_US;
    sceKernelSendMsgPipe(pipe, buf, sizeof(buf), MSG_PIPE_MODE_FULL, &sent, &timeout);
    return 0;
}

enum Form { TRY, TRY_VECTOR, TIMEOUT_0, TIMEOUT_0_VECTOR, FORM_COUNT };
static const char *const form_names[FORM_COUNT] = { "try", "try vector", "timeout 0", "timeout 0 vector" };

enum Situation { EMPTY, SOME_DATA, RECEIVER_WAITS, FULL_PIPE, SOME_ROOM, SENDER_WAITS };
static const char *const situation_names[] = {
    "empty pipe", "3 bytes there", "empty pipe with a receiver waiting",
    "full pipe", "room for 4", "full pipe with a sender waiting",
};

// Moves ASKED bytes to or from buf with form and mode, writing the size to *size
static int transfer(int form, int receive, SceUInt32 mode, char *buf, SceSize *size) {
    const MsgPipeVector pieces[2] = { { buf, ASKED / 2 }, { buf + ASKED / 2, ASKED / 2 } };
    SceUInt32 timeout = 0;
    switch (form) {
    case TRY:
        return receive ? sceKernelTryReceiveMsgPipe(pipe, buf, ASKED, mode, size) : sceKernelTrySendMsgPipe(pipe, buf, ASKED, mode, size);
    case TRY_VECTOR:
        return receive ? sceKernelTryReceiveMsgPipeVector(pipe, pieces, 2, mode, size) : sceKernelTrySendMsgPipeVector(pipe, pieces, 2, mode, size);
    case TIMEOUT_0:
        return receive ? sceKernelReceiveMsgPipe(pipe, buf, ASKED, mode, size, &timeout) : sceKernelSendMsgPipe(pipe, buf, ASKED, mode, size, &timeout);
    default:
        return receive ? sceKernelReceiveMsgPipeVector(pipe, pieces, 2, mode, size, &timeout) : sceKernelSendMsgPipeVector(pipe, pieces, 2, mode, size, &timeout);
    }
}

static int free_size(void) {
    SceKernelMppInfo info = { .size = sizeof(info) };
    sceKernelGetMsgPipeInfo(pipe, &info);
    return info.freeSize;
}

static void run_case(int form, SceUInt32 mode, int situation) {
    const int receive = situation <= RECEIVER_WAITS;
    pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, PIPE_SIZE, NULL);
    SceSize ignored;
    SceUID waiter = -1;
    switch (situation) {
    case SOME_DATA:
        sceKernelTrySendMsgPipe(pipe, fill, 3, MSG_PIPE_MODE_FULL, &ignored);
        break;
    case RECEIVER_WAITS:
        waiter = test_start_thread(receiver_entry);
        TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_MSG_PIPE, pipe));
        break;
    case FULL_PIPE:
        sceKernelTrySendMsgPipe(pipe, fill, PIPE_SIZE, MSG_PIPE_MODE_FULL, &ignored);
        break;
    case SOME_ROOM:
        sceKernelTrySendMsgPipe(pipe, fill, PIPE_SIZE - 4, MSG_PIPE_MODE_FULL, &ignored);
        break;
    case SENDER_WAITS:
        sceKernelTrySendMsgPipe(pipe, fill, PIPE_SIZE, MSG_PIPE_MODE_FULL, &ignored);
        waiter = test_start_thread(sender_entry);
        TEST_WAIT_FOR(test_in_wait(waiter, WAIT_TYPE_MSG_PIPE, pipe));
        break;
    default:
        break;
    }

    char buf[ASKED] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    const int free_before = free_size();
    SceSize size = 0xDEAD;
    const int r = transfer(form, receive, mode, buf, &size);
    const char *const op = receive ? "receive" : "send";
    const char *const mode_name = mode == MSG_PIPE_MODE_FULL ? "FULL" : "ASAP";
    PRINTF(free_before, "%s %s %s, %s: the free size before", form_names[form], op, mode_name, situation_names[situation]);
    PRINTF(r, "%s %s %s, %s: the result", form_names[form], op, mode_name, situation_names[situation]);
    PRINTF(size, "%s %s %s, %s: the size", form_names[form], op, mode_name, situation_names[situation]);

    // A cancel ends the other thread's wait. A receive here could freeze a Vita while a sender waits.
    if (waiter >= 0) {
        sceKernelCancelMsgPipe(pipe, NULL, NULL);
        test_join_thread(waiter);
    }
    sceKernelDeleteMsgPipe(pipe);
}

int main(void) {
    pipe = sceKernelCreateMsgPipe("small", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    PRINT("a pipe created with 16 bytes: its free size", free_size());
    sceKernelDeleteMsgPipe(pipe);
    for (int form = 0; form < FORM_COUNT; form++)
        for (int situation = EMPTY; situation <= SENDER_WAITS; situation++) {
            run_case(form, MSG_PIPE_MODE_ASAP, situation);
            run_case(form, MSG_PIPE_MODE_FULL, situation);
        }
    test_exit();
    return 0;
}

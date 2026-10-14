// Message pipe vector sends and receives: how the pieces join and split, the sizes they report, the
// errors for bad vector lists, long lists of empty pieces, pieces that add up to more than or
// exactly what the pipe holds, and lists of empty pieces in each mode.
//
// On a PS Vita TV on firmware 3.65 a list takes at least 8192 pieces, and only the size of all its
// pieces together is checked against what the pipe holds, failing with 0x8002000B. A list of only
// empty pieces ends at once with a size of 0 in both modes, unlike a plain ASAP call of 0 bytes,
// which needs a byte to move (msgpipe_edges). Every piece needs a buffer, even an empty one.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define MAX_PIECES 8192

static MsgPipeVector many[MAX_PIECES];
static char piece;
static char big[6000];

int main(void) {
    for (int i = 0; i < MAX_PIECES; i++)
        many[i] = (MsgPipeVector){ &piece, 0 };
    const SceUID pipe = sceKernelCreateMsgPipe("pipe", MSG_PIPE_TYPE_USER, 0, 4096, NULL);
    char a[3] = { 'a', 'b', 'c' }, b[2] = { 'd', 'e' }, c[4] = { 'f', 'g', 'h', 'i' };
    MsgPipeVector send[3] = { { a, sizeof(a) }, { b, sizeof(b) }, { c, sizeof(c) } };
    SceSize size = 0xDEAD;
    SceUInt32 timeout = 1000;

    PRINT("send three pieces", sceKernelSendMsgPipeVector(pipe, send, 3, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("the size sent", size);
    char x[4] = { 0 }, y[5] = { 0 };
    MsgPipeVector receive[2] = { { x, sizeof(x) }, { y, sizeof(y) } };
    size = 0xDEAD;
    timeout = 1000;
    PRINT("receive into two pieces", sceKernelReceiveMsgPipeVector(pipe, receive, 2, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("the size received", size);
    PRINT("the first piece holds the first 4 bytes", sceClibMemcmp(x, "abcd", 4) == 0);
    PRINT("the second piece holds the rest", sceClibMemcmp(y, "efghi", 5) == 0);

    // Pieces of 0 bytes and an empty list
    MsgPipeVector empty_pieces[2] = { { a, 0 }, { b, 0 } };
    size = 0xDEAD;
    timeout = 1000;
    PRINT("send two empty pieces", sceKernelSendMsgPipeVector(pipe, empty_pieces, 2, MSG_PIPE_MODE_ASAP, &size, &timeout));
    PRINT("the size sent", size);
    size = 0xDEAD;
    timeout = 1000;
    PRINT("send a list of 0 pieces", sceKernelSendMsgPipeVector(pipe, send, 0, MSG_PIPE_MODE_ASAP, &size, &timeout));
    PRINT("the size sent", size);

    // ASAP takes what is there
    sceKernelSendMsgPipeVector(pipe, send, 1, MSG_PIPE_MODE_FULL, &size, NULL);
    sceClibMemset(x, 0, sizeof(x));
    sceClibMemset(y, 0, sizeof(y));
    size = 0xDEAD;
    timeout = 1000;
    PRINT("receive ASAP with less in the pipe than asked", sceKernelReceiveMsgPipeVector(pipe, receive, 2, MSG_PIPE_MODE_ASAP, &size, &timeout));
    PRINT("the size received", size);
    PRINT("it fills the first piece first", sceClibMemcmp(x, "abc", 3) == 0 && x[3] == 0);
    size = 0xDEAD;
    timeout = 1000;
    PRINT("receive FULL from an empty pipe", sceKernelReceiveMsgPipeVector(pipe, receive, 2, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("the size received", size);

    // Bad lists
    PRINT("send to an unknown UID", sceKernelSendMsgPipeVector(0x12345, send, 3, MSG_PIPE_MODE_ASAP, &size, NULL));
    PRINT("receive from an unknown UID", sceKernelReceiveMsgPipeVector(0x12345, receive, 2, MSG_PIPE_MODE_ASAP, &size, NULL));
    PRINT("send with mode 0x200", sceKernelSendMsgPipeVector(pipe, send, 1, 0x200, &size, NULL));

    // Long lists of empty pieces
    static const SceUInt32 counts[3] = { 1000, 4096, MAX_PIECES };
    for (int i = 0; i < 3; i++) {
        timeout = 1000;
        PRINTF(sceKernelSendMsgPipeVector(pipe, many, counts[i], MSG_PIPE_MODE_ASAP, &size, &timeout), "send a list of %u empty pieces", (unsigned)counts[i]);
        timeout = 1000;
        PRINTF(sceKernelReceiveMsgPipeVector(pipe, many, counts[i], MSG_PIPE_MODE_ASAP, &size, &timeout), "receive into a list of %u empty pieces", (unsigned)counts[i]);
    }

    // Pieces that add up to more than the pipe holds, one piece that alone does, and pieces that add
    // up to exactly what it holds
    MsgPipeVector over[2] = { { big, 3000 }, { big + 3000, 3000 } };
    MsgPipeVector one_over[1] = { { big, 4097 } };
    MsgPipeVector exact[2] = { { big, 2048 }, { big + 2048, 2048 } };
    timeout = 1000;
    PRINT("send pieces that add up to more than the pipe holds", sceKernelSendMsgPipeVector(pipe, over, 2, MSG_PIPE_MODE_ASAP, &size, &timeout));
    timeout = 1000;
    PRINT("receive into pieces that add up to more than the pipe holds", sceKernelReceiveMsgPipeVector(pipe, over, 2, MSG_PIPE_MODE_ASAP, &size, &timeout));
    timeout = 1000;
    PRINT("send one piece larger than the pipe", sceKernelSendMsgPipeVector(pipe, one_over, 1, MSG_PIPE_MODE_ASAP, &size, &timeout));
    size = 0xDEAD;
    timeout = 1000;
    PRINT("send pieces that add up to what the pipe holds, FULL", sceKernelSendMsgPipeVector(pipe, exact, 2, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("the size sent", size);
    size = 0xDEAD;
    timeout = 1000;
    PRINT("receive it all into two pieces, FULL", sceKernelReceiveMsgPipeVector(pipe, exact, 2, MSG_PIPE_MODE_FULL, &size, &timeout));
    PRINT("the size received", size);

    // A list of empty pieces with each mode, into an empty pipe
    static const SceUInt32 modes[2] = { MSG_PIPE_MODE_ASAP, MSG_PIPE_MODE_FULL };
    static const char *const mode_names[2] = { "ASAP", "FULL" };
    for (int m = 0; m < 2; m++) {
        size = 0xDEAD;
        timeout = 1000;
        PRINTF(sceKernelSendMsgPipeVector(pipe, empty_pieces, 2, modes[m], &size, &timeout), "send two empty pieces %s", mode_names[m]);
        PRINTF(size, "send two empty pieces %s: the size sent", mode_names[m]);
        size = 0xDEAD;
        timeout = 1000;
        PRINTF(sceKernelReceiveMsgPipeVector(pipe, empty_pieces, 2, modes[m], &size, &timeout), "receive into two empty pieces %s", mode_names[m]);
        PRINTF(size, "receive into two empty pieces %s: the size received", mode_names[m]);
    }

    // NULL pointers last, so an implementation that crashes on them loses nothing else
    PRINT("send a NULL list", sceKernelSendMsgPipeVector(pipe, NULL, 1, MSG_PIPE_MODE_ASAP, &size, NULL));
    MsgPipeVector null_piece[1] = { { NULL, 4 } };
    PRINT("send a piece with a NULL buffer", sceKernelSendMsgPipeVector(pipe, null_piece, 1, MSG_PIPE_MODE_ASAP, &size, NULL));
    MsgPipeVector null_empty_piece[1] = { { NULL, 0 } };
    PRINT("send an empty piece with a NULL buffer", sceKernelSendMsgPipeVector(pipe, null_empty_piece, 1, MSG_PIPE_MODE_ASAP, &size, NULL));
    PRINT("receive into a NULL list", sceKernelReceiveMsgPipeVector(pipe, NULL, 1, MSG_PIPE_MODE_ASAP, &size, NULL));

    test_exit();
    return 0;
}

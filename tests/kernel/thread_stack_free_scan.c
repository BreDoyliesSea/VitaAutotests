// How sceKernelGetThreadStackFreeSize looks at a thread's stack. Each case uses a new thread that
// waits right after it starts, so only the top of its stack is used, and the main thread then
// changes one spot far below that while it waits: a whole word, one byte at each offset within a
// word, a word set to 0xFFFFFFFF, the word just above the lowest one, and the lowest word. Each
// case prints whether the free size is the one the untouched stack had, 16 times the index of the
// changed word, or 4 times the offset of the changed byte.
//
// On a PS Vita TV on firmware 3.65 the free size is 16 times the index of the lowest word above the
// lowest one that isn't 0xFFFFFFFF, whatever is above it: a byte at any offset counts its whole
// word, a word set to 0xFFFFFFFF counts as unused, and the lowest word doesn't count.
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define STACK_SIZE 0x4000
// The changed word, far below where the thread's own use reaches
#define WORD 0x100

static SceUID sema;

static int waiter(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceUInt timeout = TEST_WAIT_US;
    sceKernelWaitSema(sema, 1, &timeout);
    return 0;
}

typedef enum Change { NONE, WORD_VALUE, BYTE_0, BYTE_1, BYTE_2, BYTE_3, WORD_FILL, WORD_1, LOWEST } Change;

// Starts a new thread, changes its stack as asked while it waits, and returns its free size
static int free_size_after(Change change) {
    const SceUID thread = sceKernelCreateThread("scan", waiter, SCE_KERNEL_DEFAULT_PRIORITY_USER, STACK_SIZE, 0, 0, NULL);
    sceKernelStartThread(thread, 0, NULL);
    TEST_WAIT_FOR(test_in_wait(thread, WAIT_TYPE_SEMA, sema));
    SceKernelThreadInfo info = { .size = sizeof(info) };
    sceKernelGetThreadInfo(thread, &info);
    volatile SceUInt32 *words = (volatile SceUInt32 *)info.stack;
    volatile unsigned char *bytes = (volatile unsigned char *)info.stack;
    switch (change) {
    case NONE: break;
    case WORD_VALUE: words[WORD] = 0x12345678; break;
    case BYTE_0:
    case BYTE_1:
    case BYTE_2:
    case BYTE_3: bytes[WORD * 4 + (change - BYTE_0)] = 0x00; break;
    case WORD_FILL: words[WORD] = 0xFFFFFFFF; break;
    case WORD_1: words[1] = 0x12345678; break;
    case LOWEST: words[0] = 0xFFFFFFFF; break;
    }
    const int size = sceKernelGetThreadStackFreeSize(thread);
    sceKernelSignalSema(sema, 1);
    test_join_thread(thread);
    return size;
}

int main(void) {
    sema = sceKernelCreateSema("sema", 0, 0, 1, NULL);
    const int untouched = free_size_after(NONE);
    PRINT("untouched: more than 16 times the index of the changed word", untouched > 16 * WORD);

    const int word = free_size_after(WORD_VALUE);
    PRINT("a word changed: the free size of the untouched stack", word == untouched);
    PRINT("a word changed: 16 times its index", word == 16 * WORD);

    static const char *const names[4] = { "byte 0", "byte 1", "byte 2", "byte 3" };
    for (int b = 0; b < 4; b++) {
        const int size = free_size_after((Change)(BYTE_0 + b));
        PRINTF(size == untouched, "%s of the word changed: the free size of the untouched stack", names[b]);
        PRINTF(size == 16 * WORD, "%s of the word changed: 16 times the index of its word", names[b]);
        PRINTF(size == 4 * (WORD * 4 + b), "%s of the word changed: 4 times its offset", names[b]);
    }

    const int fill = free_size_after(WORD_FILL);
    PRINT("a word set to 0xFFFFFFFF: the free size of the untouched stack", fill == untouched);

    const int word1 = free_size_after(WORD_1);
    PRINT("the word above the lowest changed: the free size", word1);

    const int lowest = free_size_after(LOWEST);
    PRINT("the lowest word set to 0xFFFFFFFF: the free size of the untouched stack", lowest == untouched);
    PRINT("the lowest word set to 0xFFFFFFFF: 4 times the stack size", lowest == 4 * STACK_SIZE);

    sceKernelDeleteSema(sema);
    test_exit();
    return 0;
}

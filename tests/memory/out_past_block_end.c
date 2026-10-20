// Thread, sync and memory block calls given an out pointer near the end of a memory block that
// has no block after it, to see whether a call checks the whole range it writes before writing,
// and whether it goes by the size word of an info struct. A Vita doesn't promise where blocks go,
// so the test makes 16 blocks of 64 KiB USER_RW, each followed by a 64 KiB spacer block it frees
// after, and uses the first block with 64 KiB in no block right after it, as memblock_find_gaps
// does. It ends with test_skip() if there is none. Every object a case needs is made before the
// blocks, so nothing takes the addresses after the block. Before each case the test sets the last
// 0x100 bytes of the block to a marker and the size word, if the call takes one. Each case prints
// the result and a mask of what the call changed between the pointer and the end of the block: bit
// n for word n, or for byte n when the pointer isn't word aligned. The cases, in this order:
// - the whole struct or value fits before the end: the info of a semaphore, a thread and a mutex,
//   sceKernelGetMemBlockInfoByAddr, sceKernelGetFreeMemorySize, sceKernelGetMemBlockBase, the
//   process time, a timer's time and a thread's exit status
// - an info call given a size word smaller than its struct, with only that much room: 0x20 for a
//   semaphore, 4 and 0 for a semaphore, 0x40 for a thread and 0x20 for a mutex
// - a size word bigger than the struct: 0x40 and 0x100 for a semaphore, and 0x20 for the memory
//   block info, to see which check comes first
// - the struct or value starts before the end and runs past it: the info calls with their struct's
//   size, a timer's time 4 bytes before the end (not the process time, since
//   sceKernelGetProcessTime crashes the app on a Vita given memory it can't write), the block base,
//   a thread's exit status, the bits of an event flag poll with clear and the count of a semaphore
//   cancel 2 bytes before it, and a message pipe receive of 16 bytes 8 bytes before it. The poll,
//   the cancel and the receive also print what they did anyway: the pattern left, the count set and
//   the bytes left in the pipe.
// - a lightweight condition variable's info by work area and by UID: the whole struct, then size
//   words of 0x20, 8, 4 and 0 with only that much room, the cases by work area last
// The app itself never touches the addresses after the block.
//
// On a PS Vita TV on firmware 3.65 every call writes its whole struct or value when it fits, and an
// info call given a smaller size word writes only that many bytes, nothing for size 0. A size word
// bigger than the struct fails with 0x8002710B (NO_MEMORY) for the semaphore info and 0x80020005
// (INVALID_ARGUMENT) for the memory block info, before anything is written. A struct that runs past
// the block's end fails with 0x80022005 (INVALID_MEMORY_ACCESS) and writes nothing, so the info
// calls check their whole struct first. A single value that runs past the end fails the same way,
// but writes the part that fits, and the calls that change their object still do it: the poll
// clears the bits, the cancel sets the count and the receive takes all 16 bytes. The lightweight
// condition variable info by UID needs only the room its size word asks for, while the info by
// work area fails with 0x80022005 and writes nothing whenever its whole struct of 0x38 bytes
// doesn't fit, whatever the size word says.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"
#include "threadmgr_extra.h"

#define KIB 0x400
#define SIZE (64 * KIB)
#define CANDIDATES 16
#define AREA 0x100
#define MARKER 0xA5A5A5A5
#define NO_SIZE 0xFFFFFFFF
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct Block {
    SceUID uid;
    char *base;
} Block;

enum Call {
    SEMA_INFO,
    THREAD_INFO,
    MUTEX_INFO,
    BLOCK_INFO,
    FREE_SIZE,
    BLOCK_BASE,
    PROCESS_TIME,
    TIMER_TIME,
    EXIT_STATUS,
    POLL_FLAG,
    CANCEL_SEMA,
    RECEIVE,
    LW_COND_INFO,
    LW_COND_INFO_BY_ID,
};

typedef struct Case {
    const char *name;
    enum Call call;
    // How many bytes before the end of the block the pointer is
    unsigned int back;
    // The size word set at the pointer, or NO_SIZE
    SceUInt32 size;
} Case;

static const Case cases[] = {
    { "semaphore info, all of it fits", SEMA_INFO, 0x3C, 0x3C },
    { "thread info, all of it fits", THREAD_INFO, 0x80, 0x80 },
    { "mutex info, all of it fits", MUTEX_INFO, 0x40, 0x40 },
    { "block info, all of it fits", BLOCK_INFO, 0x18, 0x18 },
    { "free memory size, all of it fits", FREE_SIZE, 0x10, 0x10 },
    { "block base, all of it fits", BLOCK_BASE, 4, NO_SIZE },
    { "process time, all of it fits", PROCESS_TIME, 8, NO_SIZE },
    { "timer time, all of it fits", TIMER_TIME, 8, NO_SIZE },
    { "exit status, all of it fits", EXIT_STATUS, 4, NO_SIZE },

    { "semaphore info of size 0x20, 0x20 bytes before the end", SEMA_INFO, 0x20, 0x20 },
    { "semaphore info of size 4, 4 bytes before the end", SEMA_INFO, 4, 4 },
    { "semaphore info of size 0, 4 bytes before the end", SEMA_INFO, 4, 0 },
    { "thread info of size 0x40, 0x40 bytes before the end", THREAD_INFO, 0x40, 0x40 },
    { "mutex info of size 0x20, 0x20 bytes before the end", MUTEX_INFO, 0x20, 0x20 },

    { "semaphore info of size 0x40, 0x3C bytes before the end", SEMA_INFO, 0x3C, 0x40 },
    { "semaphore info of size 0x100, 4 bytes before the end", SEMA_INFO, 4, 0x100 },
    { "block info of size 0x20, 0x18 bytes before the end", BLOCK_INFO, 0x18, 0x20 },

    { "semaphore info, 0x20 bytes before the end", SEMA_INFO, 0x20, 0x3C },
    { "thread info, 0x40 bytes before the end", THREAD_INFO, 0x40, 0x80 },
    { "mutex info, 0x20 bytes before the end", MUTEX_INFO, 0x20, 0x40 },
    { "block info, 0x10 bytes before the end", BLOCK_INFO, 0x10, 0x18 },
    { "block info, 4 bytes before the end", BLOCK_INFO, 4, 0x18 },
    { "free memory size, 8 bytes before the end", FREE_SIZE, 8, 0x10 },
    { "timer time, 4 bytes before the end", TIMER_TIME, 4, NO_SIZE },
    { "block base, 2 bytes before the end", BLOCK_BASE, 2, NO_SIZE },
    { "exit status, 2 bytes before the end", EXIT_STATUS, 2, NO_SIZE },
    { "event flag poll with clear, the bits 2 bytes before the end", POLL_FLAG, 2, NO_SIZE },
    { "semaphore cancel to 3, the count 2 bytes before the end", CANCEL_SEMA, 2, NO_SIZE },
    { "message pipe receive of 16 bytes, 8 bytes before the end", RECEIVE, 8, NO_SIZE },

    { "lightweight condition variable info, all of it fits", LW_COND_INFO, 0x38, 0x38 },
    { "lightweight condition variable info by UID, all of it fits", LW_COND_INFO_BY_ID, 0x38, 0x38 },
    { "lightweight condition variable info by UID of size 0x20, 0x20 bytes before the end", LW_COND_INFO_BY_ID, 0x20, 0x20 },
    { "lightweight condition variable info by UID of size 4, 4 bytes before the end", LW_COND_INFO_BY_ID, 4, 4 },
    { "lightweight condition variable info of size 0x20, 0x20 bytes before the end", LW_COND_INFO, 0x20, 0x20 },
    { "lightweight condition variable info of size 8, 8 bytes before the end", LW_COND_INFO, 8, 8 },
    { "lightweight condition variable info of size 4, 4 bytes before the end", LW_COND_INFO, 4, 4 },
    { "lightweight condition variable info of size 0, 4 bytes before the end", LW_COND_INFO, 4, 0 },
};

static SceUID gap_error, sema, mutex, timer, ended, flag, cancelled, pipe, lw_cond_id;
static SceKernelLwMutexWork lw_mutex;
static SceKernelLwCondWork lw_cond;
static const Block *block;

static int ends(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0x1234;
}

static int call(enum Call c, void *out) {
    switch (c) {
    case SEMA_INFO: return sceKernelGetSemaInfo(sema, out);
    case THREAD_INFO: return sceKernelGetThreadInfo(0, out);
    case MUTEX_INFO: return sceKernelGetMutexInfo(mutex, out);
    case BLOCK_INFO: return sceKernelGetMemBlockInfoByAddr(block->base, out);
    case FREE_SIZE: return sceKernelGetFreeMemorySize(out);
    case BLOCK_BASE: return sceKernelGetMemBlockBase(block->uid, out);
    case PROCESS_TIME: return sceKernelGetProcessTime(out);
    case TIMER_TIME: return sceKernelGetTimerTime(timer, out);
    case EXIT_STATUS: return sceKernelGetThreadExitStatus(ended, out);
    case POLL_FLAG:
        return sceKernelPollEventFlag(flag, 0x1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, out);
    case CANCEL_SEMA: return sceKernelCancelSema(cancelled, 3, out);
    case RECEIVE: return sceKernelTryReceiveMsgPipe(pipe, out, 16, MSG_PIPE_MODE_FULL, NULL);
    case LW_COND_INFO: return sceKernelGetLwCondInfo(&lw_cond, out);
    case LW_COND_INFO_BY_ID: return sceKernelGetLwCondInfoById(lw_cond_id, out);
    }
    return -1;
}

// What a call did to its object anyway, for the calls that change one, or -1
static int effect(enum Call c) {
    if (c == POLL_FLAG) {
        SceKernelEventFlagInfo info = { .size = sizeof(info) };
        sceKernelGetEventFlagInfo(flag, &info);
        return info.currentPattern;
    }
    if (c == CANCEL_SEMA) {
        SceKernelSemaInfo info = { .size = sizeof(info) };
        sceKernelGetSemaInfo(cancelled, &info);
        return info.currentCount;
    }
    if (c == RECEIVE) {
        SceKernelMppInfo info = { .size = sizeof(info) };
        sceKernelGetMsgPipeInfo(pipe, &info);
        return info.bufSize - info.freeSize;
    }
    return -1;
}

static void run_case(const Case *test) {
    char *const end = block->base + SIZE;
    SceUInt32 *const area = (SceUInt32 *)(end - AREA);
    for (unsigned int i = 0; i < AREA / 4; i++)
        area[i] = MARKER;
    char *const out = end - test->back;
    if (test->size != NO_SIZE)
        *(SceUInt32 *)out = test->size;
    const int result = call(test->call, out);
    PRINTF(result, "%s", test->name);

    int mask = 0;
    if (test->back % 4 == 0) {
        for (unsigned int i = 0; i < test->back / 4; i++) {
            const SceUInt32 before = i == 0 && test->size != NO_SIZE ? test->size : MARKER;
            if (((SceUInt32 *)out)[i] != before)
                mask |= 1 << i;
        }
        PRINTF(mask, "%s: the words it changed", test->name);
    } else {
        for (unsigned int i = 0; i < test->back; i++)
            if ((unsigned char)out[i] != 0xA5)
                mask |= 1 << i;
        PRINTF(mask, "%s: the bytes it changed", test->name);
    }
    const int what = effect(test->call);
    if (what >= 0)
        PRINTF(what, "%s: what it did", test->name);
}

// Whether the 64 KiB from addr are in no block, a page at a time
static int in_no_block(const char *addr) {
    for (SceSize offset = 0; offset < SIZE; offset += 4 * KIB)
        if (sceKernelFindMemBlockByAddr(addr + offset, 0) != gap_error)
            return 0;
    return 1;
}

// Allocates 64 KiB of USER_RW into made, and returns whether it has a base
static int make(Block *made, const char *name) {
    made->uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, NULL);
    void *base = NULL;
    const int ok = made->uid >= 0 && sceKernelGetMemBlockBase(made->uid, &base) == 0 && base;
    made->base = base;
    return ok;
}

static void make_objects(void) {
    sema = sceKernelCreateSema("past end", 0, 0, 1, NULL);
    mutex = sceKernelCreateMutex("past end", 0, 0, NULL);
    timer = sceKernelCreateTimer("past end", 0, NULL);
    sceKernelStartTimer(timer);
    ended = test_start_thread(ends);
    sceKernelWaitThreadEnd(ended, NULL, NULL);
    flag = sceKernelCreateEventFlag("past end", 0, 0x3, NULL);
    cancelled = sceKernelCreateSema("past end cancel", 0, 0, 5, NULL);
    pipe = sceKernelCreateMsgPipe("past end", MSG_PIPE_TYPE_USER, 0, 16, NULL);
    static const char message[16] = "sixteen bytes..";
    sceKernelTrySendMsgPipe(pipe, (void *)message, sizeof(message), MSG_PIPE_MODE_FULL, NULL);
    sceKernelCreateLwMutex(&lw_mutex, "past end", 0, 0, NULL);
    sceKernelCreateLwCond(&lw_cond, "past end", 0, &lw_mutex, NULL);
    LwCondInfo info = { .size = sizeof(info) };
    sceKernelGetLwCondInfo(&lw_cond, &info);
    lw_cond_id = info.uid;
}

static void delete_objects(void) {
    sceKernelDeleteLwCond(&lw_cond);
    sceKernelDeleteLwMutex(&lw_mutex);
    sceKernelDeleteMsgPipe(pipe);
    sceKernelDeleteSema(cancelled);
    sceKernelDeleteEventFlag(flag);
    sceKernelDeleteThread(ended);
    sceKernelDeleteTimer(timer);
    sceKernelDeleteMutex(mutex);
    sceKernelDeleteSema(sema);
}

int main(void) {
    make_objects();
    static Block blocks[CANDIDATES], spacers[CANDIDATES];
    int made = 1;
    for (int i = 0; i < CANDIDATES; i++) {
        made &= make(&blocks[i], "past end");
        made &= make(&spacers[i], "past end spacer");
    }
    // Nothing is made from here on, so the spacers' addresses stay in no block
    for (int i = 0; i < CANDIDATES; i++)
        sceKernelFreeMemBlock(spacers[i].uid);
    // A block that isn't made fails the test, since only a layout without gaps is a skip
    if (!made) {
        PRINT("the blocks are made", 0);
        test_exit();
        return 0;
    }
    gap_error = sceKernelFindMemBlockByAddr(spacers[0].base, 0);
    if (gap_error >= 0) {
        PRINT("find the base of a freed spacer", gap_error);
        test_exit();
        return 0;
    }
    for (int i = 0; i < CANDIDATES && !block; i++)
        if (in_no_block(blocks[i].base + SIZE))
            block = &blocks[i];
    if (!block) {
        for (int i = 0; i < CANDIDATES; i++)
            sceKernelFreeMemBlock(blocks[i].uid);
        delete_objects();
        test_skip("no block has 64 KiB in no block after it");
        return 0;
    }
    // Printed once the test knows it runs, since a skip prints nothing but its own line
    PRINT("find the base of a freed spacer", gap_error);

    for (unsigned int i = 0; i < COUNT(cases); i++)
        run_case(&cases[i]);

    for (int i = 0; i < CANDIDATES; i++)
        sceKernelFreeMemBlock(blocks[i].uid);
    delete_objects();
    test_exit();
    return 0;
}

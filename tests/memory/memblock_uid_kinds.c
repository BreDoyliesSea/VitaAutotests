// sceKernelGetMemBlockBase, sceKernelCloseMemBlock and sceKernelFreeMemBlock on each kind of UID.
// The blocks are 1 MiB of USER_RW, USER_RW with ENABLE_OPEN, USER_RW_UNCACHE, USER_CDRAM_RW,
// USER_MAIN_PHYCONT_RW and USER_MAIN_CDIALOG_RW, and blocks opened with sceKernelOpenMemBlock with
// flags 0x10 and 0x30 from a USER_RW block with ENABLE_OPEN. For each kind, a block closed first:
// the close, then for an allocated block whether the free sizes are back to what they were before
// it was made, and for an opened one whether its original is still found by its base, then the
// base, a find by its base, closing it again and freeing it. Then the same for a block freed
// first, with the close last. Then the three calls on UIDs that aren't memory blocks: a freed
// block's, an unknown one, 0, -1, a semaphore's, a mutex's and the thread's own, whether a
// sceKernelGetMemBlockBase that fails leaves the out value alone, and whether the semaphore and
// the mutex still work after them. Last, a NULL out pointer with an unknown UID, to see which
// check comes first.
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID.
//
// On a PS Vita TV on firmware 3.65 the close and the free each end every kind of block, allocated
// or opened, with the free sizes back and an opened block's original still there, and then both
// calls and the base give 0x80024501 (INVALID_UID). A freed block's UID, an unknown one and 0 give
// 0x80024501 to all three calls, -1 gives it to the base call and 0x80020005 (INVALID_ARGUMENT) to
// the other two, and a semaphore, a mutex and the thread give 0x80024508 (INVALID_UID_CLASS) and
// still work after. A failing base call leaves the out value alone, and an unknown UID with a NULL
// out pointer gives 0x80020005.
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define OUT_MARKER ((void *)0x13572468)

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static const struct {
    const char *name;
    SceUInt32 type;
    SceUInt32 attr;
    int open_flags;
} kinds[] = {
    { "USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 0, 0 },
    { "USER_RW with ENABLE_OPEN", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ENABLE_OPEN, 0 },
    { "USER_RW_UNCACHE", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 0, 0 },
    { "USER_CDRAM_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0, 0 },
    { "USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, 0, 0 },
    { "USER_MAIN_CDIALOG_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_RW, 0, 0 },
    { "opened with 0x10", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ENABLE_OPEN, 0x10 },
    { "opened with 0x30", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ENABLE_OPEN, 0x30 },
};

static void *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(void *base, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr(base, 0);
    return found == block ? 0 : found < 0 ? found : 1;
}

static SceKernelFreeMemorySizeInfo free_sizes(void) {
    SceKernelFreeMemorySizeInfo info = { .size = sizeof(info) };
    sceKernelGetFreeMemorySize(&info);
    return info;
}

static int same_sizes(const SceKernelFreeMemorySizeInfo *a, const SceKernelFreeMemorySizeInfo *b) {
    return a->size_user == b->size_user && a->size_cdram == b->size_cdram
        && a->size_phycont == b->size_phycont;
}

// Makes a block of the kind, writing the allocated block to *original, and returns the UID to try
static SceUID make(unsigned int kind, SceUID *original) {
    AllocOpt opt = { .size = sizeof(opt), .attr = kinds[kind].attr };
    *original
        = sceKernelAllocMemBlock("kind", kinds[kind].type, MIB, (SceKernelAllocMemBlockOpt *)&opt);
    if (*original < 0 || !kinds[kind].open_flags)
        return *original;
    return sceKernelOpenMemBlock("kind", kinds[kind].open_flags);
}

// Makes a block of the kind and ends it first with the close or with the free, then prints what
// the calls give after that
static void end_first(unsigned int kind, int closing) {
    const char *name = kinds[kind].name, *how = closing ? "close" : "free";
    int (*const end)(SceUID) = closing ? sceKernelCloseMemBlock : sceKernelFreeMemBlock;
    int (*const other)(SceUID) = closing ? sceKernelFreeMemBlock : sceKernelCloseMemBlock;
    const SceKernelFreeMemorySizeInfo before = free_sizes();
    SceUID original;
    const SceUID uid = make(kind, &original);
    PRINTF(test_created(uid), "%s: make one to %s", name, how);
    if (uid < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        return;
    }
    void *base = base_of(uid), *original_base = base_of(original);
    PRINTF(end(uid), "%s: %s it", name, how);
    if (kinds[kind].open_flags) {
        const int found = find_result(original_base, original);
        PRINTF(found, "%s: find the original after the %s", name, how);
    } else {
        const SceKernelFreeMemorySizeInfo now = free_sizes();
        PRINTF(same_sizes(&before, &now), "%s: the free sizes are back after the %s", name, how);
    }
    void *out = NULL;
    PRINTF(sceKernelGetMemBlockBase(uid, &out), "%s: get the base after the %s", name, how);
    PRINTF(find_result(base, uid), "%s: find by its base after the %s", name, how);
    PRINTF(end(uid), "%s: %s it again", name, how);
    PRINTF(other(uid), "%s: %s it after that", name, closing ? "free" : "close");
    if (kinds[kind].open_flags)
        sceKernelFreeMemBlock(original);
}

// The three calls on a UID that isn't a memory block
static void not_a_block(const char *name, SceUID uid) {
    void *out = OUT_MARKER;
    PRINTF(sceKernelGetMemBlockBase(uid, &out), "%s: get the base", name);
    PRINTF(out == OUT_MARKER, "%s: the out value is left alone", name);
    PRINTF(sceKernelCloseMemBlock(uid), "%s: close", name);
    PRINTF(sceKernelFreeMemBlock(uid), "%s: free", name);
}

int main(void) {
    for (unsigned int i = 0; i < COUNT(kinds); i++) {
        end_first(i, 1);
        end_first(i, 0);
    }

    const SceUID freed
        = sceKernelAllocMemBlock("kind freed", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
    sceKernelFreeMemBlock(freed);
    not_a_block("a freed block", freed);
    not_a_block("an unknown UID", 0x12345);
    not_a_block("UID 0", 0);
    not_a_block("UID -1", -1);
    const SceUID sema = sceKernelCreateSema("kind sema", 0, 0, 1, NULL);
    not_a_block("a semaphore", sema);
    const SceUID mutex = sceKernelCreateMutex("kind mutex", 0, 0, NULL);
    not_a_block("a mutex", mutex);
    not_a_block("the thread", sceKernelGetThreadId());
    PRINT("the semaphore still works after them", sceKernelSignalSema(sema, 1));
    PRINT("the mutex still works after them", sceKernelLockMutex(mutex, 1, NULL));
    sceKernelUnlockMutex(mutex, 1);
    sceKernelDeleteMutex(mutex);
    sceKernelDeleteSema(sema);

    // A NULL pointer last, so an implementation that crashes on it loses nothing else
    PRINT("get the base of an unknown UID into a NULL pointer",
        sceKernelGetMemBlockBase(0x12345, NULL));
    test_exit();
    return 0;
}

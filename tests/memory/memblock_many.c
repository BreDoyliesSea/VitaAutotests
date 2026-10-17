// Many memory blocks held at once, though few enough to be safe: a version that made up to 4096
// blocks, then 256 opened ones, left a PS Vita TV hung. First 4 KiB USER_RW blocks made one after
// another, up to 512 or until one fails: whether at least 64 and 256 and all 512 were made, the
// error of the one that failed, whether every block has a UID and a base of its
// own with no two overlapping, and whether they take 4 KiB of main memory each, measured to the
// page the way app_free_sizes does, if that measure still finds room for its probes. Then whether
// every free works and the main free size is back. The count itself isn't printed. Then 64 blocks
// of 4 KiB with ENABLE_OPEN, each under a name of its own and holding its index, opened by name in
// reverse order with flags 0x10: whether each is made, whether each open gives a block that reads
// the index of the block with its name, and whether every close and free works. A block is read
// only while the find gives it back for its base.
//
// On a PS Vita TV on firmware 3.65 all 512 blocks are made, each with a UID and a base of its own,
// none overlapping, and every free works. But making them takes more main memory than 4 KiB each,
// and the main free size isn't back after the frees, so not all of what they took comes back. All
// 64 named blocks open by name in reverse order, each giving the block with its name, and close and
// free.
#include <stdlib.h>

#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define ENABLE_OPEN 0x4000
#define PROBES 320
#define MAX_BLOCKS 512
#define NAMED 64

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static SceUID probes[PROBES];
static SceUID blocks[MAX_BLOCKS];
static SceUIntPtr bases[MAX_BLOCKS];

static int reported(void) {
    SceKernelFreeMemorySizeInfo sizes = { .size = sizeof(sizes) };
    sceKernelGetFreeMemorySize(&sizes);
    return sizes.size_user;
}

// The main free size to the page, or -1 if it never drops
static int measure(void) {
    const int start = reported();
    int made = 0, now = start;
    while (made < PROBES && now == start) {
        const SceUID block
            = sceKernelAllocMemBlock("free probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
        if (block < 0)
            break;
        probes[made++] = block;
        now = reported();
    }
    for (int i = 0; i < made; i++)
        sceKernelFreeMemBlock(probes[i]);
    return now == start ? -1 : start + (made - 1) * 4 * KIB;
}

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int compare_uids(const void *a, const void *b) {
    const SceUID x = *(const SceUID *)a, y = *(const SceUID *)b;
    return x < y ? -1 : x > y;
}

static int compare_bases(const void *a, const void *b) {
    const SceUIntPtr x = *(const SceUIntPtr *)a, y = *(const SceUIntPtr *)b;
    return x < y ? -1 : x > y;
}

static void plain(void) {
    const int before = measure();
    int count = 0;
    SceUID error = 0;
    while (count < MAX_BLOCKS) {
        const SceUID block
            = sceKernelAllocMemBlock("many", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
        if (block < 0) {
            error = block;
            break;
        }
        blocks[count] = block;
        bases[count] = (SceUIntPtr)base_of(block);
        count++;
    }
    const int with = measure();

    qsort(blocks, count, sizeof(blocks[0]), compare_uids);
    qsort(bases, count, sizeof(bases[0]), compare_bases);
    int own_uids = 1, apart = 1;
    for (int i = 0; i + 1 < count; i++) {
        own_uids &= blocks[i] != blocks[i + 1];
        apart &= bases[i] != 0 && bases[i] + 4 * KIB <= bases[i + 1];
    }

    int freed = 0;
    for (int i = 0; i < count; i++)
        freed += sceKernelFreeMemBlock(blocks[i]) == 0;
    const int after = measure();

    PRINT("4 KiB blocks: at least 64 are made", count >= 64);
    PRINT("4 KiB blocks: at least 256 are made", count >= 256);
    PRINT("4 KiB blocks: all 512 are made", count == MAX_BLOCKS);
    PRINT("4 KiB blocks: the error of the one that fails", error);
    PRINT("4 KiB blocks: each has a UID of its own", own_uids);
    PRINT("4 KiB blocks: each has a base of its own and no two overlap", apart);
    PRINT("4 KiB blocks: the measure finds room with them all made", with >= 0);
    if (with >= 0 && before >= 0)
        PRINT("4 KiB blocks: they take 4 KiB of main memory each",
              before - with == count * 4 * KIB);
    PRINT("4 KiB blocks: every free works", freed == count);
    PRINT("4 KiB blocks: the main free size is back after the frees",
          before >= 0 && after == before);
}

static void named(void) {
    int made = 0;
    for (int i = 0; i < NAMED; i++) {
        char name[32];
        sceClibSnprintf(name, sizeof(name), "many named %d", i);
        AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
        blocks[i] = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB,
                                           (SceKernelAllocMemBlockOpt *)&opt);
        if (blocks[i] >= 0) {
            base_of(blocks[i])[0] = i;
            made++;
        }
    }
    int opens = 0, reads = 0, closes = 0;
    for (int i = NAMED - 1; i >= 0; i--) {
        char name[32];
        sceClibSnprintf(name, sizeof(name), "many named %d", i);
        const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
        if (opened < 0)
            continue;
        opens++;
        volatile SceUInt32 *const theirs = base_of(opened);
        reads += theirs && sceKernelFindMemBlockByAddr((void *)theirs, 0) == opened
            && theirs[0] == (SceUInt32)i;
        closes += sceKernelCloseMemBlock(opened) == 0;
    }
    int freed = 0;
    for (int i = 0; i < NAMED; i++)
        freed += blocks[i] >= 0 && sceKernelFreeMemBlock(blocks[i]) == 0;

    PRINT("named blocks: every block is made", made == NAMED);
    PRINT("named blocks: every name opens", opens == NAMED);
    PRINT("named blocks: each opened block reads the index of its name", reads == NAMED);
    PRINT("named blocks: every close works", closes == opens);
    PRINT("named blocks: every free works", freed == made);
}

int main(void) {
    // The first measure only warms up
    measure();
    plain();
    named();
    test_exit();
    return 0;
}

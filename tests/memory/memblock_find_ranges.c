// sceKernelFindMemBlockByAddr, sceKernelGetMemBlockInfoByRange and sceKernelGetMemBlockInfoByAddr
// for addresses and sizes inside one memory block: sizes of 0, 1, 2, a page and more, at the base,
// at unaligned addresses, across a page boundary inside the block, in the middle and up to the
// end. The block is 256 KiB of USER_RW. A range ends at least 64 KiB before the block's end, or
// right at its end from a page aligned address and with a whole number of pages, so no case
// depends on what follows the block. memblock_find_gaps has the ranges that end past a block.
// Then a few of the same for 256 KiB of USER_CDRAM_RW, and for a block opened with
// sceKernelOpenMemBlock with flags 0x30, where each lookup should give the opened block.
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID. An info call prints its error, or 0 when it reports the block's base and size, and 1 when
// it reports something else.
//
// On a PS Vita TV on firmware 3.65 both info calls report the block for every range here. The find
// gives the block with size 0 at any address in it, and with the block's base and its whole size,
// but every other size fails with 0x80020001 (ERROR), even a byte at the base. The CDRAM block and
// the opened block behave the same.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define SIZE (256 * KIB)
#define ENABLE_OPEN 0x4000
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

typedef struct Range {
    SceSize offset;
    SceSize size;
} Range;

// Every range for the USER_RW block
static const Range ranges[] = {
    { 0, 0 },
    { 0, 1 },
    { 0, 2 },
    { 0, 4 * KIB - 1 },
    { 0, 4 * KIB },
    { 0, 4 * KIB + 1 },
    { 0, 64 * KIB },
    { 0, SIZE - 64 * KIB },
    { 0, SIZE },
    { 1, 0 },
    { 1, 1 },
    { 1, 4 * KIB - 1 },
    { 1, 4 * KIB },
    { 2, 2 },
    { 3, 2 },
    { 4 * KIB - 1, 0 },
    { 4 * KIB - 1, 1 },
    { 4 * KIB - 1, 2 },
    { 4 * KIB - 4, 8 },
    { 4 * KIB, 0 },
    { 4 * KIB, 1 },
    { 4 * KIB, 4 * KIB },
    { 4 * KIB, SIZE - 4 * KIB },
    { SIZE / 2 - 1, 2 },
    { SIZE / 2, 0 },
    { SIZE / 2, 1 },
    { SIZE / 2 + 1, 0 },
    { SIZE / 2 + 0x800, 4 * KIB },
    { SIZE - 4 * KIB, 0 },
    { SIZE - 4 * KIB, 4 * KIB },
};

// The few for the other blocks
static const Range few_ranges[] = {
    { 0, 0 },
    { 0, SIZE },
    { 1, 1 },
    { 4 * KIB - 1, 2 },
    { SIZE / 2, 1 },
    { SIZE - 4 * KIB, 4 * KIB },
};

// The offsets for the info call by address, which takes no size
static const SceSize offsets[]
    = { 0, 1, 4 * KIB - 1, 4 * KIB, SIZE / 2 + 1, SIZE - 4 * KIB, SIZE - 1 };
static const SceSize few_offsets[] = { SIZE / 2 + 1, SIZE - 1 };

static char *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(SceUID found, SceUID block) {
    return found == block ? 0 : found < 0 ? found : 1;
}

static int info_result(int error, const SceKernelMemBlockInfo *info, const char *base) {
    if (error < 0)
        return error;
    return info->mappedBase == base && info->mappedSize == SIZE ? 0 : 1;
}

// Prints the find and the info by range for each range of the block
static void check_ranges(const char *name, SceUID block, const Range *list, unsigned int count) {
    char *base = base_of(block);
    for (unsigned int i = 0; i < count; i++) {
        char *addr = base + list[i].offset;
        const SceSize size = list[i].size;
        PRINTF(find_result(sceKernelFindMemBlockByAddr(addr, size), block),
            "%s, +0x%X, 0x%X bytes: find", name, list[i].offset, size);
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        const int error = sceKernelGetMemBlockInfoByRange(addr, size, &info);
        PRINTF(info_result(error, &info, base), "%s, +0x%X, 0x%X bytes: info by range", name,
            list[i].offset, size);
    }
}

// Prints the info by address for each offset into the block
static void check_offsets(const char *name, SceUID block, const SceSize *list, unsigned int count) {
    char *base = base_of(block);
    for (unsigned int i = 0; i < count; i++) {
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        const int error = sceKernelGetMemBlockInfoByAddr(base + list[i], &info);
        PRINTF(info_result(error, &info, base), "%s, +0x%X: info by address", name, list[i]);
    }
}

int main(void) {
    const SceUID block
        = sceKernelAllocMemBlock("ranges", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, NULL);
    PRINT("allocate the USER_RW block", test_created(block));
    if (block >= 0) {
        check_ranges("USER_RW", block, ranges, COUNT(ranges));
        check_offsets("USER_RW", block, offsets, COUNT(offsets));
        sceKernelFreeMemBlock(block);
    }

    const SceUID cdram
        = sceKernelAllocMemBlock("cdram", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, SIZE, NULL);
    PRINT("allocate the USER_CDRAM_RW block", test_created(cdram));
    if (cdram >= 0) {
        check_ranges("USER_CDRAM_RW", cdram, few_ranges, COUNT(few_ranges));
        check_offsets("USER_CDRAM_RW", cdram, few_offsets, COUNT(few_offsets));
        sceKernelFreeMemBlock(cdram);
    }

    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID original = sceKernelAllocMemBlock(
        "ranges open", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("allocate the openable block", test_created(original));
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("ranges open", 0x30) : original;
    PRINT("open it with 0x30", test_created(opened));
    if (opened >= 0) {
        check_ranges("opened", opened, few_ranges, COUNT(few_ranges));
        check_offsets("opened", opened, few_offsets, COUNT(few_offsets));
        sceKernelCloseMemBlock(opened);
    }
    if (original >= 0)
        sceKernelFreeMemBlock(original);

    test_exit();
    return 0;
}

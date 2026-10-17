// sceKernelFindMemBlockByAddr and sceKernelGetMemBlockInfoByRange for ranges that reach out of a
// memory block into addresses no block has. A Vita doesn't promise where blocks go, so the test
// makes 16 blocks of 64 KiB USER_RW, each followed by a 64 KiB spacer block it frees after, and
// looks for one with 64 KiB in no block right after it and one with 64 KiB in no block right
// before it. It ends with test_skip() if either is missing. A page is in no block when the find
// gives it the error it gives the base of a freed spacer, which the test prints first. The cases,
// as offsets from the block's base:
// - after the block: its end with sizes 0, 1 and a page, the last byte with sizes 0, 1 and 2, the
//   byte before it with 1 and 2, the last word with 4 and 8, the last page with a page and a byte
//   and two pages, the whole block less a byte, and with a byte or a page more, and ranges from
//   unaligned addresses up to the end and a byte past it. Also the info call by address at the
//   end.
// - before the block: the byte before with sizes 0, 1 and 2, and the page before with a page, a
//   page and a byte, and two pages
// - last, sizes that wrap around the address space and end inside the block
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID. An info call prints its error, or 0 when it reports the block's base and size, and 1 when
// it reports something else.
//
// On a PS Vita TV on firmware 3.65 a page in no block gives 0x80022007 (VA2PA_FAULT) to both calls,
// and so does every range that reaches past the block's end or starts before it, even by one byte.
// Inside the block the info call takes any range, while the find with a size other than 0 fails
// with 0x80020001 (ERROR), as in memblock_find_ranges. Sizes that wrap around the address space
// give 0x80020005 (INVALID_ARGUMENT) to both.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define SIZE (64 * KIB)
#define CANDIDATES 16
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct Block {
    SceUID uid;
    char *base;
} Block;

typedef struct Range {
    int offset;
    SceSize size;
} Range;

static const Range after[] = {
    { SIZE, 0 },
    { SIZE, 1 },
    { SIZE, 4 * KIB },
    { SIZE - 1, 0 },
    { SIZE - 1, 1 },
    { SIZE - 1, 2 },
    { SIZE - 2, 1 },
    { SIZE - 2, 2 },
    { SIZE - 4, 4 },
    { SIZE - 4, 8 },
    { SIZE - 4 * KIB, 4 * KIB + 1 },
    { SIZE - 4 * KIB, 8 * KIB },
    { SIZE - 4 * KIB + 1, 4 * KIB - 1 },
    { SIZE - 4 * KIB + 1, 4 * KIB },
    { 0, SIZE - 1 },
    { 0, SIZE + 1 },
    { 0, SIZE + 4 * KIB },
    { 1, SIZE - 1 },
    { 1, SIZE },
    { 4 * KIB + 1, SIZE - 4 * KIB - 1 },
    { 4 * KIB + 1, SIZE - 4 * KIB },
};

static const Range before[] = {
    { -1, 0 },
    { -1, 1 },
    { -1, 2 },
    { -4 * KIB, 4 * KIB },
    { -4 * KIB, 4 * KIB + 1 },
    { -4 * KIB, 8 * KIB },
};

static const Range wraps[] = {
    { SIZE / 2, 0xFFFFFFFF },
    { SIZE / 2, 0xFFFFF000 },
};

static SceUID gap_error;

static int find_result(SceUID found, SceUID block) {
    return found == block ? 0 : found < 0 ? found : 1;
}

// Whether the 64 KiB from addr are in no block, a page at a time
static int in_no_block(const char *addr) {
    for (SceSize offset = 0; offset < SIZE; offset += 4 * KIB)
        if (sceKernelFindMemBlockByAddr(addr + offset, 0) != gap_error)
            return 0;
    return 1;
}

// Allocates 64 KiB of USER_RW into block, and returns whether it has a base
static int make(Block *block, const char *name) {
    block->uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SIZE, NULL);
    void *base = NULL;
    const int made = block->uid >= 0 && sceKernelGetMemBlockBase(block->uid, &base) == 0 && base;
    block->base = base;
    return made;
}

static void check_ranges(
    const char *name, const Block *block, const Range *list, unsigned int count) {
    for (unsigned int i = 0; i < count; i++) {
        char *addr = block->base + list[i].offset;
        const SceSize size = list[i].size;
        const char *sign = list[i].offset < 0 ? "-" : "+";
        const unsigned int offset = list[i].offset < 0 ? -list[i].offset : list[i].offset;
        PRINTF(find_result(sceKernelFindMemBlockByAddr(addr, size), block->uid),
            "%s, %s0x%X, 0x%X bytes: find", name, sign, offset, size);
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        int result = sceKernelGetMemBlockInfoByRange(addr, size, &info);
        if (result == 0)
            result = info.mappedBase == block->base && info.mappedSize == SIZE ? 0 : 1;
        PRINTF(result, "%s, %s0x%X, 0x%X bytes: info by range", name, sign, offset, size);
    }
}

int main(void) {
    static Block blocks[CANDIDATES], spacers[CANDIDATES];
    int made = 1;
    for (int i = 0; i < CANDIDATES; i++) {
        made &= make(&blocks[i], "gaps");
        made &= make(&spacers[i], "gaps spacer");
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

    const Block *end_gap = NULL, *start_gap = NULL;
    for (int i = 0; i < CANDIDATES; i++) {
        if (!end_gap && in_no_block(blocks[i].base + SIZE))
            end_gap = &blocks[i];
        if (!start_gap && in_no_block(blocks[i].base - SIZE))
            start_gap = &blocks[i];
    }
    if (!end_gap || !start_gap) {
        for (int i = 0; i < CANDIDATES; i++)
            sceKernelFreeMemBlock(blocks[i].uid);
        test_skip("no block has 64 KiB in no block after it, or none before it");
        return 0;
    }
    // Printed once the test knows it runs, since a skip prints nothing but its own line
    PRINT("find the base of a freed spacer", gap_error);

    check_ranges("after", end_gap, after, COUNT(after));
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetMemBlockInfoByAddr(end_gap->base + SIZE, &info),
        "after, +0x%X: info by address", SIZE);
    check_ranges("before", start_gap, before, COUNT(before));
    // Sizes that wrap, last, so an implementation that walks the range loses nothing else
    check_ranges("wrap", end_gap, wraps, COUNT(wraps));

    for (int i = 0; i < CANDIDATES; i++)
        sceKernelFreeMemBlock(blocks[i].uid);
    test_exit();
    return 0;
}

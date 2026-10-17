// How long blocks opened with sceKernelOpenMemBlock live next to their original and to each
// other. Each original is 1 MiB of USER_RW with ENABLE_OPEN. The cases:
// - three opened blocks, with flags 0x30, 0x10 and 0x30: whether a write through the first shows
//   through the other two, then the first closed and the third freed with the usual free, and
//   after each, whether the second is still found by its base and reads what the original holds.
//   Then opening the original again, and whether that one reads it too.
// - the original closed with sceKernelCloseMemBlock while an opened block lives, then for the
//   opened block the base, a find by its base and the info call, opening the original's name
//   again and closing the opened block
// - the same with the original freed with the usual free
// In the last two, how much main memory is taken, in KiB, while both live, once the original is
// gone and once the opened block is closed too. Nothing is read through an opened block once its
// original is gone, since that faults on a Vita, and a read first checks that the find call
// still gives the opened block.
//
// A find prints 0 when it gives the block, the error when it fails and 1 when it gives another
// UID.
//
// On a PS Vita TV on firmware 3.65 a write through one opened block shows through the others and
// the original. Closing one opened block or freeing another with the usual free leaves the rest
// working, and the original opens again. Closing or freeing the original works while an opened
// block lives, and gives its memory back at once. The opened block then still gives its old base by
// its UID, but the find and the info call give 0x80022007 for that address, the original's name no
// longer opens (0x8002450A), and closing the opened block works.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static SceUID alloc_openable(const char *name) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    return sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB, (SceKernelAllocMemBlockOpt *)&opt);
}

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(volatile SceUInt32 *base, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr((void *)base, 0);
    return found == block ? 0 : found < 0 ? found : 1;
}

// Whether the block at base is still found and holds value, read only if it's found
static int reads(volatile SceUInt32 *base, SceUID block, SceUInt32 value) {
    return find_result(base, block) == 0 && base[0] == value;
}

// Writes value at base if the block is still found there
static void write_found(volatile SceUInt32 *base, SceUID block, SceUInt32 value) {
    if (find_result(base, block) == 0)
        base[0] = value;
}

static int main_taken(const SceKernelFreeMemorySizeInfo *before) {
    SceKernelFreeMemorySizeInfo now = { .size = sizeof(now) };
    sceKernelGetFreeMemorySize(&now);
    return (before->size_user - now.size_user) / KIB;
}

static void three_opened(void) {
    const SceUID original = alloc_openable("lifetime");
    PRINT("allocate the original", test_created(original));
    if (original < 0)
        return;
    volatile SceUInt32 *mine = base_of(original);
    mine[0] = 0x11111111;
    const SceUID first = sceKernelOpenMemBlock("lifetime", 0x30);
    const SceUID second = sceKernelOpenMemBlock("lifetime", 0x10);
    const SceUID third = sceKernelOpenMemBlock("lifetime", 0x30);
    PRINT("open it with 0x30", test_created(first));
    PRINT("open it with 0x10", test_created(second));
    PRINT("open it with 0x30 again", test_created(third));
    if (first < 0 || second < 0 || third < 0) {
        sceKernelFreeMemBlock(original);
        return;
    }
    volatile SceUInt32 *first_base = base_of(first), *second_base = base_of(second),
                       *third_base = base_of(third);
    write_found(first_base, first, 0x22222222);
    PRINT("a write through the first shows through the second",
        reads(second_base, second, 0x22222222));
    PRINT("and through the third", reads(third_base, third, 0x22222222));
    PRINT("and in the original", reads(mine, original, 0x22222222));

    PRINT("close the first", sceKernelCloseMemBlock(first));
    PRINT("find the first after that", find_result(first_base, first));
    PRINT("find the second after that", find_result(second_base, second));
    write_found(mine, original, 0x33333333);
    PRINT("the second reads the original after that", reads(second_base, second, 0x33333333));

    PRINT("free the third with the usual free", sceKernelFreeMemBlock(third));
    PRINT("find the third after that", find_result(third_base, third));
    PRINT("find the second after that", find_result(second_base, second));
    PRINT("find the original after that", find_result(mine, original));
    write_found(mine, original, 0x44444444);
    PRINT("the second reads the original after that", reads(second_base, second, 0x44444444));

    const SceUID again = sceKernelOpenMemBlock("lifetime", 0x10);
    PRINT("open the original again", test_created(again));
    if (again >= 0) {
        PRINT("the new one reads the original", reads(base_of(again), again, 0x44444444));
        sceKernelCloseMemBlock(again);
    }
    sceKernelCloseMemBlock(second);
    sceKernelFreeMemBlock(original);
}

// Ends the original with end while an opened block lives, and prints what's left of that block
static void original_gone(const char *how, int (*end)(SceUID)) {
    SceKernelFreeMemorySizeInfo before = { .size = sizeof(before) };
    sceKernelGetFreeMemorySize(&before);
    const SceUID original = alloc_openable("lifetime gone");
    const SceUID opened = original >= 0 ? sceKernelOpenMemBlock("lifetime gone", 0x30) : original;
    PRINTF(test_created(original), "%s: allocate the original", how);
    PRINTF(test_created(opened), "%s: open it", how);
    if (opened < 0) {
        if (original >= 0)
            sceKernelFreeMemBlock(original);
        return;
    }
    volatile SceUInt32 *theirs = base_of(opened);
    PRINTF(main_taken(&before), "%s: KiB of main memory taken while both live", how);

    PRINTF(end(original), "%s: end the original", how);
    PRINTF(main_taken(&before), "%s: KiB of main memory taken after that", how);
    void *out = NULL;
    PRINTF(sceKernelGetMemBlockBase(opened, &out), "%s: get the opened one's base after that", how);
    PRINTF(out == theirs, "%s: it's the same base", how);
    PRINTF(find_result(theirs, opened), "%s: find the opened block after that", how);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    PRINTF(sceKernelGetMemBlockInfoByAddr((void *)theirs, &info),
        "%s: info for the opened block after that", how);
    const SceUID again = sceKernelOpenMemBlock("lifetime gone", 0x30);
    PRINTF(test_created(again), "%s: open the original's name after that", how);
    if (again >= 0)
        sceKernelCloseMemBlock(again);

    PRINTF(sceKernelCloseMemBlock(opened), "%s: close the opened block", how);
    PRINTF(main_taken(&before), "%s: KiB of main memory taken after that", how);
    // In case the original outlived the end
    sceKernelFreeMemBlock(original);
}

int main(void) {
    three_opened();
    original_gone("closed", sceKernelCloseMemBlock);
    original_gone("freed", sceKernelFreeMemBlock);
    test_exit();
    return 0;
}

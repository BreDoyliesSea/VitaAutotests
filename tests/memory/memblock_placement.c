// Whether two memory blocks ever share an address, and what the lookup calls report for ranges that
// cross blocks. Addresses are printed as comparisons. The cases:
// - ten live blocks of every type: whether any two share an address
// - find and info calls for a range that ends past a block, and for an address in no block.
//   memblock_neighbors has the ones for a range over two blocks next to each other.
// - how much of each free memory size a block of each type takes
// - allocating 16 MiB of USER_RW, 16 MiB of CDRAM and 1 MiB of physically contiguous memory until
//   it fails: the error, and whether at least a few fit. After freeing them, whether one fits
//   again.
//
// On a PS Vita TV on firmware 3.65 no two blocks share an address. Blocks often go right after each
// other but not always, and a freed range isn't always reused, so nothing here depends on where
// they go. A find or info call for a range past the end of a block fails, and for an address in no
// block it fails with 0x80022007 (SCE_KERNEL_ERROR_VA2PA_FAULT). A block takes its size from the
// main, CDRAM or phycont free size by its type, and gives it back when freed. Full main and phycont
// memory fail with 0x80024302 (SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE), and full CDRAM with
// 0x80024309.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

typedef struct Block {
    SceUID uid;
    SceUIntPtr base;
    SceSize size;
} Block;

static Block alloc(const char *name, SceUInt32 type, SceSize size) {
    Block block = { .uid = sceKernelAllocMemBlock(name, type, size, NULL), .size = size };
    void *base = NULL;
    if (block.uid >= 0)
        sceKernelGetMemBlockBase(block.uid, &base);
    block.base = (SceUIntPtr)base;
    return block;
}

static void overlap(void) {
    static const struct {
        SceUInt32 type;
        SceSize size;
    } kinds[] = {
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 4 * KIB },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB },
        { SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MIB },
    };
    Block blocks[10];
    int made = 1;
    for (int i = 0; i < 10; i++) {
        blocks[i] = alloc("overlap", kinds[i % 5].type, kinds[i % 5].size);
        made &= blocks[i].uid >= 0;
    }
    int shared = 0;
    for (int i = 0; i < 10; i++)
        for (int j = i + 1; j < 10; j++)
            shared |= blocks[i].base < blocks[j].base + blocks[j].size && blocks[j].base < blocks[i].base + blocks[i].size;
    PRINT("ten blocks of every type are made", made);
    PRINT("two of them share an address", shared);
    for (int i = 0; i < 10; i++)
        sceKernelFreeMemBlock(blocks[i].uid);
}

static void lookups(void) {
    const Block block = alloc("lookup", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB);
    // The error depends on whether another block follows, so only whether it fails is printed
    const SceUID past = sceKernelFindMemBlockByAddr((void *)block.base, block.size + 4 * KIB);
    PRINT("find a range that ends past a block: it fails", past < 0);
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    PRINT("info for a range that ends past a block: it fails", sceKernelGetMemBlockInfoByRange((void *)block.base, block.size + 4 * KIB, &info) < 0);
    sceKernelFreeMemBlock(block.uid);
    PRINT("find an address in no block", sceKernelFindMemBlockByAddr((void *)block.base, 0));
    info.size = sizeof(info);
    PRINT("info for an address in no block", sceKernelGetMemBlockInfoByAddr((void *)block.base, &info));
}

static SceKernelFreeMemorySizeInfo free_sizes(void) {
    SceKernelFreeMemorySizeInfo info = { .size = sizeof(info) };
    sceKernelGetFreeMemorySize(&info);
    return info;
}

static void budgets(void) {
    static const struct {
        const char *name;
        SceUInt32 type;
        SceSize size;
    } kinds[] = {
        { "USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB },
        { "USER_RW_UNCACHE", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, MIB },
        { "USER_CDRAM_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, MIB },
        { "USER_MAIN_PHYCONT_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB },
        { "USER_MAIN_PHYCONT_NC_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, MIB },
    };
    for (unsigned int i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        const SceKernelFreeMemorySizeInfo before = free_sizes();
        const Block block = alloc("budget", kinds[i].type, kinds[i].size);
        const SceKernelFreeMemorySizeInfo after = free_sizes();
        sceKernelFreeMemBlock(block.uid);
        const SceKernelFreeMemorySizeInfo freed = free_sizes();
        PRINTF((before.size_user - after.size_user) / KIB, "%s, 1 MiB: KiB taken from main", kinds[i].name);
        PRINTF((before.size_cdram - after.size_cdram) / KIB, "%s, 1 MiB: KiB taken from CDRAM", kinds[i].name);
        PRINTF((before.size_phycont - after.size_phycont) / KIB, "%s, 1 MiB: KiB taken from phycont", kinds[i].name);
        PRINTF(freed.size_user == before.size_user && freed.size_cdram == before.size_cdram && freed.size_phycont == before.size_phycont,
            "%s, 1 MiB: freeing it gives it all back", kinds[i].name);
    }
}

#define MAX_BLOCKS 64

static void exhaust(const char *name, SceUInt32 type, SceSize size, int few) {
    static SceUID blocks[MAX_BLOCKS];
    int count = 0;
    SceUID last = 0;
    while (count < MAX_BLOCKS) {
        last = sceKernelAllocMemBlock("fill", type, size, NULL);
        if (last < 0)
            break;
        blocks[count++] = last;
    }
    // Printing only once they're freed, since the output file may need memory
    for (int i = 0; i < count; i++)
        sceKernelFreeMemBlock(blocks[i]);
    const SceUID again = sceKernelAllocMemBlock("again", type, size, NULL);
    if (again >= 0)
        sceKernelFreeMemBlock(again);
    PRINTF(last < 0 ? last : 0, "%s: the error when it's full", name);
    PRINTF(count >= few, "%s: at least %d fit", name, few);
    PRINTF(test_created(again), "%s: one fits again after freeing them", name);
}

int main(void) {
    overlap();
    lookups();
    budgets();
    exhaust("16 MiB of USER_RW", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 16 * MIB, 4);
    exhaust("16 MiB of CDRAM", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 16 * MIB, 2);
    exhaust("1 MiB of physically contiguous memory", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB, 4);
    test_exit();
    return 0;
}

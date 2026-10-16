// Whether an app can make a memory block from a base block, so that two blocks share memory. The
// base block is 64 KiB of USER_RW holding a known word at its start. Each case asks for a block
// with option size 0x14, naming the base block by UID with HAS_MIRROR_BLOCKID (0x40) or by name
// with 0x200, alone or with 0x800000 (share the base's virtual address) and 0x1000000 (share its
// physical pages). For a block it makes: whether it starts at the base block's address, whether it
// sees the base block's word, whether a write through it shows in the base block, its type and
// mapped size, and after it's freed, whether the base block is still mapped with its data. Then the
// same with an uncached type over a cached base, with sizes larger and smaller than the base, with
// a CDRAM base, with a base that doesn't exist, and with the sharing bits but no base. Then freeing
// the base block while a block made from it still exists, and last a NULL base name.
//
// On a PS Vita TV on firmware 3.65 every one of these fails with 0x80020005
// (SCE_KERNEL_ERROR_INVALID_ARGUMENT), by UID or by name, with or without the sharing bits,
// whatever the type and size. So an app can't make two blocks share memory this way.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400

#define HAS_BASE 0x40
#define HAS_BASE_NAME 0x200
#define SHARE_VBASE 0x800000
#define SHARE_PHYPAGE 0x1000000

typedef struct Opt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUID baseBlock;
    const char *baseBlockName;
} Opt;

static SceUID base_block;
static volatile SceUInt32 *base_words;

static void *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static void make_base(void) {
    base_block = sceKernelAllocMemBlock("base block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    base_words = base_of(base_block);
}

// Makes a block from the base and returns its UID or the error
static SceUID alloc_from(SceUInt32 attr, SceUID base, const char *base_name, SceUInt32 type, SceSize size) {
    Opt opt = { .size = sizeof(opt), .attr = attr, .baseBlock = base, .baseBlockName = base_name };
    return sceKernelAllocMemBlock("from base", type, size, (SceKernelAllocMemBlockOpt *)&opt);
}

// Whether the base block is still mapped where it was
static int base_mapped(void) {
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    return sceKernelGetMemBlockInfoByAddr((void *)base_words, &info) == 0 && info.mappedBase == (void *)base_words;
}

// Prints what a block made from the base block is, then frees it
static void check_block(const char *label, SceUID block, SceSize base_size) {
    PRINTF(test_created(block), "%s", label);
    if (block < 0)
        return;
    volatile SceUInt32 *words = base_of(block);
    base_words[0] = 0x600DF00D;
    PRINTF(words == base_words, "%s: it starts at the base block's address", label);
    PRINTF(words[0] == 0x600DF00D, "%s: it sees the base block's word", label);
    words[1] = 0xCAFE0001;
    PRINTF(base_words[1] == 0xCAFE0001, "%s: a write through it shows in the base block", label);
    base_words[1] = 0;
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    sceKernelGetMemBlockInfoByAddr((void *)words, &info);
    PRINTF(info.type, "%s: its type", label);
    PRINTF(info.mappedSize == base_size, "%s: its mapped size is the base block's", label);
    PRINTF(sceKernelFreeMemBlock(block), "%s: free it", label);
    const int mapped = base_mapped();
    PRINTF(mapped, "%s: the base block is still mapped after", label);
    PRINTF(mapped && base_words[0] == 0x600DF00D, "%s: and keeps its data", label);
    // The next case needs a mapped base block
    if (!mapped) {
        sceKernelFreeMemBlock(base_block);
        make_base();
    }
}

int main(void) {
    make_base();
    PRINT("allocate the base block", test_created(base_block));

    static const struct {
        const char *name;
        SceUInt32 share;
    } shares[] = {
        { "", 0 },
        { ", sharing the address", SHARE_VBASE },
        { ", sharing the pages", SHARE_PHYPAGE },
        { ", sharing both", SHARE_VBASE | SHARE_PHYPAGE },
    };
    char label[96];
    for (unsigned int i = 0; i < sizeof(shares) / sizeof(shares[0]); i++) {
        sceClibSnprintf(label, sizeof(label), "by UID%s", shares[i].name);
        check_block(label, alloc_from(HAS_BASE | shares[i].share, base_block, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB), 64 * KIB);
        sceClibSnprintf(label, sizeof(label), "by name%s", shares[i].name);
        check_block(label, alloc_from(HAS_BASE_NAME | shares[i].share, 0, "base block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB), 64 * KIB);
    }

    const SceUInt32 by_pages = HAS_BASE | SHARE_PHYPAGE;
    check_block("uncached by UID, sharing the pages", alloc_from(by_pages, base_block, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB), 64 * KIB);
    check_block("twice the size by UID, sharing the pages", alloc_from(by_pages, base_block, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 128 * KIB), 64 * KIB);
    check_block("half the size by UID, sharing the pages", alloc_from(by_pages, base_block, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 32 * KIB), 64 * KIB);
    PRINT("an unknown UID", test_created(alloc_from(by_pages, 0x12345, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB)));
    PRINT("an unknown name", test_created(alloc_from(HAS_BASE_NAME | SHARE_PHYPAGE, 0, "no such block", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB)));
    PRINT("sharing the pages with no base", test_created(alloc_from(SHARE_PHYPAGE, 0, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB)));
    PRINT("sharing the address with no base", test_created(alloc_from(SHARE_VBASE, 0, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB)));

    // A CDRAM base
    const SceUID cdram = sceKernelAllocMemBlock("cdram base", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, NULL);
    const SceUID from_cdram = alloc_from(by_pages, cdram, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB);
    PRINT("CDRAM by UID, sharing the pages", test_created(from_cdram));
    if (from_cdram >= 0) {
        volatile SceUInt32 *cdram_words = base_of(cdram), *words = base_of(from_cdram);
        cdram_words[0] = 0x0DDBA11;
        PRINT("CDRAM by UID, sharing the pages: it sees the base block's word", words[0] == 0x0DDBA11);
        sceKernelFreeMemBlock(from_cdram);
    }
    sceKernelFreeMemBlock(cdram);

    // Freeing the base block while a block made from it exists, with the first kind that works
    SceUID from = -1;
    const char *kind = NULL;
    for (unsigned int i = 0; i < sizeof(shares) / sizeof(shares[0]) && from < 0; i++) {
        from = alloc_from(HAS_BASE | shares[i].share, base_block, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB);
        kind = shares[i].name;
    }
    PRINT("a block from the base exists to free the base under", from >= 0);
    if (from >= 0) {
        volatile SceUInt32 *words = base_of(from);
        base_words[0] = 0x5EED5EED;
        PRINTF(sceKernelFreeMemBlock(base_block), "free the base block under one made by UID%s", kind);
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        const int mapped = sceKernelGetMemBlockInfoByAddr((void *)words, &info) == 0;
        PRINT("the block made from it is still mapped", mapped);
        PRINT("and still has the data", mapped && words[0] == 0x5EED5EED);
        PRINT("free the block made from it", sceKernelFreeMemBlock(from));
        PRINT("free the base block after", sceKernelFreeMemBlock(base_block));
    } else {
        PRINT("free the base block", sceKernelFreeMemBlock(base_block));
    }

    // NULL pointers go last, so an implementation that crashes on them loses nothing else
    PRINT("a NULL name", test_created(alloc_from(HAS_BASE_NAME | SHARE_PHYPAGE, 0, NULL, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB)));
    test_exit();
    return 0;
}

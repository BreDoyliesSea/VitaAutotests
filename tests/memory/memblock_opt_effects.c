// What the option sizes and attr bits that sceKernelAllocMemBlock takes from an app do, with the
// names vita-headers gives them:
// - with option sizes 0x38, 0x3C and 0x40 and the words after strBaseBlockName set to a marker,
//   whether the call leaves the first five words alone, and what it leaves in each later word: 0 if
//   unchanged, 1 the block's base, 2 its size, 3 its memory type, 4 its access, 5 its type, 6
//   something else. With 0x40 also the marker in words 5 to 14 only, and in word 15 only.
// - ENABLE_OPEN (0x4000): whether the UID it gives has bit 30 set, as a process UID does, next to a
//   plain allocation's, and whether the base, info, find and free calls take it. Then
//   sceKernelOpenMemBlock with flags 0, 0x10 and 0x30 on a block with ENABLE_OPEN and one without:
//   whether it gives the same UID, maps at the same address, sees a write through the block if it
//   doesn't, and closes. Also opening a name no block has with flags 0x10, and ENABLE_OPEN with
//   HAS_OPEN_RESTRICTIONS (0x20) and word 5 at 0, 0x10 and 0x30.
// - PHYCONT (0x200000) with USER_RW at 4 KiB, 64 KiB, 1 MiB and 1 MiB and 4 KiB, with
//   USER_RW_UNCACHE, USER_CDRAM_RW and USER_MAIN_PHYCONT_RW, and with HAS_ALIGNMENT, next to the
//   same allocations without it: the result, and for a block it makes, how much it takes from the
//   main, CDRAM and phycont free sizes, its type and its memory type
// - all three bits at once
//
// On a PS Vita TV on firmware 3.65 option sizes 0x38 and 0x3C write nothing back, and 0x40 fails
// with 0x80020005 (SCE_KERNEL_ERROR_INVALID_ARGUMENT) unless its last word is 0. ENABLE_OPEN gives
// a process UID that every call takes. sceKernelOpenMemBlock with flags 0x10 or 0x30 opens an
// ENABLE_OPEN block as a new UID that maps the same memory at another address, so a write through
// one shows through the other. Flags 0 fail with 0x80020005, and a block without ENABLE_OPEN, like
// a name no block has, fails with 0x8002450A (SCE_KERNEL_ERROR_UID_CANNOT_FIND_BY_NAME).
// ENABLE_OPEN works with HAS_OPEN_RESTRICTIONS whatever word 5 holds. PHYCONT keeps the type and
// the memory type, and takes whole MiB from the main free size: 2 MiB for 1 MiB and 4 KiB, where a
// plain block of that size takes 1 MiB. The main and CDRAM free sizes move in steps of 1 MiB, so a
// block under 1 MiB may take nothing from them.
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000

#define ENABLE_OPEN 0x4000
#define HAS_OPEN_RESTRICTIONS 0x20
#define MARKER 0xA5A5A5A5

typedef struct Opt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
    SceUInt32 extra[11];
} Opt;

static void *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

// What a later option word holds after the call, by the codes in the header
static int word_kind(SceUInt32 word, const SceKernelMemBlockInfo *info) {
    if (word == MARKER)
        return 0;
    if (word == (SceUInt32)(SceUIntPtr)info->mappedBase)
        return 1;
    if (word == info->mappedSize)
        return 2;
    if (word == (SceUInt32)info->memoryType)
        return 3;
    if (word == info->access)
        return 4;
    if (word == info->type)
        return 5;
    return 6;
}

// Allocates with the option size and the marker in words from to to, and prints what each later word holds
static void output_words(SceSize size, unsigned int from, unsigned int to) {
    Opt opt;
    sceClibMemset(&opt, 0, sizeof(opt));
    opt.size = size;
    for (unsigned int i = from; i <= to; i++)
        opt.extra[i - 5] = MARKER;
    const SceUID block = sceKernelAllocMemBlock("words", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINTF(test_created(block), "option size 0x%X, the marker in words %u to %u", size, from, to);
    if (block < 0)
        return;
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    sceKernelGetMemBlockInfoByAddr(base_of(block), &info);
    sceKernelFreeMemBlock(block);
    PRINTF(opt.size == size && opt.attr == 0 && opt.alignment == 0 && opt.reserved == 0 && opt.baseBlockName == NULL,
        "option size 0x%X, the marker in words %u to %u: the first five words are unchanged", size, from, to);
    for (unsigned int i = from; i <= to; i++)
        PRINTF(word_kind(opt.extra[i - 5], &info), "option size 0x%X, the marker in words %u to %u: word %u", size, from, to, i);
}

static SceUID alloc_attr(SceUInt32 type, SceSize size, SceUInt32 attr, SceSize alignment) {
    Opt opt = { .size = 0x14, .attr = attr, .alignment = alignment };
    return sceKernelAllocMemBlock("attr", type, size, (SceKernelAllocMemBlockOpt *)&opt);
}

static void enable_open(void) {
    const SceUID plain = sceKernelAllocMemBlock("plain", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, NULL);
    const SceUID block = alloc_attr(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, ENABLE_OPEN, 0);
    PRINT("a plain block's UID has bit 30 set", plain >= 0 && ((SceUInt32)plain >> 30 & 1));
    PRINT("ENABLE_OPEN", test_created(block));
    if (block >= 0) {
        PRINT("ENABLE_OPEN: its UID has bit 30 set", (SceUInt32)block >> 30 & 1);
        void *base = NULL;
        PRINT("ENABLE_OPEN: get the base", sceKernelGetMemBlockBase(block, &base));
        SceKernelMemBlockInfo info = { .size = sizeof(info) };
        PRINT("ENABLE_OPEN: info by address", sceKernelGetMemBlockInfoByAddr(base, &info));
        const SceUID found = sceKernelFindMemBlockByAddr(base, 0);
        PRINT("ENABLE_OPEN: find gives its UID", found == block);
        PRINT("ENABLE_OPEN: free", sceKernelFreeMemBlock(block));
    }
    sceKernelFreeMemBlock(plain);
}

// Opens a block by name with sceKernelOpenMemBlock, prints the result and what the UID it gives maps
static void open_block(const char *label, const char *name, int flags, SceUID original) {
    const SceUID opened = sceKernelOpenMemBlock(name, flags);
    PRINTF(test_created(opened), "%s", label);
    if (opened < 0)
        return;
    PRINTF(opened == original, "%s: it gives the same UID", label);
    volatile SceUInt32 *mine = base_of(original), *theirs = base_of(opened);
    PRINTF(theirs == mine, "%s: it maps at the same address", label);
    if (mine && theirs && theirs != mine) {
        // A value the opened mapping doesn't hold yet, so only a write after the open can show
        const SceUInt32 value = theirs[0] + 1;
        mine[0] = value;
        PRINTF(theirs[0] == value, "%s: it sees a write through the block", label);
    }
    PRINTF(sceKernelCloseMemBlock(opened), "%s: close it", label);
}

static void open_blocks(void) {
    const SceUID openable = alloc_attr(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, ENABLE_OPEN, 0);
    const SceUID plain = sceKernelAllocMemBlock("not openable", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, NULL);
    PRINT("allocate an ENABLE_OPEN block", test_created(openable));
    static const int flags[] = { 0, 0x10, 0x30 };
    char label[64];
    for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        sceClibSnprintf(label, sizeof(label), "open the ENABLE_OPEN block, flags 0x%X", flags[i]);
        open_block(label, "attr", flags[i], openable);
        sceClibSnprintf(label, sizeof(label), "open a block without it, flags 0x%X", flags[i]);
        open_block(label, "not openable", flags[i], plain);
    }
    open_block("open a name no block has, flags 0x10", "no such block", 0x10, -1);
    sceKernelFreeMemBlock(plain);
    sceKernelFreeMemBlock(openable);

    // HAS_OPEN_RESTRICTIONS with the word vitasdk calls flags
    static const SceUInt32 restrictions[] = { 0, 0x10, 0x30 };
    for (unsigned int i = 0; i < sizeof(restrictions) / sizeof(restrictions[0]); i++) {
        Opt opt = { .size = 0x40, .attr = ENABLE_OPEN | HAS_OPEN_RESTRICTIONS };
        opt.extra[0] = restrictions[i];
        const SceUID block = sceKernelAllocMemBlock("restricted", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
        PRINTF(test_created(block), "ENABLE_OPEN and HAS_OPEN_RESTRICTIONS, word 5 0x%X", restrictions[i]);
        if (block >= 0)
            sceKernelFreeMemBlock(block);
    }
}

static SceKernelFreeMemorySizeInfo free_sizes(void) {
    SceKernelFreeMemorySizeInfo info = { .size = sizeof(info) };
    sceKernelGetFreeMemorySize(&info);
    return info;
}

// Allocates with PHYCONT and prints the result, what it took from each free size, and the block's types
static void phycont(const char *label, SceUInt32 type, SceSize size, SceUInt32 attr, SceSize alignment) {
    const SceKernelFreeMemorySizeInfo before = free_sizes();
    const SceUID block = alloc_attr(type, size, attr, alignment);
    const SceKernelFreeMemorySizeInfo after = free_sizes();
    SceKernelMemBlockInfo info = { .size = sizeof(info) };
    if (block >= 0) {
        sceKernelGetMemBlockInfoByAddr(base_of(block), &info);
        sceKernelFreeMemBlock(block);
    }
    PRINTF(test_created(block), "%s", label);
    if (block < 0)
        return;
    PRINTF((before.size_user - after.size_user) / KIB, "%s: KiB taken from main", label);
    PRINTF((before.size_cdram - after.size_cdram) / KIB, "%s: KiB taken from CDRAM", label);
    PRINTF((before.size_phycont - after.size_phycont) / KIB, "%s: KiB taken from phycont", label);
    PRINTF(info.type, "%s: its type", label);
    PRINTF(info.memoryType, "%s: its memory type", label);
}

int main(void) {
    output_words(0x38, 5, 13);
    output_words(0x3C, 5, 14);
    output_words(0x40, 5, 15);
    output_words(0x40, 5, 14);
    output_words(0x40, 15, 15);

    enable_open();
    open_blocks();

    // The same sizes without PHYCONT, to compare what each takes
    phycont("no attr, USER_RW, 4 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, 0, 0);
    phycont("no attr, USER_RW, 64 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, 0, 0);
    phycont("no attr, USER_RW, 1 MiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB, 0, 0);
    phycont("no attr, USER_RW, 1 MiB and 4 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB + 4 * KIB, 0, 0);
    phycont("no attr, USER_CDRAM_RW, 256 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, 0, 0);

    const SceUInt32 pc = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_PHYCONT;
    phycont("PHYCONT, USER_RW, 4 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4 * KIB, pc, 0);
    phycont("PHYCONT, USER_RW, 64 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, pc, 0);
    phycont("PHYCONT, USER_RW, 1 MiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB, pc, 0);
    phycont("PHYCONT, USER_RW, 1 MiB and 4 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB + 4 * KIB, pc, 0);
    phycont("PHYCONT, USER_RW_UNCACHE, 64 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, 64 * KIB, pc, 0);
    phycont("PHYCONT, USER_CDRAM_RW, 256 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 256 * KIB, pc, 0);
    phycont("PHYCONT, USER_MAIN_PHYCONT_RW, 1 MiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, MIB, pc, 0);
    phycont("PHYCONT and HAS_ALIGNMENT 64 KiB, USER_RW, 64 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
        pc | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT, 64 * KIB);
    phycont("all three bits, USER_RW, 64 KiB", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB,
        pc | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT | ENABLE_OPEN, 64 * KIB);
    test_exit();
    return 0;
}

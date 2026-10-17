// How the name of a block with ENABLE_OPEN lives next to the blocks opened from it with
// sceKernelOpenMemBlock, and whether an opened block keeps its original's alignment. Each original
// is 64 KiB of USER_RW holding a word of its own, apart from the 1 MiB one for the alignment. The
// cases:
// - an original ended while a block opened from it with flags 0x30 lives, by the usual free and by
//   sceKernelCloseMemBlock: then a new block with ENABLE_OPEN under the same name, which block an
//   open of the name maps, by the word it reads, the old opened block's base by its UID, and
//   closing the old opened block, after which the new block is still found by its base, still
//   holds its word, and its name still opens it
// - 1 MiB with ENABLE_OPEN that HAS_ALIGNMENT puts on 16 MiB, opened four times with 0x10: whether
//   the original's base and every opened block's base are 16 MiB aligned, and then flags 0x4010
//   and 0x4030, ENABLE_OPEN with the open flags
// A block is read only while the find gives it back for its base. "the block it maps" prints 1
// for the first block made under the name, 2 for the second, and 0 for neither.
//
// On a PS Vita TV on firmware 3.65 the name is free again once the original ends, by a free or a
// close, even while an opened block lives: a new ENABLE_OPEN block takes it, and opening the name
// gives the new block. The old opened block still gives its old base by its UID and closes, which
// leaves the new block and its name as they were. The opened blocks of a 16 MiB aligned original
// aren't 16 MiB aligned, and the open flags 0x4010 and 0x4030 give 0x80020005 (INVALID_ARGUMENT).
#include <psp2/kernel/sysmem.h>

#include "test.h"

#define KIB 0x400
#define MIB 0x100000
#define ENABLE_OPEN 0x4000
#define FIRST_WORD 0x11111111
#define SECOND_WORD 0x22222222
#define OPENS 4

// The option struct games pass, which ends at strBaseBlockName
typedef struct AllocOpt {
    SceSize size;
    SceUInt32 attr;
    SceSize alignment;
    SceUInt32 reserved;
    const char *baseBlockName;
} AllocOpt;

static volatile SceUInt32 *base_of(SceUID block) {
    void *base = NULL;
    sceKernelGetMemBlockBase(block, &base);
    return base;
}

static int find_result(volatile SceUInt32 *base, SceUID block) {
    const SceUID found = sceKernelFindMemBlockByAddr((void *)base, 0);
    return found == block ? 0 : found < 0 ? found : 1;
}

// Makes a block with ENABLE_OPEN under the name holding word
static SceUID make(const char *name, SceUInt32 word) {
    AllocOpt opt = { .size = sizeof(opt), .attr = ENABLE_OPEN };
    const SceUID block = sceKernelAllocMemBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 64 * KIB, (SceKernelAllocMemBlockOpt *)&opt);
    if (block >= 0)
        base_of(block)[0] = word;
    return block;
}

// Which word the block at base holds, if the find gives it back
static int which(volatile SceUInt32 *base, SceUID block) {
    if (!base || find_result(base, block) != 0)
        return 0;
    return base[0] == FIRST_WORD ? 1 : base[0] == SECOND_WORD ? 2 : 0;
}

// Opens the name with 0x10 and prints the result and which block it maps
static void open_which(const char *how, const char *what, const char *name) {
    const SceUID opened = sceKernelOpenMemBlock(name, 0x10);
    PRINTF(test_created(opened), "%s: %s", how, what);
    if (opened < 0)
        return;
    PRINTF(which(base_of(opened), opened), "%s: %s: the block it maps", how, what);
    sceKernelCloseMemBlock(opened);
}

static void reuse(const char *how, const char *name, int (*end)(SceUID)) {
    const SceUID first = make(name, FIRST_WORD);
    PRINTF(test_created(first), "%s: allocate the original", how);
    if (first < 0)
        return;
    const SceUID old = sceKernelOpenMemBlock(name, 0x30);
    PRINTF(test_created(old), "%s: open it with 0x30", how);
    if (old < 0) {
        sceKernelFreeMemBlock(first);
        return;
    }
    volatile SceUInt32 *const old_base = base_of(old);
    PRINTF(end(first), "%s: end the original", how);

    const SceUID second = make(name, SECOND_WORD);
    PRINTF(test_created(second), "%s: allocate a new block with the name", how);
    open_which(how, "open the name", name);
    void *out = NULL;
    PRINTF(sceKernelGetMemBlockBase(old, &out), "%s: the old opened block's base by its UID", how);
    PRINTF(out == (void *)old_base, "%s: it's the base it had", how);
    PRINTF(sceKernelCloseMemBlock(old), "%s: close the old opened block", how);
    if (second >= 0) {
        volatile SceUInt32 *const second_base = base_of(second);
        PRINTF(find_result(second_base, second), "%s: find the new block after that", how);
        PRINTF(which(second_base, second) == 2, "%s: the new block still holds its word", how);
    }
    open_which(how, "open the name after that", name);
    if (second >= 0)
        sceKernelFreeMemBlock(second);
    // In case the original outlived its end
    sceKernelFreeMemBlock(first);
}

static void aligned(void) {
    AllocOpt opt = { .size = sizeof(opt),
                     .attr = ENABLE_OPEN | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT,
                     .alignment = 16 * MIB };
    const SceUID original = sceKernelAllocMemBlock(
        "open aligned", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, MIB, (SceKernelAllocMemBlockOpt *)&opt);
    PRINT("aligned: allocate the original", test_created(original));
    if (original < 0)
        return;
    PRINT("aligned: its base is 16 MiB aligned", (SceUIntPtr)base_of(original) % (16 * MIB) == 0);
    SceUID opened[OPENS];
    int made = 0, all_aligned = 1;
    for (int i = 0; i < OPENS; i++) {
        opened[i] = sceKernelOpenMemBlock("open aligned", 0x10);
        if (opened[i] < 0)
            continue;
        made++;
        all_aligned &= (SceUIntPtr)base_of(opened[i]) % (16 * MIB) == 0;
    }
    PRINT("aligned: opens that work", made);
    PRINT("aligned: every opened block's base is 16 MiB aligned", made > 0 && all_aligned);
    for (int i = 0; i < OPENS; i++)
        if (opened[i] >= 0)
            sceKernelCloseMemBlock(opened[i]);

    static const int flags[] = { 0x4010, 0x4030 };
    for (unsigned int i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        const SceUID with = sceKernelOpenMemBlock("open aligned", flags[i]);
        PRINTF(test_created(with), "aligned: open it with flags 0x%X", flags[i]);
        if (with >= 0)
            sceKernelCloseMemBlock(with);
    }
    PRINT("aligned: free the original", sceKernelFreeMemBlock(original));
}

int main(void) {
    reuse("freed", "name reuse freed", sceKernelFreeMemBlock);
    reuse("closed", "name reuse closed", sceKernelCloseMemBlock);
    aligned();
    test_exit();
    return 0;
}
